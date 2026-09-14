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

#include "audio_diag.h"
#include "ptp_clock.h"
#include "ptp_clock_engine.h"
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

/* Realtime ALAC follows the current filtered remote-to-local offset.
 * Freezing it at startup hides relative clock drift from the audio servo. */

#define PTP_TASK_PRIORITY_LEGACY   6U
#define PTP_TASK_PRIORITY_REALTIME 8U

#define PTP_STEP_FILTER_NS        500000LL  /* 0.5 ms */
#define PTP_STEP_RAW_FILTER_NS   2000000LL /* 2.0 ms */
#define PTP_STEP_LOG_COOLDOWN_NS 1000000000LL

// PTP state
static struct {
  bool running;
  TaskHandle_t task_handle;
  spiram_task_mem_t task_mem;
  int event_socket;
  int general_socket;

  // Synchronization state
  bool locked;
  uint32_t lock_candidate_start_ms;
  uint32_t last_sync_ms;
  int64_t filtered_offset_ns; // PTP_time = local_time + offset
  uint32_t sample_count;

  // Asymmetric smoothing state (replaces median ring buffer)
  int64_t previous_offset;
  uint32_t previous_offset_time_ms; // 0 = no previous sample yet
  uint32_t mastership_start_ms;     // when continuous tracking began

  // Two-step sync tracking
  uint16_t last_sync_seq;
  int64_t last_sync_local_ns;
  bool awaiting_followup;
  int64_t last_sync_correction_ns;
  uint64_t last_sync_source_clock_id;
  int64_t last_sync_process_lag_ns;

  /* NQPTP-derived buffered clock estimator. Socket admission / pairing stays
   * in this file; filtering and source/GM epochs live in the pure engine. */
  ptp_clock_engine_t legacy_engine;
  int64_t legacy_last_step_log_ns;


  /* AirPlay 2 realtime-only clock-domain state. Buffered AAC does not use
   * these fields and continues through the legacy path above. */
  bool realtime_mode;
  uint32_t timing_peer_ip;        // network byte order; 0 = accept any source
  uint64_t source_clock_id;       // PTP sourcePortIdentity clock id
  uint64_t grandmaster_clock_id;  // Announce grandmasterIdentity

  int64_t rt_master_offset_ns;
  int64_t rt_raw_offset_ns;
  int64_t rt_previous_offset_ns;
  int64_t rt_previous_offset_time_ns;
  int64_t rt_mastership_start_ns;
  int64_t rt_last_followup_rx_ns;
  uint32_t rt_sample_count;
  bool rt_master_ready;
  uint32_t rt_gm_changes;

  /* Realtime two-step PTP pairing. A Follow_Up carries the precise origin
   * timestamp of the corresponding Sync, but the local receive timestamp that
   * belongs in the offset calculation is the Sync receive time, not the
   * Follow_Up receive time. Keep exactly one outstanding Sync, matching the
   * cadence used by AirPlay/NQPTP and by the legacy path above. */
  uint16_t rt_sync_seq;
  int64_t rt_sync_local_ns;
  int64_t rt_sync_correction_ns;
  uint64_t rt_sync_source_clock_id;
  uint32_t rt_sync_source_ip;
  bool rt_awaiting_followup;

  // Master clock filter / realtime anchor-clock hint (0 = unspecified).
  uint64_t expected_clock_id;

  /* Legacy/buffered PTP can begin acquiring before RTSP delivers the
   * SETRATEANCHORTIME clock id. Remember which PTP source built that
   * pre-anchor estimator so a later matching anchor can confirm it instead
   * of destroying a freshly acquired lock. If more than one source is seen
   * before the anchor, be conservative and force the old reset path. */
  uint64_t legacy_sample_clock_id;
  bool legacy_source_mixed;
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

/* PTP correctionField is a signed 64-bit fixed-point value with 16 fractional
 * bits. Return whole nanoseconds, matching the existing legacy behaviour. */
static int64_t parse_ptp_correction_ns(const uint8_t *data, size_t len) {
  if (!data || len < 16U) return 0;

  uint64_t raw = 0;
  for (size_t i = 8U; i < 16U; ++i) raw = (raw << 8) | data[i];
  return ((int64_t)raw) / 65536LL;
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
typedef struct {
  bool emit;
  const char *reason;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t epoch;
  int64_t raw_offset_ns;
  int64_t filtered_offset_ns;
  int64_t raw_filter_delta_ns;
  int64_t filtered_step_ns;
  int64_t rx_lag_ns;
  uint16_t seq;
} ptp_step_log_t;

static int64_t abs_i64(int64_t v) {
  if (v >= 0) return v;
  if (v == INT64_MIN) return INT64_MAX;
  return -v;
}

static void log_ptp_step(const ptp_step_log_t *ev) {
  if (!ev || !ev->emit || !ev->reason) return;
  ESP_LOGW(TAG,
           "PTP | STEP | reason=%s | src=%016llx | gm=%016llx | epoch=%lu | "
           "raw=%+.3fms | filt=%+.3fms | ptpD=%+.3fms | dFilt=%+.3fms | "
           "rxLag=%.3fms | seq=%u",
           ev->reason,
           (unsigned long long)ev->source_clock_id,
           (unsigned long long)ev->grandmaster_clock_id,
           (unsigned long)ev->epoch,
           (double)ev->raw_offset_ns / 1000000.0,
           (double)ev->filtered_offset_ns / 1000000.0,
           (double)ev->raw_filter_delta_ns / 1000000.0,
           (double)ev->filtered_step_ns / 1000000.0,
           (double)ev->rx_lag_ns / 1000000.0,
           (unsigned)ev->seq);
}

/* Caller holds ptp_state_mux. Only accepted samples advance freshness, lock
 * qualification and public sample_count. This fixes the old behaviour where
 * an outlier could keep a stale clock looking alive. */
static void update_legacy_offset_locked(int64_t raw_offset_ns,
                                        int64_t reception_ns,
                                        int64_t rx_lag_ns,
                                        uint16_t seq,
                                        ptp_step_log_t *step_ev) {
  if (step_ev) memset(step_ev, 0, sizeof(*step_ev));

  const ptp_clock_engine_sample_t r = ptp_clock_engine_add_sample(
      &ptp.legacy_engine, raw_offset_ns, reception_ns);
  if (!r.accepted) {
    if (r.outlier) AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_OUTLIER);
    return;
  }


  ptp.filtered_offset_ns = r.filtered_offset_ns;
  ptp.sample_count = r.accepted_samples;
  ptp.last_sync_ms = (uint32_t)(reception_ns / 1000000LL);

  const int64_t dev = abs_i64(r.raw_filter_delta_ns);
  if (ptp.sample_count >= MIN_SAMPLES_FOR_LOCK) {
    if (dev < LOCK_THRESHOLD_NS) {
      if (!ptp.locked) {
        const uint32_t now_ms = (uint32_t)(reception_ns / 1000000LL);
        if (ptp.lock_candidate_start_ms == 0)
          ptp.lock_candidate_start_ms = now_ms;
        if ((now_ms - ptp.lock_candidate_start_ms) >= LOCK_STABLE_TIME_MS) {
          ptp.locked = true;
          ptp.lock_candidate_start_ms = 0;
        }
      }
    } else {
      ptp.lock_candidate_start_ms = 0;
      if (ptp.locked && dev > LOCK_THRESHOLD_NS * 4)
        ptp.locked = false;
    }
  }

  /* Permanent event log: only meaningful estimator motion, and at most once
   * per second. Ordinary PTP counters remain in compile-time audio_diag. */
  if (step_ev && r.accepted_samples >= MIN_SAMPLES_FOR_LOCK &&
      abs_i64(r.filtered_step_ns) >= PTP_STEP_FILTER_NS &&
      (ptp.legacy_last_step_log_ns == 0 ||
       reception_ns - ptp.legacy_last_step_log_ns >= PTP_STEP_LOG_COOLDOWN_NS)) {
    step_ev->emit = true;
    step_ev->reason = abs_i64(r.raw_filter_delta_ns) >= PTP_STEP_RAW_FILTER_NS
                          ? "raw-filter-gap" : "offset";
    step_ev->source_clock_id = ptp.legacy_engine.source_clock_id;
    step_ev->grandmaster_clock_id = ptp.legacy_engine.grandmaster_clock_id;
    step_ev->epoch = ptp.legacy_engine.epoch;
    step_ev->raw_offset_ns = r.raw_offset_ns;
    step_ev->filtered_offset_ns = r.filtered_offset_ns;
    step_ev->raw_filter_delta_ns = r.raw_filter_delta_ns;
    step_ev->filtered_step_ns = r.filtered_step_ns;
    step_ev->rx_lag_ns = rx_lag_ns;
    step_ev->seq = seq;
    ptp.legacy_last_step_log_ns = reception_ns;
  }
}

static void fill_domain_step_locked(const char *reason,
                                    const ptp_clock_engine_domain_event_t *de,
                                    int64_t reception_ns, bool preserve_holdover,
                                    ptp_step_log_t *step_ev) {
  if (!step_ev || !de || (!de->source_changed && !de->gm_changed)) return;
  memset(step_ev, 0, sizeof(*step_ev));
  step_ev->emit = true;
  step_ev->reason = reason;
  step_ev->source_clock_id = de->new_source_clock_id;
  step_ev->grandmaster_clock_id = de->new_grandmaster_clock_id;
  step_ev->epoch = de->epoch;
  /* set_domain() intentionally starts the new estimator clean. For a normal
   * same-source GM handover, log the old active clock estimate instead of a
   * misleading zero and keep that estimate as short holdover until the new
   * GM produces accepted samples. */
  step_ev->raw_offset_ns = preserve_holdover && de->old_estimator_valid
                               ? de->old_raw_offset_ns
                               : ptp.legacy_engine.raw_offset_ns;
  step_ev->filtered_offset_ns = preserve_holdover && de->old_estimator_valid
                                    ? de->old_filtered_offset_ns
                                    : ptp.legacy_engine.filtered_offset_ns;
  step_ev->raw_filter_delta_ns =
      step_ev->raw_offset_ns - step_ev->filtered_offset_ns;
  step_ev->filtered_step_ns = 0;
  step_ev->rx_lag_ns = 0;
  step_ev->seq = 0;
  ptp.legacy_last_step_log_ns = reception_ns;


  /* Never pair a pre-handover Sync with a post-handover Follow_Up. */
  ptp.awaiting_followup = false;
  ptp.last_sync_seq = 0;
  ptp.last_sync_local_ns = 0;
  ptp.last_sync_correction_ns = 0;
  ptp.last_sync_source_clock_id = 0;
  ptp.last_sync_process_lag_ns = 0;

  if (!preserve_holdover) {
    /* A source-clock change is a true timing-domain break: old clock state
     * must not drive RTP presentation for the new source. */
    ptp.locked = false;
    ptp.lock_candidate_start_ms = 0;
    ptp.last_sync_ms = 0;
    ptp.sample_count = 0;
    ptp.filtered_offset_ns = 0;
  }
}

/* Realtime source selection follows Shairport/NQPTP: the RTSP client IP is
 * the timing peer. sourcePortIdentity is diagnostic; Announce tells us which
 * grandmaster that peer is currently forwarding. Caller holds ptp_state_mux. */
static bool realtime_source_matches_locked(uint32_t source_ip) {
  if (ptp.timing_peer_ip == 0 || source_ip == ptp.timing_peer_ip) return true;
  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_REJECTED_SOURCE);
  return false;
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
  ptp.rt_raw_offset_ns = 0;
  ptp.rt_previous_offset_ns = 0;
  ptp.rt_previous_offset_time_ns = 0;
  ptp.rt_mastership_start_ns = 0;
  ptp.rt_last_followup_rx_ns = 0;
  ptp.rt_sample_count = 0;
  ptp.rt_master_ready = false;
  ptp.rt_sync_seq = 0;
  ptp.rt_sync_local_ns = 0;
  ptp.rt_sync_correction_ns = 0;
  ptp.rt_sync_source_clock_id = 0;
  ptp.rt_sync_source_ip = 0;
  ptp.rt_awaiting_followup = false;

  /* Realtime PTP lock means "the current GM estimator is ready" only. It no
   * longer means audio must stop: ALAC playout is in ESP-local time. */
  ptp.locked = false;
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
 * raw_offset_ns is the precise Sync origin timestamp (+ Sync/Follow_Up
 * correction fields) minus the userspace reception timestamp of that same
 * Sync packet. */
static bool realtime_update_offset_locked(int64_t raw_offset_ns,
                                          int64_t reception_ns) {
  const bool was_ready = ptp.rt_master_ready;
  ptp.rt_raw_offset_ns = raw_offset_ns;
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


  /* Publish the same filtered estimate to audio and diagnostics. */
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
  }
  return !was_ready && ptp.rt_master_ready;
}

/* Track the source identity behind the legacy estimator while no RTSP
 * anchor clock has been supplied yet. Caller holds ptp_state_mux. */
static void legacy_note_source_locked(uint64_t source_clock_id) {
  if (source_clock_id == 0) return;
  if (ptp.legacy_sample_clock_id == 0) {
    ptp.legacy_sample_clock_id = source_clock_id;
  } else if (ptp.legacy_sample_clock_id != source_clock_id) {
    ptp.legacy_source_mixed = true;
  }
}

/* Buffered mode follows one PTP source. Before SETRATEANCHORTIME gives us the
 * expected source, lock onto the first observed source and mark any competing
 * source as mixed rather than letting estimators alternate between clocks. */
static bool legacy_source_admitted_locked(uint64_t source_clock_id) {
  if (source_clock_id == 0) return false;
  if (ptp.expected_clock_id != 0) {
    if (source_clock_id == ptp.expected_clock_id) return true;
    AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_REJECTED_SOURCE);
    return false;
  }
  if (ptp.legacy_engine.source_clock_id == 0 ||
      ptp.legacy_engine.source_clock_id == source_clock_id)
    return true;

  ptp.legacy_source_mixed = true;
  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_REJECTED_SOURCE);
  return false;
}

static void process_announce_legacy(const uint8_t *data, size_t len,
                                    int64_t reception_ns) {
  if (len < PTP_HEADER_SIZE) return;
  const uint64_t source_clock = parse_ptp_clock_id(data);
  const uint64_t gm = parse_announce_grandmaster_id(data, len);
  if (source_clock == 0 || gm == 0) return;

  ptp_step_log_t step_ev = {0};
  taskENTER_CRITICAL(&ptp_state_mux);
  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_RX_ANNOUNCE);
  if (legacy_source_admitted_locked(source_clock)) {
    legacy_note_source_locked(source_clock);
    ptp_clock_engine_domain_event_t de = {0};
    if (ptp_clock_engine_set_domain(&ptp.legacy_engine, source_clock, gm, &de)) {
      if (de.gm_changed || de.source_changed) {
        const bool gm_holdover = de.gm_changed && !de.source_changed &&
                                 de.old_estimator_valid && ptp.locked &&
                                 ptp.last_sync_ms != 0;
        fill_domain_step_locked(gm_holdover ? "gm-handover"
                                            : (de.gm_changed ? "gm-change"
                                                             : "source-change"),
                                &de, reception_ns, gm_holdover, &step_ev);
      }
    }
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
  log_ptp_step(&step_ev);
}

static void process_sync_legacy(const uint8_t *data, size_t len, uint16_t seq,
                                int64_t reception_ns) {
  if (len < PTP_HEADER_SIZE || reception_ns <= 0) return;
  const int64_t processing_lag_ns = get_local_time_ns() - reception_ns;
  const uint64_t source_clock = parse_ptp_clock_id(data);
  const int64_t sync_correction_ns = parse_ptp_correction_ns(data, len);
  ptp_step_log_t step_ev = {0};

  taskENTER_CRITICAL(&ptp_state_mux);
  if (!legacy_source_admitted_locked(source_clock)) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  legacy_note_source_locked(source_clock);
  if (ptp.legacy_engine.source_clock_id != source_clock) {
    ptp_clock_engine_domain_event_t de = {0};
    if (ptp_clock_engine_set_domain(&ptp.legacy_engine, source_clock, 0, &de) &&
        de.source_changed) {
      fill_domain_step_locked("source-change", &de, reception_ns, false,
                              &step_ev);
    }
  }

  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_RX_SYNC);
  if (ptp.awaiting_followup)
    AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_PAIR_MISMATCH);

  ptp.last_sync_seq = seq;
  ptp.last_sync_local_ns = reception_ns;
  ptp.last_sync_correction_ns = sync_correction_ns;
  ptp.last_sync_source_clock_id = source_clock;
  ptp.last_sync_process_lag_ns = processing_lag_ns > 0 ? processing_lag_ns : 0;
  ptp.awaiting_followup = true;

  const uint16_t flags = ((uint16_t)data[6] << 8) | data[7];
  const bool two_step = (flags & 0x0200) != 0;
  if (!two_step && len >= PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) {
    const uint64_t ptp_time_ns =
        parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
    if (ptp_time_ns <= INT64_MAX) {
      const int64_t raw_offset = (int64_t)ptp_time_ns + sync_correction_ns -
                                 reception_ns;
      update_legacy_offset_locked(raw_offset, reception_ns,
                                  ptp.last_sync_process_lag_ns, seq, &step_ev);
    }
    ptp.awaiting_followup = false;
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
  log_ptp_step(&step_ev);
}

static void process_followup_legacy(const uint8_t *data, size_t len,
                                    uint16_t seq, int64_t reception_ns) {
  if (len < PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) return;
  const uint64_t followup_source = parse_ptp_clock_id(data);
  const int64_t followup_correction_ns = parse_ptp_correction_ns(data, len);
  const uint64_t ptp_time_ns =
      parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
  if (ptp_time_ns > INT64_MAX) return;

  ptp_step_log_t step_ev = {0};
  taskENTER_CRITICAL(&ptp_state_mux);
  if (!legacy_source_admitted_locked(followup_source)) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_RX_FOLLOWUP);
  if (!ptp.awaiting_followup) {
    AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_ORPHAN_FOLLOWUP);
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }
  if (seq != ptp.last_sync_seq ||
      followup_source != ptp.last_sync_source_clock_id) {
    AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_PAIR_MISMATCH);
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  const int64_t sync_local_ns = ptp.last_sync_local_ns;
  const int64_t raw_offset = (int64_t)ptp_time_ns +
                             ptp.last_sync_correction_ns +
                             followup_correction_ns - sync_local_ns;
  ptp.awaiting_followup = false;
  AUDIO_DIAG_SYNC_GAP_NS(reception_ns >= sync_local_ns
                             ? reception_ns - sync_local_ns : 0);
  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_PAIR_OK);
  update_legacy_offset_locked(raw_offset, reception_ns,
                              ptp.last_sync_process_lag_ns, seq, &step_ev);
  taskEXIT_CRITICAL(&ptp_state_mux);
  log_ptp_step(&step_ev);
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
  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_RX_ANNOUNCE);
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
    if (old_gm != 0) {
      ptp_step_log_t step_ev = {
          .emit = true,
          .reason = "gm-change",
          .source_clock_id = source_clock,
          .grandmaster_clock_id = gm,
          .epoch = ptp.rt_gm_changes + 1U,
      };
      log_ptp_step(&step_ev);
    }
#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
    struct in_addr src = {.s_addr = source_ip};
    struct in_addr peer = {.s_addr = peer_ip};
    char src_text[INET_ADDRSTRLEN] = {0};
    char peer_text[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &src, src_text, sizeof(src_text));
    if (peer_ip) inet_ntop(AF_INET, &peer, peer_text, sizeof(peer_text));
    ESP_LOGI(TAG,
             "RT ANNOUNCE | peer=%s | wanted=%s | source=%016llx | gm=%016llx | oldGM=%016llx",
             src_text, peer_ip ? peer_text : "any",
             (unsigned long long)source_clock, (unsigned long long)gm,
             (unsigned long long)old_gm);
#else
    (void)peer_ip;
#endif
  }
}

static void process_sync_realtime(const uint8_t *data, size_t len,
                                  uint16_t seq, uint32_t source_ip,
                                  int64_t reception_ns) {
  if (len < PTP_HEADER_SIZE) return;

  const uint64_t source_clock = parse_ptp_clock_id(data);
  const int64_t sync_correction_ns = parse_ptp_correction_ns(data, len);

  taskENTER_CRITICAL(&ptp_state_mux);
  if (!ptp.realtime_mode || !realtime_source_matches_locked(source_ip)) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_RX_SYNC);
  if (ptp.source_clock_id == 0) ptp.source_clock_id = source_clock;

  /* A new Sync before the previous pair completed means the old pair can no
   * longer be used safely. Count it and replace it with the newest Sync. */
  if (ptp.rt_awaiting_followup)
    AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_PAIR_MISMATCH);
  ptp.rt_sync_seq = seq;
  ptp.rt_sync_local_ns = reception_ns;
  ptp.rt_sync_correction_ns = sync_correction_ns;
  ptp.rt_sync_source_clock_id = source_clock;
  ptp.rt_sync_source_ip = source_ip;
  ptp.rt_awaiting_followup = true;
  taskEXIT_CRITICAL(&ptp_state_mux);
}

static void process_followup_realtime(const uint8_t *data, size_t len,
                                      uint16_t seq, uint32_t source_ip,
                                      int64_t reception_ns) {
  if (len < PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) return;

  const uint64_t ptp_time_ns =
      parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
  if (ptp_time_ns > INT64_MAX) return;
  const int64_t followup_correction_ns = parse_ptp_correction_ns(data, len);
  const uint64_t followup_source_clock = parse_ptp_clock_id(data);

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

  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_RX_FOLLOWUP);

  if (!ptp.rt_awaiting_followup) {
    AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_ORPHAN_FOLLOWUP);
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  if (seq != ptp.rt_sync_seq || source_ip != ptp.rt_sync_source_ip ||
      followup_source_clock != ptp.rt_sync_source_clock_id) {
    /* Keep the current pending Sync: an out-of-order/old Follow_Up must not
     * destroy the chance to receive the correct Follow_Up for it. */
    AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_PAIR_MISMATCH);
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  const int64_t sync_local_ns = ptp.rt_sync_local_ns;
  const int64_t sync_correction_ns = ptp.rt_sync_correction_ns;
  ptp.rt_awaiting_followup = false;
  AUDIO_DIAG_SYNC_GAP_NS(reception_ns >= sync_local_ns
                             ? reception_ns - sync_local_ns
                             : 0);
  AUDIO_DIAG_SYNC_COUNT(AUDIO_DIAG_SYNC_PAIR_OK);

  const int64_t raw_offset =
      (int64_t)ptp_time_ns + sync_correction_ns + followup_correction_ns -
      sync_local_ns;
  became_ready = realtime_update_offset_locked(raw_offset, reception_ns);
  gm = ptp.grandmaster_clock_id;
  source = ptp.source_clock_id;
  master_off = ptp.rt_master_offset_ns;
  samples = ptp.rt_sample_count;
  if (ptp.rt_mastership_start_ns > 0 && reception_ns >= ptp.rt_mastership_start_ns)
    master_age = (uint32_t)((reception_ns - ptp.rt_mastership_start_ns) / 1000000LL);
  taskEXIT_CRITICAL(&ptp_state_mux);

#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
  if (became_ready) {
    ESP_LOGI(TAG,
             "RT MASTER READY | gm=%016llx | source=%016llx | age=%lums | samples=%lu | masterOff=%+lldns",
             (unsigned long long)gm, (unsigned long long)source,
             (unsigned long)master_age, (unsigned long)samples,
             (long long)master_off);
  }
#else
  (void)became_ready;
  (void)gm;
  (void)source;
  (void)master_off;
  (void)master_age;
  (void)samples;
#endif
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
      if (is_event_port) process_sync_legacy(data, len, seq, reception_ns);
      break;
    case PTP_MSG_FOLLOW_UP:
      if (!is_event_port) process_followup_legacy(data, len, seq, reception_ns);
      break;
    case PTP_MSG_ANNOUNCE:
      if (!is_event_port) process_announce_legacy(data, len, reception_ns);
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
  (void)pvParameters;
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
  ptp_clock_engine_init(&ptp.legacy_engine, OUTLIER_THRESHOLD_NS);

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
  ptp.lock_candidate_start_ms = 0;
  ptp.last_sync_ms = 0;
  ptp.filtered_offset_ns = 0;
  ptp.sample_count = 0;
  ptp.previous_offset = 0;
  ptp.previous_offset_time_ms = 0;
  ptp.mastership_start_ms = 0;
  ptp.last_sync_seq = 0;
  ptp.last_sync_local_ns = 0;
  ptp.awaiting_followup = false;
  ptp.expected_clock_id = 0;
  ptp.legacy_sample_clock_id = 0;
  ptp.legacy_source_mixed = false;
  ptp_clock_engine_init(&ptp.legacy_engine, OUTLIER_THRESHOLD_NS);
  ptp.legacy_last_step_log_ns = 0;

  /* Keep mode + timing peer across a stream-level clear, but drop the
   * realtime GM estimator. No audio continuity state lives in ptp_clock. */
  ptp.source_clock_id = 0;
  ptp.grandmaster_clock_id = 0;
  ptp.rt_master_offset_ns = 0;
  ptp.rt_raw_offset_ns = 0;
  ptp.rt_previous_offset_ns = 0;
  ptp.rt_previous_offset_time_ns = 0;
  ptp.rt_mastership_start_ns = 0;
  ptp.rt_last_followup_rx_ns = 0;
  ptp.rt_sample_count = 0;
  ptp.rt_master_ready = false;
  ptp.rt_sync_seq = 0;
  ptp.rt_sync_local_ns = 0;
  ptp.rt_sync_correction_ns = 0;
  ptp.rt_sync_source_clock_id = 0;
  ptp.rt_sync_source_ip = 0;
  ptp.rt_awaiting_followup = false;
  taskEXIT_CRITICAL(&ptp_state_mux);
}

void ptp_clock_notify_resume(uint32_t pause_duration_ms) {
  if (pause_duration_ms < PTP_LONG_PAUSE_THRESHOLD_MS) return;

  ptp_step_log_t step_ev = {0};
  taskENTER_CRITICAL(&ptp_state_mux);
  const bool realtime = ptp.realtime_mode;
  if (realtime) {
    ptp.rt_master_offset_ns = 0;
    ptp.rt_raw_offset_ns = 0;
    ptp.rt_previous_offset_ns = 0;
    ptp.rt_previous_offset_time_ns = 0;
    ptp.rt_mastership_start_ns = 0;
    ptp.rt_last_followup_rx_ns = 0;
    ptp.rt_sample_count = 0;
    ptp.rt_master_ready = false;
    ptp.rt_sync_seq = 0;
    ptp.rt_sync_local_ns = 0;
    ptp.rt_sync_correction_ns = 0;
    ptp.rt_sync_source_clock_id = 0;
    ptp.rt_sync_source_ip = 0;
    ptp.rt_awaiting_followup = false;
    ptp.locked = false;
    ptp.filtered_offset_ns = 0;
    step_ev.source_clock_id = ptp.source_clock_id;
    step_ev.grandmaster_clock_id = ptp.grandmaster_clock_id;
    step_ev.epoch = ptp.grandmaster_clock_id ? ptp.rt_gm_changes + 1U : 0U;
  } else {
    ptp_clock_engine_reset_filter(&ptp.legacy_engine, true);
    ptp.filtered_offset_ns = 0;
    ptp.sample_count = 0;
    ptp.last_sync_ms = 0;
    ptp.locked = false;
    ptp.lock_candidate_start_ms = 0;
    ptp.awaiting_followup = false;
    step_ev.source_clock_id = ptp.legacy_engine.source_clock_id;
    step_ev.grandmaster_clock_id = ptp.legacy_engine.grandmaster_clock_id;
    step_ev.epoch = ptp.legacy_engine.epoch;
  }
  step_ev.emit = true;
  step_ev.reason = "clock-reset";
  taskEXIT_CRITICAL(&ptp_state_mux);

  log_ptp_step(&step_ev);
#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
  ESP_LOGI(TAG, "PTP resume reset | pause=%lums | mode=%s",
           (unsigned long)pause_duration_ms,
           realtime ? "realtime" : "buffered");
#endif
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


void ptp_clock_get_snapshot(ptp_clock_snapshot_t *snapshot) {
  if (!snapshot) return;
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->sample_age_ms = UINT32_MAX;

  const int64_t now_ns = get_local_time_ns();
  const uint32_t now_ms = (uint32_t)(now_ns / 1000000LL);
  taskENTER_CRITICAL(&ptp_state_mux);

  /* Snapshot is the buffered timing read boundary, so expire a stale lock
   * here. Audio can then continue from its cached local anchor without treating
   * stale PTP as newly qualified. */
  if (!ptp.realtime_mode && ptp.locked && ptp.last_sync_ms > 0 &&
      (now_ms - ptp.last_sync_ms) > LOCK_TIMEOUT_MS) {
    ptp.locked = false;
    ptp.lock_candidate_start_ms = 0;
  }

  snapshot->realtime_mode = ptp.realtime_mode;
  snapshot->locked = ptp.locked;
  if (ptp.realtime_mode) {
    snapshot->valid = ptp.rt_master_ready && ptp.rt_last_followup_rx_ns > 0;
    snapshot->source_clock_id = ptp.source_clock_id;
    snapshot->grandmaster_clock_id = ptp.grandmaster_clock_id;
    snapshot->epoch = ptp.grandmaster_clock_id ? ptp.rt_gm_changes + 1U : 0U;
    snapshot->raw_offset_ns = ptp.rt_raw_offset_ns;
    snapshot->filtered_offset_ns = ptp.rt_master_offset_ns;
    snapshot->raw_filter_delta_ns =
        ptp.rt_raw_offset_ns - ptp.rt_master_offset_ns;
    snapshot->sample_count = ptp.rt_sample_count;
    if (ptp.rt_last_followup_rx_ns > 0 && now_ns >= ptp.rt_last_followup_rx_ns) {
      const uint64_t age_ms = (uint64_t)((now_ns - ptp.rt_last_followup_rx_ns) /
                                         1000000LL);
      snapshot->sample_age_ms = age_ms > UINT32_MAX ? UINT32_MAX
                                                    : (uint32_t)age_ms;
    }
  } else {
    snapshot->valid = ptp.legacy_engine.valid;
    snapshot->source_clock_id = ptp.legacy_engine.source_clock_id;
    snapshot->grandmaster_clock_id = ptp.legacy_engine.grandmaster_clock_id;
    snapshot->epoch = ptp.legacy_engine.epoch;
    snapshot->raw_offset_ns = ptp.legacy_engine.raw_offset_ns;
    snapshot->filtered_offset_ns = ptp.legacy_engine.filtered_offset_ns;
    snapshot->raw_filter_delta_ns =
        ptp.legacy_engine.raw_offset_ns - ptp.legacy_engine.filtered_offset_ns;
    if (ptp.legacy_engine.mastership_start_time_ns > 0 &&
        now_ns >= ptp.legacy_engine.mastership_start_time_ns) {
      const uint64_t age_ms =
          (uint64_t)((now_ns - ptp.legacy_engine.mastership_start_time_ns) /
                     1000000LL);
      snapshot->mastership_age_ms = age_ms > UINT32_MAX ? UINT32_MAX
                                                        : (uint32_t)age_ms;
    }
    snapshot->sample_count = ptp.legacy_engine.accepted_samples;
    if (ptp.legacy_engine.last_accepted_time_ns > 0 &&
        now_ns >= ptp.legacy_engine.last_accepted_time_ns) {
      const uint64_t age_ms =
          (uint64_t)((now_ns - ptp.legacy_engine.last_accepted_time_ns) /
                     1000000LL);
      snapshot->sample_age_ms = age_ms > UINT32_MAX ? UINT32_MAX
                                                    : (uint32_t)age_ms;
    }
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
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
    ptp.lock_candidate_start_ms = 0;
    ptp.last_sync_ms = 0;
    ptp.filtered_offset_ns = 0;
    ptp.sample_count = 0;
    ptp.previous_offset = 0;
    ptp.previous_offset_time_ms = 0;
    ptp.mastership_start_ms = 0;
    ptp.last_sync_seq = 0;
    ptp.last_sync_local_ns = 0;
    ptp.awaiting_followup = false;
    ptp.expected_clock_id = 0;
    ptp.legacy_sample_clock_id = 0;
    ptp.legacy_source_mixed = false;
    ptp_clock_engine_init(&ptp.legacy_engine, OUTLIER_THRESHOLD_NS);
    ptp.legacy_last_step_log_ns = 0;
    ptp.source_clock_id = 0;
    ptp.grandmaster_clock_id = 0;
    ptp.rt_master_offset_ns = 0;
    ptp.rt_previous_offset_ns = 0;
    ptp.rt_previous_offset_time_ns = 0;
    ptp.rt_mastership_start_ns = 0;
    ptp.rt_last_followup_rx_ns = 0;
    ptp.rt_sample_count = 0;
    ptp.rt_master_ready = false;
    ptp.rt_sync_seq = 0;
    ptp.rt_sync_local_ns = 0;
    ptp.rt_sync_correction_ns = 0;
    ptp.rt_sync_source_clock_id = 0;
    ptp.rt_sync_source_ip = 0;
    ptp.rt_awaiting_followup = false;
  }
  task_handle = ptp.task_handle;
  taskEXIT_CRITICAL(&ptp_state_mux);

  if (changed && task_handle) {
    vTaskPrioritySet(task_handle, enabled ? PTP_TASK_PRIORITY_REALTIME
                                         : PTP_TASK_PRIORITY_LEGACY);
  }
#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
  if (changed) {
    struct in_addr peer = {.s_addr = timing_peer_ip};
    char peer_text[INET_ADDRSTRLEN] = {0};
    if (timing_peer_ip) inet_ntop(AF_INET, &peer, peer_text, sizeof(peer_text));
    ESP_LOGI(TAG, "PTP mode=%s | timingPeer=%s",
             enabled ? "AP2-realtime" : "legacy/buffered",
             enabled && timing_peer_ip ? peer_text : "none");
  }
#else
  (void)changed;
#endif
}

void ptp_clock_note_realtime_d7(uint64_t clock_id) {
  if (clock_id == 0) return;
  taskENTER_CRITICAL(&ptp_state_mux);
  if (ptp.realtime_mode) {
    ptp.expected_clock_id = clock_id; /* anchor hint only; no realtime filtering */
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
}

bool ptp_clock_realtime_time_to_local(uint64_t clock_id,
                                      uint64_t remote_ptp_ns,
                                      uint64_t *local_ns) {
  ptp_realtime_snapshot_t snapshot;
  ptp_clock_get_realtime_snapshot(&snapshot);
  return ptp_clock_realtime_snapshot_to_local(
      &snapshot, clock_id, remote_ptp_ns, local_ns);
}

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *snapshot) {
  if (!snapshot) return;
  memset(snapshot, 0, sizeof(*snapshot));
  taskENTER_CRITICAL(&ptp_state_mux);
  const int64_t now_ns = get_local_time_ns();
  snapshot->realtime_mode = ptp.realtime_mode;
  const int64_t age_ns = now_ns - ptp.rt_last_followup_rx_ns;
  snapshot->sample_age_ms = ptp.rt_last_followup_rx_ns > 0 && age_ns >= 0
      ? (uint32_t)((uint64_t)(age_ns / 1000000LL) > UINT32_MAX
                       ? UINT32_MAX : (uint64_t)(age_ns / 1000000LL))
      : UINT32_MAX;
  snapshot->master_ready = ptp.rt_master_ready &&
      snapshot->sample_age_ms <= LOCK_TIMEOUT_MS;
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
  bool preserved_legacy = false;
  bool was_locked = false;
  uint64_t learned_source = 0;
  bool source_mixed = false;
  ptp_step_log_t step_ev = {0};

  taskENTER_CRITICAL(&ptp_state_mux);
  realtime = ptp.realtime_mode;
  if (clock_id != ptp.expected_clock_id) {
    changed = true;
    was_locked = ptp.locked;
    learned_source = ptp.legacy_engine.source_clock_id;
    source_mixed = ptp.legacy_source_mixed;
    ptp.expected_clock_id = clock_id;
    if (!realtime) {
      preserved_legacy = clock_id != 0 && ptp.legacy_engine.valid &&
          !ptp.legacy_source_mixed &&
          ptp.legacy_engine.source_clock_id == clock_id;
      if (!preserved_legacy) {
        const uint32_t old_epoch = ptp.legacy_engine.epoch;
        ptp_clock_engine_domain_event_t de = {0};
        if (clock_id != 0 && ptp.legacy_engine.source_clock_id != clock_id) {
          (void)ptp_clock_engine_set_domain(&ptp.legacy_engine, clock_id, 0, &de);
        } else {
          ptp_clock_engine_reset_filter(&ptp.legacy_engine, true);
        }
        if (ptp.legacy_engine.epoch == old_epoch)
          ptp_clock_engine_reset_filter(&ptp.legacy_engine, true);

        ptp.locked = false;
        ptp.lock_candidate_start_ms = 0;
        ptp.last_sync_ms = 0;
        ptp.filtered_offset_ns = 0;
        ptp.sample_count = 0;
        ptp.last_sync_seq = 0;
        ptp.last_sync_local_ns = 0;
        ptp.last_sync_correction_ns = 0;
        ptp.last_sync_source_clock_id = 0;
        ptp.last_sync_process_lag_ns = 0;
        ptp.awaiting_followup = false;
        ptp.legacy_sample_clock_id = clock_id;
        ptp.legacy_source_mixed = false;

        step_ev.emit = clock_id != 0;
        step_ev.reason = "clock-reset";
        step_ev.source_clock_id = clock_id;
        step_ev.grandmaster_clock_id = ptp.legacy_engine.grandmaster_clock_id;
        step_ev.epoch = ptp.legacy_engine.epoch;
      }
    }
  }
  taskEXIT_CRITICAL(&ptp_state_mux);

  log_ptp_step(&step_ev);
  if (changed) {
#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
    if (!realtime && preserved_legacy) {
      ESP_LOGI(TAG,
               "PTP anchor clock confirmed | clock=%016llx | source=%016llx | estimator=%s",
               (unsigned long long)clock_id,
               (unsigned long long)learned_source,
               was_locked ? "locked" : "acquiring");
    } else if (!realtime) {
      ESP_LOGI(TAG,
               "PTP anchor clock reset | clock=%016llx | source=%016llx | mixed=%d",
               (unsigned long long)clock_id,
               (unsigned long long)learned_source, source_mixed ? 1 : 0);
    } else {
      ESP_LOGI(TAG, "RT anchor clock hint | clock=%016llx",
               (unsigned long long)clock_id);
    }
#else
    (void)was_locked;
    (void)learned_source;
    (void)source_mixed;
#endif
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
