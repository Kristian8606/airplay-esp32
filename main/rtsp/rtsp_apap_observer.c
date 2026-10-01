#include "rtsp_apap_observer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aac_decoder.h"
#include "audio_receiver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "socket_utils.h"
#include "sodium.h"

static const char *TAG = "apap_audio";

#define APAP_OBSERVER_STACK 8192
#define APAP_OBSERVER_POLL_US 100000
#define APAP_PACKAGE_MAX (64U * 1024U)
#define APAP_CAPTURE_MAX 256U
#define APAP_HEADER_BYTES 15U

typedef enum {
  APAP_DEC_UNKNOWN = 0,
  APAP_DEC_PLAIN,
  APAP_DEC_CHACHA64_EXPLICIT_AAD12,
  APAP_DEC_CHACHA64_COUNTER_AAD12,
  APAP_DEC_CHACHA64_EXPLICIT_AAD15, /* diagnostic fallback only */
  APAP_DEC_CHACHA_IETF_AAD12,      /* diagnostic fallback only */
} apap_decrypt_mode_t;

struct rtsp_apap_observer {
  int listen_socket;
  int client_socket;
  uint16_t port;
  uint32_t expected_client_ip;
  bool uses_stream_encryption_key;
  uint8_t stream_key[32];
  size_t stream_key_len;
  volatile bool stop;
  TaskHandle_t task;

  apap_decrypt_mode_t decrypt_mode;
  aac_decoder_t *decoder;
  uint8_t *plain;
  uint8_t *aac_input;
  int16_t *pcm;
  uint32_t decoded_packets;
  uint32_t published_packets;
  uint32_t decode_failures;

  volatile bool flush_deferred_active;
  volatile bool flush_deferred_started;
  volatile uint32_t flush_from_seq;
  volatile uint32_t flush_until_seq;
  volatile bool flush_immediate_active;
  volatile uint32_t flush_immediate_until_seq;
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

static void log_hex_prefix(const char *what, const uint8_t *p, size_t n) {
  char line[3 * 32 + 1];
  const size_t limit = n > APAP_CAPTURE_MAX ? APAP_CAPTURE_MAX : n;
  ESP_LOGI(TAG, "%s: %u byte prefix", what, (unsigned)limit);
  for (size_t off = 0; off < limit; off += 32) {
    const size_t chunk = (limit - off) > 32 ? 32 : (limit - off);
    size_t pos = 0;
    for (size_t i = 0; i < chunk && pos + 3 < sizeof(line); ++i) {
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%02x%s",
                              p[off + i], (i + 1 == chunk) ? "" : " ");
    }
    line[pos] = '\0';
    ESP_LOGI(TAG, "  +%04u %s", (unsigned)off, line);
  }
}

static bool looks_like_apap_header(const uint8_t *p, size_t n) {
  if (!p || n < APAP_HEADER_BYTES) return false;
  const uint32_t timescale = read_be32(p + 8);
  return timescale >= 1000U && timescale <= 1000000000U;
}

static const char *decrypt_mode_name(apap_decrypt_mode_t mode) {
  switch (mode) {
    case APAP_DEC_PLAIN: return "plain";
    case APAP_DEC_CHACHA64_EXPLICIT_AAD12: return "ChaCha20-Poly1305-64 explicit nonce + APAP12 AAD";
    case APAP_DEC_CHACHA64_COUNTER_AAD12: return "ChaCha20-Poly1305-64 counter nonce + APAP12 AAD";
    case APAP_DEC_CHACHA64_EXPLICIT_AAD15: return "ChaCha20-Poly1305-64 explicit nonce + APAP15 AAD (fallback)";
    case APAP_DEC_CHACHA_IETF_AAD12: return "ChaCha20-Poly1305-IETF explicit nonce + APAP12 AAD (fallback)";
    default: return "unknown";
  }
}

/* Apple APSAPAPBBufEncode()/Decode() is unusually helpful here: it calls
 * APSCryptor with exactly 12 AAD bytes from the APAP header, leaves all 15
 * fixed header bytes clear, and crypts in place from byte 15 onward. Apple's
 * ChaCha20-Poly1305 cryptor has two forms:
 *   mode=true  -> ciphertext || tag16 || explicit_nonce8 (24 B overhead)
 *   mode=false -> ciphertext || tag16, nonce is an internal LE64 counter
 * The HomePod-style stream asks for stream encryption, so probe the exact
 * explicit form first and the counter form second. APAP15/IETF are retained
 * only as diagnostics in case a later OS changes the wrapper. */
static int decrypt_candidate(const rtsp_apap_observer_t *ctx,
                             apap_decrypt_mode_t mode,
                             const uint8_t *packet, size_t packet_len,
                             uint64_t package_no,
                             uint8_t *out, size_t out_cap) {
  if (!packet || packet_len <= APAP_HEADER_BYTES || !out) return -1;
  const uint8_t *enc = packet + APAP_HEADER_BYTES;
  const size_t enc_len = packet_len - APAP_HEADER_BYTES;

  if (mode == APAP_DEC_PLAIN) {
    if (enc_len > out_cap) return -1;
    memcpy(out, enc, enc_len);
    return (int)enc_len;
  }

  if (ctx->stream_key_len != 32) return -1;

  const uint8_t *aad = packet;
  size_t aad_len = 12U;
  const uint8_t *cipher_and_tag = enc;
  size_t cipher_tag_len = enc_len;
  uint8_t nonce8[crypto_aead_chacha20poly1305_NPUBBYTES] = {0};
  bool use_ietf = false;

  switch (mode) {
    case APAP_DEC_CHACHA64_EXPLICIT_AAD12:
      if (enc_len < 24U) return -1;
      cipher_tag_len = enc_len - 8U;
      memcpy(nonce8, enc + cipher_tag_len, sizeof(nonce8));
      break;
    case APAP_DEC_CHACHA64_COUNTER_AAD12: {
      if (enc_len < 16U || package_no == 0U) return -1;
      /* Apple increments byte 0 first, i.e. a little-endian 64-bit counter. */
      uint64_t ctr = package_no - 1U;
      for (size_t i = 0; i < sizeof(nonce8); ++i) {
        nonce8[i] = (uint8_t)(ctr & 0xffU);
        ctr >>= 8;
      }
      break;
    }
    case APAP_DEC_CHACHA64_EXPLICIT_AAD15:
      if (enc_len < 24U) return -1;
      aad_len = APAP_HEADER_BYTES;
      cipher_tag_len = enc_len - 8U;
      memcpy(nonce8, enc + cipher_tag_len, sizeof(nonce8));
      break;
    case APAP_DEC_CHACHA_IETF_AAD12:
      if (enc_len < 24U) return -1;
      cipher_tag_len = enc_len - 8U;
      memcpy(nonce8, enc + cipher_tag_len, sizeof(nonce8));
      use_ietf = true;
      break;
    default:
      return -1;
  }

  unsigned long long plain_len = 0;
  int rc = -1;
  if (!use_ietf) {
    rc = crypto_aead_chacha20poly1305_decrypt(
        out, &plain_len, NULL, cipher_and_tag, cipher_tag_len,
        aad, aad_len, nonce8, ctx->stream_key);
  } else {
    uint8_t nonce12[crypto_aead_chacha20poly1305_ietf_NPUBBYTES] = {0};
    memcpy(nonce12 + sizeof(nonce12) - sizeof(nonce8), nonce8, sizeof(nonce8));
    rc = crypto_aead_chacha20poly1305_ietf_decrypt(
        out, &plain_len, NULL, cipher_and_tag, cipher_tag_len,
        aad, aad_len, nonce12, ctx->stream_key);
  }
  if (rc != 0 || plain_len > out_cap) return -1;
  return (int)plain_len;
}

/* APAP extension format is a sequence of unsigned base-128 key/length pairs,
 * each followed by value bytes, terminated by key 0. APSAPAPEncode() writes
 * this terminator even when there are no extensions. Return the media offset
 * into decrypted/plain APAP body. */
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
      if (pos >= plain_len) return false;
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

static bool decode_and_publish(rtsp_apap_observer_t *ctx,
                            const uint8_t *packet, size_t packet_len,
                            uint64_t package_no) {
  if (!looks_like_apap_header(packet, packet_len)) return false;

  const int64_t media_time = (int64_t)read_be64(packet);
  const uint32_t timescale = read_be32(packet + 8);
  const uint32_t seq = read_be24(packet + 12);

  apap_decrypt_mode_t modes[5];
  size_t mode_count = 0;
  if (ctx->decrypt_mode != APAP_DEC_UNKNOWN) {
    modes[mode_count++] = ctx->decrypt_mode;
  } else if (ctx->uses_stream_encryption_key) {
    /* iPhone 27.2 on the observed HomePod profile authenticated with the IETF
     * explicit-nonce form. Keep older Apple variants only as compatibility
     * probes after the known-good modern dialect. */
    modes[mode_count++] = APAP_DEC_CHACHA_IETF_AAD12;
    modes[mode_count++] = APAP_DEC_CHACHA64_EXPLICIT_AAD12;
    modes[mode_count++] = APAP_DEC_CHACHA64_COUNTER_AAD12;
    modes[mode_count++] = APAP_DEC_CHACHA64_EXPLICIT_AAD15;
    modes[mode_count++] = APAP_DEC_PLAIN;
  } else {
    modes[mode_count++] = APAP_DEC_PLAIN;
  }

  for (size_t mi = 0; mi < mode_count; ++mi) {
    const apap_decrypt_mode_t mode = modes[mi];
    const int plain_len = decrypt_candidate(ctx, mode, packet, packet_len,
                                            package_no, ctx->plain,
                                            APAP_PACKAGE_MAX);
    if (plain_len <= 0) continue;
    if (ctx->decrypt_mode == APAP_DEC_UNKNOWN && package_no <= 3U) {
      ESP_LOGI(TAG, "APAP probe authenticated/plain mode=%s body=%dB seq=%" PRIu32,
               decrypt_mode_name(mode), plain_len, seq);
    }

    size_t media_off = 0;
    if (!find_media_offset(ctx->plain, (size_t)plain_len, &media_off)) {
      if (ctx->decrypt_mode == APAP_DEC_UNKNOWN && package_no <= 3U) {
        ESP_LOGW(TAG, "APAP probe body authenticated but extension parse failed mode=%s",
                 decrypt_mode_name(mode));
        log_hex_prefix("authenticated APAP body", ctx->plain, (size_t)plain_len);
      }
      continue;
    }
    if (media_off > (size_t)plain_len) continue;
    if (media_off == (size_t)plain_len) {
      if (ctx->decrypt_mode == APAP_DEC_UNKNOWN) ctx->decrypt_mode = mode;
      ESP_LOGD(TAG, "APAP extension-only packet seq=%" PRIu32, seq);
      return true;
    }
    const size_t aac_len = (size_t)plain_len - media_off;
    if (aac_len == 0 || aac_len > APAP_PACKAGE_MAX - AAC_DECODER_INPUT_HEADROOM)
      continue;

    memcpy(ctx->aac_input + AAC_DECODER_INPUT_HEADROOM,
           ctx->plain + media_off, aac_len);

    aac_decoder_t *decoder = ctx->decoder;
    bool probe_decoder = false;
    if (!decoder || ctx->decrypt_mode == APAP_DEC_UNKNOWN) {
      aac_decoder_config_t cfg = {
          .sample_rate = (int)timescale,
          .channels = 2,
          .bits_per_sample = 16,
      };
      decoder = aac_decoder_create(&cfg);
      if (!decoder) continue;
      probe_decoder = true;
    }

    aac_decode_info_t info = {0};
    const int frames = aac_decoder_decode(
        decoder, ctx->aac_input + AAC_DECODER_INPUT_HEADROOM,
        aac_len, ctx->pcm, 2048U, &info);
    if (frames <= 0 || info.channels != 2) {
      if (ctx->decrypt_mode == APAP_DEC_UNKNOWN && package_no <= 3U) {
        ESP_LOGW(TAG,
                 "APAP probe decrypted mode=%s ext=%uB AAC=%uB but decoder rejected it",
                 decrypt_mode_name(mode), (unsigned)media_off, (unsigned)aac_len);
      }
      if (probe_decoder) aac_decoder_destroy(decoder);
      continue;
    }

    if (ctx->decrypt_mode == APAP_DEC_UNKNOWN) {
      if (ctx->decoder) aac_decoder_destroy(ctx->decoder);
      ctx->decoder = decoder;
      ctx->decrypt_mode = mode;
      probe_decoder = false;
      ESP_LOGW(TAG,
               "APAP decrypt/AAC probe LOCKED: %s; ext=%uB aac=%uB frames=%d seq=%" PRIu32,
               decrypt_mode_name(mode), (unsigned)media_off,
               (unsigned)aac_len, frames, seq);
      log_hex_prefix("first authenticated/decrypted APAP body", ctx->plain,
                     (size_t)plain_len);
    }

    ++ctx->decoded_packets;
    const bool published = audio_receiver_publish_timed_pcm(
        media_time, timescale, ctx->pcm, (size_t)frames, info.channels);
    if (published) ++ctx->published_packets;
    if (ctx->decoded_packets <= 5U || (ctx->decoded_packets % 100U) == 0U) {
      ESP_LOGI(TAG,
               "APAP decoded #%" PRIu32 " seq=%" PRIu32
               " time=%" PRId64 "/%" PRIu32 " aac=%uB pcm=%d published=%d",
               ctx->decoded_packets, seq, media_time, timescale,
               (unsigned)aac_len, frames, published ? 1 : 0);
    }
    return true;
  }

  if ((++ctx->decode_failures <= 5U) || ((ctx->decode_failures % 100U) == 0U)) {
    ESP_LOGW(TAG,
             "APAP: no authenticated+decodable AAC candidate package=%" PRIu64
             " seq=%" PRIu32 " len=%u failures=%" PRIu32,
             package_no, seq, (unsigned)packet_len, ctx->decode_failures);
  }
  return false;
}

static int32_t seq24_delta(uint32_t a, uint32_t b) {
  uint32_t d = (a - b) & 0x00ffffffU;
  if (d & 0x00800000U) d |= 0xff000000U;
  return (int32_t)d;
}

static bool apap_flush_drop(rtsp_apap_observer_t *ctx, uint32_t seq) {
  if (__atomic_load_n(&ctx->flush_immediate_active, __ATOMIC_ACQUIRE)) {
    const uint32_t until =
        __atomic_load_n(&ctx->flush_immediate_until_seq, __ATOMIC_RELAXED);
    if (seq24_delta(seq, until) < 0) return true;
    __atomic_store_n(&ctx->flush_immediate_active, false, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "APAP immediate fshb reached untilSeq=%" PRIu32
                  " at seq=%" PRIu32, until, seq);
  }

  if (__atomic_load_n(&ctx->flush_deferred_active, __ATOMIC_ACQUIRE)) {
    const uint32_t from =
        __atomic_load_n(&ctx->flush_from_seq, __ATOMIC_RELAXED);
    const uint32_t until =
        __atomic_load_n(&ctx->flush_until_seq, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&ctx->flush_deferred_started, __ATOMIC_RELAXED)) {
      if (seq24_delta(seq, from) < 0) return false;
      __atomic_store_n(&ctx->flush_deferred_started, true, __ATOMIC_RELEASE);
      audio_receiver_external_buffered_flush();
      ESP_LOGI(TAG, "APAP deferred fshb entered fromSeq=%" PRIu32
                    " untilSeq=%" PRIu32, from, until);
    }
    if (seq24_delta(seq, until) < 0) return true;
    __atomic_store_n(&ctx->flush_deferred_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->flush_deferred_started, false, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "APAP deferred fshb completed at seq=%" PRIu32, seq);
  }
  return false;
}

static void observe_stream(rtsp_apap_observer_t *ctx, int fd) {
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
          log_hex_prefix("bad APAP length prefix", hdr, sizeof(hdr));
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
        if (package_no <= 3U) {
          log_hex_prefix("BufferedAPAP payload", packet, package_have);
          if (looks_like_apap_header(packet, package_have)) {
            ESP_LOGI(TAG,
                     "package #%" PRIu64 " header time=%" PRId64 "/%" PRIu32
                     " seq=0x%06" PRIx32,
                     package_no, (int64_t)read_be64(packet), read_be32(packet + 8),
                     read_be24(packet + 12));
          }
        }
        if (looks_like_apap_header(packet, package_have)) {
          const uint32_t seq = read_be24(packet + 12);
          if (!apap_flush_drop(ctx, seq)) {
            (void)decode_and_publish(ctx, packet, package_have, package_no);
          }
        }
      }
    }
  }

done:
  ESP_LOGI(TAG,
           "APAP client disconnected after %" PRIu64 " bytes packages=%" PRIu64
           " decoded=%" PRIu32 " published=%" PRIu32 " mode=%s",
           total_rx, package_no, ctx->decoded_packets, ctx->published_packets,
           decrypt_mode_name(ctx->decrypt_mode));
  free(rx);
  free(packet);
}

static void apap_task(void *arg) {
  rtsp_apap_observer_t *ctx = (rtsp_apap_observer_t *)arg;

  while (!ctx->stop) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx->listen_socket, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = APAP_OBSERVER_POLL_US};
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
    ESP_LOGI(TAG,
             "APAP TCP client connected port=%u useStreamEncryptionKey=%d key=%uB",
             ctx->port, ctx->uses_stream_encryption_key ? 1 : 0,
             (unsigned)ctx->stream_key_len);
    observe_stream(ctx, fd);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    ctx->client_socket = -1;
  }

  ctx->task = NULL;
  vTaskDelete(NULL);
}

esp_err_t rtsp_apap_observer_start(uint32_t expected_client_ip,
                                   bool uses_stream_encryption_key,
                                   const uint8_t *stream_key,
                                   size_t stream_key_len,
                                   rtsp_apap_observer_t **out_observer,
                                   uint16_t *out_port) {
  if (!out_observer || !out_port) return ESP_ERR_INVALID_ARG;
  if (*out_observer) rtsp_apap_observer_stop(out_observer);
  if (uses_stream_encryption_key && (!stream_key || stream_key_len != 32U)) {
    ESP_LOGE(TAG, "APAP requested stream encryption but key_len=%u",
             (unsigned)stream_key_len);
    return ESP_ERR_INVALID_ARG;
  }

  rtsp_apap_observer_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return ESP_ERR_NO_MEM;
  ctx->listen_socket = -1;
  ctx->client_socket = -1;
  ctx->expected_client_ip = expected_client_ip;
  ctx->uses_stream_encryption_key = uses_stream_encryption_key;
  if (stream_key && stream_key_len == 32U) {
    memcpy(ctx->stream_key, stream_key, 32U);
    ctx->stream_key_len = 32U;
  }

  ctx->plain = heap_caps_malloc(APAP_PACKAGE_MAX,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ctx->aac_input = heap_caps_malloc(APAP_PACKAGE_MAX + AAC_DECODER_INPUT_HEADROOM,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ctx->pcm = heap_caps_malloc(2048U * 2U * sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ctx->plain) ctx->plain = malloc(APAP_PACKAGE_MAX);
  if (!ctx->aac_input)
    ctx->aac_input = malloc(APAP_PACKAGE_MAX + AAC_DECODER_INPUT_HEADROOM);
  if (!ctx->pcm) ctx->pcm = malloc(2048U * 2U * sizeof(int16_t));
  if (!ctx->plain || !ctx->aac_input || !ctx->pcm) {
    free(ctx->plain);
    free(ctx->aac_input);
    free(ctx->pcm);
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  ctx->listen_socket = socket_utils_bind_tcp_listener(0, 1, false, &ctx->port);
  if (ctx->listen_socket < 0 || ctx->port == 0) {
    if (ctx->listen_socket >= 0) close(ctx->listen_socket);
    free(ctx->plain);
    free(ctx->aac_input);
    free(ctx->pcm);
    free(ctx);
    return ESP_FAIL;
  }

  BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
      apap_task, "apap_audio", APAP_OBSERVER_STACK, ctx, 5, &ctx->task, 0,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ret != pdPASS) {
    close(ctx->listen_socket);
    free(ctx->plain);
    free(ctx->aac_input);
    free(ctx->pcm);
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  *out_port = ctx->port;
  *out_observer = ctx;
  ESP_LOGI(TAG,
           "APAP audio endpoint ready TCP %u streamKey=%d key=%uB -> normal PCM/PTP pipeline",
           ctx->port, uses_stream_encryption_key ? 1 : 0,
           (unsigned)ctx->stream_key_len);
  return ESP_OK;
}

void rtsp_apap_observer_flush(rtsp_apap_observer_t *ctx,
                              bool have_from_seq, uint32_t from_seq,
                              bool have_until_seq, uint32_t until_seq) {
  if (!ctx) return;
  from_seq &= 0x00ffffffU;
  until_seq &= 0x00ffffffU;

  if (have_from_seq && have_until_seq && until_seq != 0U) {
    __atomic_store_n(&ctx->flush_from_seq, from_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->flush_until_seq, until_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->flush_deferred_started, false, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->flush_deferred_active, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "APAP fshb deferred armed fromSeq=%" PRIu32
                  " untilSeq=%" PRIu32, from_seq, until_seq);
    return;
  }

  if (have_until_seq && until_seq != 0U) {
    /* A later immediate fshb supersedes an earlier deferred range for the
     * same transition. Do not let the deferred boundary invalidate the fresh
     * strt/anch map again when untilSeq is eventually reached. */
    __atomic_store_n(&ctx->flush_deferred_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->flush_deferred_started, false, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->flush_immediate_until_seq, until_seq,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->flush_immediate_active, true, __ATOMIC_RELEASE);
    audio_receiver_external_buffered_flush();
    ESP_LOGI(TAG, "APAP fshb immediate armed untilSeq=%" PRIu32, until_seq);
    return;
  }

  __atomic_store_n(&ctx->flush_deferred_active, false, __ATOMIC_RELEASE);
  __atomic_store_n(&ctx->flush_deferred_started, false, __ATOMIC_RELAXED);
  __atomic_store_n(&ctx->flush_immediate_active, false, __ATOMIC_RELEASE);
  audio_receiver_external_buffered_flush();
  ESP_LOGI(TAG, "APAP fshb without usable seq -> timing/cache reset");
}

void rtsp_apap_observer_stop(rtsp_apap_observer_t **observer) {
  if (!observer || !*observer) return;
  rtsp_apap_observer_t *ctx = *observer;
  ctx->stop = true;
  if (ctx->client_socket >= 0) shutdown(ctx->client_socket, SHUT_RDWR);
  if (ctx->listen_socket >= 0) shutdown(ctx->listen_socket, SHUT_RDWR);

  int ticks = 100;
  while (ctx->task != NULL && ticks-- > 0) vTaskDelay(pdMS_TO_TICKS(10));
  if (ctx->task != NULL) {
    ESP_LOGW(TAG, "APAP audio task did not stop; context retained");
    return;
  }

  if (ctx->decoder) aac_decoder_destroy(ctx->decoder);
  sodium_memzero(ctx->stream_key, sizeof(ctx->stream_key));
  free(ctx->plain);
  free(ctx->aac_input);
  free(ctx->pcm);
  if (ctx->client_socket >= 0) close(ctx->client_socket);
  if (ctx->listen_socket >= 0) close(ctx->listen_socket);
  free(ctx);
  *observer = NULL;
}
