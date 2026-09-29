#include "rtsp_server.h"

#include <errno.h>
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
#include "freertos/semphr.h"
#include "freertos/task.h"

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
 * so place every RTSP client task directly below it.  The accept/listen task is
 * not latency-critical and stays at the normal application priority. */
#define RTSP_CLIENT_TASK_PRIORITY 17
#define RTSP_SERVER_TASK_PRIORITY 5

static int server_socket = -1;
static TaskHandle_t server_task_handle = NULL;
static bool server_running = false;

// RTSP tasks are restartable. Use dynamic TCB allocation so
// reconnect/start-stop paths cannot reuse static task memory before FreeRTOS
// idle finishes deletion.

/* Shairport Sync connection model.
 *
 * Every RTSP connection gets its own client task, like Shairport's
 * rtsp_conversation_thread. Accepting a connection never touches the audio
 * engine. Exactly one connection at a time holds the play lock (Shairport
 * "principal_conn"); only it may drive the global audio/PTP/volume/amplifier
 * state. A connection takes the lock when it starts to play (AirPlay 2 initial
 * SETUP with timing, or stream SETUP); taking it stops the previous owner's
 * session first. GET /info, pairing, remote-control SETUP or another device
 * probing the speaker do not end playback.
 *
 * Slots are limited by RAM/lwIP sockets. When all are busy, the oldest
 * connection that does NOT own the play lock is closed to make room. */
#define RTSP_MAX_CLIENTS 3

typedef struct {
  rtsp_conn_t *conn;
  TaskHandle_t task;
  int socket;
  uint32_t accept_seq;      // accept order, for choosing an eviction victim
  volatile bool should_stop;
} client_slot_t;

static client_slot_t clients[RTSP_MAX_CLIENTS] = {0};
static uint32_t s_accept_seq = 0;

/* Play-lock owner slot (-1 = none). Guarded by s_owner_mutex, which is only
 * ever held for a few instructions (never across a wait). */
static SemaphoreHandle_t s_owner_mutex = NULL;
static int s_owner_slot = -1;

static void signal_client_stop(int slot_idx);
static bool wait_client_stopped(int slot_idx, TickType_t timeout_ticks);

static int slot_of_conn(const rtsp_conn_t *conn) {
  for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
    if (clients[i].conn == conn) return i;
  }
  return -1;
}

bool rtsp_server_acquire_play_lock(rtsp_conn_t *conn) {
  if (!conn) return false;
  if (conn->play_owner) return true;
  const int me = slot_of_conn(conn);
  if (me < 0 || !s_owner_mutex) return false;

  xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
  const int previous = s_owner_slot;
  s_owner_slot = me;
  conn->play_owner = true;
  xSemaphoreGive(s_owner_mutex);

  if (previous >= 0 && previous != me) {
    /* Shairport get_play_lock(): stop the current principal connection first.
     * Its cleanup still runs as owner (its conn->play_owner stays true) and
     * performs the global audio/PTP/event-port reset. This task must not touch
     * the audio engine until that has finished. */
    ESP_LOGI(TAG, "Client slot %d takes the play lock from slot %d", me,
             previous);
    signal_client_stop(previous);
    if (!wait_client_stopped(previous, pdMS_TO_TICKS(3000))) {
      ESP_LOGE(TAG, "Previous owner slot %d did not release audio in time",
               previous);
      xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
      if (s_owner_slot == me) s_owner_slot = -1;
      conn->play_owner = false;
      xSemaphoreGive(s_owner_mutex);
      return false;
    }
  } else {
    ESP_LOGD(TAG, "Client slot %d acquired the play lock", me);
  }

  /* Volume set on this connection before it started playing applies now. */
  audio_receiver_set_volume_q15(conn->volume_q15);
  return true;
}

static void release_play_lock(int slot_idx, rtsp_conn_t *conn) {
  if (!s_owner_mutex) return;
  xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
  if (s_owner_slot == slot_idx) s_owner_slot = -1;
  if (conn) conn->play_owner = false;
  xSemaphoreGive(s_owner_mutex);
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
    char *header_str = malloc(header_len + 1);
    if (!header_str) {
      *buf_len = 0;
      break;
    }
    memcpy(header_str, buffer, header_len);
    header_str[header_len] = '\0';

    int content_len = rtsp_parse_content_length(header_str);
    if (content_len < 0) {
      content_len = 0;
    }

    size_t total_len = header_len + (size_t)content_len;
    if (total_len > RTSP_BUFFER_LARGE || *buf_len < total_len) {
      free(header_str);
      if (total_len > RTSP_BUFFER_LARGE) {
        *buf_len = 0;
      }
      break;
    }

    // Null-terminate so strcasestr in parse_raw_header won't read past
    // the message boundary. The allocation always has one guard byte beyond
    // the logical receive capacity, including the exact-capacity case.
    uint8_t saved = buffer[total_len];
    buffer[total_len] = '\0';
    rtsp_dispatch(slot->socket, slot->conn, buffer, total_len);
    buffer[total_len] = saved;
    free(header_str);

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
    close(slot->socket);
    slot->socket = -1;
    slot->task = NULL;
    vTaskDelete(NULL);
    return;
  }
  slot->conn = conn;
  AUDIO_DIAG_FLUSH_RTSP_SESSION_RESET(slot->socket);

  // Client IP: realtime PTP source filter and retransmit-request target
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
    rtsp_conn_free(conn);
    slot->conn = NULL;
    close(slot->socket);
    slot->socket = -1;
    slot->task = NULL;
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

  // Shairport Sync (rtsp.c, since 4.1.1): TCP keepalive on the RTSP control
  // connection, so a sender that vanishes without FIN/RST (left Wi-Fi, battery
  // died) ends the session after ~2 minutes instead of holding it - and the
  // amplifier - forever. Same timing: 95 s idle, then 5 probes 5 s apart.
  int keepalive = 1;
  int keep_idle_s = 95;
  int keep_intvl_s = 5;
  int keep_cnt = 5;
  setsockopt(slot->socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  setsockopt(slot->socket, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle_s, sizeof(keep_idle_s));
  setsockopt(slot->socket, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl_s, sizeof(keep_intvl_s));
  setsockopt(slot->socket, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));

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
          if (slot->should_stop || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            goto cleanup;
          }
          continue;
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
      if (recv_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
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
  ESP_LOGI(TAG, "Client slot %d disconnected%s", slot_idx,
           conn->play_owner ? " (was playing)" : "");
  free(buffer);
  close(slot->socket);
  slot->socket = -1;

  const bool was_owner = conn->play_owner;
  if (was_owner) {
    /* Global teardown runs under the handler mutex so it never interleaves
     * with a handler of another connection. */
    rtsp_handlers_lock();
    // Immediate: stop audio
    audio_receiver_stop();
    /* Unexpected socket loss is a full AirPlay session boundary.  Do not let
     * the next client inherit the previous stream selector or session key. */
    audio_receiver_set_stream_type(AUDIO_STREAM_NONE);
    audio_receiver_set_encryption(NULL);

    // Stop the AirPlay 2 event listener before rtsp_conn_free() closes its
    // listening socket.  Closing it first can wake select()/accept() on a
    // descriptor that is being torn down and report EINVAL on a normal
    // disconnect.  rtsp_stop_event_port_task() sets the stop flag first,
    // shuts down the sockets to unblock the task, and waits for it to exit.
    rtsp_stop_event_port_task();
  }

  // rtsp_conn_free() resets PTP / persists volume only for the play owner.
  if (!was_owner) rtsp_handlers_lock();
  rtsp_conn_free(conn);
  rtsp_handlers_unlock();
  release_play_lock(slot_idx, NULL);
  if (was_owner) rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);

  slot->conn = NULL;
  slot->socket = -1;
  slot->should_stop = false;
  slot->task = NULL;

  vTaskDelete(NULL);
}

// Signal a client to stop.  A new play-lock owner waits for the task to finish
// its global audio/PTP cleanup before touching the audio engine.
static void signal_client_stop(int slot_idx) {
  client_slot_t *slot = &clients[slot_idx];
  if (slot->task == NULL) {
    return;
  }

  ESP_LOGI(TAG, "Signaling client slot %d to stop", slot_idx);
  slot->should_stop = true;

  // Shutdown socket to unblock recv/crypto read immediately.
  if (slot->socket >= 0) {
    shutdown(slot->socket, SHUT_RDWR);
  }
}

static bool wait_client_stopped(int slot_idx, TickType_t timeout_ticks) {
  client_slot_t *slot = &clients[slot_idx];
  TickType_t start = xTaskGetTickCount();
  /* Wait for THIS task to go away. The accept loop may reuse the slot for a
   * new connection as soon as it is free; that must count as stopped. */
  TaskHandle_t const task = slot->task;

  while (task != NULL && slot->task == task) {
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

  // Initialize slots
  for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
    clients[i].socket = -1;
    clients[i].conn = NULL;
    clients[i].task = NULL;
    clients[i].should_stop = false;
    clients[i].accept_seq = 0;
  }
  s_owner_slot = -1;

  server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(RTSP_PORT);

  if (bind(server_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  if (listen(server_socket, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);
  server_running = true;

  while (server_running) {
    int new_socket = accept(server_socket, (struct sockaddr *)&client_addr,
                            &client_addr_len);
    if (new_socket < 0) {
      if (server_running) {
        ESP_LOGE(TAG, "Failed to accept: %d", errno);
      }
      continue;
    }

    ESP_LOGI(TAG, "New client connected");

    /* Shairport model: a new connection never stops playback by itself.
     * Use a free slot; if none is free, close the oldest connection that does
     * not hold the play lock (it only answered /info, pairing, etc.). */
    int new_slot = -1;
    for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
      if (clients[i].task == NULL) {
        new_slot = i;
        break;
      }
    }
    if (new_slot < 0) {
      int owner = -1;
      xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
      owner = s_owner_slot;
      xSemaphoreGive(s_owner_mutex);
      int victim = -1;
      for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
        if (i == owner) continue;
        if (victim < 0 || clients[i].accept_seq < clients[victim].accept_seq)
          victim = i;
      }
      if (victim < 0) victim = owner; /* cannot happen with >= 2 slots */
      ESP_LOGI(TAG, "All %d RTSP slots busy; closing idle slot %d",
               RTSP_MAX_CLIENTS, victim);
      signal_client_stop(victim);
      if (!wait_client_stopped(victim, pdMS_TO_TICKS(3000))) {
        ESP_LOGE(TAG, "Slot %d task did not exit in time", victim);
        close(new_socket);
        continue;
      }
      new_slot = victim;
    }

    clients[new_slot].socket = new_socket;
    clients[new_slot].should_stop = false;
    clients[new_slot].accept_seq = ++s_accept_seq;

    // Start new client task immediately.
    clients[new_slot].task = NULL;
    BaseType_t task_ret =
        xTaskCreatePinnedToCore(client_task, "rtsp_client", CLIENT_STACK_SIZE,
                                (void *)(intptr_t)new_slot, RTSP_CLIENT_TASK_PRIORITY,
                                &clients[new_slot].task, 0);
    if (task_ret != pdPASS || clients[new_slot].task == NULL) {
      ESP_LOGE(TAG, "Failed to create client task");
      close(new_socket);
      clients[new_slot].socket = -1;
      clients[new_slot].task = NULL;
    } else {
      ESP_LOGI(TAG, "Client slot %d started", new_slot);
    }
  }

  // Stop all clients
  for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
    if (clients[i].task != NULL) {
      clients[i].should_stop = true;
      if (clients[i].socket >= 0) {
        shutdown(clients[i].socket, SHUT_RDWR);
      }
    }
  }

  vTaskDelay(pdMS_TO_TICKS(500));

  if (server_socket >= 0) {
    close(server_socket);
    server_socket = -1;
  }

  server_task_handle = NULL;
  vTaskDelete(NULL);
}

static bool rtsp_server_wait_for_task_stopped(int timeout_ticks) {
  while (server_task_handle != NULL && timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return server_task_handle == NULL;
}

esp_err_t rtsp_server_start(void) {
  if (!s_owner_mutex) {
    s_owner_mutex = xSemaphoreCreateMutex();
    if (!s_owner_mutex) return ESP_ERR_NO_MEM;
  }
  esp_err_t err = rtsp_handlers_init();
  if (err != ESP_OK) return err;
  if (server_task_handle != NULL) {
    if (server_running) {
      return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "RTSP server task still stopping, waiting");
    if (!rtsp_server_wait_for_task_stopped(40)) {
      ESP_LOGE(TAG, "Previous RTSP server task did not stop");
      return ESP_ERR_INVALID_STATE;
    }
  }

  BaseType_t task_ret =
      xTaskCreatePinnedToCore(server_task, "rtsp_server", SERVER_STACK_SIZE, NULL,
                              RTSP_SERVER_TASK_PRIORITY, &server_task_handle, 0);
  if (task_ret != pdPASS || server_task_handle == NULL) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

void rtsp_server_stop(void) {
  server_running = false;

  if (server_socket >= 0) {
    shutdown(server_socket, SHUT_RDWR);
    close(server_socket);
    server_socket = -1;
  }

  if (server_task_handle != NULL) {
    if (!rtsp_server_wait_for_task_stopped(40)) {
      ESP_LOGW(TAG, "RTSP server task did not exit within timeout");
    }
  }

  /* server_task asks every client to stop, but its fixed grace delay is not
   * an ownership barrier. Callers that are about to free global audio memory
   * must not return until client cleanup has completed audio_receiver_stop(). */
  for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
    if (clients[i].task != NULL &&
        !wait_client_stopped(i, pdMS_TO_TICKS(3000))) {
      ESP_LOGW(TAG, "RTSP client slot %d did not exit within stop timeout", i);
    }
  }
}
