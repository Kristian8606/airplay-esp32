#include "realtime_receiver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "alac_decoder.h"
#include "audio_crypto.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network/socket_utils.h"

#define RT_PACKET_MAX          8192U
#define RT_PCM_CAPACITY_FRAMES 4096U
#define RT_RX_STACK            7168U
#define RT_RX_PRIORITY         7
#define RT_RX_CORE             0
#define RT_FIRST_PACKET_LOGS   5U
#define RT_SYNC_PT              84U
#define RT_RETRANSMIT_PT        86U
#define RT_RESEND_REQUEST_PT    85U
#define RT_MAX_NACK_COUNT       32U
#define RT_REORDER_SLOTS         64U
#define RT_RESEND_FIRST_MS         0U
#define RT_RESEND_RETRY_MS        35U
#define RT_RESEND_SAFETY_MS       50U
#define RT_RESEND_SCAN_MS         10U
#define RT_CONTROL_BUDGET_US     6000U
#define RT_CONTROL_MAX_PACKETS      4U
#define RT_NACK_TRACK_SLOTS      128U

static const char *TAG = "airplay_rt";

typedef struct {
  bool valid;
  bool missing;
  uint16_t seq;
  uint32_t missing_rtp;
  TickType_t missing_since;
  TickType_t last_nack;
  uint8_t nack_count;
} reorder_slot_t;

typedef struct {
  bool valid;
  uint16_t seq;
  int64_t sent_us;
} nack_track_t;

typedef struct {
  realtime_receiver_config_t cfg;
  int data_sock;
  int control_sock;
  volatile bool running;
  TaskHandle_t task;
  uint8_t *packet;
  uint8_t *decrypt_buf;
  int16_t *pcm;
  uint32_t rx_packets;
  uint32_t decoded_packets;
  uint32_t decrypt_errors;
  uint32_t decode_errors;
  uint32_t sink_drops;
  uint16_t last_seq;
  bool last_seq_valid;
  struct sockaddr_in client_control_addr;
  bool client_control_valid;
  uint16_t nack_request_seq;
  uint32_t nack_requests;
  uint32_t retransmit_packets;
  uint32_t retransmit_bad;
  reorder_slot_t *reorder;
  uint16_t expected_seq;
  bool expected_valid;
  uint16_t newest_seq;
  bool newest_valid;
  uint32_t reorder_late;
  uint32_t reorder_overwrite;
  uint32_t gap_skips;
  uint32_t resend_scans;
  uint32_t resend_retries;
  uint32_t resend_giveups;
  uint32_t hard_resyncs;
  uint32_t gap_events;
  uint32_t missing_packets;
  uint32_t select_errors;
  uint32_t recv_errors;
  uint32_t nack_send_errors;
  int64_t last_data_rx_us;
  uint32_t processing_samples;
  uint64_t processing_sum_us;
  uint32_t interval_max_processing_us;
  uint32_t interval_max_control_us;
  uint32_t interval_max_interarrival_us;
  uint16_t interval_max_gap_packets;
  nack_track_t nack_track[RT_NACK_TRACK_SLOTS];
  uint32_t rtx_latency_samples;
  uint64_t rtx_latency_sum_us;
  uint32_t rtx_latency_min_us;
  uint32_t rtx_latency_max_us;

  /* Passive PT=84 source-timeline observation. Never used to steer playout. */
  uint32_t sync_packets;
  uint32_t sync_malformed;
  uint16_t last_sync_flags;
  uint32_t last_sync_rtp_less_latency;
  uint32_t last_sync_rtp;
  uint32_t last_sync_latency_frames;
  uint32_t last_sync_time_seconds;
  uint32_t last_sync_time_fraction;
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

static bool send_retransmit_request(uint16_t first_missing, uint16_t count) {
  if (!s_rt.client_control_valid || s_rt.control_sock < 0 || count == 0) {
    return false;
  }
  if (count > RT_MAX_NACK_COUNT) {
    count = RT_MAX_NACK_COUNT;
  }

  /* AirTunes retransmit request (PT=85):
   *   0: 0x80
   *   1: 0xD5 (marker + PT 85)
   *   2..3: request sequence
   *   4..5: first missing audio sequence
   *   6..7: number of packets requested
   */
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
  ssize_t n = sendto(s_rt.control_sock, req, sizeof(req), 0,
                     (const struct sockaddr *)&s_rt.client_control_addr,
                     sizeof(s_rt.client_control_addr));
  if (n == (ssize_t)sizeof(req)) {
    /* Track the send time per requested RTP sequence. This is diagnostic
     * only; it does not affect retransmit/reorder behavior. */
    for (uint16_t i = 0; i < count; ++i) {
      const uint16_t seq = (uint16_t)(first_missing + i);
      nack_track_t *t = &s_rt.nack_track[(uint32_t)seq % RT_NACK_TRACK_SLOTS];
      t->valid = true;
      t->seq = seq;
      t->sent_us = sent_us;
    }
    s_rt.nack_requests++;
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


static void reorder_clear_state(void) {
  if (s_rt.reorder) {
    memset(s_rt.reorder, 0, RT_REORDER_SLOTS * sizeof(*s_rt.reorder));
  }
  memset(s_rt.nack_track, 0, sizeof(s_rt.nack_track));
  s_rt.expected_seq = 0;
  s_rt.expected_valid = false;
  s_rt.newest_seq = 0;
  s_rt.newest_valid = false;
}

static void reorder_hard_resync(uint16_t got_seq, uint16_t missing) {
  const uint16_t old_expected = s_rt.expected_seq;
  reorder_clear_state();
  s_rt.expected_seq = got_seq;
  s_rt.expected_valid = true;
  s_rt.hard_resyncs++;

  ESP_LOGW(TAG,
           "RTP hard resync #%" PRIu32
           " expected=%u got=%u missing=%u window=%u",
           s_rt.hard_resyncs, old_expected, got_seq, missing,
           (unsigned)RT_REORDER_SLOTS);

  if (s_rt.cfg.resync_cb) {
    s_rt.cfg.resync_cb(s_rt.cfg.resync_ctx);
  }
}

static reorder_slot_t *reorder_slot_for(uint16_t seq) {
  return &s_rt.reorder[(uint32_t)seq % RT_REORDER_SLOTS];
}

static bool reorder_has(uint16_t seq) {
  reorder_slot_t *slot = reorder_slot_for(seq);
  return slot->valid && slot->seq == seq;
}

static bool reorder_is_missing(uint16_t seq) {
  reorder_slot_t *slot = reorder_slot_for(seq);
  return slot->missing && slot->seq == seq && !slot->valid;
}

static void reorder_mark_missing(uint16_t seq, uint32_t rtp, TickType_t now) {
  reorder_slot_t *slot = reorder_slot_for(seq);
  if (slot->valid && slot->seq == seq) {
    return;
  }
  if (slot->seq != seq) {
    memset(slot, 0, sizeof(*slot));
    slot->seq = seq;
  }
  if (!slot->missing) {
    slot->missing = true;
    slot->missing_rtp = rtp;
    slot->missing_since = now;
    slot->last_nack = 0;
    slot->nack_count = 0;
  }
}

static bool reorder_store(const uint8_t *packet, size_t packet_len,
                          bool retransmitted, bool *should_decode) {
  (void)retransmitted;
  if (should_decode) *should_decode = false;
  if (!packet || packet_len < 12U) return false;

  const uint16_t seq =
      (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);

  /* expected_seq is only the oldest unresolved sequence in the 64-packet
   * loss/NACK window. Audio bytes are never queued here: every accepted
   * normal or retransmitted ALAC packet is decoded immediately and the RTP
   * PCM ring performs the actual out-of-order placement. */
  if (s_rt.expected_valid && (int16_t)(seq - s_rt.expected_seq) < 0) {
    s_rt.reorder_late++;

    /* The transport frontier may have advanced because we stopped requesting
     * this hole at the resend safety margin. A retransmit that was already in
     * flight can still be useful until the chronological ALAC staging cursor
     * has actually consumed that RTP range. Decode old RTX packets and let
     * realtime_pcm_sink() make that final RTP/cursor decision. Normal old/
     * duplicate data packets remain ignored. */
    if (retransmitted && should_decode) {
      *should_decode = true;
    }
    return true;
  }

  reorder_slot_t *slot = reorder_slot_for(seq);
  if (slot->valid && slot->seq != seq) s_rt.reorder_overwrite++;
  if (slot->valid && slot->seq == seq) return true;

  slot->seq = seq;
  slot->valid = true;
  slot->missing = false;
  slot->missing_rtp = 0;
  slot->missing_since = 0;
  slot->last_nack = 0;
  slot->nack_count = 0;

  if (!s_rt.newest_valid || (int16_t)(seq - s_rt.newest_seq) > 0) {
    s_rt.newest_seq = seq;
    s_rt.newest_valid = true;
  }
  if (should_decode) *should_decode = true;
  return true;
}

static void reorder_advance_frontier(void) {
  if (!s_rt.expected_valid) return;
  while (reorder_has(s_rt.expected_seq)) {
    reorder_slot_t *slot = reorder_slot_for(s_rt.expected_seq);
    memset(slot, 0, sizeof(*slot));
    s_rt.expected_seq = (uint16_t)(s_rt.expected_seq + 1U);
  }
}

static void reorder_note_gap(uint16_t got_seq, uint32_t got_rtp) {
  if (!s_rt.expected_valid) {
    s_rt.expected_seq = got_seq;
    s_rt.expected_valid = true;
    return;
  }
  const int16_t delta = (int16_t)(got_seq - s_rt.expected_seq);
  if (delta <= 0) {
    return;
  }
  const uint16_t missing = (uint16_t)delta;

  /* The reorder store is modulo RT_REORDER_SLOTS. Once the sender is one
   * complete window ahead, retaining expected_seq would make new packets
   * overwrite the very old slots we are still waiting for and recovery can
   * never converge. Treat that as a new realtime epoch instead. */
  if (missing >= RT_REORDER_SLOTS) {
    reorder_hard_resync(got_seq, missing);
    return;
  }

  const TickType_t now = xTaskGetTickCount();
  const uint32_t frame_samples =
      s_rt.cfg.format.frame_size > 0 ? (uint32_t)s_rt.cfg.format.frame_size : 352U;
  uint16_t newly_missing = 0;
  for (uint16_t d = 0; d < missing; ++d) {
    const uint16_t seq = (uint16_t)(s_rt.expected_seq + d);
    if (!reorder_has(seq) && !reorder_is_missing(seq)) {
      const uint16_t packets_before_got = (uint16_t)(missing - d);
      const uint32_t missing_rtp =
          got_rtp - (uint32_t)packets_before_got * frame_samples;
      reorder_mark_missing(seq, missing_rtp, now);
      newly_missing++;
    }
  }

  if (newly_missing != 0U) {
    s_rt.gap_events++;
    s_rt.missing_packets += newly_missing;
    if (missing > s_rt.interval_max_gap_packets) {
      s_rt.interval_max_gap_packets = missing;
    }
  }
}

static bool resend_time_to_play_us(const reorder_slot_t *slot,
                                   int64_t *out_us) {
  if (!slot || !out_us || !s_rt.cfg.deadline_cb) {
    return false;
  }
  return s_rt.cfg.deadline_cb(slot->missing_rtp, out_us,
                              s_rt.cfg.deadline_ctx);
}

static bool resend_slot_due(const reorder_slot_t *slot, TickType_t now) {
  if (!slot || !slot->missing || slot->valid) {
    return false;
  }

  const TickType_t age = now - slot->missing_since;
  if (age < pdMS_TO_TICKS(RT_RESEND_FIRST_MS)) {
    return false;
  }

  int64_t time_to_play_us = 0;
  const bool deadline_valid = resend_time_to_play_us(slot, &time_to_play_us);
  if (deadline_valid &&
      time_to_play_us <= (int64_t)RT_RESEND_SAFETY_MS * 1000LL) {
    return false;
  }

  if (slot->nack_count == 0U) {
    return true;
  }

  /* Before the first valid realtime anchor, allow one request to avoid
   * deadlocking expected_seq on an early hole, but do not keep retrying an
   * audio range whose playout deadline is not known yet. */
  if (!deadline_valid) {
    return false;
  }

  return (now - slot->last_nack) >= pdMS_TO_TICKS(RT_RESEND_RETRY_MS);
}

static void reorder_resend_scan(void) {
  if (!s_rt.expected_valid || !s_rt.newest_valid) {
    return;
  }

  const TickType_t now = xTaskGetTickCount();
  s_rt.resend_scans++;

  uint16_t seq = s_rt.expected_seq;
  const uint16_t span = (uint16_t)((int16_t)(s_rt.newest_seq - s_rt.expected_seq) + 1);
  for (uint16_t walked = 0; walked < span && walked < RT_REORDER_SLOTS;) {
    reorder_slot_t *slot = reorder_slot_for(seq);
    if (!reorder_is_missing(seq) || !resend_slot_due(slot, now)) {
      seq = (uint16_t)(seq + 1U);
      walked++;
      continue;
    }

    const uint16_t first = seq;
    uint16_t count = 0;
    while (walked + count < span && count < RT_MAX_NACK_COUNT &&
           count < RT_REORDER_SLOTS) {
      const uint16_t candidate = (uint16_t)(first + count);
      reorder_slot_t *candidate_slot = reorder_slot_for(candidate);
      if (!reorder_is_missing(candidate) ||
          !resend_slot_due(candidate_slot, now)) {
        break;
      }
      count++;
    }

    if (count != 0U && send_retransmit_request(first, count)) {
      for (uint16_t i = 0; i < count; ++i) {
        reorder_slot_t *requested = reorder_slot_for((uint16_t)(first + i));
        if (requested->nack_count != 0U) {
          s_rt.resend_retries++;
        }
        requested->last_nack = now;
        if (requested->nack_count != UINT8_MAX) {
          requested->nack_count++;
        }
      }
    }

    seq = (uint16_t)(first + (count ? count : 1U));
    walked = (uint16_t)(walked + (count ? count : 1U));
  }
}

static void reorder_giveup_expired(void) {
  if (!s_rt.expected_valid) {
    return;
  }

  uint16_t skipped = 0;
  const uint16_t first = s_rt.expected_seq;
  int64_t first_deadline_us = 0;

  while (skipped < RT_REORDER_SLOTS && reorder_is_missing(s_rt.expected_seq)) {
    reorder_slot_t *slot = reorder_slot_for(s_rt.expected_seq);
    int64_t time_to_play_us = 0;

    /* No anchor yet: keep the hole buffered. A single bootstrap NACK may have
     * been sent by resend_slot_due(), but there is no safe basis for dropping
     * audio until RTP has a physical PTP playout deadline. */
    if (!resend_time_to_play_us(slot, &time_to_play_us)) {
      break;
    }
    if (time_to_play_us > (int64_t)RT_RESEND_SAFETY_MS * 1000LL) {
      break;
    }
    if (skipped == 0U) {
      first_deadline_us = time_to_play_us;
    }

    memset(slot, 0, sizeof(*slot));
    s_rt.expected_seq = (uint16_t)(s_rt.expected_seq + 1U);
    skipped++;
  }

  if (skipped != 0U) {
    s_rt.gap_skips += skipped;
    s_rt.resend_giveups += skipped;
    (void)first;
    (void)first_deadline_us;
    reorder_advance_frontier();
  }
}

static void reorder_service(void) {
  /* Drop holes that cannot make their playout deadline before requesting
   * anything else. The tracker no longer gates audio decode. */
  reorder_giveup_expired();
  reorder_resend_scan();
}

static void note_retransmit_latency(const uint8_t *packet, size_t packet_len) {
  if (!packet || packet_len < 4U) {
    return;
  }
  const uint16_t seq =
      (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);
  nack_track_t *t = &s_rt.nack_track[(uint32_t)seq % RT_NACK_TRACK_SLOTS];
  if (!t->valid || t->seq != seq) {
    return;
  }

  int64_t delta_us = esp_timer_get_time() - t->sent_us;
  t->valid = false;
  if (delta_us < 0) {
    return;
  }
  uint32_t us = delta_us > UINT32_MAX ? UINT32_MAX : (uint32_t)delta_us;
  if (s_rt.rtx_latency_samples == 0U || us < s_rt.rtx_latency_min_us) {
    s_rt.rtx_latency_min_us = us;
  }
  if (us > s_rt.rtx_latency_max_us) {
    s_rt.rtx_latency_max_us = us;
  }
  s_rt.rtx_latency_sum_us += us;
  s_rt.rtx_latency_samples++;

}

static void handle_control_socket(alac_decoder_t *decoder,
                                  uint32_t *first_logs) {
  uint8_t *buf = s_rt.packet;
  if (!buf) return;

  /* Never let a retransmit burst monopolize the realtime receiver. Service a
   * small bounded amount of control traffic, then return to select() so live
   * UDP data gets another chance immediately. Remaining RTX packets stay in
   * the socket and are picked up on the next pass. */
  const int64_t budget_start_us = esp_timer_get_time();
  uint32_t processed = 0;
  while (s_rt.control_sock >= 0) {
    if (processed >= RT_CONTROL_MAX_PACKETS ||
        esp_timer_get_time() - budget_start_us >= RT_CONTROL_BUDGET_US) {
      break;
    }

    ssize_t n = recv(s_rt.control_sock, buf, RT_PACKET_MAX, MSG_DONTWAIT);
    if (n <= 0) break;
    processed++;
    if (n < 4) continue;

    const uint8_t pt = buf[1] & 0x7fU;
    if (pt == RT_RETRANSMIT_PT) {
      if (n > 4) note_retransmit_latency(buf + 4, (size_t)n - 4U);

      bool should_decode = false;
      if (n <= 4 ||
          !reorder_store(buf + 4, (size_t)n - 4U, true, &should_decode)) {
        s_rt.retransmit_bad++;
      } else {
        if (should_decode) {
          (void)decode_audio_packet(decoder, buf + 4, (size_t)n - 4U, true,
                                    first_logs);
        }
        reorder_advance_frontier();
      }
      continue;
    }

    if (pt == RT_SYNC_PT) {
      /* AirPlay sync packet (marker + PT=84). Shairport Sync documents the
       * classic/realtime layout as:
       *   2..3   flags
       *   4..7   RTP timestamp less latency
       *   8..15  source/network time in 32.32 fixed-point seconds
       *   16..19 RTP timestamp before subtracting latency
       *
       * Decode it for diagnostics only. The local realtime anchor remains the
       * sole playout input in this test build. */
      if (n < 20) {
        s_rt.sync_malformed++;
        continue;
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
      continue;
    }

    /* Other control payload types remain intentionally ignored. */
  }
}

static void realtime_task(void *arg) {
  (void)arg;
  alac_decoder_t *decoder = NULL;
  uint32_t first_logs = 0;

  alac_decoder_config_t dcfg = {
      .sample_rate = s_rt.cfg.format.sample_rate,
      .channels = s_rt.cfg.format.channels,
      .bits_per_sample = s_rt.cfg.format.bits_per_sample,
      .frame_size = s_rt.cfg.format.frame_size,
  };
  decoder = alac_decoder_create(&dcfg);
  if (!decoder) {
    ESP_LOGE(TAG, "failed to create ALAC decoder");
    s_rt.running = false;
    s_rt.task = NULL;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "realtime ALAC receiver started core=%d sr=%d ch=%d frame=%d",
           xPortGetCoreID(), dcfg.sample_rate, dcfg.channels, dcfg.frame_size);

  while (s_rt.running) {
    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;
    if (s_rt.data_sock >= 0) {
      FD_SET(s_rt.data_sock, &rfds);
      if (s_rt.data_sock > maxfd) maxfd = s_rt.data_sock;
    }
    if (s_rt.control_sock >= 0) {
      FD_SET(s_rt.control_sock, &rfds);
      if (s_rt.control_sock > maxfd) maxfd = s_rt.control_sock;
    }
    if (maxfd < 0) {
      break;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = RT_RESEND_SCAN_MS * 1000U};
    int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (!s_rt.running) {
      break;
    }
    if (ready < 0) {
      if (errno != EINTR && s_rt.running) {
        s_rt.select_errors++;
      }
      continue;
    }
    if (ready == 0) {
      reorder_service();
      continue;
    }

    if (s_rt.control_sock >= 0 && FD_ISSET(s_rt.control_sock, &rfds)) {
      const int64_t control_start_us = esp_timer_get_time();
      handle_control_socket(decoder, &first_logs);
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

    if (s_rt.data_sock < 0 || !FD_ISSET(s_rt.data_sock, &rfds)) {
      continue;
    }

    ssize_t n = recv(s_rt.data_sock, s_rt.packet, RT_PACKET_MAX, 0);
    if (n <= 0) {
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        s_rt.recv_errors++;
      }
      continue;
    }

    const int64_t packet_rx_us = esp_timer_get_time();
    if (s_rt.last_data_rx_us != 0) {
      const int64_t gap_us64 = packet_rx_us - s_rt.last_data_rx_us;
      if (gap_us64 > 0) {
        const uint32_t gap_us = gap_us64 > UINT32_MAX ? UINT32_MAX : (uint32_t)gap_us64;
        if (gap_us > s_rt.interval_max_interarrival_us) {
          s_rt.interval_max_interarrival_us = gap_us;
        }
      }
    }
    s_rt.last_data_rx_us = packet_rx_us;

    const size_t packet_len = (size_t)n;
    if (packet_len >= 12U) {
      const uint16_t seq =
          (uint16_t)(((uint16_t)s_rt.packet[2] << 8) | s_rt.packet[3]);
      const uint32_t rtp = ((uint32_t)s_rt.packet[4] << 24) |
                           ((uint32_t)s_rt.packet[5] << 16) |
                           ((uint32_t)s_rt.packet[6] << 8) |
                           (uint32_t)s_rt.packet[7];
      s_rt.rx_packets++;
      if (!s_rt.expected_valid) {
        s_rt.expected_seq = seq;
        s_rt.expected_valid = true;
      }
      if ((int16_t)(seq - s_rt.expected_seq) > 0) {
        reorder_note_gap(seq, rtp);
      }
      bool should_decode = false;
      if (!reorder_store(s_rt.packet, packet_len, false, &should_decode)) {
        s_rt.retransmit_bad++;
      } else if (should_decode) {
        /* Decode every live packet immediately. Missing earlier sequence
         * numbers remain only in the NACK/deadline tracker; PCM placement is
         * by RTP timestamp in the common ring. */
        (void)decode_audio_packet(decoder, s_rt.packet, packet_len, false,
                                  &first_logs);
      }
      reorder_advance_frontier();
      reorder_service();

      const int64_t processing_us64 = esp_timer_get_time() - packet_rx_us;
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
    }
  }

  alac_decoder_destroy(decoder);
  ESP_LOGI(TAG,
           "realtime receiver stopped rx=%" PRIu32 " dec=%" PRIu32
           " decrypt_err=%" PRIu32 " decode_err=%" PRIu32
           " sink_drop=%" PRIu32 " nack=%" PRIu32 " rtx=%" PRIu32
           " rtx_bad=%" PRIu32 " late=%" PRIu32 " skip=%" PRIu32
           " retry=%" PRIu32 " giveup=%" PRIu32 " resync=%" PRIu32
           " pt84=%" PRIu32 "/bad=%" PRIu32
           " rtx_lat=%.2f/%.2f/%.2fms(n=%" PRIu32 ")",
           s_rt.rx_packets, s_rt.decoded_packets, s_rt.decrypt_errors,
           s_rt.decode_errors, s_rt.sink_drops, s_rt.nack_requests,
           s_rt.retransmit_packets, s_rt.retransmit_bad,
           s_rt.reorder_late, s_rt.gap_skips, s_rt.resend_retries,
           s_rt.resend_giveups, s_rt.hard_resyncs,
           s_rt.sync_packets, s_rt.sync_malformed,
           s_rt.rtx_latency_samples ? (double)s_rt.rtx_latency_min_us / 1000.0 : 0.0,
           s_rt.rtx_latency_samples ? ((double)s_rt.rtx_latency_sum_us /
                                       (double)s_rt.rtx_latency_samples) / 1000.0 : 0.0,
           s_rt.rtx_latency_samples ? (double)s_rt.rtx_latency_max_us / 1000.0 : 0.0,
           s_rt.rtx_latency_samples);
  s_rt.task = NULL;
  vTaskDelete(NULL);
}

esp_err_t realtime_receiver_start(uint16_t data_port, uint16_t control_port,
                                  const realtime_receiver_config_t *config) {
  if (!config || !config->pcm_sink || data_port == 0 ||
      strcmp(config->format.codec, "ALAC") != 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_rt.running) {
    return ESP_OK;
  }
  if (s_rt.task) {
    return ESP_ERR_INVALID_STATE;
  }

  memset(&s_rt.cfg, 0, sizeof(s_rt.cfg));
  s_rt.cfg = *config;
  s_rt.rx_packets = 0;
  s_rt.decoded_packets = 0;
  s_rt.decrypt_errors = 0;
  s_rt.decode_errors = 0;
  s_rt.sink_drops = 0;
  s_rt.last_seq_valid = false;
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
  reorder_clear_state();
  s_rt.rtx_latency_samples = 0;
  s_rt.rtx_latency_sum_us = 0;
  s_rt.rtx_latency_min_us = 0;
  s_rt.rtx_latency_max_us = 0;
  s_rt.sync_packets = 0;
  s_rt.sync_malformed = 0;
  s_rt.last_sync_flags = 0;
  s_rt.last_sync_rtp_less_latency = 0;
  s_rt.last_sync_rtp = 0;
  s_rt.last_sync_latency_frames = 0;
  s_rt.last_sync_time_seconds = 0;
  s_rt.last_sync_time_fraction = 0;

  if (!s_rt.packet) {
    s_rt.packet = heap_caps_malloc(RT_PACKET_MAX,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.packet) s_rt.packet = malloc(RT_PACKET_MAX);
  }
  if (!s_rt.decrypt_buf) {
    s_rt.decrypt_buf = heap_caps_malloc(RT_PACKET_MAX,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.decrypt_buf) s_rt.decrypt_buf = malloc(RT_PACKET_MAX);
  }
  if (!s_rt.reorder) {
    s_rt.reorder = heap_caps_calloc(RT_REORDER_SLOTS, sizeof(*s_rt.reorder),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.reorder) {
      s_rt.reorder = calloc(RT_REORDER_SLOTS, sizeof(*s_rt.reorder));
    }
  }
  if (!s_rt.pcm) {
    s_rt.pcm = heap_caps_malloc(RT_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_rt.pcm) {
      s_rt.pcm = malloc(RT_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
    }
  }
  if (!s_rt.packet || !s_rt.decrypt_buf || !s_rt.pcm || !s_rt.reorder) {
    return ESP_ERR_NO_MEM;
  }
  memset(s_rt.reorder, 0, RT_REORDER_SLOTS * sizeof(*s_rt.reorder));

  uint16_t bound_data = 0;
  s_rt.data_sock = socket_utils_bind_udp(data_port, 0, 64 * 1024, &bound_data);
  if (s_rt.data_sock < 0 || bound_data != data_port) {
    realtime_receiver_stop();
    return ESP_FAIL;
  }

  if (control_port != 0) {
    uint16_t bound_control = 0;
    s_rt.control_sock =
        socket_utils_bind_udp(control_port, 0, 8 * 1024, &bound_control);
    if (s_rt.control_sock < 0 || bound_control != control_port) {
      realtime_receiver_stop();
      return ESP_FAIL;
    }
  }

  s_rt.running = true;
  if (xTaskCreatePinnedToCore(realtime_task, "airplay_rt", RT_RX_STACK, NULL,
                              RT_RX_PRIORITY, &s_rt.task,
                              RT_RX_CORE) != pdPASS) {
    s_rt.running = false;
    realtime_receiver_stop();
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "UDP realtime ports data=%u control=%u",
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
  for (int i = 0; s_rt.task != NULL && i < 50; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
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
                                bool reset_interval_peaks) {
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
  out->sync_packets = s_rt.sync_packets;
  out->sync_malformed = s_rt.sync_malformed;
  out->last_sync_flags = s_rt.last_sync_flags;
  out->last_sync_rtp_less_latency = s_rt.last_sync_rtp_less_latency;
  out->last_sync_rtp = s_rt.last_sync_rtp;
  out->last_sync_latency_frames = s_rt.last_sync_latency_frames;
  out->last_sync_time_seconds = s_rt.last_sync_time_seconds;
  out->last_sync_time_fraction = s_rt.last_sync_time_fraction;

  if (reset_interval_peaks) {
    s_rt.interval_max_processing_us = 0;
    s_rt.interval_max_control_us = 0;
    s_rt.interval_max_interarrival_us = 0;
    s_rt.interval_max_gap_packets = 0;
  }
}

bool realtime_receiver_is_running(void) { return s_rt.running; }
