#include "ap2_buffered_fifo.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#if defined(ESP_PLATFORM)
#include "lwip/sockets.h"
#endif
#include <unistd.h>

#include "audio_diag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network/socket_utils.h"
#include "sodium.h"

/* Match Shairport Sync 5.x buffered_read.c: recv at most 4096 bytes, and once
 * more than 16 KiB is queued, sleep 10 ms after each recv. This intentionally
 * lets AutoMix bursts accumulate in the large compressed FIFO without making
 * the TCP reader monopolise the CPU. */
#define FIFO_RECV_CHUNK       4096U
#define FIFO_PACE_THRESHOLD  16384U
#define FIFO_PACE_SLEEP_MS      10U
#define FIFO_MIN_WIRE_LEN       14U

/* Buffered APAP (lab): the sender's TCP stream is a sequence of packages
 *   4-byte big-endian total length (including itself)
 *   15-byte clear header: mediaTime i64 BE, timescale u32 BE, seq u24 BE
 *   ciphertext || Poly1305 tag (16) || nonce (8)
 * ChaCha20-Poly1305-IETF with the stream key (shk), AAD = header bytes 0..11,
 * nonce = 4 zero bytes + the trailing 8. The plaintext is a list of
 * extensions (uintv key, uintv length, value; key 0 ends the list) followed
 * by one AAC access unit (absent in metadata-only packages).
 *
 * The reader turns each package into the classic buffered packet the
 * processor already understands -- [2-byte length][seq][rtp][ssrc][AAC] --
 * with rtp = mediaTime in samples and the AAC already in clear text, so
 * timing, FLUSH and decoding stay unchanged. */
#define APAP_HEADER_BYTES       15U
#define APAP_PACKAGE_MAX     16384U
#define APAP_AAC_MAX          8180U /* processor block limit minus header */
#define APAP_SSRC_AAC_44100 0x16000000U

static const char *TAG = "aac_fifo";

struct ap2_buffered_fifo {
  uint8_t *buffer;
  size_t capacity;
  size_t read_pos;
  size_t write_pos;
  size_t occupancy;
  uint64_t total_written;
  uint64_t total_read;


  SemaphoreHandle_t fifo_mutex;
  SemaphoreHandle_t not_full;
  SemaphoreHandle_t control_wake;

  bool connected;
  int listen_sock;
  int client_sock;
  uint16_t port;
  volatile bool running;
  TaskHandle_t reader_task;
  volatile uint32_t stream_epoch;

  int task_core;
  int task_priority;
  uint32_t task_stack;

  /* Buffered APAP mode; set only while stopped. */
  bool apap;
  uint8_t apap_key[32];
  uint8_t *apap_pkg;   /* APAP_PACKAGE_MAX, one package being received */
  uint8_t *apap_out;   /* 2 + 12 + APAP_PACKAGE_MAX, converted packet */
  uint32_t apap_packages;
  uint32_t apap_auth_failures;
};

static uint32_t next_epoch(ap2_buffered_fifo_t *fifo) {
  uint32_t v = __atomic_add_fetch(&fifo->stream_epoch, 1U, __ATOMIC_ACQ_REL);
  if (v == 0U) {
    __atomic_store_n(&fifo->stream_epoch, 1U, __ATOMIC_RELEASE);
    v = 1U;
  }
  return v;
}

static void signal_all(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  if (fifo->not_full) xSemaphoreGive(fifo->not_full);
  if (fifo->control_wake) xSemaphoreGive(fifo->control_wake);
}

void ap2_buffered_fifo_notify(ap2_buffered_fifo_t *fifo) { signal_all(fifo); }

void ap2_buffered_fifo_wait(ap2_buffered_fifo_t *fifo, uint32_t timeout_ms) {
  if (!fifo || !fifo->control_wake) return;
  TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (ticks == 0) ticks = 1;
  (void)xSemaphoreTake(fifo->control_wake, ticks);
}

static void fifo_discard_all(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  /* Any in-flight read belongs to the previous byte stream. */
  (void)next_epoch(fifo);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);
}

static uint32_t rd_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool apap_read_uintv(const uint8_t *p, size_t n, size_t *io, uint64_t *out) {
  uint64_t v = 0;
  for (unsigned count = 0; count < 10 && *io < n; ++count) {
    const uint8_t b = p[(*io)++];
    v = (v << 7) | (uint64_t)(b & 0x7fU);
    if ((b & 0x80U) == 0) {
      *out = v;
      return true;
    }
  }
  return false;
}

/* Offset of the AAC access unit after the extension list. */
static bool apap_media_offset(const uint8_t *plain, size_t n, size_t *off) {
  size_t pos = 0;
  for (unsigned items = 0; pos < n && items < 32U; ++items) {
    uint64_t key = 0, len = 0;
    if (!apap_read_uintv(plain, n, &pos, &key)) return false;
    if (key == 0) {
      *off = pos;
      return true;
    }
    if (!apap_read_uintv(plain, n, &pos, &len) || len > n - pos) return false;
    pos += (size_t)len;
  }
  return false;
}

/* Copy one complete converted packet into the ring, waiting for room.
 * Returns false when the stream ended (stop, reconnect or reset). */
static bool fifo_write_packet(ap2_buffered_fifo_t *fifo, const uint8_t *data,
                              size_t len, uint32_t epoch) {
  if (len > fifo->capacity) return true; /* cannot ever fit: drop */
  while (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) != epoch ||
        !fifo->connected) {
      xSemaphoreGive(fifo->fifo_mutex);
      return false;
    }
    if (fifo->capacity - fifo->occupancy >= len) {
      size_t first = fifo->capacity - fifo->write_pos;
      if (first > len) first = len;
      memcpy(fifo->buffer + fifo->write_pos, data, first);
      if (first < len) memcpy(fifo->buffer, data + first, len - first);
      fifo->write_pos = (fifo->write_pos + len) % fifo->capacity;
      fifo->occupancy += len;
      fifo->total_written += len;
      xSemaphoreGive(fifo->fifo_mutex);
      xSemaphoreGive(fifo->control_wake);
      return true;
    }
    xSemaphoreGive(fifo->fifo_mutex);
    (void)xSemaphoreTake(fifo->not_full, pdMS_TO_TICKS(100));
  }
  return false;
}

/* Decrypt one APAP package and queue it as a classic buffered packet.
 * Returns false only when the stream ended. */
static bool apap_convert(ap2_buffered_fifo_t *fifo, size_t pkg_len,
                         uint32_t epoch) {
  const uint8_t *pkg = fifo->apap_pkg;
  if (pkg_len < APAP_HEADER_BYTES + 24U) return true;
  const int64_t media_time = (int64_t)(((uint64_t)rd_be32(pkg) << 32) | rd_be32(pkg + 4));
  const uint32_t timescale = rd_be32(pkg + 8);
  const uint32_t seq = ((uint32_t)pkg[12] << 16) | ((uint32_t)pkg[13] << 8) | pkg[14];
  if (media_time < 0 || timescale == 0) return true;
  uint64_t sample = (uint64_t)media_time;
  if (timescale != 44100U) {
    const uint64_t whole = sample / timescale, rem = sample % timescale;
    sample = whole * 44100U + (rem * 44100U) / timescale;
  }

  const uint8_t *body = pkg + APAP_HEADER_BYTES;
  const size_t body_len = pkg_len - APAP_HEADER_BYTES;
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, body + body_len - 8U, 8);
  uint8_t *out = fifo->apap_out;
  uint8_t *plain = out + 14;
  unsigned long long plain_len = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(plain, &plain_len, NULL, body,
                                                body_len - 8U, pkg, 12, nonce,
                                                fifo->apap_key) != 0) {
    if ((fifo->apap_auth_failures++ % 100U) == 0U)
      ESP_LOGW(TAG, "APAP authentication failed seq=%lu len=%u (%lu total)",
               (unsigned long)seq, (unsigned)pkg_len,
               (unsigned long)fifo->apap_auth_failures);
    return true;
  }
  size_t off = 0;
  if (!apap_media_offset(plain, (size_t)plain_len, &off)) {
    ESP_LOGW(TAG, "APAP extension parse failed seq=%lu", (unsigned long)seq);
    return true;
  }
  const size_t aac_len = (size_t)plain_len - off;
  if (aac_len > APAP_AAC_MAX) {
    ESP_LOGW(TAG, "APAP AAC unit too large seq=%lu len=%u", (unsigned long)seq,
             (unsigned)aac_len);
    return true;
  }
  if (off) memmove(plain, plain + off, aac_len);

  const size_t wire_len = 14U + aac_len;
  const uint32_t w0 = 0x80000000U | (seq & 0x007fffffU);
  const uint32_t rtp = (uint32_t)sample;
  const uint32_t words[3] = {w0, rtp, APAP_SSRC_AAC_44100};
  out[0] = (uint8_t)(wire_len >> 8);
  out[1] = (uint8_t)wire_len;
  for (int i = 0; i < 3; i++) {
    out[2 + 4 * i] = (uint8_t)(words[i] >> 24);
    out[3 + 4 * i] = (uint8_t)(words[i] >> 16);
    out[4 + 4 * i] = (uint8_t)(words[i] >> 8);
    out[5 + 4 * i] = (uint8_t)words[i];
  }
  fifo->apap_packages++;
  if (fifo->apap_packages <= 3U || (fifo->apap_packages % 2000U) == 0U)
    ESP_LOGI(TAG, "APAP package #%lu seq=%lu mediaTime=%lld/%lu sample=%llu aac=%uB ext=%uB",
             (unsigned long)fifo->apap_packages, (unsigned long)seq,
             (long long)media_time, (unsigned long)timescale,
             (unsigned long long)sample, (unsigned)aac_len, (unsigned)off);
  return fifo_write_packet(fifo, out, wire_len, epoch);
}

/* APAP receive loop for one connected client. */
static void apap_reader_loop(ap2_buffered_fifo_t *fifo, int c, uint32_t epoch) {
  uint8_t len_bytes[4];
  size_t len_have = 0;
  size_t pkg_len = 0, pkg_have = 0;
  bool skipping = false;
  uint8_t sink[256];
  fifo->apap_packages = 0;
  fifo->apap_auth_failures = 0;

  while (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
    ssize_t n;
    if (len_have < sizeof(len_bytes)) {
      n = recv(c, len_bytes + len_have, sizeof(len_bytes) - len_have, 0);
    } else {
      size_t want = pkg_len - pkg_have;
      if (skipping) {
        if (want > sizeof(sink)) want = sizeof(sink);
        n = recv(c, sink, want, 0);
      } else {
        if (want > FIFO_RECV_CHUNK) want = FIFO_RECV_CHUNK;
        n = recv(c, fifo->apap_pkg + pkg_have, want, 0);
      }
    }
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      return;
    }
    if (len_have < sizeof(len_bytes)) {
      len_have += (size_t)n;
      if (len_have < sizeof(len_bytes)) continue;
      const uint32_t total = rd_be32(len_bytes);
      if (total <= 4U) {
        ESP_LOGW(TAG, "invalid APAP package length %lu", (unsigned long)total);
        return;
      }
      pkg_len = total - 4U;
      pkg_have = 0;
      skipping = pkg_len > APAP_PACKAGE_MAX;
      if (skipping)
        ESP_LOGW(TAG, "APAP package of %u bytes skipped", (unsigned)pkg_len);
      continue;
    }
    pkg_have += (size_t)n;
    if (pkg_have < pkg_len) continue;
    len_have = 0;
    if (!skipping && !apap_convert(fifo, pkg_len, epoch)) return;
    skipping = false;

    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    const bool have_time_to_sleep = fifo->occupancy > FIFO_PACE_THRESHOLD;
    xSemaphoreGive(fifo->fifo_mutex);
    if (have_time_to_sleep) {
      TickType_t ticks = pdMS_TO_TICKS(FIFO_PACE_SLEEP_MS);
      if (ticks == 0) ticks = 1;
      vTaskDelay(ticks);
    }
  }
}

static void tcp_reader_task(void *arg) {
  ap2_buffered_fifo_t *fifo = (ap2_buffered_fifo_t *)arg;
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_TCP_READER,
                                    xPortGetCoreID(), fifo->task_priority, 0U);

  while (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    int c = accept(fifo->listen_sock, (struct sockaddr *)&addr, &alen);
    if (c < 0) {
      if (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR)
        ESP_LOGW(TAG, "accept errno=%d", errno);
      if (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    /* A newly accepted connection is a fresh sequential byte stream. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    fifo->read_pos = 0;
    fifo->write_pos = 0;
    fifo->occupancy = 0;
    fifo->total_written = 0;
    fifo->total_read = 0;
    fifo->connected = true;
    fifo->client_sock = c;
    const uint32_t conn_epoch = next_epoch(fifo);
    xSemaphoreGive(fifo->fifo_mutex);

    ESP_LOGI(TAG, "buffered TCP connected fifo=%uKiB%s",
             (unsigned)(fifo->capacity / 1024U), fifo->apap ? " (APAP)" : "");
    signal_all(fifo);

    if (fifo->apap) apap_reader_loop(fifo, c, conn_epoch);

    while (!fifo->apap && __atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
      size_t write_pos = 0;
      size_t request = 0;
      uint32_t write_epoch = 0;

      xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
      if (fifo->occupancy == fifo->capacity) {
        xSemaphoreGive(fifo->fifo_mutex);
        (void)xSemaphoreTake(fifo->not_full, pdMS_TO_TICKS(100));
        continue;
      }

      const size_t free_bytes = fifo->capacity - fifo->occupancy;
      request = free_bytes < FIFO_RECV_CHUNK ? free_bytes : FIFO_RECV_CHUNK;
      const size_t to_end = fifo->capacity - fifo->write_pos;
      if (request > to_end) request = to_end;
      write_pos = fifo->write_pos;
      write_epoch = __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
      xSemaphoreGive(fifo->fifo_mutex);

      const ssize_t n = recv(c, fifo->buffer + write_pos, request, 0);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
          continue;
        break;
      }

      xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
      if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) == write_epoch &&
          fifo->connected) {
        fifo->write_pos += (size_t)n;
        if (fifo->write_pos == fifo->capacity) fifo->write_pos = 0;
        fifo->occupancy += (size_t)n;
        fifo->total_written += (uint64_t)n;
      }
      const bool have_time_to_sleep = fifo->occupancy > FIFO_PACE_THRESHOLD;
      xSemaphoreGive(fifo->fifo_mutex);
      xSemaphoreGive(fifo->control_wake);

      if (have_time_to_sleep) {
        TickType_t ticks = pdMS_TO_TICKS(FIFO_PACE_SLEEP_MS);
        if (ticks == 0) ticks = 1;
        vTaskDelay(ticks);
      }
    }

    /* The reader owns close(); detach the shared descriptor before closing it
     * so stop/abort can never shutdown a recycled lwIP descriptor. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    if (fifo->client_sock == c) fifo->client_sock = -1;
    fifo->connected = false;
    fifo->read_pos = fifo->write_pos;
    fifo->occupancy = 0;
    fifo->total_read = fifo->total_written;
    (void)next_epoch(fifo);
    xSemaphoreGive(fifo->fifo_mutex);

    (void)shutdown(c, SHUT_RDWR);
    close(c);
    signal_all(fifo);
    ESP_LOGI(TAG, "buffered TCP disconnected");
  }

  __atomic_store_n(&fifo->reader_task, NULL, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

esp_err_t ap2_buffered_fifo_create_with_storage(
    ap2_buffered_fifo_t **out, const ap2_buffered_fifo_config_t *cfg,
    void *storage, size_t storage_bytes) {
  if (!out || !cfg || !storage || cfg->buffer_bytes < 16384U ||
      storage_bytes < cfg->buffer_bytes)
    return ESP_ERR_INVALID_ARG;

  ap2_buffered_fifo_t *fifo = calloc(1, sizeof(*fifo));
  if (!fifo) return ESP_ERR_NO_MEM;
  fifo->buffer = (uint8_t *)storage;
  fifo->capacity = cfg->buffer_bytes;
  fifo->task_core = cfg->task_core;
  fifo->task_priority = cfg->task_priority;
  fifo->task_stack = cfg->task_stack;
  fifo->listen_sock = -1;
  fifo->client_sock = -1;
  fifo->stream_epoch = 1U;
  fifo->fifo_mutex = xSemaphoreCreateMutex();
  fifo->not_full = xSemaphoreCreateBinary();
  fifo->control_wake = xSemaphoreCreateBinary();
  if (!fifo->fifo_mutex || !fifo->not_full ||
      !fifo->control_wake) {
    (void)ap2_buffered_fifo_destroy(fifo);
    return ESP_ERR_NO_MEM;
  }
  *out = fifo;
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_destroy(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return ESP_OK;
  if (fifo->fifo_mutex) {
    ap2_buffered_fifo_stop(fifo);
  } else {
    __atomic_store_n(&fifo->running, false, __ATOMIC_RELEASE);
  }
  if (__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE)) {
    ESP_LOGE(TAG, "destroy refused: TCP reader still active");
    return ESP_ERR_TIMEOUT;
  }
  if (fifo->fifo_mutex) vSemaphoreDelete(fifo->fifo_mutex);
  if (fifo->not_full) vSemaphoreDelete(fifo->not_full);
  if (fifo->control_wake) vSemaphoreDelete(fifo->control_wake);
  heap_caps_free(fifo->apap_pkg);
  heap_caps_free(fifo->apap_out);
  sodium_memzero(fifo->apap_key, sizeof(fifo->apap_key));
  free(fifo);
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_start(ap2_buffered_fifo_t *fifo,
                                  uint16_t requested_port,
                                  uint16_t *bound_port) {
  if (!fifo) return ESP_ERR_INVALID_ARG;
  if (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
    if (bound_port) *bound_port = fifo->port;
    return ESP_OK;
  }
  if (__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE))
    return ESP_ERR_INVALID_STATE;

  uint16_t bound = requested_port;
  fifo->listen_sock =
      socket_utils_bind_tcp_listener(requested_port, 1, true, &bound);
  if (fifo->listen_sock < 0) return ESP_FAIL;
  fifo->port = bound;
  __atomic_store_n(&fifo->running, true, __ATOMIC_RELEASE);
  if (xTaskCreatePinnedToCore(tcp_reader_task, "aac_fifo_rx", fifo->task_stack,
                              fifo, fifo->task_priority, &fifo->reader_task,
                              fifo->task_core) != pdPASS) {
    __atomic_store_n(&fifo->running, false, __ATOMIC_RELEASE);
    close(fifo->listen_sock);
    fifo->listen_sock = -1;
    return ESP_FAIL;
  }
  if (bound_port) *bound_port = bound;
  return ESP_OK;
}

void ap2_buffered_fifo_stop(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  __atomic_store_n(&fifo->running, false, __ATOMIC_RELEASE);

  int client = -1;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  client = fifo->client_sock;
  fifo->client_sock = -1;
  fifo->connected = false;
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  if (client >= 0) (void)shutdown(client, SHUT_RDWR);
  (void)next_epoch(fifo);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);

  if (fifo->listen_sock >= 0) shutdown(fifo->listen_sock, SHUT_RDWR);
  for (int i = 0;
       __atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) && i < 100; ++i)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (!__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) &&
      fifo->listen_sock >= 0) {
    close(fifo->listen_sock);
    fifo->listen_sock = -1;
  }
  fifo->port = 0;
}

esp_err_t ap2_buffered_fifo_set_apap(ap2_buffered_fifo_t *fifo,
                                     const uint8_t *key32) {
  if (!fifo) return ESP_ERR_INVALID_ARG;
  if (!ap2_buffered_fifo_is_idle(fifo)) return ESP_ERR_INVALID_STATE;
  if (!key32) {
    fifo->apap = false;
    sodium_memzero(fifo->apap_key, sizeof(fifo->apap_key));
    /* Keep the buffers for the next APAP session. */
    return ESP_OK;
  }
  if (!fifo->apap_pkg)
    fifo->apap_pkg = heap_caps_malloc(APAP_PACKAGE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fifo->apap_out)
    fifo->apap_out = heap_caps_malloc(14U + APAP_PACKAGE_MAX,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fifo->apap_pkg || !fifo->apap_out) return ESP_ERR_NO_MEM;
  memcpy(fifo->apap_key, key32, sizeof(fifo->apap_key));
  fifo->apap = true;
  return ESP_OK;
}

bool ap2_buffered_fifo_is_apap(const ap2_buffered_fifo_t *fifo) {
  return fifo && fifo->apap;
}

bool ap2_buffered_fifo_is_idle(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return true;
  return !__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) == NULL;
}

void ap2_buffered_fifo_clear(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  /* FLUSHBUFFERED never calls this. It is reserved for stopped session/codec
   * boundaries, so a hard byte reset cannot split a live framed packet. */
  fifo_discard_all(fifo);
}

/* Abort only the current buffered TCP client after fatal framing corruption.
 * The listening socket remains available for the RTSP session. */
static void ap2_buffered_fifo_abort_client(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;

  int client = -1;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  client = fifo->client_sock;
  fifo->client_sock = -1;
  fifo->connected = false;
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  (void)next_epoch(fifo);
  if (client >= 0) (void)shutdown(client, SHUT_RDWR);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);
}

size_t ap2_buffered_fifo_capacity(const ap2_buffered_fifo_t *fifo) {
  return fifo ? fifo->capacity : 0U;
}

bool ap2_buffered_fifo_epoch_is_current(const ap2_buffered_fifo_t *fifo,
                                        uint32_t stream_epoch) {
  return fifo && __atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) && stream_epoch ==
      __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
}

void ap2_buffered_fifo_get_usage(ap2_buffered_fifo_t *fifo,
                                 ap2_buffered_fifo_usage_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!fifo) return;

  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  out->capacity_bytes = fifo->capacity;
  out->used_bytes = fifo->occupancy;
  out->bytes_received = fifo->total_written;
  xSemaphoreGive(fifo->fifo_mutex);

}

/* Called with fifo_mutex held. No cursor or media-policy change. */
static void fifo_copy_at(const ap2_buffered_fifo_t *fifo, size_t pos,
                         uint8_t *dst, size_t len) {
  size_t first = fifo->capacity - pos;
  if (first > len) first = len;
  memcpy(dst, fifo->buffer + pos, first);
  if (first < len) memcpy(dst + first, fifo->buffer, len - first);
}

esp_err_t ap2_buffered_fifo_peek_head(ap2_buffered_fifo_t *fifo,
                                      size_t block_capacity,
                                      ap2_buffered_fifo_head_t *head) {
  if (!fifo || !head || block_capacity < sizeof(head->header))
    return ESP_ERR_INVALID_ARG;

  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  if (!__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
    xSemaphoreGive(fifo->fifo_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (fifo->occupancy < 2U) {
    xSemaphoreGive(fifo->fifo_mutex);
    return ESP_ERR_TIMEOUT;
  }
  uint8_t length_bytes[2];
  fifo_copy_at(fifo, fifo->read_pos, length_bytes, sizeof(length_bytes));
  const uint16_t wire_len = ((uint16_t)length_bytes[0] << 8) | length_bytes[1];
  if (wire_len < FIFO_MIN_WIRE_LEN || wire_len > fifo->capacity ||
      (size_t)wire_len - 2U > block_capacity) {
    xSemaphoreGive(fifo->fifo_mutex);
    ESP_LOGW(TAG, "invalid buffered block length=%u; aborting client to restore framing",
             (unsigned)wire_len);
    ap2_buffered_fifo_abort_client(fifo);
    return ESP_ERR_INVALID_SIZE;
  }
  if (fifo->occupancy < wire_len) {
    xSemaphoreGive(fifo->fifo_mutex);
    return ESP_ERR_TIMEOUT;
  }

  head->wire_len = wire_len;
  head->stream_epoch = __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
  head->read_pos = fifo->read_pos;
  head->read_serial = fifo->total_read;
  fifo_copy_at(fifo, (fifo->read_pos + 2U) % fifo->capacity,
               head->header, sizeof(head->header));
  xSemaphoreGive(fifo->fifo_mutex);
  return ESP_OK;
}

/* Caller holds fifo_mutex. */
static bool fifo_head_is_current_locked(const ap2_buffered_fifo_t *fifo,
                                         const ap2_buffered_fifo_head_t *head) {
  return __atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) && head->stream_epoch ==
      __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) &&
      head->read_pos == fifo->read_pos && head->read_serial == fifo->total_read &&
      head->wire_len >= FIFO_MIN_WIRE_LEN && head->wire_len <= fifo->occupancy;
}

esp_err_t ap2_buffered_fifo_visit_head(ap2_buffered_fifo_t *fifo,
                                       const ap2_buffered_fifo_head_t *head,
                                       ap2_buffered_fifo_head_visit_t visit,
                                       void *context) {
  if (!fifo || !head || !visit) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  const bool valid = fifo_head_is_current_locked(fifo, head);
  if (valid) visit(context);
  xSemaphoreGive(fifo->fifo_mutex);
  return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t fifo_consume_head(ap2_buffered_fifo_t *fifo,
                                    const ap2_buffered_fifo_head_t *head,
                                    uint8_t *dst, size_t capacity) {
  if (!fifo || !head || head->wire_len < FIFO_MIN_WIRE_LEN)
    return ESP_ERR_INVALID_ARG;
  if (dst && (size_t)head->wire_len - 2U > capacity)
    return ESP_ERR_INVALID_SIZE;

  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  if (!fifo_head_is_current_locked(fifo, head)) {
    xSemaphoreGive(fifo->fifo_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (dst) {
    fifo_copy_at(fifo, (fifo->read_pos + 2U) % fifo->capacity,
                 dst, (size_t)head->wire_len - 2U);
  }
  fifo->read_pos = (fifo->read_pos + head->wire_len) % fifo->capacity;
  fifo->occupancy -= head->wire_len;
  fifo->total_read += head->wire_len;
  xSemaphoreGive(fifo->fifo_mutex);
  xSemaphoreGive(fifo->not_full);
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_discard_head(ap2_buffered_fifo_t *fifo,
                                         const ap2_buffered_fifo_head_t *head) {
  return fifo_consume_head(fifo, head, NULL, 0U);
}

esp_err_t ap2_buffered_fifo_read_head(ap2_buffered_fifo_t *fifo,
                                      const ap2_buffered_fifo_head_t *head,
                                      uint8_t *block_storage,
                                      size_t block_capacity) {
  if (!block_storage) return ESP_ERR_INVALID_ARG;
  return fifo_consume_head(fifo, head, block_storage, block_capacity);
}
