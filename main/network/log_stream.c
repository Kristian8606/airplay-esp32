/**
 * WebSocket-based log streaming over HTTP.
 *
 * Intercepts ESP-IDF log output via esp_log_set_vprintf(), stores lines
 * in a ring buffer, and broadcasts them to any connected WebSocket
 * client on /ws/logs.  UART output is preserved.
 */

#include "log_stream.h"
#include "spiram_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Ring buffer size — must be power of two for masking. */
#define LOG_RING_SIZE 8192
#define LOG_RING_MASK (LOG_RING_SIZE - 1)

#define BROADCAST_TASK_STACK          4096
#define BROADCAST_TASK_PRIORITY       2
#define BROADCAST_ACTIVE_INTERVAL_MS  100
#define BROADCAST_IDLE_INTERVAL_MS    1000
#define MAX_SEND_CHUNK                1024
#define WS_RX_MAX_PAYLOAD             128

static char *s_ring;
static volatile size_t s_head; /* next write position  */
static volatile size_t s_tail; /* next read position   */
static SemaphoreHandle_t s_mutex;

static httpd_handle_t s_server;

static vprintf_like_t s_orig_vprintf;

/* ------------------------------------------------------------------ */
/*  Ring buffer helpers (protected by s_mutex)                         */
/* ------------------------------------------------------------------ */

static inline size_t ring_used(void) {
  return (s_head - s_tail) & LOG_RING_MASK;
}

static void ring_write(const char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    /* If head is about to overwrite tail, discard oldest byte. */
    if (((s_head + 1) & LOG_RING_MASK) == (s_tail & LOG_RING_MASK)) {
      s_tail = (s_tail + 1) & LOG_RING_MASK;
    }
    s_ring[s_head & LOG_RING_MASK] = data[i];
    s_head = (s_head + 1) & LOG_RING_MASK;
  }
}

static size_t ring_read(char *buf, size_t max) {
  size_t avail = ring_used();
  if (avail > max) {
    avail = max;
  }
  for (size_t i = 0; i < avail; i++) {
    buf[i] = s_ring[s_tail & LOG_RING_MASK];
    s_tail = (s_tail + 1) & LOG_RING_MASK;
  }
  return avail;
}

/* ------------------------------------------------------------------ */
/*  Log hook — called from any task/ISR-safe context by esp_log       */
/* ------------------------------------------------------------------ */

static int log_vprintf_hook(const char *fmt, va_list args) {
  /* A va_list may be consumed by vprintf. Give UART its own copy so the
   * original list remains valid for the WebSocket copy below. */
  va_list uart_args;
  va_copy(uart_args, args);
  int ret = s_orig_vprintf(fmt, uart_args);
  va_end(uart_args);

  /* Format into a stack buffer and push to ring. */
  char buf[256];
  va_list copy;
  va_copy(copy, args);
  int len = vsnprintf(buf, sizeof(buf), fmt, copy);
  va_end(copy);

  if (len > 0) {
    if ((size_t)len >= sizeof(buf)) {
      len = sizeof(buf) - 1;
    }
    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
      ring_write(buf, (size_t)len);
      xSemaphoreGive(s_mutex);
    }
    /* If the mutex is held we silently drop — better than blocking a log call.
     */
  }
  return ret;
}

/* ------------------------------------------------------------------ */
/*  WebSocket handler                                                  */
/* ------------------------------------------------------------------ */

static void close_ws_session(int fd, const char *reason) {
  if (!s_server || fd < 0) {
    return;
  }

  esp_err_t err = httpd_sess_trigger_close(s_server, fd);
  if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
    ESP_LOGD("log_stream", "WS close fd=%d (%s) failed: %s", fd,
             reason ? reason : "unknown", esp_err_to_name(err));
  }
}

static esp_err_t ws_log_handler(httpd_req_t *req) {
  /* The server completes the WebSocket handshake internally; depending on the
   * IDF build the handshake GET may or may not reach here. Return OK for it. */
  if (req->method == HTTP_GET) {
    return ESP_OK;
  }

  const int fd = httpd_req_to_sockfd(req);
  httpd_ws_frame_t frame = {0};

  /* First call only parses the WS header and reports the payload length. */
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    /* A dead/stale browser socket must not remain in the HTTPD session list;
     * otherwise the broadcast task keeps sending to it and HTTPD repeatedly
     * attempts to parse a stream which is no longer a valid WebSocket. */
    close_ws_session(fd, "recv-header");
    return ESP_OK;
  }

  /* /ws/logs is receive-only from the browser's point of view. Normal browser
   * control frames are tiny. If a client sends a larger payload, close the
   * session rather than leaving an unconsumed frame in the socket. */
  if (frame.len > WS_RX_MAX_PAYLOAD) {
    ESP_LOGD("log_stream", "Closing WS fd=%d: unexpected payload %u bytes",
             fd, (unsigned)frame.len);
    close_ws_session(fd, "oversize-frame");
    return ESP_OK;
  }

  if (frame.len > 0) {
    uint8_t buf[WS_RX_MAX_PAYLOAD];
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
      close_ws_session(fd, "recv-payload");
      return ESP_OK;
    }
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    close_ws_session(fd, "peer-close");
  }
  return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Broadcast task                                                     */
/* ------------------------------------------------------------------ */

static void broadcast_task(void *arg) {
  (void)arg;
  char buf[MAX_SEND_CHUNK];
  TickType_t interval = pdMS_TO_TICKS(BROADCAST_IDLE_INTERVAL_MS);

  while (1) {
    vTaskDelay(interval);

    if (!s_server) {
      interval = pdMS_TO_TICKS(BROADCAST_IDLE_INTERVAL_MS);
      continue;
    }

    /* Discover active WebSocket sessions fresh each pass. With no viewer the
     * task wakes only once per second; while a viewer is connected it returns
     * to the 100 ms cadence used for live log streaming. */
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t fd_count = CONFIG_LWIP_MAX_SOCKETS;
    if (httpd_get_client_list(s_server, &fd_count, fds) != ESP_OK) {
      interval = pdMS_TO_TICKS(BROADCAST_IDLE_INTERVAL_MS);
      continue;
    }

    int ws_fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t ws_count = 0;
    for (size_t i = 0; i < fd_count; i++) {
      if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
        ws_fds[ws_count++] = fds[i];
      }
    }
    if (ws_count == 0) {
      interval = pdMS_TO_TICKS(BROADCAST_IDLE_INTERVAL_MS);
      continue; /* leave data in the ring as backlog for the next viewer */
    }
    interval = pdMS_TO_TICKS(BROADCAST_ACTIVE_INTERVAL_MS);

    size_t len = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      len = ring_read(buf, sizeof(buf));
      xSemaphoreGive(s_mutex);
    }
    if (len == 0) {
      continue;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = len,
    };

    for (size_t i = 0; i < ws_count; i++) {
      esp_err_t err = httpd_ws_send_frame_async(s_server, ws_fds[i], &frame);
      if (err != ESP_OK) {
        /* Do not keep a dead browser session around. It otherwise remains in
         * the client list long enough to generate repeated send/recv warnings. */
        close_ws_session(ws_fds[i], "send-failed");
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t log_stream_init(void) {
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    return ESP_ERR_NO_MEM;
  }

#ifdef CONFIG_SPIRAM
  s_ring = heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!s_ring) {
    s_ring = malloc(LOG_RING_SIZE);
  }
  if (!s_ring) {
    return ESP_ERR_NO_MEM;
  }

  s_head = s_tail = 0;

  /* Hook into esp_log — keep the original so UART output continues. */
  s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);

  return ESP_OK;
}

esp_err_t log_stream_register(httpd_handle_t server) {
  s_server = server;

  httpd_uri_t ws_uri = {
      .uri = "/ws/logs",
      .method = HTTP_GET,
      .handler = ws_log_handler,
      .is_websocket = true,
  };
  esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
  if (err != ESP_OK) {
    ESP_LOGE("log_stream", "Failed to register /ws/logs: %s",
             esp_err_to_name(err));
    return err;
  }

  task_create_pinned_spiram(broadcast_task, "log_ws", BROADCAST_TASK_STACK,
                            NULL, BROADCAST_TASK_PRIORITY, NULL, 0, NULL);
  ESP_LOGI("log_stream", "Log streaming on /ws/logs");
  return ESP_OK;
}
