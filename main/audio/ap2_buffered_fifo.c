#include "ap2_buffered_fifo.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#if defined(ESP_PLATFORM)
#include "lwip/sockets.h"
#endif
#include <unistd.h>

#include "audio_diag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network/socket_utils.h"

/* Match Shairport Sync 5.x buffered_read.c: recv at most 4096 bytes, and once
 * more than 16 KiB is queued, sleep 10 ms after each recv. This intentionally
 * lets AutoMix bursts accumulate in the large compressed FIFO without making
 * the TCP reader monopolise the CPU. */
#define FIFO_RECV_CHUNK       4096U
#define FIFO_PACE_THRESHOLD  16384U
#define FIFO_PACE_SLEEP_MS      10U
#define FIFO_MIN_WIRE_LEN       14U

static const char *TAG = "aac_fifo";

struct ap2_buffered_fifo {
  uint8_t *buffer;
  size_t capacity;
  size_t read_pos;
  size_t write_pos;
  size_t occupancy;
  uint64_t total_written; /* bytes accepted on the current connection */

  SemaphoreHandle_t fifo_mutex;
  SemaphoreHandle_t not_empty;
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

  /* Reader diagnostics, written only by the reader task. */
  volatile uint32_t rx_state;
  volatile uint32_t rx_state_since_ms;
  volatile uint32_t rx_last_byte_ms;
  volatile uint32_t rx_recv_timeouts;
};

static inline uint32_t now_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline void set_rx_state(ap2_buffered_fifo_t *fifo,
                                ap2_fifo_rx_state_t st) {
  if (fifo->rx_state == (uint32_t)st) return;
  fifo->rx_state_since_ms = now_ms();
  fifo->rx_state = (uint32_t)st;
}

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
  if (fifo->not_empty) xSemaphoreGive(fifo->not_empty);
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

static inline void fifo_advance_locked(ap2_buffered_fifo_t *fifo, size_t n) {
  fifo->read_pos += n;
  if (fifo->read_pos >= fifo->capacity) fifo->read_pos -= fifo->capacity;
  fifo->occupancy -= n;
}

/* Copy `len` bytes starting `offset` bytes after read_pos (wrap-aware). */
static inline void fifo_peek_locked(const ap2_buffered_fifo_t *fifo,
                                    size_t offset, uint8_t *dst, size_t len) {
  size_t pos = fifo->read_pos + offset;
  if (pos >= fifo->capacity) pos -= fifo->capacity;
  const size_t first = len < fifo->capacity - pos ? len : fifo->capacity - pos;
  memcpy(dst, fifo->buffer + pos, first);
  if (first < len) memcpy(dst + first, fifo->buffer, len - first);
}

static void fifo_discard_all(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  /* Any in-flight read belongs to the previous byte stream. */
  (void)next_epoch(fifo);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);
}

/* Consume exactly `len` bytes in stream order. dst == NULL consumes them
 * without copying (the processor has already decided to drop the block).
 * Returns false if the connection ended or the stream epoch changed. */
static bool fifo_consume_exact(ap2_buffered_fifo_t *fifo, uint8_t *dst,
                               size_t len, uint32_t expected_epoch) {
  size_t done = 0;
  while (done < len && fifo->running) {
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) != expected_epoch) {
      xSemaphoreGive(fifo->fifo_mutex);
      return false;
    }
    if (fifo->occupancy == 0U) {
      const bool connected = fifo->connected;
      xSemaphoreGive(fifo->fifo_mutex);
      if (!connected) return false;
      (void)xSemaphoreTake(fifo->not_empty, pdMS_TO_TICKS(20));
      continue;
    }

    size_t n = len - done;
    if (n > fifo->occupancy) n = fifo->occupancy;
    if (dst) {
      const size_t to_end = fifo->capacity - fifo->read_pos;
      if (n > to_end) n = to_end;
      memcpy(dst + done, fifo->buffer + fifo->read_pos, n);
    }
    fifo_advance_locked(fifo, n);
    xSemaphoreGive(fifo->fifo_mutex);
    done += n;
    xSemaphoreGive(fifo->not_full);
  }
  return done == len;
}

static bool wait_for_connected_epoch(ap2_buffered_fifo_t *fifo,
                                     uint32_t *epoch) {
  while (fifo->running) {
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    const bool connected = fifo->connected;
    const size_t occupancy = fifo->occupancy;
    const uint32_t ep =
        __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
    xSemaphoreGive(fifo->fifo_mutex);
    if (connected || occupancy != 0U) {
      *epoch = ep;
      return true;
    }
    (void)xSemaphoreTake(fifo->not_empty, pdMS_TO_TICKS(20));
  }
  return false;
}

static void tcp_reader_task(void *arg) {
  ap2_buffered_fifo_t *fifo = (ap2_buffered_fifo_t *)arg;
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_TCP_READER,
                                    xPortGetCoreID(), fifo->task_priority, 0U);

  while (fifo->running) {
    set_rx_state(fifo, AP2_FIFO_RX_ACCEPT);
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    int c = accept(fifo->listen_sock, (struct sockaddr *)&addr, &alen);
    if (c < 0) {
      if (fifo->running && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR)
        ESP_LOGW(TAG, "accept errno=%d", errno);
      if (fifo->running) vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    /* A newly accepted connection is a fresh sequential byte stream. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    fifo->read_pos = 0;
    fifo->write_pos = 0;
    fifo->occupancy = 0;
    fifo->total_written = 0;
    fifo->connected = true;
    fifo->client_sock = c;
    (void)next_epoch(fifo);
    fifo->rx_last_byte_ms = now_ms();
    fifo->rx_recv_timeouts = 0;
    xSemaphoreGive(fifo->fifo_mutex);

    ESP_LOGI(TAG, "buffered TCP connected fifo=%uKiB",
             (unsigned)(fifo->capacity / 1024U));
    signal_all(fifo);

    bool peer_closed = false;
    while (fifo->running) {
      size_t write_pos = 0;
      size_t request = 0;
      uint32_t write_epoch = 0;

      xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
      if (fifo->occupancy == fifo->capacity) {
        xSemaphoreGive(fifo->fifo_mutex);
        set_rx_state(fifo, AP2_FIFO_RX_WAIT_SPACE);
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

      set_rx_state(fifo, AP2_FIFO_RX_RECV);
      const ssize_t n = recv(c, fifo->buffer + write_pos, request, 0);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
          fifo->rx_recv_timeouts++;
          continue;
        }
        peer_closed = n == 0;
        break;
      }

      fifo->rx_last_byte_ms = now_ms();
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
      xSemaphoreGive(fifo->not_empty);

      if (have_time_to_sleep) {
        set_rx_state(fifo, AP2_FIFO_RX_PACE);
        TickType_t ticks = pdMS_TO_TICKS(FIFO_PACE_SLEEP_MS);
        if (ticks == 0) ticks = 1;
        vTaskDelay(ticks);
      }
    }

    /* The reader owns close(); detach the shared descriptor before closing it
     * so stop/abort can never shutdown a recycled lwIP descriptor.
     *
     * Orderly end of stream (FIN from the sender, connection still ours):
     * like Shairport's buffered_read(), bytes already received stay readable
     * until the processor has consumed them. Only stop/abort/errors, or the
     * next accepted connection, discard them. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    const bool keep_backlog =
        peer_closed && fifo->running && fifo->connected && fifo->client_sock == c;
    if (fifo->client_sock == c) fifo->client_sock = -1;
    fifo->connected = false;
    if (!keep_backlog) {
      fifo->read_pos = fifo->write_pos;
      fifo->occupancy = 0;
      (void)next_epoch(fifo);
    }
    const size_t kept = fifo->occupancy;
    xSemaphoreGive(fifo->fifo_mutex);

    (void)shutdown(c, SHUT_RDWR);
    close(c);
    signal_all(fifo);
    if (kept)
      ESP_LOGI(TAG, "buffered TCP closed by sender; %u bytes left to play",
               (unsigned)kept);
    else
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
  fifo->not_empty = xSemaphoreCreateBinary();
  fifo->not_full = xSemaphoreCreateBinary();
  fifo->control_wake = xSemaphoreCreateBinary();
  if (!fifo->fifo_mutex || !fifo->not_empty || !fifo->not_full ||
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
    fifo->running = false;
  }
  if (__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE)) {
    ESP_LOGE(TAG, "destroy refused: TCP reader still active");
    return ESP_ERR_TIMEOUT;
  }
  if (fifo->fifo_mutex) vSemaphoreDelete(fifo->fifo_mutex);
  if (fifo->not_empty) vSemaphoreDelete(fifo->not_empty);
  if (fifo->not_full) vSemaphoreDelete(fifo->not_full);
  if (fifo->control_wake) vSemaphoreDelete(fifo->control_wake);
  free(fifo);
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_start(ap2_buffered_fifo_t *fifo,
                                  uint16_t requested_port,
                                  uint16_t *bound_port) {
  if (!fifo) return ESP_ERR_INVALID_ARG;
  if (fifo->running) {
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
  fifo->running = true;
  if (xTaskCreatePinnedToCore(tcp_reader_task, "aac_fifo_rx", fifo->task_stack,
                              fifo, fifo->task_priority, &fifo->reader_task,
                              fifo->task_core) != pdPASS) {
    fifo->running = false;
    close(fifo->listen_sock);
    fifo->listen_sock = -1;
    return ESP_FAIL;
  }
  if (bound_port) *bound_port = bound;
  return ESP_OK;
}

void ap2_buffered_fifo_stop(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  fifo->running = false;

  int client = -1;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  client = fifo->client_sock;
  fifo->client_sock = -1;
  fifo->connected = false;
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  if (client >= 0) (void)shutdown(client, SHUT_RDWR);
  (void)next_epoch(fifo);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);

  if (fifo->listen_sock >= 0) {
    shutdown(fifo->listen_sock, SHUT_RDWR);
    close(fifo->listen_sock);
    fifo->listen_sock = -1;
  }
  for (int i = 0;
       __atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) && i < 100; ++i)
    vTaskDelay(pdMS_TO_TICKS(10));
  fifo->port = 0;
}

bool ap2_buffered_fifo_is_idle(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return true;
  return !fifo->running &&
         __atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) == NULL;
}

void ap2_buffered_fifo_clear(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  /* FLUSHBUFFERED never calls this. It is reserved for stopped session/codec
   * boundaries, so a hard byte reset cannot split a live framed packet. */
  fifo_discard_all(fifo);
}

void ap2_buffered_fifo_abort_client(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;

  int client = -1;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  client = fifo->client_sock;
  fifo->client_sock = -1;
  fifo->connected = false;
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  (void)next_epoch(fifo);
  if (client >= 0) (void)shutdown(client, SHUT_RDWR);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);
}

const char *ap2_buffered_fifo_rx_state_name(ap2_fifo_rx_state_t state) {
  switch (state) {
  case AP2_FIFO_RX_ACCEPT: return "accept";
  case AP2_FIFO_RX_RECV: return "recv";
  case AP2_FIFO_RX_WAIT_SPACE: return "wait-space";
  case AP2_FIFO_RX_PACE: return "pace";
  default: return "?";
  }
}

void ap2_buffered_fifo_get_rx_diag(ap2_buffered_fifo_t *fifo,
                                   ap2_buffered_fifo_rx_diag_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->socket_unread = -1;
  if (!fifo || !fifo->fifo_mutex) return;

  const uint32_t t = now_ms();
  const uint32_t state = fifo->rx_state;
  const uint32_t since = fifo->rx_state_since_ms;
  const uint32_t last = fifo->rx_last_byte_ms;
  out->state = (ap2_fifo_rx_state_t)state;
  out->state_ms = t - since;
  out->idle_ms = t - last;
  out->recv_timeouts = fifo->rx_recv_timeouts;

  /* The mutex keeps client_sock from being closed and recycled meanwhile
   * (the reader detaches it under this mutex before close()). FIONREAD only
   * reads lwIP's receive counters and never blocks. */
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  out->connected = fifo->connected;
  if (fifo->connected && fifo->client_sock >= 0) {
    int avail = 0;
    if (ioctl(fifo->client_sock, FIONREAD, &avail) == 0) out->socket_unread = avail;
  }
  xSemaphoreGive(fifo->fifo_mutex);
}

size_t ap2_buffered_fifo_capacity(const ap2_buffered_fifo_t *fifo) {
  return fifo ? fifo->capacity : 0U;
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

esp_err_t ap2_buffered_fifo_read_block_head(ap2_buffered_fifo_t *fifo,
                                            uint8_t *head, size_t head_len,
                                            size_t block_capacity,
                                            size_t *block_len,
                                            uint32_t *stream_epoch) {
  if (!fifo || !head || !block_len || !stream_epoch || head_len == 0U ||
      block_capacity < head_len)
    return ESP_ERR_INVALID_ARG;

  for (;;) {
    if (!fifo->running) return ESP_ERR_INVALID_STATE;
    uint32_t epoch = 0;
    if (!wait_for_connected_epoch(fifo, &epoch)) return ESP_ERR_INVALID_STATE;

    uint8_t length_bytes[2];
    bool have_head = false;

    /* Fast path: the length prefix and the block head are normally already
     * queued, so take them with a single lock. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) == epoch &&
        fifo->occupancy >= 2U + head_len) {
      fifo_peek_locked(fifo, 0U, length_bytes, 2U);
      fifo_peek_locked(fifo, 2U, head, head_len);
      fifo_advance_locked(fifo, 2U + head_len);
      have_head = true;
    }
    xSemaphoreGive(fifo->fifo_mutex);

    if (have_head) {
      xSemaphoreGive(fifo->not_full);
    } else if (!fifo_consume_exact(fifo, length_bytes, 2U, epoch)) {
      continue; /* connection ended or epoch changed: start over */
    }

    /* Validate the length before waiting for the rest of the head: a corrupt
     * prefix must abort the client, not wait for bytes that may never come. */
    const size_t wire_len = ((size_t)length_bytes[0] << 8) | length_bytes[1];
    if (wire_len < 2U + head_len || wire_len < FIFO_MIN_WIRE_LEN ||
        wire_len > block_capacity + 2U) {
      ESP_LOGW(TAG,
               "invalid buffered block length=%u; aborting client to restore framing",
               (unsigned)wire_len);
      ap2_buffered_fifo_abort_client(fifo);
      return ESP_ERR_INVALID_SIZE;
    }
    if (!have_head && !fifo_consume_exact(fifo, head, head_len, epoch)) {
      continue;
    }

    *block_len = wire_len - 2U;
    *stream_epoch = epoch;
    return ESP_OK;
  }
}

esp_err_t ap2_buffered_fifo_read_block_rest(ap2_buffered_fifo_t *fifo,
                                            uint8_t *dst, size_t len,
                                            uint32_t stream_epoch) {
  if (!fifo) return ESP_ERR_INVALID_ARG;
  if (len == 0U) return ESP_OK;
  return fifo_consume_exact(fifo, dst, len, stream_epoch) ? ESP_OK
                                                          : ESP_ERR_INVALID_STATE;
}
