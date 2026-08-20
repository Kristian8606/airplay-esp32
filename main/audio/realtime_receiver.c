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
#define RT_RETRANSMIT_PT        86U
#define RT_RESEND_REQUEST_PT    85U
#define RT_MAX_NACK_COUNT       32U
#define RT_REORDER_SLOTS         64U
#define RT_REORDER_PACKET_MAX    2048U
#define RT_GAP_WAIT_MS          140U
#define RT_NACK_TRACK_SLOTS      128U
#define RT_LATENCY_LOGS           20U

static const char *TAG = "airplay_rt";

typedef struct {
  bool valid;
  uint16_t seq;
  uint16_t len;
  bool retransmitted;
  uint8_t data[RT_REORDER_PACKET_MAX];
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
  TickType_t gap_since;
  bool gap_waiting;
  uint32_t reorder_late;
  uint32_t reorder_overwrite;
  uint32_t gap_skips;
  nack_track_t nack_track[RT_NACK_TRACK_SLOTS];
  uint32_t rtx_latency_samples;
  uint64_t rtx_latency_sum_us;
  uint32_t rtx_latency_min_us;
  uint32_t rtx_latency_max_us;
} realtime_state_t;

static realtime_state_t s_rt = {
    .data_sock = -1,
    .control_sock = -1,
};

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
    if (s_rt.nack_requests <= 10U) {
      ESP_LOGI(TAG, "NACK #%" PRIu32 " first=%u count=%u",
               s_rt.nack_requests, first_missing, count);
    }
    return true;
  }

  if (s_rt.nack_requests < 10U) {
    ESP_LOGW(TAG, "NACK send failed first=%u count=%u errno=%d",
             first_missing, count, errno);
  }
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
    if (s_rt.retransmit_packets <= 10U) {
      ESP_LOGI(TAG, "RETRANSMIT #%" PRIu32 " seq=%u rtp=%" PRIu32,
               s_rt.retransmit_packets, seq, rtp);
    }
  } else if (*first_logs < RT_FIRST_PACKET_LOGS) {
    (*first_logs)++;
    ESP_LOGI(TAG,
             "RTP+ALAC OK #%u seq=%u rtp=%" PRIu32
             " packet=%u alac=%d pcm_frames=%d ch=%d",
             (unsigned)*first_logs, seq, rtp, (unsigned)packet_len, dec_len,
             frames, info.channels);
  }
  return true;
}

static reorder_slot_t *reorder_slot_for(uint16_t seq) {
  return &s_rt.reorder[(uint32_t)seq % RT_REORDER_SLOTS];
}

static bool reorder_has(uint16_t seq) {
  reorder_slot_t *slot = reorder_slot_for(seq);
  return slot->valid && slot->seq == seq;
}

static bool reorder_store(const uint8_t *packet, size_t packet_len,
                          bool retransmitted) {
  if (!packet || packet_len < 12U || packet_len > RT_REORDER_PACKET_MAX) {
    return false;
  }
  const uint16_t seq =
      (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);

  if (s_rt.expected_valid && (int16_t)(seq - s_rt.expected_seq) < 0) {
    s_rt.reorder_late++;
    return true; /* Already played/decoded. */
  }

  reorder_slot_t *slot = reorder_slot_for(seq);
  if (slot->valid && slot->seq != seq) {
    s_rt.reorder_overwrite++;
    ESP_LOGW(TAG, "reorder overflow seq=%u replacing=%u", seq, slot->seq);
  }
  if (slot->valid && slot->seq == seq) {
    return true; /* Duplicate original/retransmit. */
  }

  memcpy(slot->data, packet, packet_len);
  slot->len = (uint16_t)packet_len;
  slot->seq = seq;
  slot->retransmitted = retransmitted;
  slot->valid = true;
  return true;
}

static void reorder_drain(alac_decoder_t *decoder, uint32_t *first_logs) {
  if (!s_rt.expected_valid) {
    return;
  }
  while (reorder_has(s_rt.expected_seq)) {
    reorder_slot_t *slot = reorder_slot_for(s_rt.expected_seq);
    const uint16_t seq = s_rt.expected_seq;
    (void)decode_audio_packet(decoder, slot->data, slot->len,
                              slot->retransmitted, first_logs);
    slot->valid = false;
    s_rt.expected_seq = (uint16_t)(seq + 1U);
    s_rt.gap_waiting = false;
  }
}

static void reorder_note_gap(uint16_t got_seq) {
  if (!s_rt.expected_valid) {
    s_rt.expected_seq = got_seq;
    s_rt.expected_valid = true;
    return;
  }
  const int16_t delta = (int16_t)(got_seq - s_rt.expected_seq);
  if (delta <= 0) {
    return;
  }
  if (s_rt.gap_waiting) {
    return;
  }
  uint16_t missing = (uint16_t)delta;
  ESP_LOGW(TAG, "RTP sequence gap expected=%u got=%u missing=%u",
           s_rt.expected_seq, got_seq, missing);
  send_retransmit_request(s_rt.expected_seq, missing);
  s_rt.gap_waiting = true;
  s_rt.gap_since = xTaskGetTickCount();
}

static void reorder_timeout_skip(alac_decoder_t *decoder,
                                 uint32_t *first_logs) {
  if (!s_rt.expected_valid || !s_rt.gap_waiting ||
      reorder_has(s_rt.expected_seq)) {
    return;
  }
  if ((xTaskGetTickCount() - s_rt.gap_since) < pdMS_TO_TICKS(RT_GAP_WAIT_MS)) {
    return;
  }

  /* Recovery did not arrive in time. Skip only up to the nearest buffered
   * sequence so realtime playback cannot deadlock forever. */
  for (uint16_t d = 1; d < RT_REORDER_SLOTS; ++d) {
    uint16_t candidate = (uint16_t)(s_rt.expected_seq + d);
    if (reorder_has(candidate)) {
      ESP_LOGW(TAG, "retransmit timeout; skipping %u packet(s) from seq=%u",
               d, s_rt.expected_seq);
      s_rt.gap_skips += d;
      s_rt.expected_seq = candidate;
      s_rt.gap_waiting = false;
      reorder_drain(decoder, first_logs);
      return;
    }
  }
  s_rt.gap_since = xTaskGetTickCount();
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

  if (s_rt.rtx_latency_samples <= RT_LATENCY_LOGS) {
    ESP_LOGI(TAG, "RTX latency #%" PRIu32 " seq=%u %.2f ms",
             s_rt.rtx_latency_samples, seq, (double)us / 1000.0);
  }
}

static void handle_control_socket(alac_decoder_t *decoder,
                                  uint32_t *first_logs) {
  /* Do not put RT_PACKET_MAX (8 KiB) on this task's 7 KiB stack.
   * Reuse the persistent RX packet buffer: control and data sockets are
   * serviced serially by this same task, and retransmit data is copied into
   * the reorder queue before the next recv(). */
  uint8_t *buf = s_rt.packet;
  if (!buf) {
    return;
  }
  while (s_rt.control_sock >= 0) {
    ssize_t n = recv(s_rt.control_sock, buf, RT_PACKET_MAX, MSG_DONTWAIT);
    if (n <= 0) {
      break;
    }
    if (n < 4) {
      continue;
    }

    const uint8_t pt = buf[1] & 0x7fU;
    if (pt == RT_RETRANSMIT_PT) {
      /* Retransmit response (PT=86): 4-byte resend wrapper followed by the
       * complete original RTP audio packet. */
      if (n > 4) {
        note_retransmit_latency(buf + 4, (size_t)n - 4U);
      }
      if (n <= 4 || !reorder_store(buf + 4, (size_t)n - 4U, true)) {
        s_rt.retransmit_bad++;
        if (s_rt.retransmit_bad <= 10U) {
          ESP_LOGW(TAG, "bad retransmit response bytes=%d", (int)n);
        }
      } else {
        reorder_drain(decoder, first_logs);
      }
      continue;
    }

    /* PT=84 sync/timing and other control packets are intentionally ignored
     * by the local-anchor realtime mode. */
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

    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (!s_rt.running) {
      break;
    }
    if (ready < 0) {
      if (errno != EINTR && s_rt.running) {
        ESP_LOGW(TAG, "select error: %d", errno);
      }
      continue;
    }
    if (ready == 0) {
      reorder_timeout_skip(decoder, &first_logs);
      continue;
    }

    if (s_rt.control_sock >= 0 && FD_ISSET(s_rt.control_sock, &rfds)) {
      handle_control_socket(decoder, &first_logs);
    }

    if (s_rt.data_sock < 0 || !FD_ISSET(s_rt.data_sock, &rfds)) {
      continue;
    }

    ssize_t n = recv(s_rt.data_sock, s_rt.packet, RT_PACKET_MAX, 0);
    if (n <= 0) {
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        ESP_LOGW(TAG, "recv error: %d", errno);
      }
      continue;
    }

    const size_t packet_len = (size_t)n;
    if (packet_len >= 12U) {
      const uint16_t seq =
          (uint16_t)(((uint16_t)s_rt.packet[2] << 8) | s_rt.packet[3]);
      s_rt.rx_packets++;
      if (!s_rt.expected_valid) {
        s_rt.expected_seq = seq;
        s_rt.expected_valid = true;
      }
      if ((int16_t)(seq - s_rt.expected_seq) > 0) {
        reorder_note_gap(seq);
      }
      if (!reorder_store(s_rt.packet, packet_len, false)) {
        s_rt.retransmit_bad++;
      }
      reorder_drain(decoder, &first_logs);
      reorder_timeout_skip(decoder, &first_logs);
    }
  }

  alac_decoder_destroy(decoder);
  ESP_LOGI(TAG,
           "realtime receiver stopped rx=%" PRIu32 " dec=%" PRIu32
           " decrypt_err=%" PRIu32 " decode_err=%" PRIu32
           " sink_drop=%" PRIu32 " nack=%" PRIu32 " rtx=%" PRIu32
           " rtx_bad=%" PRIu32 " late=%" PRIu32 " skip=%" PRIu32
           " rtx_lat=%.2f/%.2f/%.2fms(n=%" PRIu32 ")",
           s_rt.rx_packets, s_rt.decoded_packets, s_rt.decrypt_errors,
           s_rt.decode_errors, s_rt.sink_drops, s_rt.nack_requests,
           s_rt.retransmit_packets, s_rt.retransmit_bad,
           s_rt.reorder_late, s_rt.gap_skips,
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
  s_rt.expected_valid = false;
  s_rt.gap_waiting = false;
  s_rt.reorder_late = 0;
  s_rt.reorder_overwrite = 0;
  s_rt.gap_skips = 0;
  memset(s_rt.nack_track, 0, sizeof(s_rt.nack_track));
  s_rt.rtx_latency_samples = 0;
  s_rt.rtx_latency_sum_us = 0;
  s_rt.rtx_latency_min_us = 0;
  s_rt.rtx_latency_max_us = 0;
  if (s_rt.reorder) {
    memset(s_rt.reorder, 0, RT_REORDER_SLOTS * sizeof(*s_rt.reorder));
  }

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

bool realtime_receiver_is_running(void) { return s_rt.running; }
