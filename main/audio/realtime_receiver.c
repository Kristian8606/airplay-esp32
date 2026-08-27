#include "realtime_receiver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "alac_decoder.h"
#include "audio_crypto.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network/socket_utils.h"
#include "network/ptp_clock.h"

#define RT_PACKET_MAX             8192U
#define RT_PCM_CAPACITY_FRAMES    4096U

/* Realtime ingress is intentionally split. The DATA task does no crypto or
 * decode work; it only drains lwIP into a preallocated packet pool. CONTROL
 * receives D7/PT84 and forwards RTX into the same worker queue. ALAC_WORK is
 * the sole owner of decrypt + ALAC decoder state. RESEND owns missing state. */
#define RT_DATA_RX_STACK          4096U
#define RT_CTRL_RX_STACK          6144U
#define RT_WORK_STACK             8192U
#define RT_RESEND_STACK           4096U
#define RT_DATA_RX_PRIORITY       7
#define RT_CTRL_RX_PRIORITY       6
#define RT_WORK_PRIORITY          6
#define RT_RESEND_PRIORITY        6
#define RT_TASK_CORE              0

#define RT_DATA_POOL_SLOTS        64U
#define RT_RTX_POOL_SLOTS         32U
#define RT_WORK_QUEUE_SLOTS       96U
#define RT_RESEND_EVENT_SLOTS     512U
#define RT_SEEN_SLOTS             1024U
#define RT_MISSING_SLOTS          512U

#define RT_FIRST_PACKET_LOGS      5U
#define RT_SYNC_PT                84U
#define RT_AP2_ANCHOR_PT          87U
#define RT_RETRANSMIT_PT          86U
#define RT_RESEND_REQUEST_PT      85U
#define RT_MAX_NACK_COUNT         32U
/* Match Shairport Sync defaults: wait 100 ms before the first request, retry
 * at 250 ms intervals, and stop issuing NEW requests 100 ms before output.
 * Do not declare final loss there: an already-requested RTX may still arrive.
 * Final loss is the ordered-staging commit boundary below. */
#define RT_RESEND_FIRST_MS        100U
#define RT_RESEND_RETRY_MS        250U
#define RT_RESEND_LAST_REQUEST_MS 100U
#define RT_FINAL_LOSS_MARGIN_MS   ((uint32_t)(REALTIME_RECOVERY_FINAL_MARGIN_US / 1000LL))
#define RT_RESEND_SCAN_MS         10U
#define RT_SOCKET_TIMEOUT_MS      100U

static const char *TAG = "airplay_rt";

typedef enum {
  RT_POOL_DATA = 0,
  RT_POOL_RTX = 1,
} rt_pool_kind_t;

typedef struct {
  uint16_t len;
  uint8_t pool_kind;
  bool retransmitted;
  int64_t rx_us;
  uint8_t data[RT_PACKET_MAX];
} rt_packet_slot_t;

typedef struct {
  bool valid;
  uint32_t ext_seq;
} seen_slot_t;

typedef struct {
  bool active;
  uint32_t ext_seq;
  uint32_t missing_rtp;
  TickType_t missing_since;
  TickType_t last_nack;
  uint8_t nack_count;
  int64_t last_nack_sent_us;
} missing_slot_t;

typedef enum {
  RT_RESEND_EVENT_MISSING = 1,
  RT_RESEND_EVENT_RECEIVED = 2,
} rt_resend_event_kind_t;

typedef struct {
  uint8_t kind;
  uint16_t count;
  uint32_t ext_seq;
  uint32_t rtp;
  int64_t event_us;
} rt_resend_event_t;

typedef struct {
  realtime_receiver_config_t cfg;
  int data_sock;
  int control_sock;
  volatile bool running;

  TaskHandle_t data_task;
  TaskHandle_t control_task;
  TaskHandle_t worker_task;
  TaskHandle_t resend_task;

  QueueHandle_t data_free_q;
  QueueHandle_t rtx_free_q;
  QueueHandle_t work_q;
  QueueHandle_t resend_event_q;

  rt_packet_slot_t *data_pool;
  rt_packet_slot_t *rtx_pool;
  uint8_t *control_packet;
  uint8_t *decrypt_buf;
  int16_t *pcm;
  seen_slot_t *seen;
  missing_slot_t *missing;

  uint32_t newest_ext_seq;
  bool newest_ext_valid;

  uint32_t rx_packets;
  uint32_t decoded_packets;
  uint32_t decrypt_errors;
  uint32_t decode_errors;
  uint32_t sink_drops;
  struct sockaddr_in client_control_addr;
  bool client_control_valid;
  uint16_t nack_request_seq;
  uint32_t nack_requests;
  uint32_t retransmit_packets;
  uint32_t retransmit_bad;
  uint32_t reorder_late;
  uint32_t reorder_overwrite;
  uint32_t gap_skips;
  uint32_t resend_scans;
  uint32_t resend_retries;
  uint32_t resend_giveups;
  uint32_t hard_resyncs; /* Deliberately stays zero: transport never resets lower audio. */
  uint32_t gap_events;
  uint32_t missing_packets;
  uint32_t select_errors; /* Kept for existing diagnostics; split tasks do not use select(). */
  uint32_t recv_errors;
  uint32_t nack_send_errors;
  int64_t last_data_rx_us;
  uint32_t processing_samples;
  uint64_t processing_sum_us;
  uint32_t interval_max_processing_us;
  uint32_t interval_max_control_us;
  uint32_t interval_max_interarrival_us;
  uint16_t interval_max_gap_packets;
  uint32_t rtx_latency_samples;
  uint64_t rtx_latency_sum_us;
  uint32_t rtx_latency_min_us;
  uint32_t rtx_latency_max_us;

  uint32_t data_pool_waits;
  uint32_t rtx_pool_drops;
  uint32_t work_queue_drops;
  uint32_t resend_event_drops;
  uint32_t missing_tracker_overflow;


  /* Passive PT=84 source-timeline observation. Never used to steer playout. */
  uint32_t sync_packets;
  uint32_t sync_malformed;
  uint16_t last_sync_flags;
  uint32_t last_sync_rtp_less_latency;
  uint32_t last_sync_rtp;
  uint32_t last_sync_latency_frames;
  uint32_t last_sync_time_seconds;
  uint32_t last_sync_time_fraction;

  /* AirPlay 2 realtime D7/PT=87 observation. The first validated D7 may
   * establish the initial RTP/PTP map; later D7 packets only authorize PTP
   * clock-domain handovers and never move the running RTP cursor. */
  uint32_t d7_packets;
  uint32_t d7_malformed;
  uint32_t last_d7_frame1;
  uint32_t last_d7_frame2;
  uint32_t last_d7_delta_frames;
  uint64_t last_d7_raw_ptp_ns;
  uint64_t last_d7_ptp_ns;
  uint64_t last_d7_clock_id;
  bool initial_d7_anchor_committed;
  volatile bool d7_anchor_refresh_requested;
  uint32_t initial_d7_anchor_commit_count;
  uint64_t initial_d7_timeline_ns;

  uint64_t unhandled_control_seen[2];
} realtime_state_t;

static realtime_state_t s_rt = {
    .data_sock = -1,
    .control_sock = -1,
};

static inline uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t read_be64(const uint8_t *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
         ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
         ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static size_t rtp_payload_offset(const uint8_t *packet, size_t len) {
  if (!packet || len < 12U || (packet[0] >> 6) != 2U) {
    return 0;
  }

  const size_t cc = packet[0] & 0x0fU;
  size_t off = 12U + cc * 4U;
  if (off > len) {
    return 0;
  }

  if (packet[0] & 0x10U) { /* RTP extension */
    if (off + 4U > len) {
      return 0;
    }
    const uint16_t ext_words =
        (uint16_t)(((uint16_t)packet[off + 2] << 8) | packet[off + 3]);
    off += 4U + (size_t)ext_words * 4U;
    if (off > len) {
      return 0;
    }
  }
  return off;
}


static bool send_retransmit_request(uint16_t first_missing, uint16_t count,
                                    int64_t *sent_us_out) {
  if (!s_rt.client_control_valid || s_rt.control_sock < 0 || count == 0) {
    return false;
  }
  if (count > RT_MAX_NACK_COUNT) {
    count = RT_MAX_NACK_COUNT;
  }

  uint8_t req[8] = {0};
  const uint16_t request_seq = ++s_rt.nack_request_seq;
  req[0] = 0x80;
  req[1] = 0x80U | RT_RESEND_REQUEST_PT;
  req[2] = (uint8_t)(request_seq >> 8);
  req[3] = (uint8_t)request_seq;
  req[4] = (uint8_t)(first_missing >> 8);
  req[5] = (uint8_t)first_missing;
  req[6] = (uint8_t)(count >> 8);
  req[7] = (uint8_t)count;

  const int64_t sent_us = esp_timer_get_time();
  const ssize_t n = sendto(s_rt.control_sock, req, sizeof(req), 0,
                           (const struct sockaddr *)&s_rt.client_control_addr,
                           sizeof(s_rt.client_control_addr));
  if (n == (ssize_t)sizeof(req)) {
    s_rt.nack_requests++;
    if (sent_us_out) {
      *sent_us_out = sent_us;
    }
    return true;
  }

  s_rt.nack_send_errors++;
  return false;
}

static bool decode_audio_packet(alac_decoder_t *decoder,
                                const uint8_t *packet, size_t packet_len,
                                bool retransmitted, uint32_t *first_logs) {
  const size_t payload_off = rtp_payload_offset(packet, packet_len);
  if (payload_off == 0 || payload_off >= packet_len) {
    return false;
  }

  const uint8_t pt = packet[1] & 0x7fU;
  if (pt != 96U) {
    return false;
  }

  const uint16_t seq =
      (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);
  const uint32_t rtp = ((uint32_t)packet[4] << 24) |
                       ((uint32_t)packet[5] << 16) |
                       ((uint32_t)packet[6] << 8) |
                       (uint32_t)packet[7];

  const uint8_t *payload = packet + payload_off;
  const size_t payload_len = packet_len - payload_off;
  int dec_len = audio_crypto_decrypt_rtp(
      &s_rt.cfg.encrypt, payload, payload_len, s_rt.decrypt_buf,
      RT_PACKET_MAX, packet, packet_len);
  if (dec_len <= 0) {
    s_rt.decrypt_errors++;
    if (s_rt.decrypt_errors <= RT_FIRST_PACKET_LOGS) {
      ESP_LOGW(TAG,
               "decrypt failed seq=%u rtp=%" PRIu32 " packet=%u payload=%u%s",
               seq, rtp, (unsigned)packet_len, (unsigned)payload_len,
               retransmitted ? " retransmit" : "");
    }
    return false;
  }

  alac_decode_info_t info = {0};
  int frames = alac_decoder_decode(decoder, s_rt.decrypt_buf,
                                   (size_t)dec_len, s_rt.pcm,
                                   RT_PCM_CAPACITY_FRAMES, &info);
  if (frames <= 0) {
    s_rt.decode_errors++;
    if (s_rt.decode_errors <= RT_FIRST_PACKET_LOGS) {
      ESP_LOGW(TAG, "ALAC decode failed seq=%u rtp=%" PRIu32 " bytes=%d%s",
               seq, rtp, dec_len, retransmitted ? " retransmit" : "");
    }
    return false;
  }

  if (!s_rt.cfg.pcm_sink ||
      !s_rt.cfg.pcm_sink(rtp, s_rt.pcm, (size_t)frames, info.channels,
                         s_rt.cfg.pcm_sink_ctx)) {
    s_rt.sink_drops++;
    return false;
  }

  s_rt.decoded_packets++;
  if (retransmitted) {
    s_rt.retransmit_packets++;
  }
  (void)first_logs;
  return true;
}



static uint32_t unwrap_seq16(uint16_t seq, uint32_t reference) {
  int64_t candidate = (int64_t)(reference & 0xffff0000U) | (int64_t)seq;
  const int64_t ref = (int64_t)reference;
  if (candidate - ref > 32767LL) {
    candidate -= 65536LL;
  } else if (ref - candidate > 32768LL) {
    candidate += 65536LL;
  }
  if (candidate < 0) {
    candidate += (1LL << 32);
  }
  return (uint32_t)candidate;
}

static bool seen_has(uint32_t ext_seq) {
  if (!s_rt.seen) return false;
  seen_slot_t *slot = &s_rt.seen[ext_seq % RT_SEEN_SLOTS];
  return slot->valid && slot->ext_seq == ext_seq;
}

static void seen_mark(uint32_t ext_seq) {
  if (!s_rt.seen) return;
  seen_slot_t *slot = &s_rt.seen[ext_seq % RT_SEEN_SLOTS];
  slot->valid = true;
  slot->ext_seq = ext_seq;
}

static void release_packet_slot(rt_packet_slot_t *slot) {
  if (!slot) return;
  slot->len = 0;
  QueueHandle_t q = slot->pool_kind == RT_POOL_RTX ? s_rt.rtx_free_q
                                                   : s_rt.data_free_q;
  if (q) (void)xQueueSend(q, &slot, 0);
}

static bool queue_work_packet(rt_packet_slot_t *slot) {
  if (!slot || !s_rt.work_q) return false;
  if (xQueueSend(s_rt.work_q, &slot, 0) == pdTRUE) return true;
  s_rt.work_queue_drops++;
  return false;
}

static bool queue_resend_event(uint8_t kind, uint32_t ext_seq, uint16_t count,
                               uint32_t rtp) {
  if (!s_rt.resend_event_q || count == 0) return false;
  rt_resend_event_t ev = {
      .kind = kind, .count = count, .ext_seq = ext_seq, .rtp = rtp,
      .event_us = esp_timer_get_time(),
  };
  if (xQueueSend(s_rt.resend_event_q, &ev, pdMS_TO_TICKS(2)) == pdTRUE)
    return true;
  s_rt.resend_event_drops++;
  return false;
}

static bool missing_time_to_play_us(const missing_slot_t *slot,
                                    int64_t *out_us) {
  if (!slot || !out_us || !s_rt.cfg.deadline_cb) {
    return false;
  }
  return s_rt.cfg.deadline_cb(slot->missing_rtp, out_us,
                              s_rt.cfg.deadline_ctx);
}

static missing_slot_t *missing_slot_for(uint32_t ext_seq) {
  return &s_rt.missing[ext_seq % RT_MISSING_SLOTS];
}



static void missing_clear_slot(missing_slot_t *slot) {
  if (!slot || !slot->active) return;
  memset(slot, 0, sizeof(*slot));
}

static bool missing_find_min(uint32_t *min_ext) {
  if (!s_rt.missing || !min_ext) return false;
  bool found = false;
  uint32_t minv = UINT32_MAX;
  for (uint32_t i = 0; i < RT_MISSING_SLOTS; ++i) {
    const missing_slot_t *slot = &s_rt.missing[i];
    if (slot->active && (!found || slot->ext_seq < minv)) {
      minv = slot->ext_seq;
      found = true;
    }
  }
  if (found) *min_ext = minv;
  return found;
}

static void missing_add_range(uint32_t first_ext, uint16_t count,
                              uint32_t first_rtp, TickType_t now) {
  if (!s_rt.missing || count == 0) return;

  uint32_t min_active = 0;
  const bool have_min = missing_find_min(&min_active);
  const uint32_t frame_samples = s_rt.cfg.format.frame_size > 0
                                     ? (uint32_t)s_rt.cfg.format.frame_size
                                     : 352U;

  uint16_t tracked = 0;
  for (uint16_t i = 0; i < count; ++i) {
    const uint32_t ext = first_ext + i;
    if (have_min && ext > min_active &&
        ext - min_active >= RT_MISSING_SLOTS) {
      s_rt.missing_tracker_overflow += (uint32_t)(count - i);
      break;
    }

    missing_slot_t *slot = missing_slot_for(ext);
    if (slot->active && slot->ext_seq != ext) {
      /* Keep the older unresolved hole rather than evicting it. A gap this
       * wide is already outside the configured recovery horizon, but it must
       * never cause a lower-audio resync/cursor jump. */
      s_rt.missing_tracker_overflow++;
      continue;
    }
    if (!slot->active) {
      memset(slot, 0, sizeof(*slot));
      slot->active = true;
      slot->ext_seq = ext;
      slot->missing_rtp = first_rtp + (uint32_t)i * frame_samples;
      slot->missing_since = now;
      tracked++;
    }
  }
  (void)tracked;
}

static void note_rtx_latency_from_slot(const missing_slot_t *slot,
                                       int64_t received_us) {
  if (!slot || slot->last_nack_sent_us <= 0 || received_us <= 0) return;
  const int64_t delta = received_us - slot->last_nack_sent_us;
  if (delta < 0) return;
  const uint32_t us = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
  if (s_rt.rtx_latency_samples == 0U || us < s_rt.rtx_latency_min_us) {
    s_rt.rtx_latency_min_us = us;
  }
  if (us > s_rt.rtx_latency_max_us) {
    s_rt.rtx_latency_max_us = us;
  }
  s_rt.rtx_latency_sum_us += us;
  s_rt.rtx_latency_samples++;
}

static void missing_mark_received(uint32_t ext_seq, int64_t received_us) {
  if (!s_rt.missing) return;
  missing_slot_t *slot = missing_slot_for(ext_seq);
  if (!slot->active || slot->ext_seq != ext_seq) return;
  note_rtx_latency_from_slot(slot, received_us);
  missing_clear_slot(slot);
}

static bool missing_slot_due(const missing_slot_t *slot, TickType_t now) {
  if (!slot || !slot->active) return false;
  const TickType_t age = now - slot->missing_since;
  if (age < pdMS_TO_TICKS(RT_RESEND_FIRST_MS)) return false;

  int64_t time_to_play_us = 0;
  const bool deadline_valid = missing_time_to_play_us(slot, &time_to_play_us);
  if (deadline_valid &&
      time_to_play_us <= (int64_t)RT_RESEND_LAST_REQUEST_MS * 1000LL) {
    return false;
  }
  if (slot->nack_count == 0U) return true;
  if (!deadline_valid) return false;
  return (now - slot->last_nack) >= pdMS_TO_TICKS(RT_RESEND_RETRY_MS);
}

static void resend_process_event(const rt_resend_event_t *ev) {
  if (!ev) return;
  if (ev->kind == RT_RESEND_EVENT_MISSING) {
    missing_add_range(ev->ext_seq, ev->count, ev->rtp, xTaskGetTickCount());
  } else if (ev->kind == RT_RESEND_EVENT_RECEIVED) {
    missing_mark_received(ev->ext_seq, ev->event_us);
  }
}

static void resend_giveup_expired(void) {
  if (!s_rt.missing) return;
  for (uint32_t i = 0; i < RT_MISSING_SLOTS; ++i) {
    missing_slot_t *slot = &s_rt.missing[i];
    if (!slot->active) continue;
    int64_t time_to_play_us = 0;
    if (!missing_time_to_play_us(slot, &time_to_play_us)) continue;
    if (time_to_play_us > (int64_t)RT_FINAL_LOSS_MARGIN_MS * 1000LL) continue;

    missing_clear_slot(slot);
    s_rt.gap_skips++;
    s_rt.resend_giveups++;
  }
}

static void resend_scan_due(void) {
  if (!s_rt.missing) return;
  s_rt.resend_scans++;
  const TickType_t now = xTaskGetTickCount();

  uint32_t min_ext = 0;
  if (!missing_find_min(&min_ext)) return;

  /* Active holes are kept inside a bounded modular window. Walking from the
   * oldest active sequence preserves AirTunes' first_seq+count NACK format
   * while allowing non-contiguous holes to become separate requests. */
  for (uint32_t walked = 0; walked < RT_MISSING_SLOTS;) {
    const uint32_t ext = min_ext + walked;
    missing_slot_t *slot = missing_slot_for(ext);
    if (!slot->active || slot->ext_seq != ext || !missing_slot_due(slot, now)) {
      walked++;
      continue;
    }

    const uint32_t first_ext = ext;
    uint16_t count = 0;
    while ((uint32_t)count + walked < RT_MISSING_SLOTS &&
           count < RT_MAX_NACK_COUNT) {
      const uint32_t candidate_ext = first_ext + count;
      missing_slot_t *candidate = missing_slot_for(candidate_ext);
      if (!candidate->active || candidate->ext_seq != candidate_ext ||
          !missing_slot_due(candidate, now)) {
        break;
      }
      count++;
    }

    int64_t sent_us = 0;
    if (count != 0U &&
        send_retransmit_request((uint16_t)first_ext, count, &sent_us)) {
      for (uint16_t i = 0; i < count; ++i) {
        missing_slot_t *requested = missing_slot_for(first_ext + i);
        if (!requested->active || requested->ext_seq != first_ext + i) continue;
        if (requested->nack_count != 0U) s_rt.resend_retries++;
        requested->last_nack = now;
        requested->last_nack_sent_us = sent_us;
        if (requested->nack_count != UINT8_MAX) requested->nack_count++;
      }
    }
    walked += count ? count : 1U;
  }
}

static void reset_transport_tracking(void) {
  s_rt.newest_ext_seq = 0;
  s_rt.newest_ext_valid = false;
  if (s_rt.seen) memset(s_rt.seen, 0, RT_SEEN_SLOTS * sizeof(*s_rt.seen));
  if (s_rt.missing) memset(s_rt.missing, 0, RT_MISSING_SLOTS * sizeof(*s_rt.missing));
}

/* Commit D7 into the existing audio timing API for the initial sender anchor,
 * and again only after an explicit realtime FLUSH with no RTP boundary asks
 * for a fresh media epoch. Normal GM handovers remain entirely inside
 * ptp_clock, so D7 cannot move the lower RTP cursor during clock switch. */
static void service_initial_d7_anchor(void) {
  const bool refresh =
      __atomic_load_n(&s_rt.d7_anchor_refresh_requested, __ATOMIC_ACQUIRE);
  if ((s_rt.initial_d7_anchor_committed && !refresh) ||
      s_rt.last_d7_raw_ptp_ns == 0 || s_rt.last_d7_clock_id == 0) {
    return;
  }

  uint64_t timeline_ptp_ns = 0;
  if (!ptp_clock_translate_realtime_time(s_rt.last_d7_clock_id,
                                         s_rt.last_d7_raw_ptp_ns,
                                         &timeline_ptp_ns)) {
    return;
  }

  /* clock_id=0 is deliberate: D7 must not select/reset the PTP source. The
   * translated timestamp is already in the continuous clock domain exported
   * by ptp_clock_get_time_ns(). */
  audio_receiver_set_anchor_time(0, timeline_ptp_ns, s_rt.last_d7_frame1);
  s_rt.initial_d7_anchor_committed = true;
  __atomic_store_n(&s_rt.d7_anchor_refresh_requested, false, __ATOMIC_RELEASE);
  s_rt.initial_d7_anchor_commit_count++;
  s_rt.initial_d7_timeline_ns = timeline_ptp_ns;

  ptp_realtime_snapshot_t ps = {0};
  ptp_clock_get_realtime_snapshot(&ps);
  ESP_LOGI(TAG,
           "ALAC D7 SENDER ANCHOR raw=%" PRIu64 " timeline=%" PRIu64
           " rtp=%" PRIu32 " clock=%016" PRIx64
           " gm=%016" PRIx64 " bias=%+" PRId64 "ns age=%lums samples=%lu",
           s_rt.last_d7_raw_ptp_ns, timeline_ptp_ns, s_rt.last_d7_frame1,
           s_rt.last_d7_clock_id, ps.master_clock_id, ps.domain_bias_ns,
           (unsigned long)ps.mastership_age_ms,
           (unsigned long)ps.sample_count);
}


static void process_control_packet(const uint8_t *buf, size_t len) {
  if (!buf || len < 4U) return;
  const int64_t control_start_us = esp_timer_get_time();
  const uint8_t pt = buf[1] & 0x7fU;

  if (pt == RT_RETRANSMIT_PT) {
    if (len <= 4U || len - 4U > RT_PACKET_MAX || !s_rt.rtx_free_q) {
      s_rt.retransmit_bad++;
      return;
    }

    rt_packet_slot_t *slot = NULL;
    if (xQueueReceive(s_rt.rtx_free_q, &slot, pdMS_TO_TICKS(2)) != pdTRUE ||
        !slot) {
      s_rt.rtx_pool_drops++;
      return;
    }
    slot->pool_kind = RT_POOL_RTX;
    slot->retransmitted = true;
    slot->rx_us = esp_timer_get_time();
    slot->len = (uint16_t)(len - 4U);
    memcpy(slot->data, buf + 4U, slot->len);
    if (!queue_work_packet(slot)) {
      release_packet_slot(slot);
      s_rt.retransmit_bad++;
    }
    goto done;
  }

  if (pt == RT_AP2_ANCHOR_PT) {
    if (len < 28U) {
      s_rt.d7_malformed++;
      ESP_LOGW(TAG, "AP2 D7 malformed len=%u bad=%" PRIu32,
               (unsigned)len, s_rt.d7_malformed);
      goto done;
    }

    const uint32_t frame1 = read_be32(buf + 4);
    const uint64_t network_time_ns = read_be64(buf + 8);
    const uint32_t frame2 = read_be32(buf + 16);
    const uint64_t clock_id = read_be64(buf + 20);
    const uint32_t delta_frames = frame2 - frame1;

    s_rt.last_d7_frame1 = frame1;
    s_rt.last_d7_frame2 = frame2;
    s_rt.last_d7_delta_frames = delta_frames;
    s_rt.last_d7_raw_ptp_ns = network_time_ns;
    s_rt.last_d7_ptp_ns = network_time_ns;
    s_rt.last_d7_clock_id = clock_id;
    s_rt.d7_packets++;

    ptp_clock_note_realtime_d7(clock_id);
    uint64_t d7_timeline_ns = 0;
    if (ptp_clock_translate_realtime_time(clock_id, network_time_ns,
                                           &d7_timeline_ns)) {
      s_rt.last_d7_ptp_ns = d7_timeline_ns;
    }
    service_initial_d7_anchor();

    if (s_rt.d7_packets <= 3U || (s_rt.d7_packets % 64U) == 0U) {
      const int sr = s_rt.cfg.format.sample_rate > 0
                         ? s_rt.cfg.format.sample_rate
                         : 44100;
      const double delta_ms = (double)delta_frames * 1000.0 / (double)sr;
      ptp_realtime_snapshot_t ps = {0};
      ptp_clock_get_realtime_snapshot(&ps);
      ESP_LOGI(TAG,
               "AP2 D7 n=%" PRIu32 " frame1=%" PRIu32
               " frame2=%" PRIu32 " delta=%" PRIu32 "(%.2fms)"
               " rawPTP=%" PRIu64 " clock=%016" PRIx64
               " gm=%016" PRIx64 " src=%016" PRIx64
               " ready=%d handover=%d age=%lums bias=%+" PRId64 "ns",
               s_rt.d7_packets, frame1, frame2, delta_frames, delta_ms,
               network_time_ns, clock_id, ps.master_clock_id,
               ps.source_clock_id, ps.master_ready ? 1 : 0,
               ps.handover_active ? 1 : 0,
               (unsigned long)ps.handover_age_ms, ps.domain_bias_ns);
    }
    goto done;
  }

  if (pt == RT_SYNC_PT) {
    if (len < 20U) {
      s_rt.sync_malformed++;
      goto done;
    }
    const uint16_t flags = read_be16(buf + 2);
    const uint32_t rtp_less_latency = read_be32(buf + 4);
    const uint32_t time_seconds = read_be32(buf + 8);
    const uint32_t time_fraction = read_be32(buf + 12);
    const uint32_t rtp = read_be32(buf + 16);

    s_rt.last_sync_flags = flags;
    s_rt.last_sync_rtp_less_latency = rtp_less_latency;
    s_rt.last_sync_rtp = rtp;
    s_rt.last_sync_latency_frames = rtp - rtp_less_latency;
    s_rt.last_sync_time_seconds = time_seconds;
    s_rt.last_sync_time_fraction = time_fraction;
    s_rt.sync_packets++;
    goto done;
  }

  {
    const unsigned word = pt >> 6;
    const uint64_t bit = 1ULL << (pt & 63U);
    if (word < 2U && (s_rt.unhandled_control_seen[word] & bit) == 0U) {
      s_rt.unhandled_control_seen[word] |= bit;
      ESP_LOGI(TAG, "AP2 CTRL unhandled raw=0x%02x pt=%u len=%u",
               (unsigned)buf[1], (unsigned)pt, (unsigned)len);
    }
  }

done:
  {
    const int64_t control_us64 = esp_timer_get_time() - control_start_us;
    if (control_us64 >= 0) {
      const uint32_t control_us = control_us64 > UINT32_MAX
                                      ? UINT32_MAX
                                      : (uint32_t)control_us64;
      if (control_us > s_rt.interval_max_control_us) {
        s_rt.interval_max_control_us = control_us;
      }
    }
  }
}

static void data_rx_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "DATA_RX started core=%d prio=%d pool=%u",
           xPortGetCoreID(), RT_DATA_RX_PRIORITY, (unsigned)RT_DATA_POOL_SLOTS);

  while (s_rt.running) {
    rt_packet_slot_t *slot = NULL;
    if (xQueueReceive(s_rt.data_free_q, &slot, pdMS_TO_TICKS(20)) != pdTRUE ||
        !slot) {
      s_rt.data_pool_waits++;
      continue;
    }
    slot->pool_kind = RT_POOL_DATA;
    if (!s_rt.running || s_rt.data_sock < 0) {
      release_packet_slot(slot);
      break;
    }

    const ssize_t n = recv(s_rt.data_sock, slot->data, RT_PACKET_MAX, 0);
    if (n <= 0) {
      release_packet_slot(slot);
      if (!s_rt.running) break;
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        s_rt.recv_errors++;
      }
      continue;
    }

    const int64_t rx_us = esp_timer_get_time();
    if (s_rt.last_data_rx_us != 0) {
      const int64_t gap_us64 = rx_us - s_rt.last_data_rx_us;
      if (gap_us64 > 0) {
        const uint32_t gap_us = gap_us64 > UINT32_MAX
                                    ? UINT32_MAX
                                    : (uint32_t)gap_us64;
        if (gap_us > s_rt.interval_max_interarrival_us) {
          s_rt.interval_max_interarrival_us = gap_us;
        }
      }
    }
    s_rt.last_data_rx_us = rx_us;

    slot->retransmitted = false;
    slot->rx_us = rx_us;
    slot->len = (uint16_t)n;
    if (!queue_work_packet(slot)) {
      release_packet_slot(slot);
    }
  }

  s_rt.data_task = NULL;
  vTaskDelete(NULL);
}

static void control_rx_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "CTRL_RX started core=%d prio=%d rtx_pool=%u",
           xPortGetCoreID(), RT_CTRL_RX_PRIORITY, (unsigned)RT_RTX_POOL_SLOTS);
  while (s_rt.running && s_rt.control_sock >= 0) {
    const ssize_t n = recv(s_rt.control_sock, s_rt.control_packet, RT_PACKET_MAX, 0);
    if (n <= 0) {
      if (!s_rt.running) break;
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        s_rt.recv_errors++;
      }
      continue;
    }
    process_control_packet(s_rt.control_packet, (size_t)n);
  }
  s_rt.control_task = NULL;
  vTaskDelete(NULL);
}

static void worker_note_gap(uint32_t previous_ext, uint32_t current_ext,
                            uint32_t current_rtp) {
  if (current_ext <= previous_ext + 1U) return;
  const uint32_t gap32 = current_ext - previous_ext - 1U;
  const uint16_t gap = gap32 > UINT16_MAX ? UINT16_MAX : (uint16_t)gap32;
  const uint32_t frame_samples = s_rt.cfg.format.frame_size > 0
                                     ? (uint32_t)s_rt.cfg.format.frame_size
                                     : 352U;
  const uint32_t first_rtp = current_rtp - (uint32_t)gap * frame_samples;

  s_rt.gap_events++;
  s_rt.missing_packets += gap;
  if (gap > s_rt.interval_max_gap_packets) s_rt.interval_max_gap_packets = gap;
  (void)queue_resend_event(RT_RESEND_EVENT_MISSING, previous_ext + 1U,
                           gap, first_rtp);
}

static void alac_worker_task(void *arg) {
  (void)arg;
  alac_decoder_config_t dcfg = {
      .sample_rate = s_rt.cfg.format.sample_rate,
      .channels = s_rt.cfg.format.channels,
      .bits_per_sample = s_rt.cfg.format.bits_per_sample,
      .frame_size = s_rt.cfg.format.frame_size,
  };
  alac_decoder_t *decoder = alac_decoder_create(&dcfg);
  if (!decoder) {
    ESP_LOGE(TAG, "failed to create ALAC decoder");
    s_rt.running = false;
    s_rt.worker_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  uint32_t first_logs = 0;
  ESP_LOGI(TAG,
           "ALAC_WORK started core=%d prio=%d sr=%d ch=%d frame=%d single_decoder=1",
           xPortGetCoreID(), RT_WORK_PRIORITY, dcfg.sample_rate, dcfg.channels,
           dcfg.frame_size);
  ESP_LOGI(TAG,
           "build=FIX10_SHAIRPORT_ALAC_R23D_COMPACT_DIAG handover=400ms/5s startupHold=1s noArrivalAnchor=1 missing=%u dataPool=%u rtxPool=%u",
           (unsigned)RT_MISSING_SLOTS, (unsigned)RT_DATA_POOL_SLOTS,
           (unsigned)RT_RTX_POOL_SLOTS);

  while (s_rt.running || (s_rt.work_q && uxQueueMessagesWaiting(s_rt.work_q) != 0U)) {
    rt_packet_slot_t *slot = NULL;
    if (xQueueReceive(s_rt.work_q, &slot, pdMS_TO_TICKS(20)) != pdTRUE || !slot) {
      continue;
    }
    if (!s_rt.running) {
      release_packet_slot(slot);
      continue;
    }

    const int64_t work_start_us = esp_timer_get_time();
    const size_t packet_len = slot->len;
    if (packet_len < 12U || (slot->data[0] >> 6) != 2U) {
      s_rt.retransmit_bad++;
      release_packet_slot(slot);
      continue;
    }

    s_rt.rx_packets++;
    const uint16_t seq = read_be16(slot->data + 2);
    const uint32_t rtp = read_be32(slot->data + 4);
    uint32_t ext_seq = seq;
    bool may_clear_missing = slot->retransmitted;

    if (!s_rt.newest_ext_valid) {
      s_rt.newest_ext_seq = seq;
      s_rt.newest_ext_valid = true;
    } else {
      ext_seq = unwrap_seq16(seq, s_rt.newest_ext_seq);
      const uint32_t previous_newest = s_rt.newest_ext_seq;
      if (!slot->retransmitted && ext_seq > previous_newest) {
        worker_note_gap(previous_newest, ext_seq, rtp);
        s_rt.newest_ext_seq = ext_seq;
      } else if (ext_seq <= previous_newest) {
        may_clear_missing = true;
        if (ext_seq < previous_newest) s_rt.reorder_late++;
      }
    }

    if (seen_has(ext_seq)) {
      if (may_clear_missing) {
        (void)queue_resend_event(RT_RESEND_EVENT_RECEIVED, ext_seq, 1U, 0U);
      }
      release_packet_slot(slot);
      continue;
    }

    const bool decoded = decode_audio_packet(decoder, slot->data, packet_len,
                                             slot->retransmitted, &first_logs);
    if (decoded) {
      seen_mark(ext_seq);
      if (may_clear_missing) {
        (void)queue_resend_event(RT_RESEND_EVENT_RECEIVED, ext_seq, 1U, 0U);
      }
    } else {
      /* A packet that arrived but failed decrypt/decode is still a media hole.
       * Ask Apple for it again instead of pretending the sequence was valid. */
      (void)queue_resend_event(RT_RESEND_EVENT_MISSING, ext_seq, 1U, rtp);
    }

    const int64_t processing_us64 = esp_timer_get_time() - slot->rx_us;
    if (processing_us64 >= 0) {
      const uint32_t processing_us = processing_us64 > UINT32_MAX
                                         ? UINT32_MAX
                                         : (uint32_t)processing_us64;
      s_rt.processing_samples++;
      s_rt.processing_sum_us += processing_us;
      if (processing_us > s_rt.interval_max_processing_us) {
        s_rt.interval_max_processing_us = processing_us;
      }
    }
    (void)work_start_us;
    release_packet_slot(slot);
  }

  alac_decoder_destroy(decoder);
  s_rt.worker_task = NULL;
  vTaskDelete(NULL);
}

static void resend_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "RESEND started core=%d prio=%d scan=%ums first=%ums retry=%ums lastReq=%ums finalLoss=%ums",
           xPortGetCoreID(), RT_RESEND_PRIORITY, (unsigned)RT_RESEND_SCAN_MS,
           (unsigned)RT_RESEND_FIRST_MS, (unsigned)RT_RESEND_RETRY_MS,
           (unsigned)RT_RESEND_LAST_REQUEST_MS,
           (unsigned)RT_FINAL_LOSS_MARGIN_MS);
  while (s_rt.running) {
    rt_resend_event_t ev = {0};
    if (xQueueReceive(s_rt.resend_event_q, &ev,
                      pdMS_TO_TICKS(RT_RESEND_SCAN_MS)) == pdTRUE) {
      resend_process_event(&ev);
      while (xQueueReceive(s_rt.resend_event_q, &ev, 0) == pdTRUE) {
        resend_process_event(&ev);
      }
    }
    /* D7 state and the one-shot initial anchor are owned by CTRL_RX. Keeping
     * a single owner removes the former CTRL_RX/RESEND double-commit race. */
    resend_giveup_expired();
    resend_scan_due();

  }

  s_rt.resend_task = NULL;
  vTaskDelete(NULL);
}

static bool all_tasks_stopped(void) {
  return s_rt.data_task == NULL && s_rt.control_task == NULL &&
         s_rt.worker_task == NULL && s_rt.resend_task == NULL;
}

static esp_err_t ensure_transport_resources(void) {
  if (!s_rt.data_pool) {
    s_rt.data_pool = heap_caps_calloc(RT_DATA_POOL_SLOTS, sizeof(*s_rt.data_pool),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.data_pool) s_rt.data_pool = calloc(RT_DATA_POOL_SLOTS, sizeof(*s_rt.data_pool));
  }
  if (!s_rt.rtx_pool) {
    s_rt.rtx_pool = heap_caps_calloc(RT_RTX_POOL_SLOTS, sizeof(*s_rt.rtx_pool),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.rtx_pool) s_rt.rtx_pool = calloc(RT_RTX_POOL_SLOTS, sizeof(*s_rt.rtx_pool));
  }
  if (!s_rt.control_packet) {
    s_rt.control_packet = heap_caps_malloc(RT_PACKET_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.control_packet) s_rt.control_packet = malloc(RT_PACKET_MAX);
  }
  if (!s_rt.decrypt_buf) {
    s_rt.decrypt_buf = heap_caps_malloc(RT_PACKET_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.decrypt_buf) s_rt.decrypt_buf = malloc(RT_PACKET_MAX);
  }
  if (!s_rt.pcm) {
    s_rt.pcm = heap_caps_malloc(RT_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_rt.pcm) s_rt.pcm = malloc(RT_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
  }
  if (!s_rt.seen) {
    s_rt.seen = heap_caps_calloc(RT_SEEN_SLOTS, sizeof(*s_rt.seen),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.seen) s_rt.seen = calloc(RT_SEEN_SLOTS, sizeof(*s_rt.seen));
  }
  if (!s_rt.missing) {
    s_rt.missing = heap_caps_calloc(RT_MISSING_SLOTS, sizeof(*s_rt.missing),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.missing) s_rt.missing = calloc(RT_MISSING_SLOTS, sizeof(*s_rt.missing));
  }

  if (!s_rt.data_free_q) s_rt.data_free_q = xQueueCreate(RT_DATA_POOL_SLOTS, sizeof(rt_packet_slot_t *));
  if (!s_rt.rtx_free_q) s_rt.rtx_free_q = xQueueCreate(RT_RTX_POOL_SLOTS, sizeof(rt_packet_slot_t *));
  if (!s_rt.work_q) s_rt.work_q = xQueueCreate(RT_WORK_QUEUE_SLOTS, sizeof(rt_packet_slot_t *));
  if (!s_rt.resend_event_q)
    s_rt.resend_event_q = xQueueCreate(RT_RESEND_EVENT_SLOTS, sizeof(rt_resend_event_t));

  if (!s_rt.data_pool || !s_rt.rtx_pool || !s_rt.control_packet ||
      !s_rt.decrypt_buf || !s_rt.pcm || !s_rt.seen || !s_rt.missing ||
      !s_rt.data_free_q || !s_rt.rtx_free_q || !s_rt.work_q ||
      !s_rt.resend_event_q) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static void reset_transport_queues(void) {
  xQueueReset(s_rt.data_free_q);
  xQueueReset(s_rt.rtx_free_q);
  xQueueReset(s_rt.work_q);
  xQueueReset(s_rt.resend_event_q);

  for (uint32_t i = 0; i < RT_DATA_POOL_SLOTS; ++i) {
    rt_packet_slot_t *slot = &s_rt.data_pool[i];
    memset(slot, 0, sizeof(*slot));
    slot->pool_kind = RT_POOL_DATA;
    (void)xQueueSend(s_rt.data_free_q, &slot, 0);
  }
  for (uint32_t i = 0; i < RT_RTX_POOL_SLOTS; ++i) {
    rt_packet_slot_t *slot = &s_rt.rtx_pool[i];
    memset(slot, 0, sizeof(*slot));
    slot->pool_kind = RT_POOL_RTX;
    (void)xQueueSend(s_rt.rtx_free_q, &slot, 0);
  }
}

esp_err_t realtime_receiver_start(uint16_t data_port, uint16_t control_port,
                                  const realtime_receiver_config_t *config) {
  if (!config || !config->pcm_sink || data_port == 0 ||
      strcmp(config->format.codec, "ALAC") != 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_rt.running) return ESP_OK;
  if (!all_tasks_stopped()) return ESP_ERR_INVALID_STATE;

  memset(&s_rt.cfg, 0, sizeof(s_rt.cfg));
  s_rt.cfg = *config;
  s_rt.rx_packets = 0;
  s_rt.decoded_packets = 0;
  s_rt.decrypt_errors = 0;
  s_rt.decode_errors = 0;
  s_rt.sink_drops = 0;
  s_rt.client_control_valid = false;
  s_rt.nack_request_seq = 0;
  s_rt.nack_requests = 0;
  s_rt.retransmit_packets = 0;
  s_rt.retransmit_bad = 0;
  s_rt.reorder_late = 0;
  s_rt.reorder_overwrite = 0;
  s_rt.gap_skips = 0;
  s_rt.resend_scans = 0;
  s_rt.resend_retries = 0;
  s_rt.resend_giveups = 0;
  s_rt.hard_resyncs = 0;
  s_rt.gap_events = 0;
  s_rt.missing_packets = 0;
  s_rt.select_errors = 0;
  s_rt.recv_errors = 0;
  s_rt.nack_send_errors = 0;
  s_rt.last_data_rx_us = 0;
  s_rt.processing_samples = 0;
  s_rt.processing_sum_us = 0;
  s_rt.interval_max_processing_us = 0;
  s_rt.interval_max_control_us = 0;
  s_rt.interval_max_interarrival_us = 0;
  s_rt.interval_max_gap_packets = 0;
  s_rt.rtx_latency_samples = 0;
  s_rt.rtx_latency_sum_us = 0;
  s_rt.rtx_latency_min_us = 0;
  s_rt.rtx_latency_max_us = 0;
  s_rt.data_pool_waits = 0;
  s_rt.rtx_pool_drops = 0;
  s_rt.work_queue_drops = 0;
  s_rt.resend_event_drops = 0;
  s_rt.missing_tracker_overflow = 0;
  s_rt.sync_packets = 0;
  s_rt.sync_malformed = 0;
  s_rt.last_sync_flags = 0;
  s_rt.last_sync_rtp_less_latency = 0;
  s_rt.last_sync_rtp = 0;
  s_rt.last_sync_latency_frames = 0;
  s_rt.last_sync_time_seconds = 0;
  s_rt.last_sync_time_fraction = 0;
  s_rt.d7_packets = 0;
  s_rt.d7_malformed = 0;
  s_rt.last_d7_frame1 = 0;
  s_rt.last_d7_frame2 = 0;
  s_rt.last_d7_delta_frames = 0;
  s_rt.last_d7_raw_ptp_ns = 0;
  s_rt.last_d7_ptp_ns = 0;
  s_rt.last_d7_clock_id = 0;
  s_rt.initial_d7_anchor_committed = false;
  __atomic_store_n(&s_rt.d7_anchor_refresh_requested, false, __ATOMIC_RELEASE);
  s_rt.initial_d7_anchor_commit_count = 0;
  s_rt.initial_d7_timeline_ns = 0;
  s_rt.unhandled_control_seen[0] = 0;
  s_rt.unhandled_control_seen[1] = 0;

  esp_err_t err = ensure_transport_resources();
  if (err != ESP_OK) return err;
  reset_transport_tracking();
  reset_transport_queues();

  uint16_t bound_data = 0;
  s_rt.data_sock = socket_utils_bind_udp(data_port, 0, 128 * 1024, &bound_data);
  if (s_rt.data_sock < 0 || bound_data != data_port) {
    realtime_receiver_stop();
    return ESP_FAIL;
  }
  struct timeval tv = {.tv_sec = 0, .tv_usec = RT_SOCKET_TIMEOUT_MS * 1000U};
  (void)setsockopt(s_rt.data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  int actual_rcvbuf = 0;
  socklen_t actual_rcvbuf_len = sizeof(actual_rcvbuf);
  if (getsockopt(s_rt.data_sock, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf,
                 &actual_rcvbuf_len) == 0) {
    ESP_LOGI(TAG, "DATA SO_RCVBUF requested=131072 actual=%d", actual_rcvbuf);
  }

  if (control_port != 0) {
    uint16_t bound_control = 0;
    s_rt.control_sock = socket_utils_bind_udp(control_port, 0, 64 * 1024, &bound_control);
    if (s_rt.control_sock < 0 || bound_control != control_port) {
      realtime_receiver_stop();
      return ESP_FAIL;
    }
    (void)setsockopt(s_rt.control_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  s_rt.running = true;

  if (xTaskCreatePinnedToCore(alac_worker_task, "alac_work", RT_WORK_STACK, NULL,
                              RT_WORK_PRIORITY, &s_rt.worker_task,
                              RT_TASK_CORE) != pdPASS || !s_rt.worker_task) {
    realtime_receiver_stop();
    return ESP_FAIL;
  }
  if (xTaskCreatePinnedToCore(resend_task, "alac_resend", RT_RESEND_STACK, NULL,
                              RT_RESEND_PRIORITY, &s_rt.resend_task,
                              RT_TASK_CORE) != pdPASS || !s_rt.resend_task) {
    realtime_receiver_stop();
    return ESP_FAIL;
  }
  if (s_rt.control_sock >= 0 &&
      (xTaskCreatePinnedToCore(control_rx_task, "alac_ctrl", RT_CTRL_RX_STACK, NULL,
                               RT_CTRL_RX_PRIORITY, &s_rt.control_task,
                               RT_TASK_CORE) != pdPASS || !s_rt.control_task)) {
    realtime_receiver_stop();
    return ESP_FAIL;
  }
  if (xTaskCreatePinnedToCore(data_rx_task, "alac_data", RT_DATA_RX_STACK, NULL,
                              RT_DATA_RX_PRIORITY, &s_rt.data_task,
                              RT_TASK_CORE) != pdPASS || !s_rt.data_task) {
    realtime_receiver_stop();
    return ESP_FAIL;
  }

  ESP_LOGI(TAG,
           "UDP realtime ports data=%u control=%u tasks=DATA/CTRL/WORK/RESEND",
           (unsigned)data_port, (unsigned)control_port);
  return ESP_OK;
}

void realtime_receiver_stop(void) {
  s_rt.running = false;
  if (s_rt.data_sock >= 0) {
    shutdown(s_rt.data_sock, SHUT_RDWR);
    close(s_rt.data_sock);
    s_rt.data_sock = -1;
  }
  if (s_rt.control_sock >= 0) {
    shutdown(s_rt.control_sock, SHUT_RDWR);
    close(s_rt.control_sock);
    s_rt.control_sock = -1;
  }

  for (int i = 0; !all_tasks_stopped() && i < 100; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  ESP_LOGI(TAG,
           "realtime stopped rx=%" PRIu32 " dec=%" PRIu32
           " miss=%" PRIu32 " nack=%" PRIu32 " rtx=%" PRIu32
           " retry=%" PRIu32 " give=%" PRIu32 " resync=%" PRIu32
           " err[pool=%" PRIu32 " work=%" PRIu32 " rtx=%" PRIu32
           " ev=%" PRIu32 " miss=%" PRIu32 "]"
           " rtxLat=%.1f/%.1f/%.1fms(n=%" PRIu32 ")",
           s_rt.rx_packets, s_rt.decoded_packets, s_rt.missing_packets,
           s_rt.nack_requests, s_rt.retransmit_packets, s_rt.resend_retries,
           s_rt.resend_giveups, s_rt.hard_resyncs, s_rt.data_pool_waits,
           s_rt.work_queue_drops, s_rt.rtx_pool_drops,
           s_rt.resend_event_drops, s_rt.missing_tracker_overflow,
           s_rt.rtx_latency_samples ? (double)s_rt.rtx_latency_min_us / 1000.0 : 0.0,
           s_rt.rtx_latency_samples ?
               (double)s_rt.rtx_latency_sum_us / (double)s_rt.rtx_latency_samples / 1000.0 : 0.0,
           s_rt.rtx_latency_samples ? (double)s_rt.rtx_latency_max_us / 1000.0 : 0.0,
           s_rt.rtx_latency_samples);
}

void realtime_receiver_require_fresh_d7_anchor(void) {
  __atomic_store_n(&s_rt.d7_anchor_refresh_requested, true, __ATOMIC_RELEASE);
  ESP_LOGI(TAG, "next matching D7 armed as fresh sender anchor");
}

void realtime_receiver_set_client_control(uint32_t client_ip,
                                          uint16_t client_control_port) {
  if (client_ip == 0 || client_control_port == 0) {
    s_rt.client_control_valid = false;
    return;
  }
  memset(&s_rt.client_control_addr, 0, sizeof(s_rt.client_control_addr));
  s_rt.client_control_addr.sin_family = AF_INET;
  s_rt.client_control_addr.sin_addr.s_addr = client_ip;
  s_rt.client_control_addr.sin_port = htons(client_control_port);
  s_rt.client_control_valid = true;
  ESP_LOGI(TAG, "retransmit target=%s:%u",
           inet_ntoa(s_rt.client_control_addr.sin_addr),
           (unsigned)client_control_port);
}

void realtime_receiver_get_diag(realtime_receiver_diag_t *out,
                                bool reset_interval_maxima) {
  if (!out) {
    return;
  }

  memset(out, 0, sizeof(*out));
  out->rx_packets = s_rt.rx_packets;
  out->gap_events = s_rt.gap_events;
  out->missing_packets = s_rt.missing_packets;
  out->nack_requests = s_rt.nack_requests;
  out->retransmit_packets = s_rt.retransmit_packets;
  out->retransmit_bad = s_rt.retransmit_bad;
  out->reorder_late = s_rt.reorder_late;
  out->reorder_overwrite = s_rt.reorder_overwrite;
  out->gap_skips = s_rt.gap_skips;
  out->resend_retries = s_rt.resend_retries;
  out->resend_giveups = s_rt.resend_giveups;
  out->hard_resyncs = s_rt.hard_resyncs;
  out->select_errors = s_rt.select_errors;
  out->recv_errors = s_rt.recv_errors;
  out->nack_send_errors = s_rt.nack_send_errors;
  out->processing_samples = s_rt.processing_samples;
  out->processing_sum_us = s_rt.processing_sum_us;
  out->interval_max_processing_us = s_rt.interval_max_processing_us;
  out->interval_max_control_us = s_rt.interval_max_control_us;
  out->interval_max_interarrival_us = s_rt.interval_max_interarrival_us;
  out->interval_max_gap_packets = s_rt.interval_max_gap_packets;
  out->rtx_latency_samples = s_rt.rtx_latency_samples;
  out->rtx_latency_sum_us = s_rt.rtx_latency_sum_us;
  out->rtx_latency_min_us = s_rt.rtx_latency_min_us;
  out->rtx_latency_max_us = s_rt.rtx_latency_max_us;
  out->work_queue_depth = s_rt.work_q ? (uint32_t)uxQueueMessagesWaiting(s_rt.work_q) : 0U;
  out->data_pool_waits = s_rt.data_pool_waits;
  out->rtx_pool_drops = s_rt.rtx_pool_drops;
  out->work_queue_drops = s_rt.work_queue_drops;
  out->resend_event_drops = s_rt.resend_event_drops;
  out->missing_tracker_overflow = s_rt.missing_tracker_overflow;
  out->sync_packets = s_rt.sync_packets;
  out->sync_malformed = s_rt.sync_malformed;
  out->last_sync_flags = s_rt.last_sync_flags;
  out->last_sync_rtp_less_latency = s_rt.last_sync_rtp_less_latency;
  out->last_sync_rtp = s_rt.last_sync_rtp;
  out->last_sync_latency_frames = s_rt.last_sync_latency_frames;
  out->last_sync_time_seconds = s_rt.last_sync_time_seconds;
  out->last_sync_time_fraction = s_rt.last_sync_time_fraction;
  out->d7_packets = s_rt.d7_packets;
  out->d7_malformed = s_rt.d7_malformed;
  out->last_d7_frame1 = s_rt.last_d7_frame1;
  out->last_d7_frame2 = s_rt.last_d7_frame2;
  out->last_d7_delta_frames = s_rt.last_d7_delta_frames;
  out->last_d7_ptp_ns = s_rt.last_d7_ptp_ns;
  out->last_d7_clock_id = s_rt.last_d7_clock_id;

  if (reset_interval_maxima) {
    s_rt.interval_max_processing_us = 0;
    s_rt.interval_max_control_us = 0;
    s_rt.interval_max_interarrival_us = 0;
    s_rt.interval_max_gap_packets = 0;
  }
}

bool realtime_receiver_is_running(void) { return s_rt.running; }
