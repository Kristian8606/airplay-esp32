#include "ap2_buffered_transport.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network/socket_utils.h"

#define AP2_TCP_READ_CHUNK 4096U

static const char *TAG = "ap2_tcp_fifo";

struct ap2_buffered_transport {
  uint8_t *fifo;
  size_t size;
  size_t rd;
  size_t wr;
  size_t used;
  size_t high_water;

  SemaphoreHandle_t mutex;
  SemaphoreHandle_t data_ready;
  SemaphoreHandle_t space_ready;

  int listen_sock;
  int client_sock;
  uint16_t port;
  volatile bool running;
  volatile bool peer_closed;
  volatile int peer_error;
  TaskHandle_t reader_task;

  int task_core;
  int task_priority;
  uint32_t task_stack;

  uint64_t socket_bytes;
  uint64_t fifo_bytes_read;
};

static void fifo_reset(ap2_buffered_transport_t *t) {
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  t->rd = t->wr = t->used = 0;
  t->peer_closed = false;
  t->peer_error = 0;
  xSemaphoreGive(t->mutex);
}

static bool fifo_write_all(ap2_buffered_transport_t *t, const uint8_t *src,
                           size_t len) {
  size_t off = 0;
  while (off < len && t->running) {
    xSemaphoreTake(t->mutex, portMAX_DELAY);
    const size_t limit = t->size;
    size_t free_bytes = t->used < limit ? limit - t->used : 0;
    if (free_bytes == 0) {
      xSemaphoreGive(t->mutex);
      xSemaphoreTake(t->space_ready, pdMS_TO_TICKS(100));
      continue;
    }
    size_t n = len - off;
    if (n > free_bytes) n = free_bytes;
    size_t contiguous = t->size - t->wr;
    if (n > contiguous) n = contiguous;
    memcpy(t->fifo + t->wr, src + off, n);
    t->wr = (t->wr + n) % t->size;
    t->used += n;
    if (t->used > t->high_water) t->high_water = t->used;
    xSemaphoreGive(t->mutex);
    xSemaphoreGive(t->data_ready);
    off += n;
  }
  return off == len;
}

static void tcp_reader_task(void *arg) {
  ap2_buffered_transport_t *t = (ap2_buffered_transport_t *)arg;
  uint8_t *scratch = heap_caps_malloc(AP2_TCP_READ_CHUNK,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!scratch) scratch = malloc(AP2_TCP_READ_CHUNK);
  if (!scratch) {
    ESP_LOGE(TAG, "reader scratch allocation failed");
    t->running = false;
    t->reader_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "reader task core=%d fifo=%u KiB", xPortGetCoreID(),
           (unsigned)(t->size / 1024U));

  while (t->running) {
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    int c = accept(t->listen_sock, (struct sockaddr *)&addr, &alen);
    if (c < 0) {
      if (t->running && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "accept errno=%d", errno);
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    fifo_reset(t);
    t->client_sock = c;
    ESP_LOGI(TAG, "buffered TCP connected");

    while (t->running) {
      /* Do not read from TCP while our bounded FIFO is full. This is the
       * AirPlay-2 backpressure point: the sender is throttled by the TCP
       * receive window instead of dropping future audio. */
      xSemaphoreTake(t->mutex, portMAX_DELAY);
      bool full = (t->used >= t->size);
      xSemaphoreGive(t->mutex);
      if (full) {
          xSemaphoreTake(t->space_ready, pdMS_TO_TICKS(100));
        continue;
      }

      ssize_t n = recv(c, scratch, AP2_TCP_READ_CHUNK, 0);
      if (n > 0) {
        t->socket_bytes += (uint64_t)n;
        if (!fifo_write_all(t, scratch, (size_t)n)) break;
      } else {
        xSemaphoreTake(t->mutex, portMAX_DELAY);
        if (n == 0) t->peer_closed = true;
        else t->peer_error = errno;
        xSemaphoreGive(t->mutex);
        xSemaphoreGive(t->data_ready);
        break;
      }
    }

    shutdown(c, SHUT_RDWR);
    close(c);
    t->client_sock = -1;
    ESP_LOGI(TAG, "buffered TCP disconnected");
  }

  free(scratch);
  t->reader_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t ap2_buffered_transport_create(ap2_buffered_transport_t **out,
                                        const ap2_buffered_transport_config_t *cfg) {
  if (!out || !cfg || cfg->fifo_bytes < 16384U) return ESP_ERR_INVALID_ARG;
  ap2_buffered_transport_t *t = calloc(1, sizeof(*t));
  if (!t) return ESP_ERR_NO_MEM;
  t->fifo = heap_caps_malloc(cfg->fifo_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!t->fifo) t->fifo = malloc(cfg->fifo_bytes);
  t->mutex = xSemaphoreCreateMutex();
  t->data_ready = xSemaphoreCreateBinary();
  t->space_ready = xSemaphoreCreateBinary();
  if (!t->fifo || !t->mutex || !t->data_ready || !t->space_ready) {
    ap2_buffered_transport_destroy(t);
    return ESP_ERR_NO_MEM;
  }
  t->size = cfg->fifo_bytes;
  t->listen_sock = -1;
  t->client_sock = -1;
  t->task_core = cfg->task_core;
  t->task_priority = cfg->task_priority;
  t->task_stack = cfg->task_stack;
  *out = t;
  return ESP_OK;
}

void ap2_buffered_transport_destroy(ap2_buffered_transport_t *t) {
  if (!t) return;
  ap2_buffered_transport_stop(t);
  if (t->mutex) vSemaphoreDelete(t->mutex);
  if (t->data_ready) vSemaphoreDelete(t->data_ready);
  if (t->space_ready) vSemaphoreDelete(t->space_ready);
  free(t->fifo);
  free(t);
}

esp_err_t ap2_buffered_transport_start(ap2_buffered_transport_t *t,
                                       uint16_t requested_port,
                                       uint16_t *bound_port) {
  if (!t) return ESP_ERR_INVALID_ARG;
  if (t->running) {
    if (bound_port) *bound_port = t->port;
    return ESP_OK;
  }
  uint16_t bound = requested_port;
  t->listen_sock = socket_utils_bind_tcp_listener(requested_port, 1, true, &bound);
  if (t->listen_sock < 0) return ESP_FAIL;
  t->port = bound;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  t->high_water = t->used;
  xSemaphoreGive(t->mutex);
  t->running = true;
  if (xTaskCreatePinnedToCore(tcp_reader_task, "ap2_tcp_reader", t->task_stack,
                              t, t->task_priority, &t->reader_task,
                              t->task_core) != pdPASS) {
    t->running = false;
    close(t->listen_sock);
    t->listen_sock = -1;
    return ESP_FAIL;
  }
  if (bound_port) *bound_port = bound;
  return ESP_OK;
}

void ap2_buffered_transport_clear(ap2_buffered_transport_t *t) {
  if (!t) return;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  t->rd = t->wr = t->used = 0;
  xSemaphoreGive(t->mutex);
  xSemaphoreGive(t->space_ready);
}

void ap2_buffered_transport_stop(ap2_buffered_transport_t *t) {
  if (!t) return;
  t->running = false;
  if (t->client_sock >= 0) {
    shutdown(t->client_sock, SHUT_RDWR);
  }
  if (t->listen_sock >= 0) {
    shutdown(t->listen_sock, SHUT_RDWR);
    close(t->listen_sock);
    t->listen_sock = -1;
  }
  xSemaphoreGive(t->data_ready);
  xSemaphoreGive(t->space_ready);
  for (int i = 0; t->reader_task && i < 100; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  t->port = 0;
}

ssize_t ap2_buffered_transport_read_exact(ap2_buffered_transport_t *t,
                                          void *dst_, size_t bytes) {
  if (!t || !dst_) return -1;
  uint8_t *dst = (uint8_t *)dst_;
  size_t off = 0;
  while (off < bytes && t->running) {
    xSemaphoreTake(t->mutex, portMAX_DELAY);
    if (t->used == 0) {
      bool closed = t->peer_closed;
      int err = t->peer_error;
      xSemaphoreGive(t->mutex);
      if (closed) return off ? (ssize_t)off : 0;
      if (err) { errno = err; return -1; }
      xSemaphoreTake(t->data_ready, pdMS_TO_TICKS(100));
      continue;
    }
    size_t n = bytes - off;
    if (n > t->used) n = t->used;
    size_t contiguous = t->size - t->rd;
    if (n > contiguous) n = contiguous;
    memcpy(dst + off, t->fifo + t->rd, n);
    t->rd = (t->rd + n) % t->size;
    t->used -= n;
    t->fifo_bytes_read += n;
    xSemaphoreGive(t->mutex);
    xSemaphoreGive(t->space_ready);
    off += n;
  }
  return off == bytes ? (ssize_t)off : 0;
}

void ap2_buffered_transport_get_stats(ap2_buffered_transport_t *t,
                                      ap2_buffered_transport_stats_t *out) {
  if (!t || !out) return;
  memset(out, 0, sizeof(*out));
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  out->socket_bytes = t->socket_bytes;
  out->fifo_bytes_read = t->fifo_bytes_read;
  out->fifo_occupancy = t->used;
  out->fifo_high_water = t->high_water;
  xSemaphoreGive(t->mutex);
}

size_t ap2_buffered_transport_capacity(ap2_buffered_transport_t *t) {
  return t ? t->size : 0U;
}
