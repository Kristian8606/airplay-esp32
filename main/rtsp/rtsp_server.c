#include "rtsp_server.h"

#include <errno.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "audio_diag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "rtsp_conn.h"
#include "rtsp_crypto.h"
#include "rtsp_handlers.h"
#include "rtsp_message.h"

#include "ptp_clock.h"
#include "rtsp_events.h"

static const char *TAG = "rtsp_server";

#define RTSP_PORT           7000
#define RTSP_BUFFER_INITIAL 4096
#define RTSP_BUFFER_LARGE   ((size_t)256 * 1024)

#define CLIENT_STACK_SIZE 8192
#define SERVER_STACK_SIZE 4096

/* RTSP control must run immediately after lwIP has delivered socket data.
 * ESP-IDF keeps the TCP/IP task above application code (prio 18 on ESP32-S3),
 * so place the active RTSP client directly below it.  The accept/listen task is
 * not latency-critical and stays at the normal application priority. */
#define RTSP_CLIENT_TASK_PRIORITY 17
#define RTSP_SERVER_TASK_PRIORITY 5

static int server_socket = -1;
static _Atomic bool server_task_live;
static _Atomic bool server_running;
static _Atomic(SemaphoreHandle_t) lifecycle_mutex;

// RTSP tasks are restartable. Use dynamic TCB allocation so
// reconnect/start-stop paths cannot reuse static task memory before FreeRTOS
// idle finishes deletion.

// Client slot for tracking connections
typedef struct {
  rtsp_conn_t *conn;
  _Atomic bool live;
  int socket;
  _Atomic bool should_stop;
  _Atomic bool is_old; // Marked as old client being killed
} client_slot_t;

static client_slot_t clients[2] = {{.socket = -1}, {.socket = -1}}; // Current and old
static int current_slot = 0;

// Hold the lifetime mutex while accessing a public connection pointer.
void airplay_set_volume(float volume_db) {
  if (!lifecycle_mutex) return;
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  client_slot_t *c = &clients[current_slot];
  if (c->conn && !c->is_old) rtsp_conn_set_volume(c->conn, volume_db);
  xSemaphoreGive(lifecycle_mutex);
}

int32_t airplay_get_volume_q15(void) {
  if (!lifecycle_mutex) return 16384;
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  client_slot_t *c = &clients[current_slot];
  int32_t volume = c->conn && !c->is_old ?
      rtsp_conn_get_volume_q15(c->conn) : 16384;
  xSemaphoreGive(lifecycle_mutex);
  return volume;
}

static void detach_client_socket(client_slot_t *slot) {
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  int socket = slot->socket;
  slot->socket = -1;
  xSemaphoreGive(lifecycle_mutex);
  if (socket >= 0) close(socket);
}

static void detach_client_conn(client_slot_t *slot) {
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  slot->conn = NULL;
  xSemaphoreGive(lifecycle_mutex);
}

// Helper to grow buffer
static uint8_t *grow_buffer(uint8_t *old_buf, size_t old_size, size_t new_size,
                            size_t data_len) {
  (void)old_size;
  /* Keep one guard byte beyond the logical receive capacity.
   * process_rtsp_buffer() temporarily writes buffer[total_len] = '\0'; when
   * a request exactly fills the logical buffer this sentinel must still be
   * inside the allocation. */
  uint8_t *new_buf =
      heap_caps_malloc(new_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!new_buf) {
    new_buf = malloc(new_size + 1U);
  }
  if (!new_buf) {
    return NULL;
  }
  if (old_buf && data_len > 0) {
    memcpy(new_buf, old_buf, data_len);
  }
  free(old_buf);
  return new_buf;
}

// Process buffered RTSP requests
static void process_rtsp_buffer(client_slot_t *slot, uint8_t *buffer,
                                size_t *buf_len) {
  while (*buf_len > 0 && !slot->should_stop) {
    const uint8_t *header_end = rtsp_find_header_end(buffer, *buf_len);
    if (!header_end) {
      break;
    }

    size_t header_len = (size_t)(header_end - buffer) + 4;
    /* One guard byte already exists; inspect only the header in place. */
    uint8_t header_saved = buffer[header_len];
    buffer[header_len] = '\0';
    int content_len = rtsp_parse_content_length((const char *)buffer);
    buffer[header_len] = header_saved;
    if (content_len < 0 || header_len > RTSP_BUFFER_LARGE ||
        (size_t)content_len > RTSP_BUFFER_LARGE - header_len) {
      slot->conn->close_after_response = true;
      return; /* Never reinterpret an invalid body as a new request. */
    }
    size_t total_len = header_len + (size_t)content_len;
    if (*buf_len < total_len) break;

    // Null-terminate so strcasestr in parse_raw_header won't read past
    // the message boundary. The allocation always has one guard byte beyond
    // the logical receive capacity, including the exact-capacity case.
    uint8_t saved = buffer[total_len];
    buffer[total_len] = '\0';
    rtsp_dispatch(slot->socket, slot->conn, buffer, total_len);
    buffer[total_len] = saved;

    if (slot->conn->close_after_response) {
      // Shairport handle_teardown_2() sets conn->stop after returning the
      // TEARDOWN response. Do not process any pipelined request belonging to
      // the connection that has just been torn down.
      *buf_len = 0;
      return;
    }

    if (*buf_len > total_len) {
      memmove(buffer, buffer + total_len, *buf_len - total_len);
    }
    *buf_len -= total_len;
  }
}

// Client task
static void client_task(void *pvParameters) {
  int slot_idx = (int)(intptr_t)pvParameters;
  client_slot_t *slot = &clients[slot_idx];

  // Create connection state
  rtsp_conn_t *conn = rtsp_conn_create();
  if (!conn) {
    ESP_LOGE(TAG, "Failed to create connection state");
    detach_client_socket(slot);
    slot->live = false;
    vTaskDelete(NULL);
    return;
  }
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  slot->conn = conn;
  xSemaphoreGive(lifecycle_mutex);
  AUDIO_DIAG_FLUSH_RTSP_SESSION_RESET(slot->socket);

  // Get client IP address for timing requests
  struct sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  if (getpeername(slot->socket, (struct sockaddr *)&peer_addr, &peer_len) ==
      0) {
    conn->client_ip = peer_addr.sin_addr.s_addr;
    ESP_LOGI(TAG, "Client IP: %u.%u.%u.%u",
             (unsigned int)(conn->client_ip & 0xFF),
             (unsigned int)((conn->client_ip >> 8) & 0xFF),
             (unsigned int)((conn->client_ip >> 16) & 0xFF),
             (unsigned int)((conn->client_ip >> 24) & 0xFF));
  }

  // Allocate buffer
  size_t buf_capacity = RTSP_BUFFER_INITIAL;
  /* +1 guard byte for the temporary RTSP message terminator. */
  uint8_t *buffer = malloc(buf_capacity + 1U);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate buffer");
    detach_client_conn(slot);
    rtsp_conn_free(conn);
    detach_client_socket(slot);
    slot->live = false;
    vTaskDelete(NULL);
    return;
  }

  size_t buf_len = 0;

  // Socket timeout for stop signal responsiveness
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(slot->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Disable Nagle: RTSP control commands (volume, pause) are tiny and must
  // not wait for coalescing/delayed-ACK, which adds tens to hundreds of ms of
  // latency to every command on this connection.
  int nodelay = 1;
  setsockopt(slot->socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  while (server_running && !slot->should_stop) {
    if (conn->encrypted_mode) {
      // Encrypted mode
      while (server_running && conn->encrypted_mode && !slot->should_stop) {
        if (buf_len >= buf_capacity - 1024) {
          size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE
                                                            : buf_capacity * 2;
          if (new_cap > RTSP_BUFFER_LARGE) {
            goto cleanup;
          }
          uint8_t *new_buf =
              grow_buffer(buffer, buf_capacity, new_cap, buf_len);
          if (!new_buf) {
            goto cleanup;
          }
          buffer = new_buf;
          buf_capacity = new_cap;
        }

        int block_len = rtsp_crypto_read_block(
            slot->socket, conn, buffer + buf_len, buf_capacity - buf_len);
        if (block_len <= 0) {
          /* The helper consumes timeouts internally to preserve framing.
           * Every failure is terminal, independently of stale errno. */
          goto cleanup;
        }

        buf_len += (size_t)block_len;
        process_rtsp_buffer(slot, buffer, &buf_len);
        if (conn->close_after_response) {
          goto cleanup;
        }
      }
      goto cleanup;
    }

    // Plain-text mode
    if (buf_len >= buf_capacity - 1024) {
      size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE
                                                        : buf_capacity * 2;
      if (new_cap > RTSP_BUFFER_LARGE) {
        break;
      }
      uint8_t *new_buf = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
      if (!new_buf) {
        break;
      }
      buffer = new_buf;
      buf_capacity = new_cap;
    }

    ssize_t recv_len =
        recv(slot->socket, buffer + buf_len, buf_capacity - buf_len, 0);
    if (recv_len <= 0) {
      if (recv_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        continue;
      }
      break;
    }
    buf_len += (size_t)recv_len;
    process_rtsp_buffer(slot, buffer, &buf_len);
    if (conn->close_after_response) {
      goto cleanup;
    }
  }

cleanup:
  ESP_LOGI(TAG, "Client slot %d disconnected", slot_idx);
  free(buffer);
  detach_client_socket(slot);

  // Immediate: stop audio and NTP
  audio_receiver_stop();
  rtsp_stop_event_port_task();
  /* A timeout requests cancellation, but does not release ownership. Keep
   * this slot live until every producer and event socket actually exits. */
  while (!audio_receiver_is_idle() || !rtsp_event_port_is_idle()) {
    /* A late static DATA-task suspension must be reaped by another stop. */
    if (!audio_receiver_is_idle()) audio_receiver_stop();
    if (!rtsp_event_port_is_idle()) rtsp_stop_event_port_task();
    vTaskDelay(1);
  }
  audio_receiver_set_stream_type(AUDIO_STREAM_NONE);
  audio_receiver_set_encryption(NULL);

  // AirPlay receiver: no AirPlay 1 DACP grace/reconnect path.
  detach_client_conn(slot);
  rtsp_conn_free(conn);

  slot->should_stop = false;
  slot->is_old = false;
  slot->live = false;

  vTaskDelete(NULL);
}

// Signal a client to stop.  The server must wait for the task to finish its
// global audio/PTP cleanup before giving a replacement client stream ownership.
static void signal_client_stop(int slot_idx) {
  client_slot_t *slot = &clients[slot_idx];
  if (!slot->live) {
    return;
  }

  ESP_LOGI(TAG, "Signaling client slot %d to stop", slot_idx);
  slot->is_old = true;
  slot->should_stop = true;

  // Serialize shutdown with descriptor detach; only the owner closes.
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  if (slot->socket >= 0) shutdown(slot->socket, SHUT_RDWR);
  xSemaphoreGive(lifecycle_mutex);
}

static bool wait_client_stopped(int slot_idx, TickType_t timeout_ticks) {
  client_slot_t *slot = &clients[slot_idx];
  TickType_t start = xTaskGetTickCount();

  while (slot->live) {
    if ((TickType_t)(xTaskGetTickCount() - start) >= timeout_ticks) {
      return false;
    }
    vTaskDelay(1);
  }
  return true;
}

static void server_task(void *pvParameters) {
  (void)pvParameters;

  struct sockaddr_in server_addr, client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    goto server_exit;
  }
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  server_socket = listen_socket;
  bool stopping = !server_running;
  xSemaphoreGive(lifecycle_mutex);
  if (stopping) goto server_close;

  int opt = 1;
  setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(RTSP_PORT);

  if (bind(listen_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind: %d", errno);
    goto server_close;
  }

  if (listen(listen_socket, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen: %d", errno);
    goto server_close;
  }

  ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);
  while (server_running) {
    int new_socket = accept(listen_socket, (struct sockaddr *)&client_addr,
                            &client_addr_len);
    if (new_socket < 0) {
      if (server_running) {
        ESP_LOGE(TAG, "Failed to accept: %d", errno);
      }
      if (errno == EINTR) continue;
      break;
    }

    if (!server_running) { close(new_socket); break; }
    ESP_LOGI(TAG, "New client connected");

    // Find slot for new client (alternate between 0 and 1)
    int new_slot = 1 - current_slot;

    // A spare slot should normally already be free.  If a stale task is still
    // finishing there, do not reuse its slot until cleanup is complete.
    if (clients[new_slot].live) {
      signal_client_stop(new_slot);
      if (!wait_client_stopped(new_slot, pdMS_TO_TICKS(3000))) {
        ESP_LOGE(TAG, "Slot %d task did not exit in time", new_slot);
        close(new_socket);
        continue;
      }
    }

    // Serialize RTSP ownership.  client_task cleanup performs global
    // audio_receiver_stop() and rtsp_conn_free() -> ptp_clock_clear().
    // The replacement must not start until those operations are complete.
    if (clients[current_slot].live) {
      signal_client_stop(current_slot);
      ESP_LOGI(TAG,
               "Waiting for old client slot %d cleanup before replacement",
               current_slot);
      if (!wait_client_stopped(current_slot, pdMS_TO_TICKS(3000))) {
        ESP_LOGE(TAG,
                 "Old client slot %d did not release audio ownership in time",
                 current_slot);
        close(new_socket);
        continue;
      }
      ESP_LOGI(TAG, "Old client cleanup complete; starting replacement");
    }

    // Setup new slot only after the previous owner has completed all global
    // audio/PTP cleanup.
    xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
    if (!server_running) {
      xSemaphoreGive(lifecycle_mutex);
      close(new_socket);
      break;
    }
    clients[new_slot].socket = new_socket;
    clients[new_slot].should_stop = false;
    clients[new_slot].is_old = false;
    clients[new_slot].live = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        client_task, "rtsp_client", CLIENT_STACK_SIZE,
        (void *)(intptr_t)new_slot, RTSP_CLIENT_TASK_PRIORITY, NULL, 0);
    if (task_ret != pdPASS) {
      clients[new_slot].socket = -1;
      clients[new_slot].live = false;
    } else {
      current_slot = new_slot;
    }
    xSemaphoreGive(lifecycle_mutex);
    if (task_ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create client task");
      close(new_socket);
    }
  }

  for (int i = 0; i < 2; i++) signal_client_stop(i);

server_close:
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  server_socket = -1;
  xSemaphoreGive(lifecycle_mutex);
  close(listen_socket);
server_exit:
  server_running = false;
  server_task_live = false;
  vTaskDelete(NULL);
}

bool rtsp_server_is_idle(void) {
  return !server_task_live && !clients[0].live && !clients[1].live &&
         rtsp_event_port_is_idle();
}

esp_err_t rtsp_server_start(void) {
  if (!lifecycle_mutex) {
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (!created) return ESP_ERR_NO_MEM;
    SemaphoreHandle_t expected = NULL;
    if (!atomic_compare_exchange_strong(&lifecycle_mutex, &expected, created))
      vSemaphoreDelete(created);
  }
  if (!lifecycle_mutex) return ESP_ERR_NO_MEM;
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  if (!rtsp_server_is_idle()) {
    xSemaphoreGive(lifecycle_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  server_running = true;
  server_task_live = true;
  BaseType_t ret = xTaskCreatePinnedToCore(
      server_task, "rtsp_server", SERVER_STACK_SIZE, NULL,
      RTSP_SERVER_TASK_PRIORITY, NULL, 0);
  if (ret != pdPASS) {
    server_running = false;
    server_task_live = false;
  }
  xSemaphoreGive(lifecycle_mutex);
  return ret == pdPASS ? ESP_OK : ESP_FAIL;
}

void rtsp_server_stop(void) {
  if (!lifecycle_mutex) return;
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  server_running = false;
  if (server_socket >= 0) shutdown(server_socket, SHUT_RDWR);
  xSemaphoreGive(lifecycle_mutex);
  for (int i = 0; i < 2; ++i) signal_client_stop(i);
  TickType_t start = xTaskGetTickCount();
  while (server_task_live &&
         (TickType_t)(xTaskGetTickCount() - start) < pdMS_TO_TICKS(2000))
    vTaskDelay(1);
  for (int i = 0; i < 2; ++i) {
    if (!wait_client_stopped(i, pdMS_TO_TICKS(3000)))
      ESP_LOGW(TAG, "RTSP client slot %d still stopping", i);
  }
  if (!rtsp_server_is_idle())
    ESP_LOGW(TAG, "RTSP owners still stopping; restart/resource release forbidden");
}
