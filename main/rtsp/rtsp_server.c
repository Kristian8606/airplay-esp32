#include "rtsp_server.h"

#include <errno.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
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

#include "airplay_identity.h"
#include "mdns_airplay.h"
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

// Client slot for tracking connections. Several RTSP connections may be
// open at once (a sender's audio connection next to its remote-control-only
// connection, /info probes, a new sender pairing). Only one of them owns the
// global audio / PTP / event state: the one that did the last audio SETUP
// (see rtsp_server_claim_audio()).
#define MAX_CLIENTS 4

typedef struct {
  rtsp_conn_t *conn;
  _Atomic bool live;
  int socket;
  _Atomic bool should_stop;
  _Atomic bool is_old; // Marked as old client being killed
  _Atomic bool audio_owner;
  /* Body bytes of an oversized request still to be dropped (owned by the
   * client task). */
  size_t discard_left;
} client_slot_t;

static client_slot_t clients[MAX_CLIENTS] = {
    {.socket = -1}, {.socket = -1}, {.socket = -1}, {.socket = -1}};

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

/* First request line ("POST /command RTSP/1.0") for log messages. */
static void request_line(const uint8_t *buffer, size_t len, char *out, size_t cap) {
  size_t n = 0;
  while (n < len && n + 1 < cap && buffer[n] != '\r' && buffer[n] != '\n') {
    out[n] = (buffer[n] >= 0x20 && buffer[n] < 0x7f) ? (char)buffer[n] : '?';
    n++;
  }
  out[n] = '\0';
}

static int request_cseq(const char *header) {
  const char *c = strcasestr(header, "\r\nCSeq:");
  return c ? atoi(c + 7) : -1;
}

// Process buffered RTSP requests
static void process_rtsp_buffer(client_slot_t *slot, uint8_t *buffer,
                                size_t *buf_len) {
  while (*buf_len > 0 && !slot->should_stop) {
    if (slot->discard_left > 0) {
      /* Drop the body of an oversized request that was already answered. */
      size_t n = *buf_len < slot->discard_left ? *buf_len : slot->discard_left;
      if (*buf_len > n) memmove(buffer, buffer + n, *buf_len - n);
      *buf_len -= n;
      slot->discard_left -= n;
      if (slot->discard_left == 0) ESP_LOGI(TAG, "Oversized request body skipped");
      continue;
    }
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
    if (content_len < 0 || header_len > RTSP_BUFFER_LARGE) {
      char line[96];
      request_line(buffer, *buf_len, line, sizeof(line));
      ESP_LOGW(TAG, "Malformed request header (Content-Length %d, header %u B): "
               "closing connection | %s", content_len, (unsigned)header_len, line);
      slot->conn->close_after_response = true;
      return; /* Never reinterpret an invalid body as a new request. */
    }
    if ((size_t)content_len > RTSP_BUFFER_LARGE - header_len) {
      /* Too large to buffer (e.g. now-playing info with big artwork). Answer
       * it without reading the body and drop the body bytes as they arrive;
       * closing the connection here ends the sender's whole session. */
      char line[96];
      request_line(buffer, *buf_len, line, sizeof(line));
      buffer[header_len] = '\0';
      const int cseq = request_cseq((const char *)buffer);
      buffer[header_len] = header_saved;
      ESP_LOGW(TAG, "RTSP <- %s cseq=%d body=%d B is over the %u KiB limit: "
               "answered 200 OK, body skipped", line, cseq, content_len,
               (unsigned)(RTSP_BUFFER_LARGE / 1024U));
      rtsp_send_ok(slot->socket, slot->conn, cseq);
      slot->discard_left = (size_t)content_len;
      if (*buf_len > header_len) memmove(buffer, buffer + header_len, *buf_len - header_len);
      *buf_len -= header_len;
      continue;
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

  // Sender IP: PTP timing peer, event-port filter, realtime retransmits
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
  uint8_t *decrypted = NULL; /* Lazy bounded encrypted-frame staging. */
  slot->discard_left = 0;
  /* Why the loop ended, for the disconnect log line. */
  const char *why = "stop requested";
  int why_errno = 0;

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
        /* Read one bounded frame separately so a frame that also contains
         * the next request can straddle the receive-capacity boundary. */
        if (!decrypted) {
          decrypted = heap_caps_malloc(RTSP_ENCRYPTED_BLOCK_MAX,
              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (!decrypted) decrypted = malloc(RTSP_ENCRYPTED_BLOCK_MAX);
          if (!decrypted) { why = "out of memory"; goto cleanup; }
        }
        int block_len = rtsp_crypto_read_block(
            slot->socket, conn, decrypted, RTSP_ENCRYPTED_BLOCK_MAX);
        if (block_len <= 0) {
          why_errno = errno;
          why = slot->should_stop ? "stop requested" : "sender closed or read failed";
          goto cleanup;
        }
        size_t offset = 0;
        while (offset < (size_t)block_len) {
          if (buf_len == buf_capacity) {
            if (buf_capacity == RTSP_BUFFER_LARGE) { why = "receive buffer full"; goto cleanup; }
            size_t new_cap = buf_capacity * 2;
            if (new_cap > RTSP_BUFFER_LARGE) new_cap = RTSP_BUFFER_LARGE;
            uint8_t *new_buf = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
            if (!new_buf) { why = "out of memory"; goto cleanup; }
            buffer = new_buf;
            buf_capacity = new_cap;
          }
          size_t chunk = (size_t)block_len - offset;
          if (chunk > buf_capacity - buf_len) chunk = buf_capacity - buf_len;
          memcpy(buffer + buf_len, decrypted + offset, chunk);
          buf_len += chunk;
          offset += chunk;
          process_rtsp_buffer(slot, buffer, &buf_len);
          if (conn->close_after_response) { why = "closed after response"; goto cleanup; }
        }
      }
      goto cleanup;
    }

    // Plain-text mode
    if (buf_len == buf_capacity) {
      if (buf_capacity == RTSP_BUFFER_LARGE) { why = "receive buffer full"; break; }
      size_t new_cap = buf_capacity * 2;
      if (new_cap > RTSP_BUFFER_LARGE) new_cap = RTSP_BUFFER_LARGE;
      uint8_t *new_buf = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
      if (!new_buf) {
        why = "out of memory";
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
      why_errno = recv_len < 0 ? errno : 0;
      why = recv_len == 0 ? "sender closed" : "read failed";
      break;
    }
    buf_len += (size_t)recv_len;
    process_rtsp_buffer(slot, buffer, &buf_len);
    if (conn->close_after_response) {
      why = "closed after response";
      goto cleanup;
    }
  }

cleanup:
  {
    char err[16] = "";
    if (why_errno) snprintf(err, sizeof(err), " (errno %d)", why_errno);
    ESP_LOGI(TAG, "Client slot %d disconnected%s: %s%s (stack headroom %u of %u B)",
             slot_idx,
             conn->owns_audio ? "" : (conn->rc_only ? " (remote control)" : " (no audio)"),
             why, err, (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)CLIENT_STACK_SIZE);
  }
  free(decrypted);
  free(buffer);
  detach_client_socket(slot);

  if (conn->owns_audio) {
    // Immediate: stop audio and the event channel
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
#ifdef CONFIG_AIRPLAY_HOMEKIT
    /* Audio stopped with the connection: the receiver session is no longer
     * active (status flag bit 17). */
    if (airplay_set_session_active(false)) mdns_airplay_update_flags();
#endif
    mdns_airplay_set_group(NULL, false); /* no session: own group again */
  }

  detach_client_conn(slot);
  rtsp_conn_free(conn);

  slot->should_stop = false;
  slot->is_old = false;
  slot->audio_owner = false;
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
  /* lwIP accept waits on the listener's mailbox. Do not rely on shutdown
   * alone to wake that wait: the owner must observe cancellation even with
   * no incoming clients, then close its own descriptor. */
  const struct timeval accept_timeout = {.tv_sec = 0, .tv_usec = 250000};
  if (setsockopt(listen_socket, SOL_SOCKET, SO_RCVTIMEO,
                 &accept_timeout, sizeof(accept_timeout)) < 0) {
    ESP_LOGE(TAG, "Failed to set accept timeout: %d", errno);
    goto server_close;
  }

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
    client_addr_len = sizeof(client_addr);
    int new_socket = accept(listen_socket, (struct sockaddr *)&client_addr,
                            &client_addr_len);
    if (new_socket < 0) {
      const int accept_error = errno;
      if (!server_running) break;
      if (accept_error == EINTR || accept_error == EAGAIN ||
          accept_error == EWOULDBLOCK || accept_error == ETIMEDOUT) continue;
      ESP_LOGE(TAG, "Failed to accept: %d", accept_error);
      break;
    }

    if (!server_running) { close(new_socket); break; }

    // A new connection no longer replaces the current one here: the audio
    // owner changes only when a connection does an audio SETUP
    // (rtsp_server_claim_audio). /info probes and remote-control-only
    // connections run alongside the playing session.
    xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
    if (!server_running) {
      xSemaphoreGive(lifecycle_mutex);
      close(new_socket);
      break;
    }
    int new_slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (!clients[i].live) {
        new_slot = i;
        break;
      }
    }
    if (new_slot < 0) {
      xSemaphoreGive(lifecycle_mutex);
      ESP_LOGW(TAG, "New client rejected: all %d slots busy", MAX_CLIENTS);
      close(new_socket);
      continue;
    }
    ESP_LOGI(TAG, "New client connected (slot %d)", new_slot);
    clients[new_slot].socket = new_socket;
    clients[new_slot].should_stop = false;
    clients[new_slot].is_old = false;
    clients[new_slot].audio_owner = false;
    clients[new_slot].live = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        client_task, "rtsp_client", CLIENT_STACK_SIZE,
        (void *)(intptr_t)new_slot, RTSP_CLIENT_TASK_PRIORITY, NULL, 0);
    if (task_ret != pdPASS) {
      clients[new_slot].socket = -1;
      clients[new_slot].live = false;
    }
    xSemaphoreGive(lifecycle_mutex);
    if (task_ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create client task");
      close(new_socket);
    }
  }

  for (int i = 0; i < MAX_CLIENTS; i++) signal_client_stop(i);

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
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].live) return false;
  }
  return !server_task_live && rtsp_event_port_is_idle();
}

esp_err_t rtsp_server_claim_audio(rtsp_conn_t *conn) {
  if (!conn) return ESP_ERR_INVALID_ARG;
  if (conn->owns_audio) return ESP_OK;
  int self = -1;
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].conn == conn) self = i;
  }
  xSemaphoreGive(lifecycle_mutex);

  // Serialize audio ownership: client_task cleanup of the previous owner
  // performs global audio_receiver_stop() and rtsp_conn_free() ->
  // ptp_clock_clear(). This connection must not touch that state before the
  // previous owner has finished.
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (i == self || !clients[i].live || !clients[i].audio_owner) continue;
    ESP_LOGI(TAG, "Slot %d takes over audio; stopping previous owner slot %d",
             self, i);
    signal_client_stop(i);
    if (!wait_client_stopped(i, pdMS_TO_TICKS(3000))) {
      ESP_LOGE(TAG, "Previous owner slot %d did not release audio in time", i);
      return ESP_ERR_TIMEOUT;
    }
  }
  conn->owns_audio = true;
  if (self >= 0) clients[self].audio_owner = true;
  rtsp_conn_load_volume(conn);
  return ESP_OK;
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
  for (int i = 0; i < MAX_CLIENTS; ++i) signal_client_stop(i);
  TickType_t start = xTaskGetTickCount();
  while (server_task_live &&
         (TickType_t)(xTaskGetTickCount() - start) < pdMS_TO_TICKS(2000))
    vTaskDelay(1);
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (!wait_client_stopped(i, pdMS_TO_TICKS(3000)))
      ESP_LOGW(TAG, "RTSP client slot %d still stopping", i);
  }
  if (!rtsp_server_is_idle())
    ESP_LOGW(TAG,
             "RTSP owners still stopping: listener=%d clients=%d%d%d%d event=%d; "
             "restart/resource release forbidden",
             (int)atomic_load(&server_task_live), (int)atomic_load(&clients[0].live),
             (int)atomic_load(&clients[1].live), (int)atomic_load(&clients[2].live),
             (int)atomic_load(&clients[3].live), !rtsp_event_port_is_idle());
}
