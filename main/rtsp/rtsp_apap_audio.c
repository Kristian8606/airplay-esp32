#include "rtsp_apap_audio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aac_decoder.h"
#include "apap_frame_queue.h"
#include "audio_media_time.h"
#include "audio_receiver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "socket_utils.h"
#include "sodium.h"

static const char *TAG = "apap_audio";

#define APAP_RX_STACK 6144
#define APAP_DECODER_STACK 6144
#define APAP_RX_POLL_US 100000
#define APAP_PACKAGE_MAX (64U * 1024U)
#define APAP_HEADER_BYTES 15U

struct rtsp_apap_audio {
  int listen_socket;
  int client_socket;
  uint16_t port;
  uint32_t expected_client_ip;
  uint8_t stream_key[32];
  size_t stream_key_len;
  volatile bool stop;
  TaskHandle_t task;
  TaskHandle_t decoder_task;
  apap_frame_queue_t *queue;

  bool decrypt_logged;
  aac_decoder_t *decoder;
  uint8_t *plain;
  uint8_t *aac_input;
  int16_t *pcm;
  uint32_t enqueued_packets;
  uint32_t decoded_packets;
  uint32_t published_packets;
  uint32_t transport_failures;
  uint32_t decode_failures;

  /* MediaDataControl fshb arrives on a different task than APAP decoding.
   * Keep the complete sequence+media boundary state under one tiny spinlock;
   * 64-bit media_sample fields must never tear on Xtensa. */
  portMUX_TYPE flush_mux;
  bool flush_deferred_active;
  bool flush_deferred_started;
  bool flush_from_seq_valid;
  bool flush_until_seq_valid;
  uint32_t flush_from_seq;
  uint32_t flush_until_seq;
  bool flush_from_sample_valid;
  bool flush_until_sample_valid;
  uint64_t flush_from_sample;
  uint64_t flush_until_sample;

  bool flush_immediate_active;
  bool flush_immediate_seq_valid;
  uint32_t flush_immediate_until_seq;
  bool flush_immediate_sample_valid;
  uint64_t flush_immediate_until_sample;
};

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

static uint32_t read_be24(const uint8_t *p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* This branch intentionally accepts the one Buffered APAP audio timeline we
 * advertise and have observed from iOS 27.2: AAC-LC at 44.1 kHz. Keeping the
 * fixed header validation strict prevents random/encrypted transport bytes
 * from being mistaken for a semantic APAP frame before AEAD verification. */
static bool looks_like_apap_header(const uint8_t *packet, size_t packet_len) {
  if (!packet || packet_len < APAP_HEADER_BYTES) return false;
  return read_be32(packet + 8) == 44100U;
}

static int decrypt_apap_body(const rtsp_apap_audio_t *ctx,
                             const uint8_t *packet, size_t packet_len,
                             uint8_t *out, size_t out_cap) {
  if (!ctx || !packet || packet_len <= APAP_HEADER_BYTES || !out) return -1;

  const uint8_t *body = packet + APAP_HEADER_BYTES;
  const size_t body_len = packet_len - APAP_HEADER_BYTES;

  /* Observed iOS 27.2 Buffered APAP wire format:
   *   clear APAP header (15 B)
   *   ciphertext || Poly1305 tag16 || explicit nonce8
   * AAD is the first 12 bytes of the APAP header. The stream key is the
   * 32-byte `shk` from SETUP. This is the only encrypted APAP dialect kept
   * on this branch; the earlier 64-bit/alternate-AAD probes were diagnostic
   * scaffolding and are intentionally gone. */
  if (ctx->stream_key_len != 32U || body_len < 24U) return -1;
  const size_t cipher_tag_len = body_len - 8U;
  uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES] = {0};
  memcpy(nonce + sizeof(nonce) - 8U, body + cipher_tag_len, 8U);

  unsigned long long plain_len = 0;
  const int rc = crypto_aead_chacha20poly1305_ietf_decrypt(
      out, &plain_len, NULL, body, cipher_tag_len, packet, 12U,
      nonce, ctx->stream_key);
  if (rc != 0 || plain_len > out_cap) return -1;
  return (int)plain_len;
}

/* APAP extension format is a sequence of unsigned base-128 key/length pairs,
 * each followed by value bytes, terminated by key 0. APSAPAPEncode() writes
 * this terminator even when there are no extensions. Return the media offset
 * into the authenticated/decrypted APAP body. */
static bool read_uintv(const uint8_t *p, size_t n, size_t *io, uint64_t *out) {
  uint64_t v = 0;
  if (!p || !io || !out) return false;
  for (unsigned count = 0; count < 10 && *io < n; ++count) {
    const uint8_t b = p[(*io)++];
    if (v > (UINT64_MAX >> 7)) return false;
    v = (v << 7) | (uint64_t)(b & 0x7fU);
    if ((b & 0x80U) == 0) {
      *out = v;
      return true;
    }
  }
  return false;
}

static bool find_media_offset(const uint8_t *plain, size_t plain_len,
                              size_t *out_off) {
  size_t pos = 0;
  unsigned items = 0;
  while (pos < plain_len && items < 32U) {
    uint64_t key = 0;
    if (!read_uintv(plain, plain_len, &pos, &key)) return false;
    if (key == 0) {
      /* A valid APAP boundary/control package may contain extensions plus the
       * terminator and no AAC media bytes. Preserve it in the semantic queue
       * so fshb can reach its exact seq/mediaTime boundary. */
      *out_off = pos;
      return true;
    }
    uint64_t len = 0;
    if (!read_uintv(plain, plain_len, &pos, &len)) return false;
    if (len > plain_len - pos) return false;
    pos += (size_t)len;
    ++items;
  }
  return false;
}

static void publish_queue_stats(rtsp_apap_audio_t *ctx) {
  apap_frame_queue_stats_t qs = {0};
  apap_frame_queue_get_stats(ctx->queue, &qs);
  audio_receiver_set_apap_queue_status(qs.frames, qs.bytes,
                                       APAP_FRAME_QUEUE_CAPACITY,
                                       APAP_FRAME_QUEUE_BYTES);
}

/* Network-side APAP work stops at a semantic AAC access unit. It owns only
 * framing, authentication/decryption and APAP extension parsing. Decoding,
 * EQ and presentation timing run on the consumer side of the frame queue. */
static bool decrypt_and_enqueue(rtsp_apap_audio_t *ctx,
                                const uint8_t *packet, size_t packet_len,
                                uint64_t package_no) {
  if (!looks_like_apap_header(packet, packet_len)) return false;

  const int64_t media_time = (int64_t)read_be64(packet);
  const uint32_t timescale = read_be32(packet + 8);
  const uint32_t seq = read_be24(packet + 12);
  uint64_t media_sample = 0;
  if (!audio_media_time_to_sample64(media_time, timescale, 44100U,
                                      &media_sample))
    return false;

  const int plain_len = decrypt_apap_body(ctx, packet, packet_len, ctx->plain,
                                          APAP_PACKAGE_MAX);
  if (plain_len <= 0) goto transport_fail;

  size_t media_off = 0;
  if (!find_media_offset(ctx->plain, (size_t)plain_len, &media_off) ||
      media_off > (size_t)plain_len)
    goto transport_fail;

  if (!ctx->decrypt_logged) {
    ctx->decrypt_logged = true;
    ESP_LOGI(TAG,
             "APAP transport authenticated: ChaCha20-Poly1305-IETF explicit nonce + APAP12 AAD");
  }

  const size_t aac_len = (size_t)plain_len - media_off;
  if (aac_len > APAP_FRAME_QUEUE_MAX_AAC) {
    ESP_LOGW(TAG, "APAP AAC AU too large seq=%" PRIu32 " len=%u",
             seq, (unsigned)aac_len);
    return false;
  }

  apap_frame_meta_t meta = {
      .seq = seq,
      .media_sample = media_sample,
      .media_time = media_time,
      .timescale = timescale,
      .aac_len = (uint16_t)aac_len,
  };
  const uint8_t *aac = aac_len > 0 ? ctx->plain + media_off : NULL;
  while (!ctx->stop) {
    if (apap_frame_queue_push(ctx->queue, &meta, aac, aac_len)) {
      ++ctx->enqueued_packets;
      if (ctx->enqueued_packets <= 5U ||
          (ctx->enqueued_packets % 250U) == 0U) {
        apap_frame_queue_stats_t qs = {0};
        apap_frame_queue_get_stats(ctx->queue, &qs);
        ESP_LOGI(TAG,
                 "APAP enqueue #%" PRIu32 " seq=%" PRIu32
                 " time=%" PRId64 "/%" PRIu32 " sample=%" PRIu64
                 " aac=%uB q=%u/%u",
                 ctx->enqueued_packets, seq, media_time, timescale,
                 media_sample, (unsigned)aac_len, (unsigned)qs.frames,
                 (unsigned)APAP_FRAME_QUEUE_CAPACITY);
      }
      if (aac_len == 0U)
        ESP_LOGD(TAG, "APAP queued metadata-only boundary seq=%" PRIu32, seq);
      publish_queue_stats(ctx);
      return true;
    }
    /* Full semantic queue is the network backpressure point. Do not consume
     * more TCP until the decoder has freed a frame slot. */
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
  return false;

transport_fail:
  ++ctx->transport_failures;
  if (ctx->transport_failures <= 5U ||
      (ctx->transport_failures % 100U) == 0U) {
    ESP_LOGW(TAG,
             "APAP authentication/extension parse failed package=%" PRIu64
             " seq=%" PRIu32 " len=%u failures=%" PRIu32,
             package_no, seq, (unsigned)packet_len, ctx->transport_failures);
  }
  return false;
}

static int32_t seq24_delta(uint32_t a, uint32_t b) {
  uint32_t d = (a - b) & 0x00ffffffU;
  if (d & 0x00800000U) d |= 0xff000000U;
  return (int32_t)d;
}

static bool apap_boundary_reached(const apap_frame_meta_t *meta,
                                  bool seq_valid, uint32_t seq,
                                  bool sample_valid, uint64_t sample) {
  if (!meta) return false;
  /* Sequence is the packet-exact APAP cursor and is authoritative whenever
   * Apple supplied it. mediaTime is the fallback for commands without a
   * sequence boundary and is also retained for diagnostics/validation. */
  if (seq_valid) return seq24_delta(meta->seq, seq) >= 0;
  if (sample_valid) return meta->media_sample >= sample;
  return true;
}

static bool apap_flush_drop(rtsp_apap_audio_t *ctx,
                            const apap_frame_meta_t *meta) {
  if (!ctx || !meta) return false;

  bool drop = false;
  bool immediate_completed = false;
  bool deferred_entered = false;
  bool deferred_completed = false;

  uint32_t log_from_seq = 0;
  uint32_t log_until_seq = 0;
  uint64_t log_from_sample = 0;
  uint64_t log_until_sample = 0;
  bool log_from_seq_valid = false;
  bool log_until_seq_valid = false;
  bool log_from_sample_valid = false;
  bool log_until_sample_valid = false;

  taskENTER_CRITICAL(&ctx->flush_mux);

  if (ctx->flush_immediate_active) {
    const bool reached = apap_boundary_reached(
        meta, ctx->flush_immediate_seq_valid,
        ctx->flush_immediate_until_seq,
        ctx->flush_immediate_sample_valid,
        ctx->flush_immediate_until_sample);
    if (!reached) {
      drop = true;
    } else {
      immediate_completed = true;
      log_until_seq_valid = ctx->flush_immediate_seq_valid;
      log_until_seq = ctx->flush_immediate_until_seq;
      log_until_sample_valid = ctx->flush_immediate_sample_valid;
      log_until_sample = ctx->flush_immediate_until_sample;
      ctx->flush_immediate_active = false;
    }
  }

  if (!drop && ctx->flush_deferred_active) {
    if (!ctx->flush_deferred_started) {
      const bool reached_from = apap_boundary_reached(
          meta, ctx->flush_from_seq_valid, ctx->flush_from_seq,
          ctx->flush_from_sample_valid, ctx->flush_from_sample);
      if (!reached_from) {
        taskEXIT_CRITICAL(&ctx->flush_mux);
        return false;
      }
      ctx->flush_deferred_started = true;
      deferred_entered = true;
      log_from_seq_valid = ctx->flush_from_seq_valid;
      log_from_seq = ctx->flush_from_seq;
      log_from_sample_valid = ctx->flush_from_sample_valid;
      log_from_sample = ctx->flush_from_sample;
      log_until_seq_valid = ctx->flush_until_seq_valid;
      log_until_seq = ctx->flush_until_seq;
      log_until_sample_valid = ctx->flush_until_sample_valid;
      log_until_sample = ctx->flush_until_sample;
    }

    const bool reached_until = apap_boundary_reached(
        meta, ctx->flush_until_seq_valid, ctx->flush_until_seq,
        ctx->flush_until_sample_valid, ctx->flush_until_sample);
    if (!reached_until) {
      drop = true;
    } else {
      ctx->flush_deferred_active = false;
      ctx->flush_deferred_started = false;
      deferred_completed = true;
      log_until_seq_valid = ctx->flush_until_seq_valid;
      log_until_seq = ctx->flush_until_seq;
      log_until_sample_valid = ctx->flush_until_sample_valid;
      log_until_sample = ctx->flush_until_sample;
    }
  }

  taskEXIT_CRITICAL(&ctx->flush_mux);

  if (deferred_entered) {
    /* The old PCM timeline becomes stale exactly when the APAP decoder cursor
     * enters the sender-declared transition range. */
    audio_receiver_apap_flush();
    ESP_LOGI(TAG,
             "APAP deferred fshb entered at seq=%" PRIu32
             " sample=%" PRIu64
             " from=%s%" PRIu32 "/%s%" PRIu64
             " until=%s%" PRIu32 "/%s%" PRIu64,
             meta->seq, meta->media_sample,
             log_from_seq_valid ? "" : "n/a:", log_from_seq,
             log_from_sample_valid ? "" : "n/a:", log_from_sample,
             log_until_seq_valid ? "" : "n/a:", log_until_seq,
             log_until_sample_valid ? "" : "n/a:", log_until_sample);
  }
  if (immediate_completed) {
    ESP_LOGI(TAG,
             "APAP immediate fshb reached boundary at seq=%" PRIu32
             " sample=%" PRIu64 " until=%s%" PRIu32 "/%s%" PRIu64,
             meta->seq, meta->media_sample,
             log_until_seq_valid ? "" : "n/a:", log_until_seq,
             log_until_sample_valid ? "" : "n/a:", log_until_sample);
  }
  if (deferred_completed) {
    ESP_LOGI(TAG,
             "APAP deferred fshb completed at seq=%" PRIu32
             " sample=%" PRIu64 " until=%s%" PRIu32 "/%s%" PRIu64,
             meta->seq, meta->media_sample,
             log_until_seq_valid ? "" : "n/a:", log_until_seq,
             log_until_sample_valid ? "" : "n/a:", log_until_sample);
  }
  return drop;
}

static void observe_stream(rtsp_apap_audio_t *ctx, int fd) {
  uint8_t *rx = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t *packet = heap_caps_malloc(APAP_PACKAGE_MAX,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rx) rx = malloc(4096);
  if (!packet) packet = malloc(APAP_PACKAGE_MAX);
  if (!rx || !packet) {
    free(rx);
    free(packet);
    return;
  }

  uint8_t hdr[4] = {0};
  size_t hdr_have = 0;
  uint32_t package_remaining = 0;
  size_t package_have = 0;
  uint64_t package_no = 0;
  uint64_t total_rx = 0;

  while (!ctx->stop) {
    const ssize_t got = recv(fd, rx, 4096, 0);
    if (got == 0) break;
    if (got < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
    total_rx += (uint64_t)got;

    size_t off = 0;
    while (off < (size_t)got) {
      if (package_remaining == 0) {
        while (hdr_have < sizeof(hdr) && off < (size_t)got)
          hdr[hdr_have++] = rx[off++];
        if (hdr_have < sizeof(hdr)) break;

        const uint32_t package_total = read_be32(hdr);
        hdr_have = 0;
        if (package_total <= 4 || package_total - 4U > APAP_PACKAGE_MAX) {
          ESP_LOGW(TAG, "invalid BufferedAPAP total=%" PRIu32, package_total);
          goto done;
        }
        package_remaining = package_total - 4U;
        package_have = 0;
        ++package_no;
        if (package_no <= 3U) {
          ESP_LOGI(TAG,
                   "BufferedAPAP package #%" PRIu64 " total=%" PRIu32
                   " payload=%" PRIu32,
                   package_no, package_total, package_remaining);
        }
      }

      const size_t avail = (size_t)got - off;
      const size_t take = avail < (size_t)package_remaining
                              ? avail : (size_t)package_remaining;
      memcpy(packet + package_have, rx + off, take);
      package_have += take;
      off += take;
      package_remaining -= (uint32_t)take;

      if (package_remaining == 0) {
        if (looks_like_apap_header(packet, package_have)) {
          /* fshb is applied on the semantic queue consumer, not on raw TCP.
           * This preserves metadata-only boundary frames and makes sequence
           * plus mediaTime available to the discard decision. */
          (void)decrypt_and_enqueue(ctx, packet, package_have, package_no);
        }
      }
    }
  }

done:
  ESP_LOGI(TAG,
           "APAP client disconnected after %" PRIu64 " bytes packages=%" PRIu64
           " queued=%" PRIu32 " decoded=%" PRIu32 " published=%" PRIu32,
           total_rx, package_no, ctx->enqueued_packets, ctx->decoded_packets,
           ctx->published_packets);
  free(rx);
  free(packet);
}

static void apap_decoder_task(void *arg) {
  rtsp_apap_audio_t *ctx = (rtsp_apap_audio_t *)arg;
  apap_frame_queue_set_consumer(ctx->queue, xTaskGetCurrentTaskHandle());
  while (!ctx->stop) {
    apap_frame_meta_t meta = {0};
    if (!apap_frame_queue_pop(ctx->queue, &meta,
                              ctx->aac_input + AAC_DECODER_INPUT_HEADROOM,
                              APAP_FRAME_QUEUE_MAX_AAC)) {
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
      continue;
    }
    publish_queue_stats(ctx);

    if (apap_flush_drop(ctx, &meta)) continue;
    if (meta.aac_len == 0U) {
      ESP_LOGD(TAG, "APAP metadata-only frame seq=%" PRIu32
                    " sample=%" PRIu64, meta.seq, meta.media_sample);
      continue;
    }
    if (meta.timescale != 44100U) {
      ++ctx->decode_failures;
      ESP_LOGW(TAG, "APAP frame has unsupported media timescale=%u seq=%" PRIu32,
               (unsigned)meta.timescale, meta.seq);
      continue;
    }

    aac_decode_info_t info = {0};
    const int frames = aac_decoder_decode(
        ctx->decoder, ctx->aac_input + AAC_DECODER_INPUT_HEADROOM,
        meta.aac_len, ctx->pcm, 2048U, &info);
    if (frames <= 0 || info.channels != 2) {
      ++ctx->decode_failures;
      if (ctx->decode_failures <= 5U ||
          (ctx->decode_failures % 100U) == 0U) {
        ESP_LOGW(TAG,
                 "APAP AAC decode failed seq=%" PRIu32 " len=%u failures=%" PRIu32,
                 meta.seq, (unsigned)meta.aac_len, ctx->decode_failures);
      }
      continue;
    }

    ++ctx->decoded_packets;
    const bool published = audio_receiver_publish_apap_pcm(
        meta.media_sample, ctx->pcm, (size_t)frames, info.channels);
    if (published) ++ctx->published_packets;
    if (ctx->decoded_packets <= 5U || (ctx->decoded_packets % 100U) == 0U) {
      apap_frame_queue_stats_t qs = {0};
      apap_frame_queue_get_stats(ctx->queue, &qs);
      ESP_LOGI(TAG,
               "APAP decoded #%" PRIu32 " seq=%" PRIu32
               " time=%" PRId64 "/%" PRIu32 " sample=%" PRIu64
               " aac=%uB pcm=%d published=%d q=%u",
               ctx->decoded_packets, meta.seq, meta.media_time, meta.timescale,
               meta.media_sample,
               (unsigned)meta.aac_len, frames, published ? 1 : 0,
               (unsigned)qs.frames);
    }
  }

  apap_frame_queue_set_consumer(ctx->queue, NULL);
  ctx->decoder_task = NULL;
  vTaskDelete(NULL);
}

static void apap_task(void *arg) {
  rtsp_apap_audio_t *ctx = (rtsp_apap_audio_t *)arg;

  while (!ctx->stop) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx->listen_socket, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = APAP_RX_POLL_US};
    int r = select(ctx->listen_socket + 1, &rfds, NULL, NULL, &tv);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0 || !FD_ISSET(ctx->listen_socket, &rfds)) continue;

    struct sockaddr_in peer = {0};
    socklen_t peer_len = sizeof(peer);
    int fd = accept(ctx->listen_socket, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
      if (ctx->stop) break;
      continue;
    }

    if (ctx->expected_client_ip != 0 &&
        peer.sin_addr.s_addr != ctx->expected_client_ip) {
      ESP_LOGW(TAG, "rejected APAP client from unexpected IP");
      close(fd);
      continue;
    }

    ctx->client_socket = fd;
    struct timeval rcv_to = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
    apap_frame_queue_set_producer(ctx->queue, xTaskGetCurrentTaskHandle());
    ESP_LOGI(TAG,
             "APAP TCP client connected port=%u streamKey=%uB",
             ctx->port, (unsigned)ctx->stream_key_len);
    observe_stream(ctx, fd);
    apap_frame_queue_set_producer(ctx->queue, NULL);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    ctx->client_socket = -1;
  }

  ctx->task = NULL;
  vTaskDelete(NULL);
}

esp_err_t rtsp_apap_audio_start(uint32_t expected_client_ip,
                                const uint8_t *stream_key,
                                size_t stream_key_len,
                                rtsp_apap_audio_t **out_audio,
                                uint16_t *out_port) {
  if (!out_audio || !out_port) return ESP_ERR_INVALID_ARG;
  if (*out_audio) {
    rtsp_apap_audio_stop(out_audio);
    if (*out_audio) {
      ESP_LOGE(TAG, "previous APAP transport is still stopping");
      return ESP_ERR_INVALID_STATE;
    }
  }
  if (!stream_key || stream_key_len != 32U) {
    ESP_LOGE(TAG, "APAP requires a 32-byte stream key (got %uB)",
             (unsigned)stream_key_len);
    return ESP_ERR_INVALID_ARG;
  }

  rtsp_apap_audio_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return ESP_ERR_NO_MEM;
  ctx->listen_socket = -1;
  ctx->client_socket = -1;
  ctx->flush_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
  ctx->expected_client_ip = expected_client_ip;
  memcpy(ctx->stream_key, stream_key, 32U);
  ctx->stream_key_len = 32U;

  if (apap_frame_queue_create(&ctx->queue) != ESP_OK) {
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  ctx->plain = heap_caps_malloc(APAP_PACKAGE_MAX,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ctx->aac_input = heap_caps_malloc(APAP_FRAME_QUEUE_MAX_AAC + AAC_DECODER_INPUT_HEADROOM,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ctx->pcm = heap_caps_malloc(2048U * 2U * sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ctx->plain) ctx->plain = malloc(APAP_PACKAGE_MAX);
  if (!ctx->aac_input)
    ctx->aac_input = malloc(APAP_FRAME_QUEUE_MAX_AAC + AAC_DECODER_INPUT_HEADROOM);
  if (!ctx->pcm) ctx->pcm = malloc(2048U * 2U * sizeof(int16_t));
  if (!ctx->plain || !ctx->aac_input || !ctx->pcm) {
    free(ctx->plain);
    free(ctx->aac_input);
    free(ctx->pcm);
    apap_frame_queue_destroy(ctx->queue);
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  const aac_decoder_config_t decoder_cfg = {
      .sample_rate = 44100,
      .channels = 2,
      .bits_per_sample = 16,
  };
  ctx->decoder = aac_decoder_create(&decoder_cfg);
  if (!ctx->decoder) {
    free(ctx->plain);
    free(ctx->aac_input);
    free(ctx->pcm);
    apap_frame_queue_destroy(ctx->queue);
    free(ctx);
    return ESP_FAIL;
  }

  ctx->listen_socket = socket_utils_bind_tcp_listener(0, 1, false, &ctx->port);
  if (ctx->listen_socket < 0 || ctx->port == 0) {
    if (ctx->listen_socket >= 0) close(ctx->listen_socket);
    aac_decoder_destroy(ctx->decoder);
    ctx->decoder = NULL;
    free(ctx->plain);
    free(ctx->aac_input);
    free(ctx->pcm);
    apap_frame_queue_destroy(ctx->queue);
    free(ctx);
    return ESP_FAIL;
  }

  BaseType_t dec_ret = xTaskCreatePinnedToCoreWithCaps(
      apap_decoder_task, "apap_decode", APAP_DECODER_STACK, ctx, 5,
      &ctx->decoder_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (dec_ret != pdPASS) {
    close(ctx->listen_socket);
    aac_decoder_destroy(ctx->decoder);
    ctx->decoder = NULL;
    free(ctx->plain);
    free(ctx->aac_input);
    free(ctx->pcm);
    apap_frame_queue_destroy(ctx->queue);
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
      apap_task, "apap_rx", APAP_RX_STACK, ctx, 5, &ctx->task, 0,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ret != pdPASS) {
    ctx->stop = true;
    apap_frame_queue_wake_all(ctx->queue);
    if (ctx->listen_socket >= 0) {
      close(ctx->listen_socket);
      ctx->listen_socket = -1;
    }
    int ticks = 100;
    while (ctx->decoder_task != NULL && ticks-- > 0)
      vTaskDelay(pdMS_TO_TICKS(10));
    if (ctx->decoder_task != NULL) {
      /* Never free a context still owned by the decoder task. Hand the
       * partially-started transport back to the RTSP connection so normal
       * teardown can retry the stop after the task observes ctx->stop. */
      ESP_LOGE(TAG, "APAP RX task creation failed and decoder did not stop in time");
      *out_audio = ctx;
      *out_port = ctx->port;
      return ESP_ERR_TIMEOUT;
    }
    aac_decoder_destroy(ctx->decoder);
    ctx->decoder = NULL;
    free(ctx->plain);
    free(ctx->aac_input);
    free(ctx->pcm);
    apap_frame_queue_destroy(ctx->queue);
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  *out_port = ctx->port;
  *out_audio = ctx;
  ESP_LOGI(TAG,
           "APAP audio endpoint ready TCP %u streamKey=%uB queue=%ux%uB -> EQ/PCM/PTP",
           ctx->port, (unsigned)ctx->stream_key_len,
           (unsigned)APAP_FRAME_QUEUE_CAPACITY,
           (unsigned)APAP_FRAME_QUEUE_MAX_AAC);
  return ESP_OK;
}

void rtsp_apap_audio_flush(rtsp_apap_audio_t *ctx,
                           bool have_from_seq, uint32_t from_seq,
                           bool have_until_seq, uint32_t until_seq,
                           bool have_from_media_time,
                           int64_t from_media_time_value,
                           uint32_t from_media_time_scale,
                           bool have_until_media_time,
                           int64_t until_media_time_value,
                           uint32_t until_media_time_scale) {
  if (!ctx) return;

  from_seq &= 0x00ffffffU;
  until_seq &= 0x00ffffffU;

  uint64_t from_sample = 0;
  uint64_t until_sample = 0;
  const bool have_from_sample =
      have_from_media_time &&
      audio_media_time_to_sample64(from_media_time_value, from_media_time_scale,
                                   44100U, &from_sample);
  const bool have_until_sample =
      have_until_media_time &&
      audio_media_time_to_sample64(until_media_time_value, until_media_time_scale,
                                   44100U, &until_sample);

  const bool have_from_boundary = have_from_seq || have_from_sample;
  const bool have_until_boundary = have_until_seq || have_until_sample;

  if (have_from_boundary && have_until_boundary) {
    taskENTER_CRITICAL(&ctx->flush_mux);
    ctx->flush_immediate_active = false;

    ctx->flush_from_seq_valid = have_from_seq;
    ctx->flush_from_seq = from_seq;
    ctx->flush_until_seq_valid = have_until_seq;
    ctx->flush_until_seq = until_seq;
    ctx->flush_from_sample_valid = have_from_sample;
    ctx->flush_from_sample = from_sample;
    ctx->flush_until_sample_valid = have_until_sample;
    ctx->flush_until_sample = until_sample;

    ctx->flush_deferred_started = false;
    ctx->flush_deferred_active = true;
    taskEXIT_CRITICAL(&ctx->flush_mux);

    ESP_LOGI(TAG,
             "APAP fshb deferred armed from=%s%" PRIu32 "/%s%" PRIu64
             " until=%s%" PRIu32 "/%s%" PRIu64,
             have_from_seq ? "" : "n/a:", from_seq,
             have_from_sample ? "" : "n/a:", from_sample,
             have_until_seq ? "" : "n/a:", until_seq,
             have_until_sample ? "" : "n/a:", until_sample);
    return;
  }

  if (have_until_boundary) {
    /* A later immediate fshb supersedes an earlier deferred range. The queued
     * compressed frames are stale immediately; subsequent TCP frames are
     * retained in the semantic queue but discarded by the decoder until the
     * exact APAP boundary is reached. */
    taskENTER_CRITICAL(&ctx->flush_mux);
    ctx->flush_deferred_active = false;
    ctx->flush_deferred_started = false;

    ctx->flush_immediate_seq_valid = have_until_seq;
    ctx->flush_immediate_until_seq = until_seq;
    ctx->flush_immediate_sample_valid = have_until_sample;
    ctx->flush_immediate_until_sample = until_sample;
    ctx->flush_immediate_active = true;
    taskEXIT_CRITICAL(&ctx->flush_mux);

    apap_frame_queue_clear(ctx->queue);
    publish_queue_stats(ctx);
    audio_receiver_apap_flush();
    ESP_LOGI(TAG,
             "APAP fshb immediate armed until=%s%" PRIu32 "/%s%" PRIu64,
             have_until_seq ? "" : "n/a:", until_seq,
             have_until_sample ? "" : "n/a:", until_sample);
    return;
  }

  taskENTER_CRITICAL(&ctx->flush_mux);
  ctx->flush_deferred_active = false;
  ctx->flush_deferred_started = false;
  ctx->flush_immediate_active = false;
  taskEXIT_CRITICAL(&ctx->flush_mux);

  apap_frame_queue_clear(ctx->queue);
  publish_queue_stats(ctx);
  audio_receiver_apap_flush();
  ESP_LOGI(TAG, "APAP fshb without usable seq/mediaTime -> timing/cache reset");
}

void rtsp_apap_audio_stop(rtsp_apap_audio_t **audio) {
  if (!audio || !*audio) return;
  rtsp_apap_audio_t *ctx = *audio;
  ctx->stop = true;
  if (ctx->client_socket >= 0) shutdown(ctx->client_socket, SHUT_RDWR);
  if (ctx->listen_socket >= 0) shutdown(ctx->listen_socket, SHUT_RDWR);
  apap_frame_queue_wake_all(ctx->queue);

  int ticks = 100;
  while ((ctx->task != NULL || ctx->decoder_task != NULL) && ticks-- > 0)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (ctx->task != NULL || ctx->decoder_task != NULL) {
    ESP_LOGW(TAG, "APAP tasks did not stop; context retained rx=%p dec=%p",
             (void *)ctx->task, (void *)ctx->decoder_task);
    return;
  }

  if (ctx->decoder) aac_decoder_destroy(ctx->decoder);
  sodium_memzero(ctx->stream_key, sizeof(ctx->stream_key));
  free(ctx->plain);
  free(ctx->aac_input);
  free(ctx->pcm);
  apap_frame_queue_destroy(ctx->queue);
  audio_receiver_set_apap_queue_status(0, 0, APAP_FRAME_QUEUE_CAPACITY,
                                       APAP_FRAME_QUEUE_BYTES);
  if (ctx->client_socket >= 0) close(ctx->client_socket);
  if (ctx->listen_socket >= 0) close(ctx->listen_socket);
  free(ctx);
  *audio = NULL;
}
