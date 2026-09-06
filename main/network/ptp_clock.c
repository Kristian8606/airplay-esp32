#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ptp_clock.h"
#include "spiram_task.h"

static const char *TAG = "ptp_clock";

// PTP multicast addresses and ports
#define PTP_MULTICAST_ADDR "224.0.1.129"
#define PTP_EVENT_PORT     319
#define PTP_GENERAL_PORT   320

// PTP message types
#define PTP_MSG_SYNC       0x0
#define PTP_MSG_DELAY_REQ  0x1
#define PTP_MSG_FOLLOW_UP  0x8
#define PTP_MSG_DELAY_RESP 0x9
#define PTP_MSG_ANNOUNCE   0xB

// PTP header size and timestamp offset
#define PTP_HEADER_SIZE      34
#define PTP_TIMESTAMP_OFFSET 34
#define PTP_TIMESTAMP_SIZE   10

// Synchronization parameters
//
// WiFi-side timestamping jitter on ESP32 is ~20–30 ms in practice, so a tight
// 40 ms lock threshold can take 10+ seconds to satisfy.  Loosen the lock
// criteria to converge in <1 s while still rejecting genuine outliers via
// the median filter:
//   • LOCK_THRESHOLD_NS:    50 ms  — accept normal WiFi jitter
//   • OUTLIER_THRESHOLD_NS: 75 ms  — keep the threshold strictly larger than
//                                   LOCK_THRESHOLD_NS so a borderline sample
//                                   isn't both kept and counted against lock
//   • MIN_SAMPLES_FOR_LOCK: 4      — ~500 ms at 8 Hz SYNC rate
//   • LOCK_STABLE_TIME_MS:  250    — confirm stability without long wait
#define LOCK_THRESHOLD_NS    50000000LL // 50ms - tolerant of WiFi jitter
#define MIN_SAMPLES_FOR_LOCK 4
#define LOCK_STABLE_TIME_MS  250 // 250ms of stable readings to declare lock
#define LOCK_TIMEOUT_MS      5000
#define OUTLIER_THRESHOLD_NS 50000000LL // 50ms - reject samples beyond this
// Asymmetric filter parameters (modeled after nqptp):
// Network delays only ADD positive bias to the measured offset, so
//   offset_measured = true_offset - one_way_delay
// The LARGEST measured offsets correspond to the SHORTEST delays and are
// therefore the most accurate.  We accept positive jitter (= shorter delay)
// quickly and dampen negative jitter (= longer delay) heavily.  This causes
// the filter to converge to the minimum-delay offset, matching the behaviour
// of nqptp used by shairport-sync and ensuring tight multi-room sync.
#define SMOOTH_POS_STARTUP_DIV 1   // accept positive jitter fully at start
#define SMOOTH_POS_STEADY_DIV  16  // later, apply 1/16 of positive jitter
#define SMOOTH_NEG_DIV         256 // always apply only 1/256 of negative jitter
#define SMOOTH_NEG_CLAMP_NS    (-2500000LL) // clamp negative jitter at -2.5ms
#define STARTUP_DURATION_MS 1000 // first second: aggressive positive tracking

// Threshold above which we reset the PTP smoothing filter on resume.
// E.g. at 50 ppm crystal accuracy, 30 s of pause accumulates ~1.5 ms of drift —
// large enough to be audible in multi-room but well within the 50 ms outlier
// window.  Below this threshold the drift is negligible (<0.25 ms at 5 s).
#define PTP_LONG_PAUSE_THRESHOLD_MS 30000

/* Shairport/NQPTP does not expose a newly elected GM immediately. Keep the
 * estimator independent from audio and consider it usable only after a short
 * continuous history. The realtime audio path keeps its previous LOCAL anchor
 * while this new estimator is acquiring. */
#define RT_MASTER_READY_AGE_MS 400U
#define RT_MIN_MASTER_SAMPLES 4U

/* Realtime ALAC uses NQPTP only to establish PHASE between the current
 * remote grandmaster and ESP monotonic time. Once a GM is ready, the running
 * remote-PTP -> ESP-local map has a fixed 1:1 slope. This deliberately keeps
 * movement of the asymmetric NQPTP offset estimator out of the media rate.
 * Physical I2S rate remains controlled solely by the existing audio PID. */

#define PTP_TASK_PRIORITY_LEGACY   6U
#define PTP_TASK_PRIORITY_REALTIME 8U

// PTP state
static struct {
  bool running;
  TaskHandle_t task_handle;
  spiram_task_mem_t task_mem;
  int event_socket;
  int general_socket;

  // Synchronization state
  bool locked;
  uint32_t lock_start_ms;
  uint32_t lock_candidate_start_ms;
  uint32_t last_sync_ms;
  int64_t filtered_offset_ns; // PTP_time = local_time + offset
  uint32_t sample_count;

  // Asymmetric smoothing state (replaces median ring buffer)
  int64_t previous_offset;
  // Most recent RAW (unsmoothed) offset sample.  The smoothing filter is
  // deliberately asymmetric (see SMOOTH_* below), so filtered_offset_ns can
  // sit a long way from the truth without any existing log revealing it —
  // every timing figure the firmware prints is derived from the filtered
  // value, so an error in it is invisible to those figures.  Keeping the raw
  // sample lets callers compare the two and detect filter divergence.
  int64_t raw_offset_ns;
  uint32_t previous_offset_time_ms; // 0 = no previous sample yet
  uint32_t mastership_start_ms;     // when continuous tracking began

  // Two-step sync tracking
  uint16_t last_sync_seq;
  int64_t last_sync_local_ns;
  bool awaiting_followup;

  // Statistics
  uint32_t sync_count;
  uint32_t followup_count;
  uint32_t announce_count;
  uint32_t rejected_master_count; // SYNC/FOLLOW_UP from a non-matching master
  uint32_t outlier_count;         // samples rejected by 50ms threshold

  /* AirPlay 2 realtime-only clock-domain state. Buffered AAC does not use
   * these fields and continues through the legacy path above. */
  bool realtime_mode;
  uint32_t timing_peer_ip;        // network byte order; 0 = accept any source
  uint64_t source_clock_id;       // PTP sourcePortIdentity clock id
  uint64_t grandmaster_clock_id;  // Announce grandmasterIdentity

  int64_t rt_master_offset_ns;
  int64_t rt_previous_offset_ns;
  int64_t rt_previous_offset_time_ns;
  int64_t rt_mastership_start_ns;
  int64_t rt_last_followup_rx_ns;
  uint32_t rt_sample_count;
  bool rt_master_ready;
  uint64_t rt_last_d7_clock_id; /* observation only; never rewrites the PTP epoch */
  uint32_t rt_gm_changes;

  /* Fixed-slope remote-PTP -> ESP-local map. Phase is captured from the
   * NQPTP estimator when a GM becomes ready. The slope remains exactly 1:1
   * for the lifetime of that mastership epoch; no PTP rate learner can steer
   * the realtime audio timeline. */
  bool rt_time_map_valid;
  int64_t rt_time_ref_remote_ns;
  int64_t rt_time_ref_local_ns;

  // Master clock filter / realtime anchor-clock hint (0 = unspecified).
  uint64_t expected_clock_id;
} ptp = {0};

/* PTP state is written by the PTP task on Core 0 and read/reset from RTSP/audio
 * control paths that can run concurrently with Core 1 playout. ESP32-S3 is a
 * 32-bit CPU, so 64-bit offset/clock-id fields must not be read or written
 * lock-free across cores. Keep this lock extremely short: no socket I/O, task
 * delays or logging is performed while it is held. */
static portMUX_TYPE ptp_state_mux = portMUX_INITIALIZER_UNLOCKED;

// Parse 8-byte clockIdentity (big-endian) from PTP sourcePortIdentity
// (header bytes 20-27).
static uint64_t parse_ptp_clock_id(const uint8_t *data) {
  uint64_t id = 0;
  for (int i = 0; i < 8; i++) {
    id = (id << 8) | data[20 + i];
  }
  return id;
}

// PTPv2 Announce grandmasterIdentity is bytes 53..60.
static uint64_t parse_announce_grandmaster_id(const uint8_t *data, size_t len) {
  if (!data || len < 61U) return 0;
  uint64_t id = 0;
  for (int i = 0; i < 8; ++i) id = (id << 8) | data[53 + i];
  return id;
}

// Parse 48-bit seconds + 32-bit nanoseconds from PTP timestamp
static uint64_t parse_ptp_timestamp_ns(const uint8_t *data) {
  // Seconds: 6 bytes big-endian
  uint64_t seconds = 0;
  for (int i = 0; i < 6; i++) {
    seconds = (seconds << 8) | data[i];
  }

  // Nanoseconds: 4 bytes big-endian
  uint32_t nanos = ((uint32_t)data[6] << 24) | ((uint32_t)data[7] << 16) |
                   ((uint32_t)data[8] << 8) | (uint32_t)data[9];

  return seconds * 1000000000ULL + nanos;
}

// Get local time in nanoseconds (from esp_timer)
static inline int64_t get_local_time_ns(void) {
  return (int64_t)esp_timer_get_time() * 1000LL;
}

// Update offset with new sample using asymmetric smoothing (nqptp-style).
//
// The key insight from nqptp: since we are a passive PTP listener (no
// DELAY_REQ/DELAY_RESP), every measured offset contains a one-way network
// delay bias:  offset_measured = true_offset - delay.
// LARGER offsets come from SHORTER delays and are MORE accurate.
//
// By accepting positive jitter (larger offset = shorter delay) quickly and
// dampening negative jitter (smaller offset = longer delay) slowly, the
// filter converges to the offset corresponding to the minimum network
// delay — the best available approximation of the true clock offset.
typedef enum {
  PTP_OFFSET_EVENT_NONE = 0,
  PTP_OFFSET_EVENT_LOCKED,
  PTP_OFFSET_EVENT_LOST,
} ptp_offset_event_t;

typedef struct {
  ptp_offset_event_t event;
  int64_t filtered_offset_ns;
  int64_t dev_ns;
  uint32_t sample_count;
  uint32_t sync_count;
  uint32_t followup_count;
} ptp_offset_log_t;

static void log_offset_event(const ptp_offset_log_t *ev) {
  if (!ev) return;
  if (ev->event == PTP_OFFSET_EVENT_LOCKED) {
    ESP_LOGI(TAG,
             "LOCKED: offset=%+lldns dev=%lldns samples=%lu sync=%lu followup=%lu",
             (long long)ev->filtered_offset_ns, (long long)ev->dev_ns,
             (unsigned long)ev->sample_count, (unsigned long)ev->sync_count,
             (unsigned long)ev->followup_count);
  } else if (ev->event == PTP_OFFSET_EVENT_LOST) {
    ESP_LOGW(TAG, "LOST LOCK: dev=%lldns (threshold=%lldns)",
             (long long)ev->dev_ns, (long long)(LOCK_THRESHOLD_NS * 4));
  }
}

/* Caller must hold ptp_state_mux. */
static void update_offset_locked(int64_t new_offset_ns, uint32_t now_ms,
                                 ptp_offset_log_t *log_ev) {
  if (log_ev) memset(log_ev, 0, sizeof(*log_ev));

  ptp.last_sync_ms = now_ms;
  ptp.sample_count++;
  // Record the raw sample before any smoothing or outlier rejection, so the
  // divergence between measured and filtered offset stays observable.
  ptp.raw_offset_ns = new_offset_ns;

  int64_t smoothed_offset;

  if (ptp.previous_offset_time_ms == 0) {
    // First sample (or after reset): accept unconditionally
    smoothed_offset = new_offset_ns;
    ptp.mastership_start_ms = now_ms;
  } else {
    // Reject obvious outliers (more than 50ms from current estimate)
    int64_t diff = new_offset_ns - ptp.filtered_offset_ns;
    if (diff < 0) diff = -diff;
    if (diff > OUTLIER_THRESHOLD_NS) {
      ptp.outlier_count++;
      return;
    }

    int64_t jitter = new_offset_ns - ptp.previous_offset;
    uint32_t mastership_time_ms = now_ms - ptp.mastership_start_ms;

    if (jitter >= 0) {
      if (mastership_time_ms < STARTUP_DURATION_MS) {
        smoothed_offset = ptp.previous_offset + jitter / SMOOTH_POS_STARTUP_DIV;
      } else {
        smoothed_offset = ptp.previous_offset + jitter / SMOOTH_POS_STEADY_DIV;
      }
    } else {
      int64_t clamped_jitter = jitter;
      if (clamped_jitter < SMOOTH_NEG_CLAMP_NS) {
        clamped_jitter = SMOOTH_NEG_CLAMP_NS;
      }
      smoothed_offset = ptp.previous_offset + clamped_jitter / SMOOTH_NEG_DIV;
    }
  }

  ptp.previous_offset = smoothed_offset;
  ptp.previous_offset_time_ms = now_ms;
  ptp.filtered_offset_ns = smoothed_offset;

  if (ptp.sample_count >= MIN_SAMPLES_FOR_LOCK) {
    int64_t dev = new_offset_ns - smoothed_offset;
    if (dev < 0) dev = -dev;

    if (dev < LOCK_THRESHOLD_NS) {
      if (!ptp.locked) {
        if (ptp.lock_candidate_start_ms == 0) {
          ptp.lock_candidate_start_ms = now_ms;
        }
        if ((now_ms - ptp.lock_candidate_start_ms) >= LOCK_STABLE_TIME_MS) {
          ptp.locked = true;
          ptp.lock_start_ms = now_ms;
          ptp.lock_candidate_start_ms = 0;
          if (log_ev) {
            log_ev->event = PTP_OFFSET_EVENT_LOCKED;
            log_ev->filtered_offset_ns = ptp.filtered_offset_ns;
            log_ev->dev_ns = dev;
            log_ev->sample_count = ptp.sample_count;
            log_ev->sync_count = ptp.sync_count;
            log_ev->followup_count = ptp.followup_count;
          }
        }
      }
    } else {
      ptp.lock_candidate_start_ms = 0;
      if (ptp.locked && dev > LOCK_THRESHOLD_NS * 4) {
        ptp.locked = false;
        ptp.lock_start_ms = 0;
        if (log_ev) {
          log_ev->event = PTP_OFFSET_EVENT_LOST;
          log_ev->dev_ns = dev;
        }
      }
    }
  }
}

/* Realtime source selection follows Shairport/NQPTP: the RTSP client IP is
 * the timing peer. sourcePortIdentity is diagnostic; Announce tells us which
 * grandmaster that peer is currently forwarding. Caller holds ptp_state_mux. */
static bool realtime_source_matches_locked(uint32_t source_ip) {
  if (ptp.timing_peer_ip == 0 || source_ip == ptp.timing_peer_ip) return true;
  ptp.rejected_master_count++;
  return false;
}

/* Caller holds ptp_state_mux. Running realtime conversion intentionally
 * uses a fixed 1:1 slope:
 *   local = local_ref + (remote - remote_ref)
 * NQPTP filtered-offset convergence is therefore phase diagnostic only and
 * cannot impersonate a media clock-rate change. */
static bool realtime_affine_to_local_locked(int64_t remote_ns,
                                             int64_t *local_ns) {
  if (!local_ns || !ptp.rt_time_map_valid) return false;
  *local_ns = ptp.rt_time_ref_local_ns +
              (remote_ns - ptp.rt_time_ref_remote_ns);
  return true;
}

/* Reset only the estimator that belongs to one remote grandmaster. Audio
 * continuity is intentionally NOT represented here. Realtime ALAC owns a
 * separate RTP<->ESP-local presentation anchor and keeps using it while the
 * new GM estimator acquires. */
static void realtime_reset_master_estimator_locked(uint64_t new_gm,
                                                    uint64_t source_clock,
                                                    int64_t now_ns) {
  (void)now_ns;
  ptp.source_clock_id = source_clock;
  ptp.grandmaster_clock_id = new_gm;
  ptp.rt_master_offset_ns = 0;
  ptp.rt_previous_offset_ns = 0;
  ptp.rt_previous_offset_time_ns = 0;
  ptp.rt_mastership_start_ns = 0;
  ptp.rt_last_followup_rx_ns = 0;
  ptp.rt_sample_count = 0;
  ptp.rt_master_ready = false;
  ptp.rt_last_d7_clock_id = 0; /* reject stale D7 from the previous GM */

  ptp.rt_time_map_valid = false;
  ptp.rt_time_ref_remote_ns = 0;
  ptp.rt_time_ref_local_ns = 0;

  /* Realtime PTP lock means "the current GM estimator is ready" only. It no
   * longer means audio must stop: ALAC playout is in ESP-local time. */
  ptp.locked = false;
  ptp.lock_start_ms = 0;
  ptp.raw_offset_ns = 0;
  ptp.filtered_offset_ns = 0;
  ptp.sample_count = 0;
  ptp.previous_offset = 0;
  ptp.previous_offset_time_ms = 0;
  ptp.mastership_start_ms = 0;
  ptp.last_sync_ms = 0;
}

/* Realtime estimator copied from nqptp semantics:
 *   - first sample starts a fresh sequence;
 *   - during the first second, positive offset changes are accepted fully and
 *     negative changes are ignored;
 *   - later, positive changes use 1/16 and negative changes use 1/256 after a
 *     -2.5 ms innovation clamp.
 * raw_offset_ns is preciseOriginTimestamp(+Follow_Up correction) minus the
 * userspace Follow_Up reception timestamp, matching nqptp. */
static bool realtime_update_offset_locked(int64_t raw_offset_ns,
                                          int64_t reception_ns) {
  const bool was_ready = ptp.rt_master_ready;
  ptp.raw_offset_ns = raw_offset_ns;
  ptp.rt_last_followup_rx_ns = reception_ns;
  ptp.last_sync_ms = (uint32_t)(reception_ns / 1000000LL);
  ptp.rt_sample_count++;
  ptp.sample_count = ptp.rt_sample_count;

  int64_t smoothed = raw_offset_ns;
  if (ptp.rt_previous_offset_time_ns == 0) {
    ptp.rt_mastership_start_ns = reception_ns;
  } else {
    const int64_t jitter = raw_offset_ns - ptp.rt_previous_offset_ns;
    const int64_t mastership_ns = reception_ns - ptp.rt_mastership_start_ns;
    if (jitter < 0) {
      smoothed = ptp.rt_previous_offset_ns;
      if (mastership_ns > 1000000000LL) {
        int64_t clamped = jitter;
        if (clamped < SMOOTH_NEG_CLAMP_NS) clamped = SMOOTH_NEG_CLAMP_NS;
        smoothed += clamped / SMOOTH_NEG_DIV;
      }
    } else if (mastership_ns < 1000000000LL) {
      smoothed = ptp.rt_previous_offset_ns + jitter / SMOOTH_POS_STARTUP_DIV;
    } else {
      smoothed = ptp.rt_previous_offset_ns + jitter / SMOOTH_POS_STEADY_DIV;
    }
  }

  ptp.rt_previous_offset_ns = smoothed;
  ptp.rt_previous_offset_time_ns = reception_ns;
  ptp.rt_master_offset_ns = smoothed;


  /* Keep the generic getters meaningful for diagnostics/upper layers, but
   * realtime audio never uses this moving value as its presentation clock. */
  ptp.filtered_offset_ns = smoothed;
  ptp.previous_offset = smoothed;
  ptp.previous_offset_time_ms = (uint32_t)(reception_ns / 1000000LL);
  ptp.mastership_start_ms =
      (uint32_t)(ptp.rt_mastership_start_ns / 1000000LL);

  const uint32_t master_age_ms =
      ptp.rt_mastership_start_ns > 0 && reception_ns >= ptp.rt_mastership_start_ns
          ? (uint32_t)((reception_ns - ptp.rt_mastership_start_ns) / 1000000LL)
          : 0U;
  if (!ptp.rt_master_ready && ptp.rt_sample_count >= RT_MIN_MASTER_SAMPLES &&
      master_age_ms >= RT_MASTER_READY_AGE_MS) {
    ptp.rt_master_ready = true;
    ptp.locked = true;
    ptp.lock_start_ms = (uint32_t)(reception_ns / 1000000LL);

    /* Freeze the PHASE estimate into a fixed-slope realtime clock map. From
     * this point on NQPTP may keep converging for diagnostics/reacquisition,
     * but its moving offset cannot alter the media phase or media rate. */
    ptp.rt_time_ref_local_ns = reception_ns;
    ptp.rt_time_ref_remote_ns = reception_ns + smoothed;
    ptp.rt_time_map_valid = true;
  }
  return !was_ready && ptp.rt_master_ready;
}

// Master filter check. Caller must hold ptp_state_mux.
static bool master_matches_locked(const uint8_t *data) {
  if (ptp.expected_clock_id == 0) return true;
  if (parse_ptp_clock_id(data) == ptp.expected_clock_id) return true;
  ptp.rejected_master_count++;
  return false;
}

// Legacy/buffered SYNC path. Kept byte-for-byte in behaviour so AAC timing is
// not changed by the realtime handover work.
static void process_sync_legacy(const uint8_t *data, size_t len, uint16_t seq) {
  const int64_t local_sync_ns = get_local_time_ns();
  const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  ptp_offset_log_t log_ev = {0};

  taskENTER_CRITICAL(&ptp_state_mux);
  if (!master_matches_locked(data)) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  ptp.sync_count++;
  ptp.last_sync_seq = seq;
  ptp.last_sync_local_ns = local_sync_ns;
  ptp.awaiting_followup = true;

  uint16_t flags = ((uint16_t)data[6] << 8) | data[7];
  bool two_step = (flags & 0x0200) != 0;

  if (!two_step && len >= PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) {
    uint64_t ptp_time_ns = parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
    if (len >= 16) {
      int64_t correction_field =
          ((int64_t)data[8] << 56) | ((int64_t)data[9] << 48) |
          ((int64_t)data[10] << 40) | ((int64_t)data[11] << 32) |
          ((int64_t)data[12] << 24) | ((int64_t)data[13] << 16) |
          ((int64_t)data[14] << 8) | (int64_t)data[15];
      correction_field /= 65536;
      ptp_time_ns = (uint64_t)((int64_t)ptp_time_ns + correction_field);
    }
    const int64_t offset = (int64_t)ptp_time_ns - local_sync_ns;
    update_offset_locked(offset, now_ms, &log_ev);
    ptp.awaiting_followup = false;
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
  log_offset_event(&log_ev);
}

static void process_followup_legacy(const uint8_t *data, size_t len,
                                    uint16_t seq) {
  const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  ptp_offset_log_t log_ev = {0};

  taskENTER_CRITICAL(&ptp_state_mux);
  if (!master_matches_locked(data) || !ptp.awaiting_followup ||
      seq != ptp.last_sync_seq) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  ptp.followup_count++;
  ptp.awaiting_followup = false;
  if (len >= PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) {
    uint64_t ptp_time_ns = parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
    if (len >= 16) {
      int64_t correction_field =
          ((int64_t)data[8] << 56) | ((int64_t)data[9] << 48) |
          ((int64_t)data[10] << 40) | ((int64_t)data[11] << 32) |
          ((int64_t)data[12] << 24) | ((int64_t)data[13] << 16) |
          ((int64_t)data[14] << 8) | (int64_t)data[15];
      correction_field /= 65536;
      ptp_time_ns = (uint64_t)((int64_t)ptp_time_ns + correction_field);
    }
    const int64_t offset = (int64_t)ptp_time_ns - ptp.last_sync_local_ns;
    update_offset_locked(offset, now_ms, &log_ev);
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
  log_offset_event(&log_ev);
}

static void process_announce_realtime(const uint8_t *data, size_t len,
                                      uint32_t source_ip,
                                      int64_t reception_ns) {
  const uint64_t source_clock = parse_ptp_clock_id(data);
  const uint64_t gm = parse_announce_grandmaster_id(data, len);
  if (gm == 0) return;

  bool changed = false;
  uint64_t old_gm = 0;
  uint32_t peer_ip = 0;

  taskENTER_CRITICAL(&ptp_state_mux);
  ptp.announce_count++;
  peer_ip = ptp.timing_peer_ip;
  if (ptp.realtime_mode && realtime_source_matches_locked(source_ip)) {
    old_gm = ptp.grandmaster_clock_id;
    if (old_gm != gm) {
      if (old_gm != 0) ptp.rt_gm_changes++;
      realtime_reset_master_estimator_locked(gm, source_clock, reception_ns);
      changed = true;
    } else {
      ptp.source_clock_id = source_clock;
    }
  }
  taskEXIT_CRITICAL(&ptp_state_mux);

  if (changed) {
    struct in_addr src = {.s_addr = source_ip};
    struct in_addr peer = {.s_addr = peer_ip};
    char src_text[INET_ADDRSTRLEN] = {0};
    char peer_text[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &src, src_text, sizeof(src_text));
    if (peer_ip) inet_ntop(AF_INET, &peer, peer_text, sizeof(peer_text));
    ESP_LOGI(TAG,
             "RT ANNOUNCE peer=%s wanted=%s source=%016llx gm=%016llx oldGM=%016llx",
             src_text, peer_ip ? peer_text : "any",
             (unsigned long long)source_clock, (unsigned long long)gm,
             (unsigned long long)old_gm);
  }
}

static void process_sync_realtime(const uint8_t *data, size_t len,
                                  uint16_t seq, uint32_t source_ip,
                                  int64_t reception_ns) {
  (void)seq;
  (void)reception_ns;
  if (len < PTP_HEADER_SIZE) return;

  taskENTER_CRITICAL(&ptp_state_mux);
  if (ptp.realtime_mode && realtime_source_matches_locked(source_ip)) {
    ptp.sync_count++;
    if (ptp.source_clock_id == 0) ptp.source_clock_id = parse_ptp_clock_id(data);
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
}

static void process_followup_realtime(const uint8_t *data, size_t len,
                                      uint16_t seq, uint32_t source_ip,
                                      int64_t reception_ns) {
  (void)seq;
  if (len < PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) return;

  const uint64_t ptp_time_ns =
      parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
  int64_t followup_correction_ns = 0;
  if (len >= 16) {
    followup_correction_ns =
        ((int64_t)data[8] << 56) | ((int64_t)data[9] << 48) |
        ((int64_t)data[10] << 40) | ((int64_t)data[11] << 32) |
        ((int64_t)data[12] << 24) | ((int64_t)data[13] << 16) |
        ((int64_t)data[14] << 8) | (int64_t)data[15];
    followup_correction_ns /= 65536;
  }

  uint64_t gm = 0, source = 0;
  int64_t master_off = 0;
  uint32_t master_age = 0, samples = 0;
  bool became_ready = false;

  taskENTER_CRITICAL(&ptp_state_mux);
  if (!ptp.realtime_mode || ptp.grandmaster_clock_id == 0 ||
      !realtime_source_matches_locked(source_ip)) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  ptp.followup_count++;
  const int64_t raw_offset =
      (int64_t)ptp_time_ns + followup_correction_ns - reception_ns;
  became_ready = realtime_update_offset_locked(raw_offset, reception_ns);
  gm = ptp.grandmaster_clock_id;
  source = ptp.source_clock_id;
  master_off = ptp.rt_master_offset_ns;
  samples = ptp.rt_sample_count;
  if (ptp.rt_mastership_start_ns > 0 && reception_ns >= ptp.rt_mastership_start_ns)
    master_age = (uint32_t)((reception_ns - ptp.rt_mastership_start_ns) / 1000000LL);
  taskEXIT_CRITICAL(&ptp_state_mux);

  if (became_ready) {
    ESP_LOGI(TAG,
             "RT MASTER READY gm=%016llx source=%016llx age=%lums samples=%lu "
             "masterOff=%+lldns mapRate=0ppm fixed",
             (unsigned long long)gm, (unsigned long long)source,
             (unsigned long)master_age, (unsigned long)samples,
             (long long)master_off);
  }
}

static void process_ptp_message(const uint8_t *data, size_t len,
                                bool is_event_port, uint32_t source_ip,
                                int64_t reception_ns) {
  if (len < PTP_HEADER_SIZE) return;
  const uint8_t msg_type = data[0] & 0x0F;
  const uint16_t seq = ((uint16_t)data[30] << 8) | data[31];

  bool realtime;
  taskENTER_CRITICAL(&ptp_state_mux);
  realtime = ptp.realtime_mode;
  taskEXIT_CRITICAL(&ptp_state_mux);

  if (!realtime) {
    switch (msg_type) {
    case PTP_MSG_SYNC:
      if (is_event_port) process_sync_legacy(data, len, seq);
      break;
    case PTP_MSG_FOLLOW_UP:
      if (!is_event_port) process_followup_legacy(data, len, seq);
      break;
    case PTP_MSG_ANNOUNCE:
      taskENTER_CRITICAL(&ptp_state_mux);
      ptp.announce_count++;
      taskEXIT_CRITICAL(&ptp_state_mux);
      break;
    default:
      break;
    }
    return;
  }

  switch (msg_type) {
  case PTP_MSG_SYNC:
    if (is_event_port) process_sync_realtime(data, len, seq, source_ip, reception_ns);
    break;
  case PTP_MSG_FOLLOW_UP:
    if (!is_event_port)
      process_followup_realtime(data, len, seq, source_ip, reception_ns);
    break;
  case PTP_MSG_ANNOUNCE:
    if (!is_event_port)
      process_announce_realtime(data, len, source_ip, reception_ns);
    break;
  default:
    break;
  }
}

// Create and bind multicast socket
static int create_ptp_socket(uint16_t port) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    return -1;
  }

  // Allow address reuse
  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // Bind to port
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind to port %d: %d", port, errno);
    close(sock);
    return -1;
  }

  // Join multicast group
  struct ip_mreq mreq = {0};
  mreq.imr_multiaddr.s_addr = inet_addr(PTP_MULTICAST_ADDR);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);

  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
      0) {
    ESP_LOGE(TAG, "Failed to join multicast group: %d", errno);
    close(sock);
    return -1;
  }

  // Set receive timeout
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  return sock;
}

// PTP task - listens for messages on both ports
static void ptp_task(void *pvParameters) {
  uint8_t buffer[256];

  while (ptp.running) {
    fd_set read_fds;
    FD_ZERO(&read_fds);

    int max_fd = -1;
    if (ptp.event_socket >= 0) {
      FD_SET(ptp.event_socket, &read_fds);
      if (ptp.event_socket > max_fd) {
        max_fd = ptp.event_socket;
      }
    }
    if (ptp.general_socket >= 0) {
      FD_SET(ptp.general_socket, &read_fds);
      if (ptp.general_socket > max_fd) {
        max_fd = ptp.general_socket;
      }
    }

    if (max_fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

    if (ret < 0) {
      if (!ptp.running) {
        break; // Sockets closed during shutdown
      }
      if (errno != EINTR) {
        ESP_LOGE(TAG, "select error: %d", errno);
      }
      continue;
    }

    if (ret == 0) {
      // Timeout - check if we lost lock due to no messages
    } else {
      /* Match nqptp's userspace timestamp point: sample the local clock once
       * immediately after select() reports readable PTP sockets, then use that
       * reception timestamp for this ready batch. */
      const int64_t reception_ns = get_local_time_ns();

      // Check event port (SYNC messages)
      if (ptp.event_socket >= 0 && FD_ISSET(ptp.event_socket, &read_fds)) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        ssize_t len = recvfrom(ptp.event_socket, buffer, sizeof(buffer), 0,
                               (struct sockaddr *)&src, &src_len);
        if (len > 0) {
          process_ptp_message(buffer, (size_t)len, true,
                              src.sin_addr.s_addr, reception_ns);
        }
      }

      // Check general port (FOLLOW_UP messages)
      if (ptp.general_socket >= 0 && FD_ISSET(ptp.general_socket, &read_fds)) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        ssize_t len = recvfrom(ptp.general_socket, buffer, sizeof(buffer), 0,
                               (struct sockaddr *)&src, &src_len);
        if (len > 0) {
          process_ptp_message(buffer, (size_t)len, false,
                              src.sin_addr.s_addr, reception_ns);
        }
      }
    }
  }

  // Cleanup
  if (ptp.event_socket >= 0) {
    close(ptp.event_socket);
    ptp.event_socket = -1;
  }
  if (ptp.general_socket >= 0) {
    close(ptp.general_socket);
    ptp.general_socket = -1;
  }

  ptp.task_handle = NULL;
  vTaskDelete(NULL);
}

esp_err_t ptp_clock_init(void) {
  if (ptp.running) {
    return ESP_ERR_INVALID_STATE;
  }

  memset(&ptp, 0, sizeof(ptp));
  ptp.event_socket = -1;
  ptp.general_socket = -1;

  // Create sockets
  ptp.event_socket = create_ptp_socket(PTP_EVENT_PORT);
  if (ptp.event_socket < 0) {
    return ESP_FAIL;
  }

  ptp.general_socket = create_ptp_socket(PTP_GENERAL_PORT);
  if (ptp.general_socket < 0) {
    close(ptp.event_socket);
    ptp.event_socket = -1;
    return ESP_FAIL;
  }

  // Start task
  ptp.running = true;
  BaseType_t ret = task_create_pinned_spiram(ptp_task, "ptp_clock", 4096, NULL,
                                             PTP_TASK_PRIORITY_LEGACY,
                                             &ptp.task_handle, 0, &ptp.task_mem);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create PTP task");
    close(ptp.event_socket);
    close(ptp.general_socket);
    ptp.event_socket = -1;
    ptp.general_socket = -1;
    ptp.running = false;
    return ESP_FAIL;
  }

  return ESP_OK;
}

void ptp_clock_stop(void) {
  if (!ptp.running) {
    return;
  }

  ptp.running = false;

  // Close sockets to unblock select
  if (ptp.event_socket >= 0) {
    close(ptp.event_socket);
    ptp.event_socket = -1;
  }
  if (ptp.general_socket >= 0) {
    close(ptp.general_socket);
    ptp.general_socket = -1;
  }

  // Wait for task to exit (task sets task_handle = NULL before vTaskDelete)
  for (int i = 0; i < 20 && ptp.task_handle != NULL; i++) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (ptp.task_handle != NULL) {
    ESP_LOGW(TAG, "PTP task did not exit in time");
  }
  task_free_spiram(&ptp.task_mem);
}

void ptp_clock_clear(void) {
  taskENTER_CRITICAL(&ptp_state_mux);
  ptp.locked = false;
  ptp.lock_start_ms = 0;
  ptp.lock_candidate_start_ms = 0;
  ptp.last_sync_ms = 0;
  ptp.filtered_offset_ns = 0;
  ptp.raw_offset_ns = 0;
  ptp.sample_count = 0;
  ptp.previous_offset = 0;
  ptp.previous_offset_time_ms = 0;
  ptp.mastership_start_ms = 0;
  ptp.last_sync_seq = 0;
  ptp.last_sync_local_ns = 0;
  ptp.awaiting_followup = false;
  ptp.sync_count = 0;
  ptp.followup_count = 0;
  ptp.expected_clock_id = 0;

  /* Keep mode + timing peer across a stream-level clear, but drop the
   * realtime GM estimator. No audio continuity state lives in ptp_clock. */
  ptp.source_clock_id = 0;
  ptp.grandmaster_clock_id = 0;
  ptp.rt_master_offset_ns = 0;
  ptp.rt_previous_offset_ns = 0;
  ptp.rt_previous_offset_time_ns = 0;
  ptp.rt_mastership_start_ns = 0;
  ptp.rt_last_followup_rx_ns = 0;
  ptp.rt_sample_count = 0;
  ptp.rt_master_ready = false;
  ptp.rt_last_d7_clock_id = 0;
  ptp.rt_time_map_valid = false;
  ptp.rt_time_ref_remote_ns = 0;
  ptp.rt_time_ref_local_ns = 0;
  taskEXIT_CRITICAL(&ptp_state_mux);
}

void ptp_clock_notify_resume(uint32_t pause_duration_ms) {
  if (pause_duration_ms < PTP_LONG_PAUSE_THRESHOLD_MS) return;

  taskENTER_CRITICAL(&ptp_state_mux);
  if (ptp.realtime_mode) {
    /* Only estimator history is stale after a long pause. The realtime audio
     * presentation anchor remains in ESP-local time and is not touched. */
    ptp.rt_master_offset_ns = 0;
    ptp.rt_previous_offset_ns = 0;
    ptp.rt_previous_offset_time_ns = 0;
    ptp.rt_mastership_start_ns = 0;
    ptp.rt_last_followup_rx_ns = 0;
    ptp.rt_sample_count = 0;
    ptp.rt_master_ready = false;
    ptp.locked = false;
    ptp.filtered_offset_ns = 0;
    ptp.rt_time_map_valid = false;
    ptp.rt_time_ref_remote_ns = 0;
    ptp.rt_time_ref_local_ns = 0;
  } else {
    ptp.previous_offset_time_ms = 0;
    ptp.mastership_start_ms = 0;
  }
  taskEXIT_CRITICAL(&ptp_state_mux);

  ESP_LOGI(TAG, "notify_resume: pause=%lu ms, resetting %s PTP smoothing",
           (unsigned long)pause_duration_ms,
           ptp.realtime_mode ? "realtime-master" : "legacy");
}

bool ptp_clock_is_locked(void) {
  const int64_t now_ns = get_local_time_ns();
  const uint32_t now_ms = (uint32_t)(now_ns / 1000000LL);
  bool locked;

  taskENTER_CRITICAL(&ptp_state_mux);
  if (ptp.realtime_mode) {
    if (ptp.rt_master_ready && ptp.rt_last_followup_rx_ns > 0 &&
        now_ns - ptp.rt_last_followup_rx_ns >
            (int64_t)LOCK_TIMEOUT_MS * 1000000LL) {
      ptp.rt_master_ready = false;
      ptp.locked = false;
    }
  } else if (ptp.locked && ptp.last_sync_ms > 0 &&
             (now_ms - ptp.last_sync_ms) > LOCK_TIMEOUT_MS) {
    ptp.locked = false;
    ptp.lock_start_ms = 0;
    ptp.lock_candidate_start_ms = 0;
  }
  locked = ptp.locked;
  taskEXIT_CRITICAL(&ptp_state_mux);
  return locked;
}

uint64_t ptp_clock_get_time_ns(void) {
  const int64_t local_ns = get_local_time_ns();
  int64_t offset_ns;
  taskENTER_CRITICAL(&ptp_state_mux);
  offset_ns = ptp.filtered_offset_ns;
  taskEXIT_CRITICAL(&ptp_state_mux);
  return (uint64_t)(local_ns + offset_ns);
}

int64_t ptp_clock_get_offset_ns(void) {
  int64_t offset_ns;
  taskENTER_CRITICAL(&ptp_state_mux);
  offset_ns = ptp.filtered_offset_ns;
  taskEXIT_CRITICAL(&ptp_state_mux);
  return offset_ns;
}

void ptp_clock_set_realtime_mode(bool enabled, uint32_t timing_peer_ip) {
  bool changed = false;
  TaskHandle_t task_handle = NULL;
  taskENTER_CRITICAL(&ptp_state_mux);
  if (ptp.realtime_mode != enabled ||
      (enabled && ptp.timing_peer_ip != timing_peer_ip)) {
    changed = true;
    ptp.realtime_mode = enabled;
    ptp.timing_peer_ip = enabled ? timing_peer_ip : 0;

    /* Stream-mode transition starts the selected estimator fresh. This does
     * not touch any PCM/ring/cursor/PID/DMA/I2S state. */
    ptp.locked = false;
    ptp.lock_start_ms = 0;
    ptp.lock_candidate_start_ms = 0;
    ptp.last_sync_ms = 0;
    ptp.filtered_offset_ns = 0;
    ptp.raw_offset_ns = 0;
    ptp.sample_count = 0;
    ptp.previous_offset = 0;
    ptp.previous_offset_time_ms = 0;
    ptp.mastership_start_ms = 0;
    ptp.last_sync_seq = 0;
    ptp.last_sync_local_ns = 0;
    ptp.awaiting_followup = false;
    ptp.sync_count = 0;
    ptp.followup_count = 0;
    ptp.expected_clock_id = 0;
    ptp.source_clock_id = 0;
    ptp.grandmaster_clock_id = 0;
    ptp.rt_master_offset_ns = 0;
    ptp.rt_previous_offset_ns = 0;
    ptp.rt_previous_offset_time_ns = 0;
    ptp.rt_mastership_start_ns = 0;
    ptp.rt_last_followup_rx_ns = 0;
    ptp.rt_sample_count = 0;
    ptp.rt_master_ready = false;
    ptp.rt_last_d7_clock_id = 0;
    ptp.rt_time_map_valid = false;
    ptp.rt_time_ref_remote_ns = 0;
    ptp.rt_time_ref_local_ns = 0;
  }
  task_handle = ptp.task_handle;
  taskEXIT_CRITICAL(&ptp_state_mux);

  if (changed && task_handle) {
    vTaskPrioritySet(task_handle, enabled ? PTP_TASK_PRIORITY_REALTIME
                                         : PTP_TASK_PRIORITY_LEGACY);
  }
  if (changed) {
    struct in_addr peer = {.s_addr = timing_peer_ip};
    char peer_text[INET_ADDRSTRLEN] = {0};
    if (timing_peer_ip) inet_ntop(AF_INET, &peer, peer_text, sizeof(peer_text));
    ESP_LOGI(TAG, "PTP mode=%s timingPeer=%s",
             enabled ? "AP2-realtime" : "legacy/buffered",
             enabled && timing_peer_ip ? peer_text : "none");
  }
}

void ptp_clock_note_realtime_d7(uint64_t clock_id) {
  if (clock_id == 0) return;
  taskENTER_CRITICAL(&ptp_state_mux);
  if (ptp.realtime_mode) {
    ptp.rt_last_d7_clock_id = clock_id;
    ptp.expected_clock_id = clock_id; /* anchor hint only; no realtime filtering */
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
}

bool ptp_clock_realtime_time_to_local(uint64_t clock_id,
                                      uint64_t remote_ptp_ns,
                                      uint64_t *local_ns) {
  if (!local_ns || clock_id == 0) return false;
  bool ok = false;
  int64_t local = 0;
  taskENTER_CRITICAL(&ptp_state_mux);
  if (ptp.realtime_mode && ptp.rt_master_ready &&
      clock_id == ptp.grandmaster_clock_id &&
      ptp.rt_last_followup_rx_ns > 0 && ptp.rt_time_map_valid) {
    ok = realtime_affine_to_local_locked((int64_t)remote_ptp_ns, &local);
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
  if (!ok || local < 0) return false;
  *local_ns = (uint64_t)local;
  return true;
}

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *snapshot) {
  if (!snapshot) return;
  memset(snapshot, 0, sizeof(*snapshot));
  const int64_t now_ns = get_local_time_ns();
  taskENTER_CRITICAL(&ptp_state_mux);
  snapshot->realtime_mode = ptp.realtime_mode;
  snapshot->master_ready = ptp.rt_master_ready;
  snapshot->master_clock_id = ptp.grandmaster_clock_id;
  snapshot->source_clock_id = ptp.source_clock_id;
  snapshot->master_offset_ns = ptp.rt_master_offset_ns;
  snapshot->sample_count = ptp.rt_sample_count;
  snapshot->gm_change_count = ptp.rt_gm_changes;
  if (ptp.rt_mastership_start_ns > 0 && now_ns >= ptp.rt_mastership_start_ns)
    snapshot->mastership_age_ms =
        (uint32_t)((now_ns - ptp.rt_mastership_start_ns) / 1000000LL);
  taskEXIT_CRITICAL(&ptp_state_mux);
}

void ptp_clock_set_master_clock_id(uint64_t clock_id) {
  bool changed = false;
  bool realtime = false;

  taskENTER_CRITICAL(&ptp_state_mux);
  realtime = ptp.realtime_mode;
  if (clock_id != ptp.expected_clock_id) {
    changed = true;
    ptp.expected_clock_id = clock_id;
    if (!realtime) {
      /* Original buffered/legacy behaviour. */
      ptp.locked = false;
      ptp.lock_start_ms = 0;
      ptp.lock_candidate_start_ms = 0;
      ptp.last_sync_ms = 0;
      ptp.filtered_offset_ns = 0;
      ptp.raw_offset_ns = 0;
      ptp.sample_count = 0;
      ptp.previous_offset = 0;
      ptp.previous_offset_time_ms = 0;
      ptp.mastership_start_ms = 0;
      ptp.last_sync_seq = 0;
      ptp.last_sync_local_ns = 0;
      ptp.awaiting_followup = false;
    }
  }
  taskEXIT_CRITICAL(&ptp_state_mux);

  if (changed) {
    ESP_LOGI(TAG, "%s anchor clock hint: %016llx",
             realtime ? "RT" : "PTP", (unsigned long long)clock_id);
  }
}

uint64_t ptp_clock_get_master_clock_id(void) {
  uint64_t clock_id;
  taskENTER_CRITICAL(&ptp_state_mux);
  clock_id = ptp.realtime_mode ? ptp.grandmaster_clock_id
                               : ptp.expected_clock_id;
  taskEXIT_CRITICAL(&ptp_state_mux);
  return clock_id;
}

void ptp_clock_get_stats(ptp_stats_t *stats) {
  if (!stats) return;

  const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  taskENTER_CRITICAL(&ptp_state_mux);
  stats->sync_count = ptp.sync_count;
  stats->followup_count = ptp.followup_count;
  stats->last_offset_ns = ptp.raw_offset_ns;
  stats->filtered_offset_ns = ptp.filtered_offset_ns;
  /* ptpD is raw Follow_Up sample minus the smoothed estimator. */
  stats->raw_filter_delta_ns = ptp.raw_offset_ns - ptp.filtered_offset_ns;
  stats->outlier_count = ptp.outlier_count;
  if (ptp.locked && ptp.lock_start_ms > 0) {
    stats->lock_time_ms = now_ms - ptp.lock_start_ms;
  } else {
    stats->lock_time_ms = 0;
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
}
