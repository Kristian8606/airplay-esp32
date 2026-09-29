#include "rtsp_handlers.h"
#include "airplay_version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sodium.h"

#include "audio_receiver.h"
#include "audio_diag.h"
#include "amp_control.h"
#include "hap.h"
#include "ptp_clock.h"
#include "plist.h"
#include "rtsp_fairplay.h"
#include "rtsp_rsa.h"
#include "settings.h"
#include "socket_utils.h"
#include "tlv8.h"

#include "rtsp_crypto.h"
#include "rtsp_events.h"
#include "rtsp_server.h"
#include "mdns_airplay.h"

static const char *TAG = "rtsp_handlers";

static void amp_session_activate_once(rtsp_conn_t *conn) {
  if (!conn || conn->amp_session_active) return;
  conn->amp_session_active = true;
  amp_control_session_connected();
}

static void amp_session_deactivate_once(rtsp_conn_t *conn) {
  if (!conn || !conn->amp_session_active) return;
  conn->amp_session_active = false;
  amp_control_session_disconnected();
}

static void note_stream_pause_started(rtsp_conn_t *conn) {
  if (!conn || conn->pause_started_us != 0) return;
  conn->pause_started_us = esp_timer_get_time();
}

static void notify_timing_resume(rtsp_conn_t *conn) {
  if (!conn || conn->pause_started_us == 0) return;

  const int64_t now_us = esp_timer_get_time();
  uint64_t pause_ms = 0;
  if (now_us > conn->pause_started_us) {
    pause_ms = (uint64_t)(now_us - conn->pause_started_us) / 1000ULL;
  }
  conn->pause_started_us = 0;

  if (pause_ms > UINT32_MAX) pause_ms = UINT32_MAX;
  ptp_clock_notify_resume((uint32_t)pause_ms);
}

/* Logging-only de-duplication. This does not suppress parsing or metadata
 * events; it only avoids repeatedly formatting/streaming identical metadata.
 * Keep the state tiny: one 32-bit fingerprint plus progress log timing. */
#define PROGRESS_LOG_INTERVAL_MS 5000U
static uint32_t s_last_dmap_log_hash;
static bool s_dmap_log_hash_valid;
static TickType_t s_last_progress_log_tick;
static uint32_t s_last_progress_duration_secs;
static bool s_progress_log_valid;

static uint32_t metadata_log_hash(const rtsp_metadata_t *meta) {
  uint32_t hash = 2166136261U; /* FNV-1a */
  const char *fields[] = {meta->title, meta->artist, meta->album, meta->genre};
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    const unsigned char *p = (const unsigned char *)fields[i];
    while (*p) {
      hash ^= (uint32_t)*p++;
      hash *= 16777619U;
    }
    /* Field separator so {"ab","c"} != {"a","bc"}. */
    hash ^= 0xffU;
    hash *= 16777619U;
  }
  return hash;
}

static bool should_log_dmap_metadata(const rtsp_metadata_t *meta) {
  const uint32_t hash = metadata_log_hash(meta);
  if (s_dmap_log_hash_valid && hash == s_last_dmap_log_hash) return false;
  s_last_dmap_log_hash = hash;
  s_dmap_log_hash_valid = true;
  return true;
}

static bool should_log_progress(uint32_t duration_secs) {
  const TickType_t now = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(PROGRESS_LOG_INTERVAL_MS);
  const bool duration_changed =
      !s_progress_log_valid || duration_secs != s_last_progress_duration_secs;
  const bool interval_elapsed =
      !s_progress_log_valid ||
      (TickType_t)(now - s_last_progress_log_tick) >= interval;

  if (!duration_changed && !interval_elapsed) return false;

  s_last_progress_log_tick = now;
  s_last_progress_duration_secs = duration_secs;
  s_progress_log_valid = true;
  return true;
}

static void reset_metadata_log_dedup(void) {
  s_dmap_log_hash_valid = false;
  s_progress_log_valid = false;
}

// ============================================================================
// Codec Registry
// ============================================================================
// Codec names by bplist "ct". SETUP additionally accepts only the formats in
// ap2_audio_format_supported().

static void configure_codec(audio_format_t *fmt, const char *name, int64_t sr,
                            int64_t spf) {
  strcpy(fmt->codec, name);
  fmt->sample_rate = (int)sr;
  fmt->channels = 2;
  fmt->bits_per_sample = 16;
  fmt->frame_size = (int)spf;
}

// ct values: 2=ALAC, 4=AAC, 8=AAC-ELD, 64=OPUS (based on AirPlay 2 protocol)
static const rtsp_codec_t codec_registry[] = {
    {"ALAC", 2}, {"AAC", 4}, {"AAC-ELD", 8}, {"OPUS", 64}, {NULL, 0}};

bool rtsp_codec_configure(int64_t type_id, audio_format_t *fmt,
                          int64_t sample_rate, int64_t samples_per_frame) {
  if (!fmt) {
    return false;
  }
  memset(fmt, 0, sizeof(*fmt));
  for (const rtsp_codec_t *codec = codec_registry; codec->name; codec++) {
    if (codec->type_id == type_id) {
      configure_codec(fmt, codec->name, sample_rate, samples_per_frame);
      ESP_LOGI(TAG, "Configured codec: %s (ct=%lld, sr=%lld, spf=%lld)",
               codec->name, (long long)type_id, (long long)sample_rate,
               (long long)samples_per_frame);
      return true;
    }
  }

  /* Never guess an unknown codec as ALAC. Doing so can send arbitrary
   * compressed payload into the ALAC decoder while the I2S/timeline remains
   * configured for 44.1 kHz. Unknown formats are rejected by SETUP. */
  ESP_LOGW(TAG, "Unknown codec type %lld", (long long)type_id);
  return false;
}

static bool ap2_audio_format_supported(int64_t stream_type, int64_t codec_type,
                                       int64_t sample_rate,
                                       int64_t samples_per_frame) {
  if (stream_type == AUDIO_STREAM_REALTIME) {
    return codec_type == 2 && sample_rate == 44100 &&
           samples_per_frame == 352;
  }
  if (stream_type == AUDIO_STREAM_BUFFERED) {
    return codec_type == 4 && sample_rate == 44100 &&
           samples_per_frame == 1024;
  }
  return false;
}

// Event port task state
#define EVENT_STACK_SIZE 6144

// Default playout latency for realtime (type 96) streams when SETUP does not
// carry a usable latencyMin: 11025 samples = 250 ms at 44.1 kHz.  This is the
// standard AirPlay realtime-stream minimum latency; senders transmit audio
// ~2 s (88200 samples) ahead of this deadline.
#define AIRPLAY_RT_LATENCY_DEFAULT_SAMPLES 11025
static int event_client_socket = -1;
static int event_listen_socket = -1;
static TaskHandle_t event_task_handle = NULL;
static volatile bool event_task_should_stop = false;

void rtsp_get_device_id(char *device_id, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(device_id, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

static int rtsp_create_udp_socket(uint16_t *port) {
  uint16_t bound_port = 0;
  int sock = socket_utils_bind_udp(0, 0, 0, &bound_port);
  if (sock < 0) {
    return -1;
  }
  if (port) {
    *port = bound_port;
  }
  return sock;
}

static int rtsp_create_event_socket(uint16_t *port) {
  uint16_t bound_port = 0;
  int sock = socket_utils_bind_tcp_listener(0, 1, false, &bound_port);
  if (sock < 0) {
    return -1;
  }
  if (port) {
    *port = bound_port;
  }
  return sock;
}

static void ensure_stream_ports(rtsp_conn_t *conn) {
  int temp_socket = 0;
  if (conn->data_port == 0) {
    temp_socket = rtsp_create_udp_socket(&conn->data_port);
    if (temp_socket > 0) {
      close(temp_socket);
    }
  }
  if (conn->control_port == 0) {
    temp_socket = rtsp_create_udp_socket(&conn->control_port);
    if (temp_socket > 0) {
      close(temp_socket);
    }
  }
}

/* ---- Audio start failures -------------------------------------------------
 *
 * Every failure to start the audio path in a stream SETUP is logged with the
 * memory state; buffered start gets one hard audio-engine reset + retry. After
 * AUDIO_START_FAILURES_REBOOT consecutive failed SETUPs the device restarts
 * itself: otherwise the sender retries SETUP -> 500 -> TEARDOWN forever and
 * the device stays unusable until a power cycle. */
#define AUDIO_START_FAILURES_REBOOT 3
static uint32_t s_audio_start_failures;

static void log_memory(const char *where) {
  ESP_LOGI(TAG,
           "MEM %s internal free=%uKiB largest=%uKiB min=%uKiB, psram free=%uKiB "
           "largest=%uKiB",
           where,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                       MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                      MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));
}

static void audio_start_succeeded(void) { s_audio_start_failures = 0; }

static void audio_start_failed(const char *what, esp_err_t err) {
  s_audio_start_failures++;
  ESP_LOGE(TAG, "SETUP: %s failed: %s (%u in a row)", what, esp_err_to_name(err),
           (unsigned)s_audio_start_failures);
  log_memory("at SETUP failure");
  if (s_audio_start_failures >= AUDIO_START_FAILURES_REBOOT) {
    ESP_LOGE(TAG, "SETUP: audio cannot start %u times in a row; restarting the "
                  "device to recover", (unsigned)s_audio_start_failures);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
  }
}

/* Start the buffered (type 103) listener. On failure reset the audio engine
 * to the hard session boundary (and re-create it if it is gone), then retry
 * once. */
static esp_err_t start_buffered_with_recovery(void) {
  esp_err_t err = audio_receiver_start_buffered(0);
  if (err == ESP_OK) return ESP_OK;
  ESP_LOGE(TAG, "SETUP: buffered audio start failed (%s); resetting the audio "
                "engine and retrying", esp_err_to_name(err));
  log_memory("before audio reset");
  audio_receiver_stop();
  if (!audio_receiver_is_initialized()) {
    const esp_err_t init_err = audio_receiver_init();
    if (init_err != ESP_OK) {
      ESP_LOGE(TAG, "SETUP: audio engine re-init failed: %s",
               esp_err_to_name(init_err));
    }
  }
  err = audio_receiver_start_buffered(0);
  if (err == ESP_OK) {
    ESP_LOGW(TAG, "SETUP: buffered audio started after the reset");
  }
  return err;
}

static bool start_audio_receiver_or_fail(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         int64_t stream_type) {
  audio_receiver_set_stream_type((audio_stream_type_t)stream_type);
  esp_err_t err = audio_receiver_start_stream(
      conn->data_port, conn->control_port, conn->buffered_port);
  if (err != ESP_OK) {
    audio_start_failed("audio receiver start", err);
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                       NULL, 0);
    return false;
  }
  audio_start_succeeded();
  return true;
}

/* ---- AirPlay 2 event channel --------------------------------------------
 *
 * The sender connects to the event port announced in the initial SETUP. The
 * channel is encrypted like the control channel, with its own keys
 * ("Events-Salt") and nonce counters. Like Shairport (ap2_event_receiver.c)
 * we send "POST /command" with an updateInfo plist once the sender connects;
 * the sender answers with an RTSP response.
 *
 * Every byte the sender sends is consumed: unread bytes would keep select()
 * firing forever and spin this task (priority 5, core 0), starving the
 * buffered audio processor.
 *
 * The task stack is in PSRAM, so the task must not touch flash: the
 * updateInfo message (device name from NVS) is built before it starts. */
typedef struct {
  int listen_socket;
  bool have_keys;
  uint8_t enc_key[32];
  uint8_t dec_key[32];
  uint64_t enc_nonce;
  uint64_t dec_nonce;
  uint8_t *update_info; /* complete "POST /command" message, NULL = none */
  size_t update_info_len;
} event_ctx_t;

#define EVENT_FRAME_MAX (2U + RTSP_ENCRYPTED_BLOCK_MAX + 16U)

/* Build Shairport's updateInfo "POST /command" (ap2_event_port_post_command():
 * exactly these headers). Runs on the RTSP client task: it reads NVS. */
static uint8_t *event_build_update_info(size_t *out_len) {
  enum { TXT_CAP = 512, PLIST_CAP = 2048, HDR_CAP = 160 };
  *out_len = 0;
  uint8_t *txt = malloc(TXT_CAP);
  uint8_t *plist = malloc(PLIST_CAP);
  uint8_t *msg = heap_caps_malloc(HDR_CAP + PLIST_CAP,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  bool ok = false;
  if (txt && plist && msg) {
    char device_id[18];
    char device_name[65];
    rtsp_get_device_id(device_id, sizeof(device_id));
    settings_get_device_name(device_name, sizeof(device_name));
    const uint64_t features =
        ((uint64_t)AIRPLAY_FEATURES_HI << 32) | AIRPLAY_FEATURES_LO;
    const size_t txt_len = mdns_airplay_txt_record_data(txt, TXT_CAP);
    const size_t plist_len = bplist_build_update_info(
        plist, PLIST_CAP, device_id, device_name, AIRPLAY_MODEL,
        hap_get_public_key(), 32, features, 2, txt, txt_len);
    const int hdr_len = snprintf(
        (char *)msg, HDR_CAP,
        "POST /command RTSP/1.0\r\nContent-Length: %u\r\n"
        "Content-Type: application/x-apple-binary-plist\r\n\r\n",
        (unsigned)plist_len);
    if (txt_len > 0 && plist_len > 0 && hdr_len > 0 && hdr_len < HDR_CAP) {
      memcpy(msg + hdr_len, plist, plist_len);
      *out_len = (size_t)hdr_len + plist_len;
      ok = true;
    }
  }
  if (!ok) ESP_LOGE(TAG, "Event channel: could not build updateInfo");
  free(txt);
  free(plist);
  if (!ok) {
    heap_caps_free(msg);
    msg = NULL;
  }
  return msg;
}

static void event_send_update_info(event_ctx_t *ctx, int client) {
  if (!ctx->have_keys) {
    ESP_LOGW(TAG, "Event channel: no pairing keys, updateInfo not sent");
    return;
  }
  if (!ctx->update_info) return;
  if (rtsp_crypto_seal_send(client, ctx->enc_key, &ctx->enc_nonce,
                            ctx->update_info, ctx->update_info_len) != 0) {
    ESP_LOGW(TAG, "Event channel: sending updateInfo failed");
  }
}

/* Consume what the sender sent. Returns false when the connection ended. */
static bool event_consume(event_ctx_t *ctx, int client, uint8_t *rx,
                          size_t *rx_len, uint8_t *plain, bool *cipher_ok,
                          uint32_t *discarded) {
  const ssize_t n = recv(client, rx + *rx_len, EVENT_FRAME_MAX - *rx_len, 0);
  if (n <= 0) {
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
  }
  *rx_len += (size_t)n;

  if (!*cipher_ok) {
    if (*discarded == 0) {
      ESP_LOGW(TAG, "Event channel: sender sent %u bytes we cannot decrypt "
                    "(discarded)", (unsigned)*rx_len);
    }
    *discarded += (uint32_t)*rx_len;
    *rx_len = 0;
    return true;
  }

  while (*rx_len >= 2) {
    const size_t block_len = (size_t)rx[0] | ((size_t)rx[1] << 8);
    if (block_len == 0 || block_len > RTSP_ENCRYPTED_BLOCK_MAX) {
      ESP_LOGW(TAG, "Event channel: bad frame length %u, discarding the rest",
               (unsigned)block_len);
      *cipher_ok = false;
      *discarded += (uint32_t)*rx_len;
      *rx_len = 0;
      break;
    }
    const size_t frame_len = 2 + block_len + 16;
    if (*rx_len < frame_len) {
      break; // wait for the rest of the frame
    }
    const int plain_len =
        rtsp_crypto_open(ctx->dec_key, &ctx->dec_nonce, rx, frame_len, plain);
    if (plain_len < 0) {
      ESP_LOGW(TAG, "Event channel: decrypt failed, discarding the rest");
      *cipher_ok = false;
      *discarded += (uint32_t)*rx_len;
      *rx_len = 0;
      break;
    }
    /* The sender's replies (200 OK to POST /command) carry nothing we use. */
    memmove(rx, rx + frame_len, *rx_len - frame_len);
    *rx_len -= frame_len;
  }
  return true;
}

static void event_ctx_free(event_ctx_t *ctx) {
  heap_caps_free(ctx->update_info);
  sodium_memzero(ctx, sizeof(*ctx));
  free(ctx);
}

static void event_port_task(void *pvParameters) {
  event_ctx_t *ctx = (event_ctx_t *)pvParameters;
  const int listen_socket = ctx->listen_socket;
  event_listen_socket = listen_socket;
  uint8_t *rx = malloc(EVENT_FRAME_MAX);
  uint8_t *plain = malloc(RTSP_ENCRYPTED_BLOCK_MAX);
  if (!rx || !plain) {
    ESP_LOGE(TAG, "Event port: out of memory");
    event_task_should_stop = true;
  }

  while (!event_task_should_stop && listen_socket >= 0) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listen_socket, &read_fds);

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int ret = select(listen_socket + 1, &read_fds, NULL, NULL, &tv);

    if (event_task_should_stop) {
      break;
    }

    if (ret < 0) {
      if (errno != EINTR && !event_task_should_stop) {
        ESP_LOGE(TAG, "Event port select error: %d", errno);
      }
      break;
    }

    if (ret == 0) {
      continue;
    }

    // Check stop flag after select unblocks (shutdown() makes socket readable)
    if (event_task_should_stop) {
      break;
    }

    if (FD_ISSET(listen_socket, &read_fds)) {
      struct sockaddr_in client_addr;
      socklen_t addr_len = sizeof(client_addr);
      int client =
          accept(listen_socket, (struct sockaddr *)&client_addr, &addr_len);
      if (client < 0) {
        if (!event_task_should_stop) {
          ESP_LOGE(TAG, "Event port accept error: %d", errno);
        }
        break; // Don't continue - socket is likely invalid
      }

      if (event_client_socket >= 0) {
        close(event_client_socket);
      }
      event_client_socket = client;
      ESP_LOGI(TAG, "Event client connected");
      rtsp_events_emit(RTSP_EVENT_CLIENT_CONNECTED, NULL);

      /* A new event connection starts both nonce counters at 0, as the
       * sender does. */
      ctx->enc_nonce = 0;
      ctx->dec_nonce = 0;
      event_send_update_info(ctx, client);

      size_t rx_len = 0;
      bool cipher_ok = ctx->have_keys;
      uint32_t discarded = 0;

      while (event_client_socket >= 0 && !event_task_should_stop) {
        fd_set cfds;
        FD_ZERO(&cfds);
        FD_SET(event_client_socket, &cfds);
        struct timeval ctv = {.tv_sec = 1, .tv_usec = 0};

        ret = select(event_client_socket + 1, &cfds, NULL, NULL, &ctv);
        if (ret < 0) {
          break;
        }
        // Check stop flag after select unblocks
        if (event_task_should_stop) {
          break;
        }
        if (ret > 0 && FD_ISSET(event_client_socket, &cfds) &&
            !event_consume(ctx, event_client_socket, rx, &rx_len, plain,
                           &cipher_ok, &discarded)) {
          ESP_LOGD(TAG, "Event client disconnected");
          close(event_client_socket);
          event_client_socket = -1;
          break;
        }
      }
    }
  }

  if (event_client_socket >= 0) {
    close(event_client_socket);
    event_client_socket = -1;
  }
  free(rx);
  free(plain);
  event_ctx_free(ctx);
  event_listen_socket = -1;
  event_task_handle = NULL;
  vTaskDeleteWithCaps(NULL);
}

static bool event_port_wait_for_task_stopped(int timeout_ticks) {
  while (event_task_handle != NULL && timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return event_task_handle == NULL;
}

esp_err_t rtsp_start_event_port_task(int listen_socket,
                                     const hap_session_t *session) {
  event_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    return ESP_ERR_NO_MEM;
  }
  ctx->listen_socket = listen_socket;
  if (session && session->event_keys_valid) {
    memcpy(ctx->enc_key, session->event_encrypt_key, sizeof(ctx->enc_key));
    memcpy(ctx->dec_key, session->event_decrypt_key, sizeof(ctx->dec_key));
    ctx->have_keys = true;
    ctx->update_info = event_build_update_info(&ctx->update_info_len);
  }
  if (event_task_handle != NULL) {
    rtsp_stop_event_port_task();
    if (!event_port_wait_for_task_stopped(20)) {
      ESP_LOGE(TAG, "Previous event port task did not stop");
      event_ctx_free(ctx);
      return ESP_ERR_INVALID_STATE;
    }
  }
  event_task_should_stop = false;
  event_listen_socket = -1;
  event_task_handle = NULL;
  /* Stack in PSRAM: internal RAM is the scarce resource once the audio
   * engine is up. The task does not touch flash (see event_ctx_t). */
  BaseType_t ret =
      xTaskCreatePinnedToCoreWithCaps(event_port_task, "event_port",
                                      EVENT_STACK_SIZE, ctx, 5,
                                      &event_task_handle, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ret != pdPASS) {
    event_ctx_free(ctx);
    event_task_handle = NULL;
    ESP_LOGE(TAG, "Failed to create event port task");
    return ESP_FAIL;
  }
  return ESP_OK;
}

void rtsp_stop_event_port_task(void) {
  if (event_task_handle == NULL) {
    return;
  }

  // Signal task to stop
  event_task_should_stop = true;

  // Shutdown sockets to unblock select()
  if (event_client_socket >= 0) {
    shutdown(event_client_socket, SHUT_RDWR);
  }
  if (event_listen_socket >= 0) {
    shutdown(event_listen_socket, SHUT_RDWR);
  }

  if (!event_port_wait_for_task_stopped(20)) {
    ESP_LOGW(TAG, "Event port task did not exit within timeout");
  }
}

// Forward declarations of handlers
static void handle_options(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req, const uint8_t *raw,
                           size_t raw_len);
static void handle_get(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len);
static void handle_post(int socket, rtsp_conn_t *conn,
                        const rtsp_request_t *req, const uint8_t *raw,
                        size_t raw_len);
static void handle_announce(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len);
static void handle_setup(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_record(int socket, rtsp_conn_t *conn,
                          const rtsp_request_t *req, const uint8_t *raw,
                          size_t raw_len);
static void handle_set_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len);
static void handle_get_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len);
static void handle_pause(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_flush(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_flushbuffered(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len);
static void handle_teardown(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len);
static void handle_setrateanchortime(int socket, rtsp_conn_t *conn,
                                     const rtsp_request_t *req,
                                     const uint8_t *raw, size_t raw_len);
static void handle_setpeers(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len);

// Dispatch table
static const rtsp_method_handler_t method_handlers[] = {
    {"OPTIONS", handle_options},
    {"GET", handle_get},
    {"POST", handle_post},
    {"ANNOUNCE", handle_announce},
    {"SETUP", handle_setup},
    {"RECORD", handle_record},
    {"SET_PARAMETER", handle_set_parameter},
    {"GET_PARAMETER", handle_get_parameter},
    {"PAUSE", handle_pause},
    {"FLUSH", handle_flush},
    {"FLUSHBUFFERED", handle_flushbuffered},
    {"TEARDOWN", handle_teardown},
    {"SETRATEANCHORTIME", handle_setrateanchortime},
    {"SETPEERS", handle_setpeers},
    {"SETPEERSX", handle_setpeers},
    {NULL, NULL}};

// Parse a named header value from raw RTSP request data (case-insensitive).
// Returns pointer to a static buffer with the trimmed value, or NULL.
static const char *parse_raw_header(const uint8_t *raw, size_t raw_len,
                                    const char *name) {
  static char value_buf[64];
  if (!raw || !name) {
    return NULL;
  }

  const uint8_t *header_end = rtsp_find_header_end(raw, raw_len);
  if (!header_end) {
    return NULL;
  }

  const char *line = (const char *)raw;
  const char *end = (const char *)header_end;
  size_t name_len = strlen(name);

  while (line < end) {
    const char *line_end = line;
    while (line_end < end && *line_end != '\r' && *line_end != '\n') {
      line_end++;
    }

    if ((size_t)(line_end - line) >= name_len &&
        strncasecmp(line, name, name_len) == 0) {
      const char *hdr = line + name_len;
      // Skip optional whitespace
      while (hdr < line_end && (*hdr == ' ' || *hdr == '\t')) {
        hdr++;
      }

      size_t i = 0;
      while (i < sizeof(value_buf) - 1 && hdr < line_end) {
        value_buf[i] = *hdr;
        hdr++;
        i++;
      }
      value_buf[i] = '\0';
      return value_buf;
    }

    line = line_end;
    while (line < end && (*line == '\r' || *line == '\n')) {
      line++;
    }
  }

  return NULL;
}

static bool parse_rtp_info_rtptime(const uint8_t *raw, size_t raw_len,
                                    uint32_t *rtptime) {
  if (!rtptime) return false;
  const char *info = parse_raw_header(raw, raw_len, "RTP-Info:");
  if (!info) return false;
  const char *p = strcasestr(info, "rtptime=");
  if (!p) return false;
  p += 8;
  char *end = NULL;
  unsigned long value = strtoul(p, &end, 10);
  if (end == p) return false;
  *rtptime = (uint32_t)value;
  return true;
}

static bool request_uses_rtsp(const rtsp_request_t *req) {
  return req && strncasecmp(req->protocol, "RTSP/", 5) == 0;
}

/* ---- Handler serialisation ------------------------------------------------
 *
 * Several RTSP connections run at the same time, each in its own
 * client task (Shairport model). The handlers were written for one active
 * connection and share state: parse_raw_header()'s value buffer, the /info
 * response buffers, lazy RSA init, the event-port and metadata-log statics and
 * the global audio/PTP engine. Handler bodies therefore run one at a time.
 * Shairport serialises the same way where it matters (play lock, player
 * mutexes); RTSP requests are short, so a single mutex costs nothing.
 *
 * Lock order: the play lock is always acquired BEFORE this mutex and never
 * while holding it, because acquiring may wait for the previous owner's task,
 * which may itself be waiting here. */
static SemaphoreHandle_t s_dispatch_mutex = NULL;

esp_err_t rtsp_handlers_init(void) {
  if (!s_dispatch_mutex) {
    s_dispatch_mutex = xSemaphoreCreateMutex();
    if (!s_dispatch_mutex) return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void rtsp_handlers_lock(void) {
  if (s_dispatch_mutex) xSemaphoreTake(s_dispatch_mutex, portMAX_DELAY);
}

void rtsp_handlers_unlock(void) {
  if (s_dispatch_mutex) xSemaphoreGive(s_dispatch_mutex);
}

/* ---- Play lock (Shairport principal_conn) --------------------------------
 *
 * Commands that drive the single global audio engine, PTP state, volume or
 * amplifier. On a connection that does not hold the play lock they are
 * acknowledged (200) without side effects, as a non-principal Shairport
 * connection has no player to act on. */
static bool method_needs_play_lock(const char *method) {
  static const char *const methods[] = {
      "RECORD", "SET_PARAMETER", "PAUSE", "FLUSH", "FLUSHBUFFERED",
      "TEARDOWN", "SETRATEANCHORTIME", "SETPEERS", "SETPEERSX", NULL};
  for (int i = 0; methods[i]; ++i) {
    if (strcasecmp(method, methods[i]) == 0) return true;
  }
  return false;
}

/* Requests with which a connection starts to play and therefore takes the
 * play lock (Shairport get_play_lock()): AirPlay 2 stream SETUP, and AirPlay 2
 * initial SETUP with a timing protocol. An initial SETUP with timingProtocol
 * "None" is a remote-control-only session (Shairport remote_control_stream)
 * and must not interrupt playback. AirPlay 1 (ANNOUNCE, SETUP without a
 * bplist) is not supported and never takes the lock. */
static bool request_starts_playback(const rtsp_request_t *req) {
  if (strcasecmp(req->method, "SETUP") != 0) return false;
  const uint8_t *body = req->body;
  const size_t body_len = req->body_len;
  if (!body || body_len < 8 || memcmp(body, "bplist00", 8) != 0) return false;
  size_t streams = 0;
  if (bplist_get_streams_count(body, body_len, &streams)) return true;
  char timing[16] = {0};
  if (bplist_find_string(body, body_len, "timingProtocol", timing,
                         sizeof(timing)) &&
      strcasecmp(timing, "None") == 0) {
    return false;
  }
  return true;
}

static void handle_without_play_lock(int socket, rtsp_conn_t *conn,
                                     const rtsp_request_t *req) {
  ESP_LOGD(TAG, "%s on a connection without the play lock: acknowledged, "
                "playback untouched", req->method);
  if (strcasecmp(req->method, "SET_PARAMETER") == 0 && req->body &&
      strstr(req->content_type, "text/parameters")) {
    /* Remember the volume on this connection; it is applied if this
     * connection later takes the play lock. */
    const char *vol = strstr((const char *)req->body, "volume:");
    if (vol) rtsp_conn_set_volume(conn, strtof(vol + 7, NULL));
  }
  if (strcasecmp(req->method, "TEARDOWN") == 0 && req->body &&
      req->body_len >= 8 && memcmp(req->body, "bplist00", 8) == 0) {
    size_t streams = 0;
    conn->stream_active = false;
    if (!bplist_get_streams_count(req->body, req->body_len, &streams)) {
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Connection: close\r\n", NULL, 0);
      conn->close_after_response = true;
      return;
    }
  }
  rtsp_send_ok(socket, conn, req->cseq);
}

static int dispatch_locked(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req,
                           const uint8_t *raw_request, size_t raw_len);

int rtsp_dispatch(int socket, rtsp_conn_t *conn, const uint8_t *raw_request,
                  size_t raw_len) {
  rtsp_request_t req;
  if (rtsp_request_parse(raw_request, raw_len, &req) < 0) {
    /* Shairport rtsp_read_request() -> rtsp_read_request_response_bad_packet:
     * answer "400 Bad Request" instead of leaving the sender waiting for a
     * reply. The caller NUL-terminates the message, so CSeq can be read. */
    ESP_LOGW(TAG, "Failed to parse RTSP request: 400 Bad Request");
    rtsp_handlers_lock();
    rtsp_send_response(socket, conn, 400, "Bad Request",
                       rtsp_parse_cseq((const char *)raw_request), NULL, NULL,
                       0);
    rtsp_handlers_unlock();
    return -1;
  }

  AUDIO_DIAG_FLUSH_RTSP_BEGIN(socket, req.method);

  /* Play lock first (may wait for the previous owner's task), then the
   * handler mutex - see "Handler serialisation" above for the order. */
  if (!conn->play_owner && request_starts_playback(&req) &&
      !rtsp_server_acquire_play_lock(conn)) {
    rtsp_handlers_lock();
    rtsp_send_response(socket, conn, 453, "Not Enough Bandwidth", req.cseq,
                       NULL, NULL, 0);
    rtsp_handlers_unlock();
    AUDIO_DIAG_FLUSH_RTSP_END(socket, req.method);
    return 0;
  }

  rtsp_handlers_lock();
  const int rc = dispatch_locked(socket, conn, &req, raw_request, raw_len);
  rtsp_handlers_unlock();
  AUDIO_DIAG_FLUSH_RTSP_END(socket, req.method);
  return rc;
}

static int dispatch_locked(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req,
                           const uint8_t *raw_request, size_t raw_len) {
  if (!conn->play_owner && method_needs_play_lock(req->method)) {
    handle_without_play_lock(socket, conn, req);
    return 0;
  }

  // Find handler in dispatch table
  for (const rtsp_method_handler_t *h = method_handlers; h->method; h++) {
    if (strcasecmp(req->method, h->method) == 0) {
      h->handler(socket, conn, req, raw_request, raw_len);
      return 0;
    }
  }

  /* Shairport rtsp_conversation_thread_func(): an unknown method is logged
   * and answered "200 OK" (the dispatcher forces respcode 200), so a sender
   * probing an optional command does not treat the receiver as broken. */
  ESP_LOGI(TAG, "Unknown method: %s %s - answered 200 OK", req->method,
           req->path);
  if (request_uses_rtsp(req)) {
    rtsp_send_ok(socket, conn, req->cseq);
  } else {
    rtsp_send_http_response(socket, conn, 200, "OK", "text/plain", NULL, 0);
  }
  return 0;
}

// ============================================================================
// Handler implementations
// ============================================================================

static void handle_options(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req, const uint8_t *raw,
                           size_t raw_len) {
  const char *public_methods =
      "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, "
      "OPTIONS, POST, GET, SET_PARAMETER, GET_PARAMETER, SETPEERS, "
      "SETRATEANCHORTIME\r\n";

  // Apple-Challenge (RAOP): answered whenever present; iOS in AirPlay 2 mode
  // does not send this header.
  const char *challenge = parse_raw_header(raw, raw_len, "Apple-Challenge:");
  if (challenge) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      uint8_t mac[6];
      esp_read_mac(mac, ESP_MAC_WIFI_STA);

      char response_b64[512];
      if (rsa_apple_challenge_response(challenge, ip_info.ip.addr, mac,
                                       response_b64,
                                       sizeof(response_b64)) == 0) {
        char headers[768];
        snprintf(headers, sizeof(headers), "%sApple-Response: %s\r\n",
                 public_methods, response_b64);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq, headers, NULL,
                           0);
        return;
      }
    }
    ESP_LOGW(TAG, "Failed to build Apple-Challenge response");
  }

  rtsp_send_response(socket, conn, 200, "OK", req->cseq, public_methods, NULL,
                     0);
}

static void handle_get(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (strcmp(req->path, "/info") == 0) {
    // Build info response
    char device_id[18];
    char device_name[65];

    rtsp_get_device_id(device_id, sizeof(device_id));
    settings_get_device_name(device_name, sizeof(device_name));
    const uint8_t *pk = hap_get_public_key();
    uint64_t features =
        ((uint64_t)AIRPLAY_FEATURES_HI << 32) | AIRPLAY_FEATURES_LO;

    int64_t protocol_version = 2;

    if (request_uses_rtsp(req)) {
      static uint8_t body[1024];
      size_t body_len =
          bplist_build_info_response(body, sizeof(body), device_id, device_name,
                                     AIRPLAY_MODEL, pk, 32, features,
                                     protocol_version);
      if (body_len == 0) {
        ESP_LOGE(TAG, "Failed to build binary /info response");
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/x-apple-binary-plist\r\n",
                         (const char *)body, body_len);
      return;
    }

    static char body[4096];
    plist_t p;

    plist_init(&p, body, sizeof(body));
    plist_begin(&p);
    plist_dict_begin(&p);

    plist_dict_string(&p, "deviceid", device_id);
    plist_dict_uint(&p, "features", features);
    plist_dict_string(&p, "model", AIRPLAY_MODEL);
    plist_dict_string(&p, "protovers", "1.1");
    plist_dict_string(&p, "srcvers", AIRPLAY_SOURCE_VERSION);
    plist_dict_int(&p, "vv", protocol_version);
    plist_dict_int(&p, "statusFlags", 4);
    plist_dict_data(&p, "pk", pk, 32);
    plist_dict_string(&p, "pi", "00000000-0000-0000-0000-000000000000");
    plist_dict_string(&p, "name", device_name);

    // Audio formats array
    plist_dict_array_begin(&p, "audioFormats");
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 96);
    plist_dict_int(&p, "audioInputFormats", 0x01000000);
    plist_dict_int(&p, "audioOutputFormats", 0x01000000);
    plist_dict_end(&p);
    plist_array_end(&p);

    // Audio latencies array
    // Both type 96 (realtime/UDP) and type 103 (buffered/TCP) use PTP-based
    // anchor timing with internal output-latency compensation
    // (presentation_anchor_ns() in audio_receiver.c).  Report 0 so the sender does NOT also adjust its
    // anchor — otherwise the hardware pipeline delay is subtracted twice and
    // the ESP plays ahead of other speakers.  shairport-sync likewise
    // reports no audioLatencies at all.
    plist_dict_array_begin(&p, "audioLatencies");
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 96);
    plist_dict_int(&p, "audioType", 0x64);
    plist_dict_int(&p, "inputLatencyMicros", 0);
    plist_dict_int(&p, "outputLatencyMicros", 0);
    plist_dict_end(&p);
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 103);
    plist_dict_int(&p, "audioType", 0x64);
    plist_dict_int(&p, "inputLatencyMicros", 0);
    plist_dict_int(&p, "outputLatencyMicros", 0);
    plist_dict_end(&p);
    plist_array_end(&p);

    plist_dict_end(&p);
    size_t body_len = plist_end(&p);

    rtsp_send_http_response(socket, conn, 200, "OK", "text/x-apple-plist+xml",
                            body, body_len);
  } else {
    ESP_LOGW(TAG, "Unknown GET path: %s", req->path);
    if (request_uses_rtsp(req)) {
      rtsp_send_response(socket, conn, 404, "Not Found", req->cseq,
                         "Content-Type: text/plain\r\n", "Not Found", 9);
    } else {
      rtsp_send_http_response(socket, conn, 404, "Not Found", "text/plain",
                              "Not Found", 9);
    }
  }
}

static void handle_post(int socket, rtsp_conn_t *conn,
                        const rtsp_request_t *req, const uint8_t *raw,
                        size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  if (strstr(req->path, "/pair-setup")) {
    // Create session if needed
    if (!conn->hap_session) {
      conn->hap_session = hap_session_create();
      if (!conn->hap_session) {
        ESP_LOGE(TAG, "Failed to create HAP session");
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
    }

    uint8_t *response = malloc(2048);
    if (!response) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    size_t response_len = 0;
    esp_err_t err = ESP_FAIL;

    if (body && body_len > 0) {
      size_t state_len;
      const uint8_t *state =
          tlv8_find(body, body_len, TLV_TYPE_STATE, &state_len);

      if (state && state_len == 1) {
        switch (state[0]) {
        default:
          break;
        case 1:
          err = hap_pair_setup_m1(conn->hap_session, body, body_len, response,
                                  2048, &response_len);
          break;
        case 3:
          err = hap_pair_setup_m3(conn->hap_session, body, body_len, response,
                                  2048, &response_len);
          break;
        case 5:
          err = hap_pair_setup_m5(conn->hap_session, body, body_len, response,
                                  2048, &response_len);
          break;
        }
      }
    }

    if (err == ESP_OK && response_len > 0) {
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/octet-stream\r\n",
                         (const char *)response, response_len);

      if (conn->hap_session && conn->hap_session->pair_setup_state == 4 &&
          conn->hap_session->session_established) {
        conn->encrypted_mode = true;
      }
    } else {
      ESP_LOGE(TAG, "Pair-setup failed: err=%d", err);
      static const uint8_t error_response[] = {0x06, 0x01, 0x02,
                                               0x07, 0x01, 0x02};
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/octet-stream\r\n",
                         (const char *)error_response, sizeof(error_response));
    }

    free(response);

  } else if (strstr(req->path, "/pair-verify")) {
    if (!conn->hap_session) {
      conn->hap_session = hap_session_create();
      if (!conn->hap_session) {
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
    }

    uint8_t *response = malloc(1024);
    if (!response) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    size_t response_len = 0;
    esp_err_t err = ESP_FAIL;

    if (body && body_len > 0) {
      size_t state_len;
      const uint8_t *state =
          tlv8_find(body, body_len, TLV_TYPE_STATE, &state_len);

      if (state && state_len == 1) {
        if (state[0] == 0x01) {
          err = hap_pair_verify_m1(conn->hap_session, body, body_len, response,
                                   1024, &response_len);
        } else if (state[0] == 0x03) {
          err = hap_pair_verify_m3(conn->hap_session, body, body_len, response,
                                   1024, &response_len);
          // TLV8 pair-verify M3 establishes RTSP channel encryption
          if (err == ESP_OK &&
              conn->hap_session->pair_verify_state == PAIR_VERIFY_STATE_M4) {
            conn->encrypted_mode = true;
            ESP_LOGI(TAG, "RTSP encryption enabled (TLV8 pair-verify)");
          }
        }
      } else {
        // Raw format - used for audio encryption keys, not RTSP encryption
        if (conn->hap_session->pair_verify_state == 0) {
          err = hap_pair_verify_m1_raw(conn->hap_session, body, body_len,
                                       response, 1024, &response_len);
        } else if (conn->hap_session->pair_verify_state ==
                   PAIR_VERIFY_STATE_M2) {
          err = hap_pair_verify_m3_raw(conn->hap_session, body, body_len,
                                       response, 1024, &response_len);
          if (err == ESP_OK) {
            ESP_LOGI(TAG, "Raw pair-verify complete (RTSP unencrypted)");
          }
        }
      }
    }

    if (err == ESP_OK && response_len > 0) {
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/octet-stream\r\n",
                         (const char *)response, response_len);
    } else {
      ESP_LOGE(TAG, "Pair-verify failed, err=%d", err);
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/octet-stream\r\n",
                         "\x06\x01\x04\x07\x01\x02", 6);
    }

    free(response);

  } else if (strstr(req->path, "/fp-setup")) {
    uint8_t *fp_response = NULL;
    size_t fp_response_len = 0;

    if (body && body_len >= 16) {
      if (rtsp_fairplay_handle(body, body_len, &fp_response,
                               &fp_response_len) == 0) {
        rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                           "Content-Type: application/octet-stream\r\n",
                           (const char *)fp_response, fp_response_len);
        free(fp_response);
        return;
      }
    }

    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                       "Content-Type: application/octet-stream\r\n", "\x00", 1);

  } else if (strstr(req->path, "/command")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t cmd_type = 0;
      if (bplist_find_int(body, body_len, "type", &cmd_type)) {
        ESP_LOGI(TAG, "/command type=%lld", (long long)cmd_type);
      }
    }
    rtsp_send_ok(socket, conn, req->cseq);

  } else if (strstr(req->path, "/feedback")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        ESP_LOGI(TAG, "/feedback has networkTimeSecs=%lld", (long long)value);
      }
    }

    // Buffered streams (type 103): always answer with the stream status; it
    // acts as a keepalive that stops the iPhone sending TEARDOWN during a long
    // pause. Realtime AP2 (type 96) answers the same way while its stream is
    // set up, as Shairport Sync handle_feedback() does for 96 and 103.
    if (conn->stream_type == 103 ||
        (conn->stream_type == 96 && conn->stream_active)) {
      uint8_t response[128];
      size_t response_len = bplist_build_feedback_response(
          response, sizeof(response), conn->stream_type, 44100.0);

      if (response_len > 0) {
        ESP_LOGD(
            TAG,
            "/feedback responding with stream status (type=%lld, sr=44100)",
            (long long)conn->stream_type);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                           "Content-Type: application/x-apple-binary-plist\r\n",
                           (const char *)response, response_len);
      } else {
        // Fallback to simple OK if response build fails
        rtsp_send_ok(socket, conn, req->cseq);
      }
    } else {
      // For non-buffered streams, simple OK is fine
      rtsp_send_ok(socket, conn, req->cseq);
    }

  } else {
    rtsp_send_ok(socket, conn, req->cseq);
  }
}

/* ANNOUNCE is AirPlay 1 (RAOP) only. AirPlay 1 is not supported: its stream
 * SETUP is answered 461, so the SDP is ignored and no global audio state is
 * touched. */
static void handle_announce(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len) {
  (void)raw;
  (void)raw_len;
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_setup(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  bool is_bplist =
      strstr(req->content_type, "application/x-apple-binary-plist") != NULL;

  // Check for streams array
  bool request_has_streams = false;
  size_t stream_count = 0;
  if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    if (bplist_get_streams_count(body, body_len, &stream_count)) {
      request_has_streams = true;
    }
  }

  /* AirPlay 1 stream SETUP (Transport header, no bplist streams) and a
   * streams body without the bplist Content-Type are not supported. Reject
   * them before any audio, PTP or play-lock state is touched. */
  if ((!request_has_streams &&
       parse_raw_header(raw, raw_len, "Transport:") != NULL) ||
      (request_has_streams && !is_bplist)) {
    ESP_LOGW(TAG, "SETUP: AirPlay 1 / non-bplist stream SETUP rejected");
    rtsp_send_response(socket, conn, 461, "Unsupported Transport", req->cseq,
                       NULL, NULL, 0);
    return;
  }

  /* Remote-control-only initial SETUP (timingProtocol "None") on a connection
   * without the play lock: answer the handshake but create no event port,
   * no PTP/audio state and no amplifier session - the global event-port task
   * belongs to the playing connection. */
  if (!conn->play_owner) {
    ESP_LOGI(TAG, "SETUP: remote-control-only session; playback untouched");
    uint8_t plist_body[128];
    const size_t plist_len =
        bplist_build_initial_setup(plist_body, sizeof(plist_body), 0);
    if (plist_len == 0) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }
    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                       "Content-Type: application/x-apple-binary-plist\r\n",
                       (const char *)plist_body, plist_len);
    return;
  }

  ESP_LOGI(TAG, "SETUP: has_streams=%d, stream_count=%zu", request_has_streams,
           stream_count);

  if (request_has_streams) {

    /* This receiver owns one audio stream at a time. Validate the first
     * negotiated stream before committing any codec/stream state. Do not let
     * a later unsupported stream dictionary overwrite the active audio format. */
    int64_t stream_type = -1;
    size_t ekey_len = 0, eiv_len = 0, shk_len = 0;
    if (stream_count == 0 ||
        !bplist_get_stream_info(body, body_len, 0, &stream_type, &ekey_len,
                                &eiv_len, &shk_len)) {
      ESP_LOGE(TAG, "SETUP: malformed AirPlay 2 audio stream");
      rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    bplist_kv_info_t kv[16];
    size_t kv_count = 0;
    int64_t codec_type = -1;
    /* Keep the established 44.1 kHz / frame-size defaults when optional keys
     * are omitted, but never guess the codec itself. An explicitly supplied
     * unsupported rate/frame size is still rejected below. */
    int64_t sample_rate = 44100;
    int64_t samples_per_frame =
        stream_type == AUDIO_STREAM_BUFFERED ? 1024 :
        (stream_type == AUDIO_STREAM_REALTIME ? 352 : -1);
    if (!bplist_get_stream_kv_info(body, body_len, 0, kv, 16, &kv_count)) {
      ESP_LOGE(TAG, "SETUP: missing AirPlay 2 audio format dictionary");
      rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    for (size_t k = 0; k < kv_count; k++) {
      if (kv[k].value_type != BPLIST_VALUE_INT) {
        continue;
      }
      if (strcmp(kv[k].key, "ct") == 0) {
        codec_type = kv[k].int_value;
      } else if (strcmp(kv[k].key, "sr") == 0) {
        sample_rate = kv[k].int_value;
      } else if (strcmp(kv[k].key, "spf") == 0) {
        samples_per_frame = kv[k].int_value;
      } else if (strcmp(kv[k].key, "controlPort") == 0) {
        conn->client_control_port = (uint16_t)kv[k].int_value;
      }
    }

    audio_format_t format = {0};
    const bool known_codec = rtsp_codec_configure(
        codec_type, &format, sample_rate, samples_per_frame);
    if (!known_codec ||
        !ap2_audio_format_supported(stream_type, codec_type, sample_rate,
                                    samples_per_frame)) {
      ESP_LOGE(TAG,
               "SETUP: unsupported AP2 audio format type=%lld ct=%lld "
               "sr=%lld spf=%lld (supported: 96/ALAC/44100/352, "
               "103/AAC/44100/1024)",
               (long long)stream_type, (long long)codec_type,
               (long long)sample_rate, (long long)samples_per_frame);
      /* Reject only this SETUP dictionary.  A malformed/unsupported request
       * must not clobber a stream that is already owned by the control
       * session; the sender may retry with a supported stream immediately. */
      rtsp_send_response(socket, conn, 400, "Unsupported Audio Format",
                         req->cseq, NULL, NULL, 0);
      return;
    }

    conn->stream_type = stream_type;
    audio_receiver_set_stream_type((audio_stream_type_t)stream_type);
    audio_receiver_set_format(&format);
  }

  // Process encryption keys
  if (body && body_len > 0) {
    uint8_t ekey_encrypted[64];
    size_t ekey_len = 0;
    uint8_t eiv[16];
    size_t eiv_len = 0;
    uint8_t shk[crypto_aead_chacha20poly1305_ietf_KEYBYTES];
    size_t shk_len = 0;

    int64_t crypto_stream_type = conn->stream_type > 0 ? conn->stream_type : 96;
    bool has_stream_crypto = bplist_find_stream_crypto(
        body, body_len, crypto_stream_type, ekey_encrypted,
        sizeof(ekey_encrypted), &ekey_len, eiv, sizeof(eiv), &eiv_len, shk,
        sizeof(shk), &shk_len);

    if (!has_stream_crypto || (ekey_len == 0 && shk_len == 0)) {
      bplist_find_data_deep(body, body_len, "ekey", ekey_encrypted,
                            sizeof(ekey_encrypted), &ekey_len);
      bplist_find_data_deep(body, body_len, "eiv", eiv, sizeof(eiv), &eiv_len);
      bplist_find_data_deep(body, body_len, "shk", shk, sizeof(shk), &shk_len);
    }

    audio_encrypt_t audio_encrypt = {0};
    bool encryption_set = false;

    if (shk_len == crypto_aead_chacha20poly1305_ietf_KEYBYTES) {
      // Shairport Sync 5.5+: "shk" is a ChaCha20-Poly1305-IETF key and
      // must be exactly crypto_aead_chacha20poly1305_ietf_KEYBYTES bytes.
      audio_encrypt.type = AUDIO_ENCRYPT_CHACHA20_POLY1305;
      memcpy(audio_encrypt.key, shk, sizeof(shk));
      audio_encrypt.key_len = sizeof(shk);
      if (eiv_len >= 16) {
        memcpy(audio_encrypt.iv, eiv, 16);
      }
      audio_receiver_set_encryption(&audio_encrypt);
      encryption_set = true;
    } else if (shk_len != 0) {
      ESP_LOGW(TAG, "SETUP: ignoring invalid shk length %zu (expected %u)",
               shk_len,
               (unsigned)crypto_aead_chacha20poly1305_ietf_KEYBYTES);
    } else if (ekey_len > 16 && conn->hap_session &&
               conn->hap_session->session_established) {
      uint8_t nonce[12] = {0};
      /* AEAD may output every ciphertext byte except the authentication tag. */
      uint8_t decrypted_key[sizeof(ekey_encrypted)];
      unsigned long long decrypted_len;

      if (crypto_aead_chacha20poly1305_ietf_decrypt(
              decrypted_key, &decrypted_len, NULL, ekey_encrypted, ekey_len,
              NULL, 0, nonce, conn->hap_session->shared_secret) == 0 &&
          decrypted_len >= 16) {
        audio_encrypt.type = AUDIO_ENCRYPT_CHACHA20_POLY1305;
        memcpy(audio_encrypt.key, decrypted_key,
               decrypted_len > 32 ? 32 : decrypted_len);
        audio_encrypt.key_len = decrypted_len > 32 ? 32 : decrypted_len;
        if (eiv_len >= 16) {
          memcpy(audio_encrypt.iv, eiv, 16);
        }
        audio_receiver_set_encryption(&audio_encrypt);
        encryption_set = true;
      }
    }

    if (!encryption_set && conn->hap_session &&
        conn->hap_session->session_established) {
      audio_encrypt.type = AUDIO_ENCRYPT_CHACHA20_POLY1305;
      if (hap_derive_audio_key(conn->hap_session, audio_encrypt.key,
                               sizeof(audio_encrypt.key)) == ESP_OK) {
        audio_encrypt.key_len = 32;
        if (eiv_len >= 16) {
          memcpy(audio_encrypt.iv, eiv, 16);
        }
        audio_receiver_set_encryption(&audio_encrypt);
      }
    }
  }

  // Create event port if needed
  if (conn->event_port == 0) {
    conn->event_socket = rtsp_create_event_socket(&conn->event_port);
    if (conn->event_socket >= 0) {
      if (rtsp_start_event_port_task(conn->event_socket, conn->hap_session) ==
          ESP_OK) {
        ESP_LOGI(TAG, "SETUP: Created event port %u", conn->event_port);
      } else {
        close(conn->event_socket);
        conn->event_socket = -1;
        conn->event_port = 0;
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
    }
  }

  // Handle initial SETUP vs stream SETUP
  if (!request_has_streams) {
    reset_metadata_log_dedup();
    ESP_LOGI(TAG, "SETUP: Initial connection setup (no streams)");

    if (is_bplist) {
      uint8_t plist_body[128];
      size_t plist_len = bplist_build_initial_setup(
          plist_body, sizeof(plist_body), conn->event_port);
      if (plist_len == 0) {
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/x-apple-binary-plist\r\n",
                         (const char *)plist_body, plist_len);
    } else {
      rtsp_send_ok(socket, conn, req->cseq);
    }
    amp_session_activate_once(conn);
    return;
  }

  // Stream SETUP
  int64_t stream_type = conn->stream_type;
  if (stream_type == 0) {
    stream_type = 96;
  }

  ESP_LOGI(TAG, "SETUP: Stream setup, stream_type=%lld",
           (long long)stream_type);

  bool buffered = (stream_type == AUDIO_STREAM_BUFFERED);

  /* Only AirPlay 2 realtime ALAC uses the NQPTP-style GM estimator plus
   * local presentation-anchor path. Buffered AAC stays on the PTP anchor
   * (audio_receiver_set_anchor_time) timing path.
   * The RTSP peer IP, not D7 clockIdentity, selects the PTP packet source. */
  ptp_clock_set_realtime_mode(!buffered,
                              !buffered ? conn->client_ip : 0);
  /* PTP is a control-session clock, not an audio-stream buffer.  Preserve a
   * healthy same-session estimator across stream TEARDOWN/SETUP, exactly as
   * the sender preserves its clock timeline. set_realtime_mode() already
   * resets the estimator when the timing mode/peer really changes; a full
   * RTSP disconnect still clears it in connection cleanup.
   *
   * The FIRST stream of a connection must not inherit what the PTP task
   * collected before this session existed (boot, other devices on the
   * network); that stale state can keep the first session after boot
   * silent. Clear exactly once per connection, after SETPEERS and mode
   * selection. */
  if (!conn->ptp_session_fresh) {
    ptp_clock_clear();
    conn->ptp_session_fresh = true;
    ESP_LOGI(TAG, "SETUP: first stream of this connection, PTP estimator reset");
  }

  if (buffered) {
    esp_err_t err = start_buffered_with_recovery();
    if (err != ESP_OK) {
      audio_start_failed("buffered audio start", err);
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }
    conn->buffered_port = audio_receiver_get_buffered_port();
  }

  // AirPlay 2 buffered audio (type 103) uses the TCP dataPort plus PTP.
  // Do not allocate the UDP data/control ports for it: they are not used and
  // only consume scarce lwIP sockets.
  if (!buffered) {
    ensure_stream_ports(conn);
  } else {
    conn->data_port = 0;
    conn->control_port = 0;
  }

  uint16_t response_data_port =
      buffered ? conn->buffered_port : conn->data_port;

  if (!start_audio_receiver_or_fail(socket, conn, req, stream_type)) {
    return;
  }

  // Playout latency.  For REALTIME streams (type 96) the anchor from
  // SETRATEANCHORTIME maps an RTP timestamp onto the sender's source
  // timeline; actual playout is expected latencyMin samples later.  Every
  // reference receiver applies this, so playing at the anchor directly puts
  // this device ~250 ms AHEAD of the rest of a multi-room group (issue #54:
  // an empirical +300 ms offset on a build that subtracted 46 ms of
  // hardware latency — net +254 ms — gave near-perfect sync; 11025 samples
  // is 250.0 ms).  Use the sender's latencyMin from the SETUP stream dict
  // when present and sane, else the AirPlay default of 11025.
  // Buffered streams (type 103) schedule playout with the anchor directly.
  if (buffered) {
    audio_receiver_set_playout_latency_samples(0);
  } else {
    int64_t latency_min = 0;
    const char *latency_src = "default";
    // latencyMin is a per-stream key inside the SETUP streams[] dict, not a
    // top-level key — read it the same way as ct/sr/spf above.
    if (body && body_len > 0) {
      bplist_kv_info_t kv[16];
      size_t kv_count = 0;
      if (bplist_get_stream_kv_info(body, body_len, 0, kv, 16, &kv_count)) {
        for (size_t k = 0; k < kv_count; k++) {
          if (kv[k].value_type == BPLIST_VALUE_INT &&
              strcmp(kv[k].key, "latencyMin") == 0) {
            latency_min = kv[k].int_value;
            break;
          }
        }
      }
    }
    if (latency_min > 0 && latency_min <= 5 * 44100) {
      latency_src = "SETUP latencyMin";
    } else {
      latency_min = AIRPLAY_RT_LATENCY_DEFAULT_SAMPLES;
    }
    audio_receiver_set_playout_latency_samples((uint32_t)latency_min);
    ESP_LOGI(TAG, "Realtime playout latency: %lld samples (%lld ms, %s)",
             (long long)latency_min, (long long)(latency_min * 1000 / 44100),
             latency_src);
  }

  uint8_t plist_body[256];
  uint32_t audio_buffer_size = (stream_type == AUDIO_STREAM_BUFFERED)
      ? (uint32_t)AP2_BUFFERED_AUDIO_ADVERTISED_BYTES : 0U;
  size_t plist_len = bplist_build_stream_setup(
      plist_body, sizeof(plist_body), stream_type, response_data_port,
      conn->control_port, audio_buffer_size);
  if (plist_len == 0) {
    audio_receiver_stop();
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                       NULL, 0);
    return;
  }
  ESP_LOGI(TAG,
           "SETUP response: type=%lld dataPort=%u controlPort=%u audioBufferSize=%u",
           (long long)stream_type, response_data_port, conn->control_port,
           (unsigned)audio_buffer_size);
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Content-Type: application/x-apple-binary-plist\r\n",
                     (const char *)plist_body, plist_len);

  // Enable NACK retransmission if we know the client's control port
  if (conn->client_control_port > 0 && conn->client_ip != 0) {
    audio_receiver_set_client_control(conn->client_ip,
                                      conn->client_control_port);
  }

  conn->stream_active = true;
  amp_session_activate_once(conn);

  if (stream_type == AUDIO_STREAM_REALTIME) {
    /* AP2 realtime ALAC (type 96) needs the play gate open here: in this
     * transport the presentation mapping can arrive through D7 and the
     * sender may never send SETRATEANCHORTIME rate=1. Buffered AP2 AAC
     * (type 103) remains gated exclusively by SETRATEANCHORTIME rate=1. */
    audio_receiver_set_playing(true);
    conn->stream_paused = false;
    rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);
  }
}

static void handle_record(int socket, rtsp_conn_t *conn,
                          const rtsp_request_t *req, const uint8_t *raw,
                          size_t raw_len) {
  (void)raw;
  (void)raw_len;

  /* AirPlay 2 RECORD is a control-plane acknowledgement.  The buffered
   * transport may not even have been SETUP yet, and there is no presentation
   * anchor here.  Do not change playing/paused state here: buffered AP2 is
   * opened by SETRATEANCHORTIME, while realtime AP2 is opened by stream SETUP
   * and remains blocked downstream until D7/PTP timing becomes valid. */
  char headers[128];
  snprintf(headers, sizeof(headers),
           "Audio-Latency: 0\r\n"
           "Audio-Jack-Status: connected\r\n");
  rtsp_send_response(socket, conn, 200, "OK", req->cseq, headers, NULL, 0);
}

// ============================================================================
// Metadata and Progress Logging Helpers
// ============================================================================

/**
 * Format time in seconds as mm:ss string
 * @param seconds Time in seconds
 * @param out Output buffer (at least 8 bytes for "999:59\0")
 * @param out_size Size of output buffer
 */
static void format_time_mmss(uint32_t seconds, char *out, size_t out_size) {
  uint32_t mins = seconds / 60;
  uint32_t secs = seconds % 60;
  snprintf(out, out_size, "%" PRIu32 ":%02" PRIu32, mins, secs);
}

/**
 * Parse DMAP-tagged data and extract metadata into struct
 * DMAP format: 4-byte tag, 4-byte BE length, data
 * Common tags:
 *   minm = item name (track title)
 *   asar = artist
 *   asal = album
 *   asgn = genre
 *   asai = album id (64-bit)
 */
#define DMAP_MAX_NESTING 8

static void parse_dmap_metadata(const uint8_t *data, size_t len,
                                rtsp_metadata_t *meta, int depth) {
  size_t pos = 0;

  if (depth > DMAP_MAX_NESTING) {
    return;
  }

  while (pos + 8 <= len) {
    // Read 4-byte tag
    char tag[5] = {0};
    memcpy(tag, data + pos, 4);
    pos += 4;

    // Read 4-byte big-endian length
    uint32_t item_len = ((uint32_t)data[pos] << 24) |
                        ((uint32_t)data[pos + 1] << 16) |
                        ((uint32_t)data[pos + 2] << 8) | data[pos + 3];
    pos += 4;

    // Subtraction, not pos + item_len: that addition wraps on 32-bit size_t
    // for a hostile length and would let a malformed frame read out of bounds.
    if (item_len > len - pos) {
      break; // Malformed
    }

    // Extract known metadata tags
    if (strcmp(tag, "minm") == 0 && item_len > 0) {
      size_t copy_len = item_len < METADATA_STRING_MAX - 1
                            ? item_len
                            : METADATA_STRING_MAX - 1;
      memcpy(meta->title, data + pos, copy_len);
      meta->title[copy_len] = '\0';
    } else if (strcmp(tag, "asar") == 0 && item_len > 0) {
      size_t copy_len = item_len < METADATA_STRING_MAX - 1
                            ? item_len
                            : METADATA_STRING_MAX - 1;
      memcpy(meta->artist, data + pos, copy_len);
      meta->artist[copy_len] = '\0';
    } else if (strcmp(tag, "asal") == 0 && item_len > 0) {
      size_t copy_len = item_len < METADATA_STRING_MAX - 1
                            ? item_len
                            : METADATA_STRING_MAX - 1;
      memcpy(meta->album, data + pos, copy_len);
      meta->album[copy_len] = '\0';
    } else if (strcmp(tag, "asgn") == 0 && item_len > 0) {
      size_t copy_len = item_len < METADATA_STRING_MAX - 1
                            ? item_len
                            : METADATA_STRING_MAX - 1;
      memcpy(meta->genre, data + pos, copy_len);
      meta->genre[copy_len] = '\0';
    } else if (strcmp(tag, "mlit") == 0 || strcmp(tag, "cmst") == 0 ||
               strcmp(tag, "mdst") == 0) {
      // Container tags - recurse into them
      parse_dmap_metadata(data + pos, item_len, meta, depth + 1);
    }

    pos += item_len;
  }
}

/**
 * Parse progress string and populate metadata fields
 * Progress format: "start/current/end" in RTP timestamp units
 * Sample rate is typically 44100
 */
static void parse_progress(const char *progress_str, uint32_t sample_rate,
                           rtsp_metadata_t *meta) {
  uint64_t start = 0, current = 0, end = 0;

  // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
  if (sscanf(progress_str, "%" PRIu64 "/%" PRIu64 "/%" PRIu64, &start, &current,
             &end) == 3) {
    if (sample_rate == 0) {
      sample_rate = 44100; // Default sample rate
    }

    meta->position_secs = (uint32_t)((current - start) / sample_rate);
    meta->duration_secs = (uint32_t)((end - start) / sample_rate);

    char pos_str[16], dur_str[16];
    format_time_mmss(meta->position_secs, pos_str, sizeof(pos_str));
    format_time_mmss(meta->duration_secs, dur_str, sizeof(dur_str));

    if (should_log_progress(meta->duration_secs)) {
      ESP_LOGI(TAG,
               "Progress: %s / %s (raw: %" PRIu64 "/%" PRIu64 "/%" PRIu64 ")",
               pos_str, dur_str, start, current, end);
    }
  }
}

static void handle_set_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  rtsp_event_data_t event_data;
  memset(&event_data, 0, sizeof(event_data));
  bool has_metadata = false;

  // Check for progress header in raw request
  const char *progress_hdr = strstr((const char *)raw, "progress:");
  if (!progress_hdr) {
    progress_hdr = strstr((const char *)raw, "Progress:");
  }
  if (progress_hdr) {
    // Find end of header line
    const char *line_end = strstr(progress_hdr, "\r\n");
    if (line_end) {
      size_t val_start = 9; // Length of "progress:"
      while (progress_hdr[val_start] == ' ') {
        val_start++;
      }
      char progress_val[64];
      size_t val_len = line_end - (progress_hdr + val_start);
      if (val_len < sizeof(progress_val)) {
        memcpy(progress_val, progress_hdr + val_start, val_len);
        progress_val[val_len] = '\0';
        parse_progress(progress_val, 44100, &event_data.metadata);
        has_metadata = true;
      }
    }
  }

  if (strstr(req->content_type, "text/parameters")) {
    if (body) {
      if (strstr((const char *)body, "volume:")) {
        const char *vol = strstr((const char *)body, "volume:");
        if (vol) {
          float volume = strtof(vol + 7, NULL);
          rtsp_conn_set_volume(conn, volume);
        }
      }
      // Playback progress ("progress: start/current/end")
      const char *prog = strstr((const char *)body, "progress:");
      if (prog) {
        prog += 9;
        while (*prog == ' ') {
          prog++;
        }
        parse_progress(prog, 44100, &event_data.metadata);
        has_metadata = true;
      }
    }
  } else if (strstr(req->content_type, "application/x-dmap-tagged")) {
    // DMAP-tagged track metadata
    if (body && body_len > 0) {
      parse_dmap_metadata(body, body_len, &event_data.metadata, 0);
      if (should_log_dmap_metadata(&event_data.metadata)) {
        ESP_LOGI(TAG, "Received DMAP metadata (%zu bytes)", body_len);
        if (event_data.metadata.album[0])
          ESP_LOGI(TAG, "  Album  = %s", event_data.metadata.album);
        if (event_data.metadata.artist[0])
          ESP_LOGI(TAG, "  Artist = %s", event_data.metadata.artist);
        if (event_data.metadata.genre[0])
          ESP_LOGI(TAG, "  Genre  = %s", event_data.metadata.genre);
        if (event_data.metadata.title[0])
          ESP_LOGI(TAG, "  Title  = %s", event_data.metadata.title);
      }
      has_metadata = true;
    }
  } else if (strstr(req->content_type, "image/jpeg") ||
             strstr(req->content_type, "image/png")) {
#ifdef CONFIG_ENABLE_AIRPLAY_ARTWORK
    // Artwork is received but not used; log it.
    ESP_LOGI(TAG, "Received artwork: %s (%zu bytes)", req->content_type,
             body_len);
#else
    // Artwork reception disabled — ignore it.  The md txt record already asks
    // senders not to transmit cover art, but some send it regardless.
    ESP_LOGD(TAG, "Ignoring artwork (%s, %zu bytes): disabled in config",
             req->content_type, body_len);
#endif
  } else if (strstr(req->content_type, "application/x-apple-binary-plist")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        ESP_LOGI(TAG, "SET_PARAMETER: networkTimeSecs=%lld", (long long)value);
      }
      double rate;
      if (bplist_find_real(body, body_len, "rate", &rate)) {
        ESP_LOGI(TAG, "SET_PARAMETER: rate=%.2f", rate);
      }
      // Try to extract metadata from bplist (AirPlay 2)
      char str_val[METADATA_STRING_MAX];
      if (bplist_find_string(body, body_len, "itemName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI(TAG, "Metadata: Title = %s", str_val);
        strlcpy(event_data.metadata.title, str_val, METADATA_STRING_MAX);
        has_metadata = true;
      }
      if (bplist_find_string(body, body_len, "artistName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI(TAG, "Metadata: Artist = %s", str_val);
        strlcpy(event_data.metadata.artist, str_val, METADATA_STRING_MAX);
        has_metadata = true;
      }
      if (bplist_find_string(body, body_len, "albumName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI(TAG, "Metadata: Album = %s", str_val);
        strlcpy(event_data.metadata.album, str_val, METADATA_STRING_MAX);
        has_metadata = true;
      }
      // Progress info from bplist
      double elapsed = 0, duration = 0;
      if (bplist_find_real(body, body_len, "elapsed", &elapsed)) {
        event_data.metadata.position_secs = (uint32_t)elapsed;
        has_metadata = true;
        char elapsed_str[16];
        format_time_mmss((uint32_t)elapsed, elapsed_str, sizeof(elapsed_str));
        if (bplist_find_real(body, body_len, "duration", &duration)) {
          event_data.metadata.duration_secs = (uint32_t)duration;
          char duration_str[16];
          format_time_mmss((uint32_t)duration, duration_str,
                           sizeof(duration_str));
          if (should_log_progress(event_data.metadata.duration_secs)) {
            ESP_LOGI(TAG, "Progress: %s / %s", elapsed_str, duration_str);
          }
        } else if (should_log_progress(0)) {
          ESP_LOGI(TAG, "Progress: %s", elapsed_str);
        }
      }
    }
  }

  if (has_metadata) {
    rtsp_events_emit(RTSP_EVENT_METADATA, &event_data);
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_get_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (req->body && req->body_len > 0) {
    if (strstr((const char *)req->body, "volume")) {
      char vol_response[32];
      int vol_len = snprintf(vol_response, sizeof(vol_response),
                             "volume: %.2f\r\n", conn->volume_db);
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: text/parameters\r\n", vol_response,
                         vol_len);
      return;
    }
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_pause(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw;
  (void)raw_len;

  ESP_LOGI(TAG, "PAUSE received");

  note_stream_pause_started(conn);

  // Stop the audio consumer but leave the buffer filling.  The phone will
  // send a fresh SETRATEANCHORTIME (rate=1) anchor on resume that re-aligns
  // the buffered frames to the correct wall-clock position.
  audio_receiver_pause();
  conn->stream_paused = true;

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_flush(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  ESP_LOGI(TAG, "FLUSH received");
  if (conn->stream_type == AUDIO_STREAM_REALTIME) {
    uint32_t flush_rtp = 0;
    if (parse_rtp_info_rtptime(raw, raw_len, &flush_rtp)) {
      ESP_LOGI(TAG,
               "FLUSH realtime RTP-Info rtptime=%" PRIu32
               " — preserving D7/SETRATE timing",
               flush_rtp);
      audio_receiver_realtime_flush_to_rtp(flush_rtp);
    } else {
      /* No RTP boundary means we cannot know which buffered realtime samples
       * remain valid. Invalidate the epoch and wait for the next REAL sender
       * D7/SETRATE anchor. realtime_pcm_sink has no local-arrival fallback. */
      ESP_LOGW(TAG,
               "FLUSH realtime without RTP-Info — waiting for fresh sender anchor");
      audio_receiver_realtime_flush_wait_sender_anchor();
    }
  } else {
    // Buffered stream: seek flush.
    audio_receiver_seek_flush();
  }
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_flushbuffered(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  // AirPlay 2 FLUSHBUFFERED carries an optional bplist with:
  //   flushFromSeq / flushFromTS  — first sequence/timestamp to discard
  //   flushUntilSeq / flushUntilTS — exclusive end boundary
  //
  // Shairport-style sequential semantics:
  // - no flushFromSeq: immediate flush; the packet consumer discards forward
  //   until it reaches/overshoots flushUntilSeq.
  // - flushFromSeq present: register a deferred [from, until) discard rule.
  // The raw TCP byte FIFO is never searched or rebound by a FLUSH command.
  if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    int64_t flush_from_seq = 0, flush_from_ts = 0;
    int64_t flush_until_seq = 0, flush_until_ts = 0;
    bool got_from_seq =
        bplist_find_int(body, body_len, "flushFromSeq", &flush_from_seq);
    (void)bplist_find_int(body, body_len, "flushFromTS", &flush_from_ts);
    bool got_until_seq =
        bplist_find_int(body, body_len, "flushUntilSeq", &flush_until_seq);
    (void)bplist_find_int(body, body_len, "flushUntilTS", &flush_until_ts);

    /* Shairport Sync 5.5.2 handle_flushbuffered(): deferred iff flushFromSeq is
     * present (the other fields default to 0 when missing). */
    if (got_from_seq) {
      ESP_LOGI(TAG,
               "FLUSHBUFFERED deferred: fromSeq=%" PRId64 " fromTS=%" PRId64
               " untilSeq=%" PRId64 " untilTS=%" PRId64,
               flush_from_seq, flush_from_ts, flush_until_seq, flush_until_ts);
      esp_err_t flush_err = audio_receiver_set_deferred_flush_range(
          (uint32_t)flush_from_seq, (uint32_t)flush_from_ts,
          (uint32_t)flush_until_seq, (uint32_t)flush_until_ts);
      if (flush_err != ESP_OK) {
        /* Shairport: "no more room for deferred flush request records" is
         * only logged; the reply is still 200. */
        ESP_LOGW(TAG, "FLUSHBUFFERED deferred not stored (%s)",
                 esp_err_to_name(flush_err));
      }
    } else {
      /* Shairport 5.5.2: the absence of flushFromSeq selects immediate mode.
       * ESP deviation: flushUntilSeq 0 / missing is not taken as
       * sequence number 0 - modulo 2^23 that discards everything until the
       * sequence wraps whenever the session's numbers are above 2^22. The
       * flush then ends by timestamp (untilTS marker or new anchor). */
      const bool until_seq_valid = got_until_seq && flush_until_seq != 0;
      if (until_seq_valid) {
        ESP_LOGI(TAG,
                 "FLUSHBUFFERED immediate: untilSeq=%" PRId64
                 " untilTS=%" PRId64,
                 flush_until_seq, flush_until_ts);
      } else {
        ESP_LOGI(TAG,
                 "FLUSHBUFFERED immediate: untilSeq=%s untilTS=%" PRId64
                 " (ends by timestamp)",
                 got_until_seq ? "0" : "missing", flush_until_ts);
      }
      audio_receiver_set_immediate_flush((uint32_t)flush_until_seq,
                                         (uint32_t)flush_until_ts,
                                         until_seq_valid);
    }
  }

  /* Shairport: a FLUSHBUFFERED without a plist does nothing and returns 200. */

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_teardown(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  bool has_streams = false;
  size_t stream_count = 0;

  if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    if (bplist_get_streams_count(body, body_len, &stream_count)) {
      has_streams = true;
    }
  }

  // TEARDOWN with streams = stream teardown (may be followed by new SETUP)
  // TEARDOWN without streams = full session teardown (disconnect)
  /* Shairport Sync 5.5.2 handle_teardown_2(): no plist -> nothing done. */
  if (!(body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0)) {
    ESP_LOGW(TAG, "TEARDOWN without plist - nothing done (as Shairport)");
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }
  ESP_LOGI(TAG, "TEARDOWN: has_streams=%d stream_count=%zu", has_streams,
           stream_count);
  // Stream-level teardown is a pause: freeze playout immediately so audio
  // silences on ALL boards. playing=false makes the output emit silence at
  // once (software mute, for software-volume/DAC-less boards); the synchronous
  // PAUSED event mutes the hardware DAC.  Both run ahead of the slower
  // receiver/decoder teardown below (audio_receiver_stop can block ~1 s
  // waiting for the listener task to exit).
  if (has_streams) {
    note_stream_pause_started(conn);
    audio_receiver_set_playing(false);
    rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);
  }
  audio_receiver_stop();
  /* A stream-level TEARDOWN preserves the control session and its PTP clock.
   * Clearing it here needlessly forces a new PTP STEP/relock before the next
   * buffered stream can produce PCM.  Only a full session boundary owns a
   * full PTP reset. */
  if (!has_streams) {
    ptp_clock_clear();
    audio_receiver_set_stream_type(AUDIO_STREAM_NONE);
    audio_receiver_set_encryption(NULL);
  }
  conn->stream_active = false;
  conn->stream_paused =
      has_streams; // Keep session ready if only streams torn down

  if (!has_streams) {
    /* Full AirPlay session teardown starts the amplifier grace period now.
     * rtsp_conn_free() remains the backstop for unexpected socket loss. */
    amp_session_deactivate_once(conn);
  }

  if (!has_streams) {
    // Shairport Sync 5.1+ handle_teardown_2(): a valid AP2 TEARDOWN plist
    // without a streams item means terminate the RTSP connection. Send the
    // response first, advertise the close, then let the client task unwind.
    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                       "Connection: close\r\n", NULL, 0);
    conn->close_after_response = true;
    return;
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_setrateanchortime(int socket, rtsp_conn_t *conn,
                                     const rtsp_request_t *req,
                                     const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  double rate = 1.0;
  bool have_rate = false;
  uint64_t clock_id = 0;
  uint64_t network_time_secs = 0;
  uint64_t network_time_frac = 0;
  uint64_t rtp_time = 0;
  bool have_network_time_secs = false;

  if (body && body_len > 0 && body_len >= 8 &&
      memcmp(body, "bplist00", 8) == 0) {
    if (bplist_find_real(body, body_len, "rate", &rate)) {
      have_rate = true;
    } else {
      int64_t rate_int;
      if (bplist_find_int(body, body_len, "rate", &rate_int)) {
        rate = (double)rate_int;
        have_rate = true;
      }
    }

    int64_t value;
    if (bplist_find_int(body, body_len, "networkTimeTimelineID", &value)) {
      clock_id = (uint64_t)value;
    }
    if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
      network_time_secs = (uint64_t)value;
      have_network_time_secs = true;
    }
    if (bplist_find_int(body, body_len, "networkTimeFrac", &value)) {
      network_time_frac = (uint64_t)value;
    }
    if (bplist_find_int(body, body_len, "rtpTime", &value)) {
      rtp_time = (uint64_t)value;
    }

    ESP_LOGI(TAG,
             "SETRATEANCHORTIME: secs=%llu frac=0x%016llx rtp=%llu"
             " clock=%016llx rate=%.1f stream=%lld",
             (unsigned long long)network_time_secs,
             (unsigned long long)network_time_frac,
             (unsigned long long)rtp_time,
             (unsigned long long)clock_id, rate,
             (long long)conn->stream_type);

    /* Pause first.  Do not publish an anchor from a rate=0 message and wake a
     * processor immediately before closing the play gate. */
    if (have_rate && rate == 0.0) {
      ESP_LOGI(TAG, "SETRATEANCHORTIME: rate=0 -> PAUSING");
      note_stream_pause_started(conn);
      conn->stream_paused = true;
      audio_receiver_pause();
      rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);
      rtsp_send_ok(socket, conn, req->cseq);
      return;
    }

    /* RTP is a wrapping 32-bit timeline. rtpTime==0 is therefore perfectly
     * valid; validity comes from key presence, not from the numeric value. */
    /* Shairport Sync 5.5.2: the anchor is set whenever networkTimeSecs is
     * present; rtpTime defaults to 0 if missing. */
    if (have_network_time_secs) {
      uint64_t frac = network_time_frac >> 32;
      frac = (frac * 1000000000ULL) >> 32;
      uint64_t network_time_ns = network_time_secs * 1000000000ULL + frac;
      ESP_LOGI(TAG,
               "SETRATEANCHORTIME MAP: clock=%016llx ptp=%llu rtp=%llu",
               (unsigned long long)clock_id,
               (unsigned long long)network_time_ns,
               (unsigned long long)rtp_time);

      /* Realtime SETRATEANCHORTIME follows the same rule as D7: convert the
       * sender's current GM timestamp once into ESP monotonic time. If the new
       * GM is still acquiring, keep the already-running local ALAC map and
       * defer this observation; never inject a raw PTP epoch into audio. */
      ptp_realtime_snapshot_t ps = {0};
      ptp_clock_get_realtime_snapshot(&ps);
      if (ps.realtime_mode) {
        ptp_clock_set_master_clock_id(clock_id); /* observation/hint only */
        uint64_t local_ns = 0;
        if (ptp_clock_realtime_snapshot_to_local(&ps, clock_id, network_time_ns,
                                              &local_ns)) {
          ESP_LOGI(TAG,
                   "SETRATEANCHORTIME RT local raw=%llu local=%llu gm=%016llx age=%lums",
                   (unsigned long long)network_time_ns,
                   (unsigned long long)local_ns,
                   (unsigned long long)ps.master_clock_id,
                   (unsigned long)ps.mastership_age_ms);
          audio_realtime_anchor_result_t anchor_result = {0};
          if (!audio_receiver_set_realtime_anchor_local(
                  clock_id, ps.gm_change_count, ps.mastership_age_ms,
                  network_time_ns, local_ns, (uint32_t)rtp_time,
                  &anchor_result)) {
            ESP_LOGI(TAG,
                     "SETRATEANCHORTIME RT media rebase deferred clock=%016llx gm=%016llx epoch=%lu age=%lums",
                     (unsigned long long)clock_id,
                     (unsigned long long)ps.master_clock_id,
                     (unsigned long)ps.gm_change_count,
                     (unsigned long)ps.mastership_age_ms);
          }
        } else {
          ESP_LOGI(TAG,
                   "SETRATEANCHORTIME RT deferred clock=%016llx gm=%016llx ready=%d age=%lums",
                   (unsigned long long)clock_id,
                   (unsigned long long)ps.master_clock_id,
                   ps.master_ready ? 1 : 0,
                   (unsigned long)ps.mastership_age_ms);
        }
      } else {
        audio_receiver_set_anchor_time(clock_id, network_time_ns,
                                       (uint32_t)rtp_time);
      }
    }
  }

  /* Shairport Sync 5.5.2 handle_setrateanchori(): play state changes only
   * when "rate" is present. Without rate, update any supplied anchor and
   * leave the play state unchanged. */
  if (!have_rate) {
    ESP_LOGI(TAG, "SETRATEANCHORTIME: no rate -> play state unchanged");
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }
  ESP_LOGI(TAG, "SETRATEANCHORTIME: rate=%.1f -> RESUMING (was_paused=%d)",
           rate, conn->stream_paused);
  if (conn->stream_paused) notify_timing_resume(conn);
  conn->stream_paused = false;
  audio_receiver_set_playing(true);
  rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_setpeers(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  const bool extended = strcasecmp(req->method, "SETPEERSX") == 0;

  /* An empty peer-list means there is no advertised admission set. Clearing
   * it is safe: PTP falls back to the authoritative D7 clock id (buffered) or
   * the RTSP timing peer (realtime). */
  if (!body || body_len == 0) {
    ptp_clock_set_peers(NULL, 0);
    ESP_LOGI(TAG, "%s: peers cleared", req->method);
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }

  if (body_len < 8 || memcmp(body, "bplist00", 8) != 0) {
    /* Keep the previous valid list when a sender uses an unsupported body
     * encoding. Never let peer metadata break an active audio timeline. */
    ESP_LOGW(TAG, "%s: unsupported peer-list body (%zu bytes)", req->method,
             body_len);
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }

  /* The parsed list is ~4.4 KiB (16 peers x 4 addresses x 64 B), too large
   * for the 8 KiB RTSP client stack, so it lives in PSRAM for the duration of
   * the request. */
  bplist_peer_info_t *parsed = heap_caps_calloc(
      PTP_CLOCK_MAX_PEERS, sizeof(*parsed), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!parsed) {
    ESP_LOGW(TAG, "%s: no memory for the peer list; keeping the previous one",
             req->method);
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }
  size_t advertised_count = 0;
  if (!bplist_get_peer_list(body, body_len, extended, parsed,
                            PTP_CLOCK_MAX_PEERS, &advertised_count)) {
    heap_caps_free(parsed);
    ESP_LOGW(TAG, "%s: invalid peer-list bplist", req->method);
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }

  ptp_clock_peer_t tracked[PTP_CLOCK_MAX_PEERS] = {0};
  size_t tracked_count = 0;
  size_t ipv4_count = 0;
  size_t ipv6_count = 0;
  size_t clock_count = 0;
  size_t invalid_addr_count = 0;
  const size_t parsed_count = advertised_count < PTP_CLOCK_MAX_PEERS
                                  ? advertised_count
                                  : PTP_CLOCK_MAX_PEERS;

  for (size_t i = 0; i < parsed_count; ++i) {
    bool stored_ipv4_for_peer = false;
    if (parsed[i].has_clock_id) clock_count++;

    for (size_t a = 0; a < parsed[i].address_count; ++a) {
      struct in_addr addr4 = {0};
      struct in6_addr addr6 = {0};
      const char *address = parsed[i].addresses[a];

      if (inet_pton(AF_INET, address, &addr4) == 1) {
        ipv4_count++;
        stored_ipv4_for_peer = true;
        if (tracked_count < PTP_CLOCK_MAX_PEERS) {
          bool duplicate = false;
          for (size_t j = 0; j < tracked_count; ++j) {
            if (tracked[j].ipv4_addr == addr4.s_addr &&
                tracked[j].clock_id ==
                    (parsed[i].has_clock_id ? parsed[i].clock_id : 0)) {
              duplicate = true;
              break;
            }
          }
          if (!duplicate) {
            tracked[tracked_count].ipv4_addr = addr4.s_addr;
            tracked[tracked_count].clock_id =
                parsed[i].has_clock_id ? parsed[i].clock_id : 0;
            tracked_count++;
          }
        }
      } else if (inet_pton(AF_INET6, address, &addr6) == 1) {
        /* PTP sockets are currently AF_INET. Keep IPv6 visible in diagnostics,
         * but never activate a filter from IPv6-only metadata. */
        ipv6_count++;
      } else {
        invalid_addr_count++;
      }
    }

    /* Preserve SETPEERSX identity even for IPv6-only peers. ClockID is tracked
     * for diagnostics/future port matching but is intentionally not a hard
     * admission key in ptp_clock. */
    if (!stored_ipv4_for_peer && parsed[i].has_clock_id &&
        tracked_count < PTP_CLOCK_MAX_PEERS) {
      bool duplicate = false;
      for (size_t j = 0; j < tracked_count; ++j) {
        if (tracked[j].ipv4_addr == 0 &&
            tracked[j].clock_id == parsed[i].clock_id) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        tracked[tracked_count].clock_id = parsed[i].clock_id;
        tracked_count++;
      }
    }
  }

  heap_caps_free(parsed);
  ptp_clock_set_peers(tracked, tracked_count);

  ESP_LOGI(TAG,
           "%s: peers=%zu tracked=%zu ipv4=%zu ipv6=%zu clocks=%zu%s%s",
           req->method, advertised_count, tracked_count, ipv4_count,
           ipv6_count, clock_count,
           advertised_count > PTP_CLOCK_MAX_PEERS ? " truncated" : "",
           invalid_addr_count ? " invalid-address" : "");

  /* Do NOT reset audio timing here. Buffered AAC keeps its existing RTP<->PTP
   * map; realtime ALAC keeps its RTP<->ESP-local anchor. A new admitted PTP
   * source/GM is qualified by the existing handover logic. */

  rtsp_send_ok(socket, conn, req->cseq);
}
