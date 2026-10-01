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
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sodium.h"

#include "audio_receiver.h"
#include "apap_frame_queue.h"
#include "audio_diag.h"
#include "amp_control.h"
#include "hap.h"
#include "ptp_clock.h"
#include "ptp_clock_engine.h"
#include "plist.h"
#include "rtsp_fairplay.h"
#include "rtsp_rsa.h"
#include "settings.h"
#include "socket_utils.h"
#include "tlv8.h"

#include "rtsp_crypto.h"
#include "rtsp_connection_model.h"
#include "rtsp_timeline_control.h"
#include "rtsp_datastream.h"
#include "rtsp_apap_audio.h"
#include "rtsp_protocol_trace.h"
#include "media_remote_state.h"
#include "airplay_identity.h"
#include "ap2_control_watch.h"
#include "rtsp_events.h"
#include "rtsp_server.h"
#include "mdns_airplay.h"
#include "wifi.h"

static const char *TAG = "rtsp_handlers";

static uint64_t diag_session_elapsed_ms(const rtsp_conn_t *conn) {
  if (!conn || conn->diag_audio_setup_us == 0) return 0;
  const int64_t now_us = esp_timer_get_time();
  if (now_us <= 0 || (uint64_t)now_us <= conn->diag_audio_setup_us) return 0;
  return ((uint64_t)now_us - conn->diag_audio_setup_us) / 1000ULL;
}

static void diag_log_bplist(const char *label, const uint8_t *body, size_t len) {
  if (!label) label = "plist";
  if (!body || len == 0) {
    ESP_LOGI(TAG, "%s: <empty>", label);
    return;
  }
  if (len < 8 || memcmp(body, "bplist00", 8) != 0) {
    ESP_LOGI(TAG, "%s: %uB non-bplist", label, (unsigned)len);
    return;
  }
  char *desc = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!desc) {
    ESP_LOGW(TAG, "%s: %uB bplist (describe alloc failed)", label,
             (unsigned)len);
    return;
  }
  if (bplist_describe(body, len, desc, 4096) > 0) {
    ESP_LOGI(TAG, "%s: %s", label, desc);
  } else {
    ESP_LOGW(TAG, "%s: %uB bplist (describe failed)", label, (unsigned)len);
  }
  heap_caps_free(desc);
}

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
  if (stream_type == AUDIO_STREAM_APAP) {
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
/* updateInfo is intentionally deferred until RECORD has been acknowledged.
 * The event task owns the event-channel TX nonce, so the RTSP task only sets
 * this request flag; the event task performs the encrypted send itself. */
static volatile bool event_update_info_requested = false;
static volatile bool event_update_info_sent = false;

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

static bool start_realtime_receiver_or_fail(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req) {
  esp_err_t err =
      audio_receiver_start_realtime(conn->data_port, conn->control_port);
  if (err != ESP_OK) {
    audio_start_failed("audio receiver start", err);
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                       NULL, 0);
    return false;
  }
  audio_start_succeeded();
  return true;
}

/* ---- Protocol trace (Kconfig AIRPLAY_PROTOCOL_TRACE) ----------------------
 *
 * Shows what a sender sends, especially after changing feature bits: every
 * RTSP request with header names and body, every decrypted event-channel
 * message, plus /command summaries. Text is built in PSRAM buffers, not on
 * the task stacks. */
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
#define TRACE_TEXT_CAP 4096U
#define TRACE_NAMES_CAP 512U

/* Discovery build: trace every request.  Feature-bit experiments often
 * trigger a method that was already seen earlier in the session; de-duplicating
 * by method/path would hide exactly the state transition we are trying to map. */

static bool trace_span_contains(const uint8_t *buf, size_t len,
                                const char *needle) {
  const size_t n = strlen(needle);
  if (!buf || n == 0 || n > len) return false;
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(buf + i, needle, n) == 0) return true;
  }
  return false;
}

/* Header names of a request or message, values left out. */
static void trace_header_names(const uint8_t *msg, size_t len, char *out,
                               size_t cap) {
  size_t pos = 0;
  out[0] = '\0';
  const uint8_t *end = rtsp_find_header_end(msg, len);
  if (!end) return;
  const char *stop = (const char *)end;
  const char *line = memchr(msg, '\n', (size_t)(stop - (const char *)msg));
  while (line && line < stop) {
    line++;
    const char *eol = memchr(line, '\n', (size_t)(stop - line));
    const char *lim = eol ? eol : stop;
    const char *colon = memchr(line, ':', (size_t)(lim - line));
    if (colon && colon > line && pos + 3 < cap) {
      if (pos) {
        out[pos++] = ',';
        out[pos++] = ' ';
      }
      size_t n = (size_t)(colon - line);
      if (n > cap - pos - 1) n = cap - pos - 1;
      memcpy(out + pos, line, n);
      pos += n;
      out[pos] = '\0';
    }
    line = eol;
  }
}

static void trace_structured_line_string(const char *label, const char *value) {
  if (value && value[0]) ESP_LOGI(TAG, "TRACE     %-22s : %s", label, value);
}

static void trace_structured_line_bool(const char *label, bool value) {
  ESP_LOGI(TAG, "TRACE     %-22s : %s", label, value ? "true" : "false");
}

static void trace_structured_setup(const uint8_t *body, size_t len) {
  size_t stream_count = 0;
  const bool have_streams = bplist_get_streams_count(body, len, &stream_count);

  ESP_LOGI(TAG, "TRACE SETUP -------------------------------------------------------");

  if (have_streams) {
    ESP_LOGI(TAG, "TRACE   Streams:");
    ESP_LOGI(TAG, "TRACE     %-22s : %u", "count", (unsigned)stream_count);

    const size_t shown = stream_count < 4U ? stream_count : 4U;
    for (size_t i = 0; i < shown; ++i) {
      int64_t stream_type = -1;
      size_t ekey_len = 0, eiv_len = 0, shk_len = 0;
      if (!bplist_get_stream_info(body, len, i, &stream_type, &ekey_len,
                                  &eiv_len, &shk_len)) {
        ESP_LOGI(TAG, "TRACE     [%u] <unparsed>", (unsigned)i);
        continue;
      }

      const bool audio_stream = stream_type == 96 || stream_type == 103;
      rtsp_connection_audio_setup_t setup = {0};
      if (audio_stream) {
        (void)rtsp_connection_model_parse_audio_setup(body, len, i,
                                                       stream_type, &setup);
      }

      int64_t audio_format = -1;
      int64_t audio_format_index = -1;
      int64_t stream_connection_id = 0;
      bool have_stream_connection_id = false;
      bool dynamic_stream_id = false;
      bplist_kv_info_t kv[20];
      size_t kv_count = 0;
      if (bplist_get_stream_kv_info(body, len, i, kv,
                                    sizeof(kv) / sizeof(kv[0]), &kv_count)) {
        for (size_t k = 0; k < kv_count; ++k) {
          if (strcmp(kv[k].key, "supportsDynamicStreamID") == 0)
            dynamic_stream_id = true;
          if (kv[k].value_type != BPLIST_VALUE_INT) continue;
          if (strcmp(kv[k].key, "audioFormat") == 0) {
            audio_format = kv[k].int_value;
          } else if (strcmp(kv[k].key, "audioFormatIndex") == 0) {
            audio_format_index = kv[k].int_value;
          } else if (strcmp(kv[k].key, "streamConnectionID") == 0) {
            stream_connection_id = kv[k].int_value;
            have_stream_connection_id = true;
          }
        }
      }

      ESP_LOGI(TAG, "TRACE     [%u]", (unsigned)i);
      ESP_LOGI(TAG, "TRACE       %-20s : %lld", "type",
               (long long)stream_type);
      if (audio_stream && setup.codec_type >= 0)
        ESP_LOGI(TAG, "TRACE       %-20s : %lld", "codec (ct)",
                 (long long)setup.codec_type);
      if (audio_stream && setup.sample_rate > 0)
        ESP_LOGI(TAG, "TRACE       %-20s : %lld", "sample rate",
                 (long long)setup.sample_rate);
      if (audio_stream && setup.samples_per_frame > 0)
        ESP_LOGI(TAG, "TRACE       %-20s : %lld", "samples/frame",
                 (long long)setup.samples_per_frame);
      if (audio_format >= 0)
        ESP_LOGI(TAG, "TRACE       %-20s : %lld", "audioFormat",
                 (long long)audio_format);
      if (audio_format_index >= 0)
        ESP_LOGI(TAG, "TRACE       %-20s : %lld", "audioFormatIndex",
                 (long long)audio_format_index);
      ESP_LOGI(TAG, "TRACE       %-20s : %s", "dynamicStreamID",
               dynamic_stream_id ? "true" : "false");
      if (have_stream_connection_id)
        ESP_LOGI(TAG, "TRACE       %-20s : %lld", "streamConnectionID",
                 (long long)stream_connection_id);

      char client_id[128] = {0};
      if (bplist_find_string_deep(body, len, "clientID", client_id,
                                  sizeof(client_id))) {
        ESP_LOGI(TAG, "TRACE       %-20s : %s", "clientID", client_id);
      }

      if (setup.has_stream_connections) {
        ESP_LOGI(TAG,
                 "TRACE       %-20s : RTP=%d RTCP=%d APAP=%d APAPkey=%d MDC=%d",
                 "streamConnections", setup.stream_connection_rtp ? 1 : 0,
                 setup.stream_connection_rtcp ? 1 : 0,
                 setup.stream_connection_apap ? 1 : 0,
                 setup.apap_use_stream_encryption_key ? 1 : 0,
                 setup.stream_connection_media_data_control ? 1 : 0);
        if (setup.media_data_control_seed_valid)
          ESP_LOGI(TAG, "TRACE       %-20s : 0x%016llx", "MDC seed",
                   (unsigned long long)setup.media_data_control_seed);
      }
      if (ekey_len || eiv_len || shk_len)
        ESP_LOGI(TAG,
                 "TRACE       %-20s : ekey=%uB eiv=%uB shk=%uB",
                 "crypto", (unsigned)ekey_len, (unsigned)eiv_len,
                 (unsigned)shk_len);
    }
    if (stream_count > shown)
      ESP_LOGI(TAG, "TRACE     ... %u more stream(s)",
               (unsigned)(stream_count - shown));

    ESP_LOGI(TAG, "TRACE -------------------------------------------------------------");
    return;
  }

  char value[160] = {0};
  bool flag = false;
  int64_t ivalue = 0;

  ESP_LOGI(TAG, "TRACE   Sender:");
  if (bplist_find_string(body, len, "name", value, sizeof(value)))
    trace_structured_line_string("name", value);
  if (bplist_find_string(body, len, "model", value, sizeof(value)))
    trace_structured_line_string("model", value);
  if (bplist_find_string(body, len, "osName", value, sizeof(value)))
    trace_structured_line_string("osName", value);
  if (bplist_find_string(body, len, "osVersion", value, sizeof(value)))
    trace_structured_line_string("osVersion", value);
  if (bplist_find_string(body, len, "osBuildVersion", value, sizeof(value)))
    trace_structured_line_string("osBuildVersion", value);
  if (bplist_find_string(body, len, "sourceVersion", value, sizeof(value)))
    trace_structured_line_string("sourceVersion", value);
  if (bplist_find_string(body, len, "deviceID", value, sizeof(value)))
    trace_structured_line_string("deviceID", value);
  if (bplist_find_string(body, len, "macAddress", value, sizeof(value)))
    trace_structured_line_string("macAddress", value);

  ESP_LOGI(TAG, "TRACE   Session:");
  if (bplist_find_string(body, len, "sessionUUID", value, sizeof(value)))
    trace_structured_line_string("sessionUUID", value);
  if (bplist_find_string(body, len, "sessionCorrelationUUID", value,
                         sizeof(value)))
    trace_structured_line_string("correlationUUID", value);
  if (bplist_find_string(body, len, "groupUUID", value, sizeof(value)))
    trace_structured_line_string("groupUUID", value);
  if (bplist_find_bool(body, len, "isMultiSelectAirPlay", &flag))
    trace_structured_line_bool("isMultiSelectAirPlay", flag);
  if (bplist_find_bool(body, len, "groupContainsGroupLeader", &flag))
    trace_structured_line_bool("groupContainsLeader", flag);
  if (bplist_find_bool(body, len, "supportsGroupCohesion", &flag))
    trace_structured_line_bool("supportsGroupCohesion", flag);
  if (bplist_find_bool(body, len, "updateSessionRequest", &flag))
    trace_structured_line_bool("updateSessionRequest", flag);

  ESP_LOGI(TAG, "TRACE   Timing:");
  if (bplist_find_string(body, len, "timingProtocol", value, sizeof(value)))
    trace_structured_line_string("protocol", value);
  if (bplist_find_bool(body, len, "asyncPTPClockConfig", &flag))
    trace_structured_line_bool("asyncPTPClockConfig", flag);
  if (bplist_find_int_deep(body, len, "ClockID", &ivalue))
    ESP_LOGI(TAG, "TRACE     %-22s : %016llx", "ClockID",
             (unsigned long long)(uint64_t)ivalue);
  if (bplist_find_int_deep(body, len, "DeviceType", &ivalue))
    ESP_LOGI(TAG, "TRACE     %-22s : %lld", "DeviceType",
             (long long)ivalue);
  if (bplist_find_string_deep(body, len, "ID", value, sizeof(value)))
    trace_structured_line_string("peer ID", value);
  if (bplist_find_string_deep(body, len, "HTGroupUUID", value, sizeof(value)))
    trace_structured_line_string("HTGroupUUID", value);
  if (bplist_find_bool_deep(body, len, "SupportsClockPortMatchingOverride",
                            &flag))
    trace_structured_line_bool("clockPortOverride", flag);

  ESP_LOGI(TAG, "TRACE   Capabilities:");
  if (bplist_find_bool(body, len, "senderSupportsRelay", &flag))
    trace_structured_line_bool("senderSupportsRelay", flag);
  if (bplist_find_bool(body, len, "combinedGetInfoWithControlSetup", &flag))
    trace_structured_line_bool("combinedGetInfo", flag);
  if (bplist_find_bool(body, len, "diagnosticsAndUsage", &flag))
    trace_structured_line_bool("diagnosticsAndUsage", flag);
  if (bplist_find_bool(body, len, "statsCollectionEnabled", &flag))
    trace_structured_line_bool("statsCollection", flag);
  if (bplist_find_bool(body, len, "internalBuild", &flag))
    trace_structured_line_bool("internalBuild", flag);

  ESP_LOGI(TAG, "TRACE -------------------------------------------------------------");
}

static void trace_structured_setpeers(const char *tag, const uint8_t *body,
                                      size_t len) {
  const bool extended = strcasecmp(tag, "SETPEERSX") == 0;
  bplist_peer_info_t *peers = heap_caps_calloc(
      PTP_CLOCK_MAX_PEERS, sizeof(*peers), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!peers) {
    ESP_LOGW(TAG, "TRACE %s: no memory for structured peer dump", tag);
    return;
  }

  size_t peer_count = 0;
  if (!bplist_get_peer_list(body, len, extended, peers, PTP_CLOCK_MAX_PEERS,
                            &peer_count)) {
    heap_caps_free(peers);
    ESP_LOGW(TAG, "TRACE %s: invalid peer-list bplist", tag);
    return;
  }

  ESP_LOGI(TAG, "TRACE %s ---------------------------------------------------", tag);
  ESP_LOGI(TAG, "TRACE   %-24s : %u", "peers", (unsigned)peer_count);
  const size_t shown = peer_count < PTP_CLOCK_MAX_PEERS
                           ? peer_count
                           : PTP_CLOCK_MAX_PEERS;
  for (size_t i = 0; i < shown; ++i) {
    ESP_LOGI(TAG, "TRACE   Peer[%u]:", (unsigned)i);
    if (peers[i].has_clock_id)
      ESP_LOGI(TAG, "TRACE     %-22s : %016llx", "ClockID",
               (unsigned long long)peers[i].clock_id);
    for (size_t a = 0; a < peers[i].address_count; ++a) {
      ESP_LOGI(TAG, "TRACE     address[%u]             : %s", (unsigned)a,
               peers[i].addresses[a]);
    }
  }
  if (peer_count > shown)
    ESP_LOGI(TAG, "TRACE   ... %u more peer(s)",
             (unsigned)(peer_count - shown));
  ESP_LOGI(TAG, "TRACE -------------------------------------------------------------");
  heap_caps_free(peers);
}

static void trace_structured_rate_anchor(const uint8_t *body, size_t len) {
  rtsp_rate_anchor_t anchor = {0};
  if (!rtsp_timeline_parse_rate_anchor(body, len, &anchor)) return;

  ESP_LOGI(TAG,
           "TRACE SETRATEANCHORTIME -------------------------------------------");
  if (anchor.have_rate)
    ESP_LOGI(TAG, "TRACE   %-24s : %.3f", "rate", anchor.rate);
  ESP_LOGI(TAG, "TRACE   %-24s : %016llx", "timeline ClockID",
           (unsigned long long)anchor.clock_id);
  if (anchor.have_network_time_secs) {
    ESP_LOGI(TAG, "TRACE   %-24s : %llu", "networkTimeSecs",
             (unsigned long long)anchor.network_time_secs);
    ESP_LOGI(TAG, "TRACE   %-24s : 0x%016llx", "networkTimeFrac",
             (unsigned long long)anchor.network_time_frac);
    ESP_LOGI(TAG, "TRACE   %-24s : %llu ns", "networkTime",
             (unsigned long long)rtsp_timeline_network_time_ns(&anchor));
  }
  ESP_LOGI(TAG, "TRACE   %-24s : %llu", "rtpTime",
           (unsigned long long)anchor.rtp_time);
  ESP_LOGI(TAG, "TRACE -------------------------------------------------------------");
}

static void trace_structured_flushbuffered(const uint8_t *body, size_t len) {
  int64_t from_seq = 0, from_ts = 0, until_seq = 0, until_ts = 0;
  const bool have_from_seq =
      bplist_find_int(body, len, "flushFromSeq", &from_seq);
  const bool have_from_ts = bplist_find_int(body, len, "flushFromTS", &from_ts);
  const bool have_until_seq =
      bplist_find_int(body, len, "flushUntilSeq", &until_seq);
  const bool have_until_ts =
      bplist_find_int(body, len, "flushUntilTS", &until_ts);

  ESP_LOGI(TAG,
           "TRACE FLUSHBUFFERED ----------------------------------------------");
  if (have_from_seq)
    ESP_LOGI(TAG, "TRACE   %-24s : %lld", "from sequence",
             (long long)from_seq);
  if (have_from_ts)
    ESP_LOGI(TAG, "TRACE   %-24s : %lld", "from RTP timestamp",
             (long long)from_ts);
  if (have_until_seq)
    ESP_LOGI(TAG, "TRACE   %-24s : %lld", "until sequence",
             (long long)until_seq);
  if (have_until_ts)
    ESP_LOGI(TAG, "TRACE   %-24s : %lld", "until RTP timestamp",
             (long long)until_ts);
  ESP_LOGI(TAG, "TRACE -------------------------------------------------------------");
}

static bool trace_structured_bplist(const char *tag, const uint8_t *body,
                                    size_t len) {
  if (!tag || !body || len < 8 || memcmp(body, "bplist00", 8) != 0)
    return false;

  if (strcasecmp(tag, "SETUP") == 0) {
    trace_structured_setup(body, len);
    return true;
  }
  if (strcasecmp(tag, "SETPEERS") == 0 ||
      strcasecmp(tag, "SETPEERSX") == 0) {
    trace_structured_setpeers(tag, body, len);
    return true;
  }
  if (strcasecmp(tag, "SETRATEANCHORTIME") == 0) {
    trace_structured_rate_anchor(body, len);
    return true;
  }
  if (strcasecmp(tag, "FLUSHBUFFERED") == 0) {
    trace_structured_flushbuffered(body, len);
    return true;
  }
  return false;
}

static void trace_body(const char *tag, const uint8_t *body, size_t len,
                       const char *content_type) {
  if (!body || len == 0) return;

  const bool structured = trace_structured_bplist(tag, body, len);
#ifndef CONFIG_AIRPLAY_PROTOCOL_TRACE_RAW_BPLIST
  if (structured) return;
#endif

  char *text = heap_caps_malloc(TRACE_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!text) return;
  if (len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    char command_type[128] = {0};
    if (bplist_find_string(body, len, "type", command_type,
                           sizeof(command_type)) &&
        (strcmp(command_type, "updateMRNowPlayingInfo") == 0 ||
         strcmp(command_type, "updateMRSupportedCommands") == 0)) {
      snprintf(text, TRACE_TEXT_CAP, "<%s %u bytes; formatted below>",
               command_type, (unsigned)len);
    } else if (bplist_describe(body, len, text, TRACE_TEXT_CAP) == 0) {
      snprintf(text, TRACE_TEXT_CAP, "<invalid bplist, %u bytes>",
               (unsigned)len);
    }
  } else if (content_type && (strncasecmp(content_type, "text/", 5) == 0 ||
                              strstr(content_type, "parameters") ||
                              strstr(content_type, "sdp"))) {
    const size_t n = len < TRACE_TEXT_CAP - 1U ? len : TRACE_TEXT_CAP - 1U;
    for (size_t i = 0; i < n; ++i) {
      text[i] = (body[i] >= 0x20 && body[i] < 0x7f) ? (char)body[i] : ' ';
    }
    text[n] = '\0';
  } else {
    snprintf(text, TRACE_TEXT_CAP, "<%u bytes %s>", (unsigned)len,
             content_type && *content_type ? content_type : "binary");
  }
  if (structured) {
    ESP_LOGI(TAG, "TRACE   %s raw: %s", tag, text);
  } else {
    ESP_LOGI(TAG, "TRACE   %s body: %s", tag, text);
  }
  heap_caps_free(text);
}

static void trace_request(rtsp_conn_t *conn, const rtsp_request_t *req,
                          const uint8_t *raw, size_t raw_len) {
  char *names = heap_caps_malloc(TRACE_NAMES_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!names) return;
  trace_header_names(raw, raw_len, names, TRACE_NAMES_CAP);
  ESP_LOGI(TAG, "TRACE %s %s [%s]", req->method, req->path, names);
  heap_caps_free(names);
  trace_body(req->method, req->body, req->body_len, req->content_type);
  if (strcasecmp(req->method, "POST") == 0 &&
      strstr(req->path, "/command") && req->body && req->body_len) {
    rtsp_protocol_trace_command(req->body, req->body_len);
  }
}

/* One decrypted event-channel frame from the sender.  Do not cap messages in
 * the discovery build: later feature-bit reactions may happen minutes after
 * the event socket is established. */
static void trace_event_message(const uint8_t *msg, size_t len) {
  size_t line = 0;
  while (line < len && line < 120 && msg[line] != '\r' && msg[line] != '\n') {
    line++;
  }
  char *names = heap_caps_malloc(TRACE_NAMES_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!names) return;
  trace_header_names(msg, len, names, TRACE_NAMES_CAP);
  ESP_LOGI(TAG, "TRACE event from sender: %.*s [%s] (%u bytes)", (int)line,
           (const char *)msg, names, (unsigned)len);
  heap_caps_free(names);
  const uint8_t *end = rtsp_find_header_end(msg, len);
  if (end && (size_t)(end - msg) + 4U < len) {
    const size_t off = (size_t)(end - msg) + 4U;
    const uint8_t *body = msg + off;
    const size_t body_len = len - off;
    trace_body("event", body, body_len, NULL);
    if (line >= 5U && memcmp(msg, "POST ", 5) == 0 &&
        trace_span_contains(msg, line, "/command")) {
      rtsp_protocol_trace_command(body, body_len);
    }
  }
}
#endif /* CONFIG_AIRPLAY_PROTOCOL_TRACE */

/* ---- AirPlay 2 event channel --------------------------------------------
 *
 * The sender connects to the event port announced in the initial SETUP. The
 * channel is encrypted like the control channel, with its own keys
 * ("Events-Salt") and nonce counters. Like Shairport (ap2_event_receiver.c)
 * we send "POST /command" with an updateInfo plist only after RECORD has
 * been acknowledged.  This matches the AirPlay 2 lifecycle: event TCP first,
 * then RECORD, then receiver state advertisement over the event channel.
 * The sender answers the POST with an RTSP response.
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
static uint8_t *event_build_update_info(size_t *out_len, uint32_t client_ip,
                                        uint16_t client_rtsp_port) {
  enum { TXT_CAP = 512, PLIST_CAP = 4096, HDR_CAP = 160 };
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
    char features_ex[40] = {0};
    char sender_address[64] = {0};
    struct in_addr sender_ip = {.s_addr = client_ip};
    const char *ip = inet_ntop(AF_INET, &sender_ip, sender_address,
                               sizeof(sender_address));
    if (!ip) {
      snprintf(sender_address, sizeof(sender_address), "0.0.0.0");
    }
    size_t sender_len = strlen(sender_address);
    snprintf(sender_address + sender_len, sizeof(sender_address) - sender_len,
             ":%u", (unsigned)client_rtsp_port);
    const size_t fex_len =
        mdns_airplay_features_ex_string(features_ex, sizeof(features_ex));
    const size_t txt_len = mdns_airplay_txt_record_data(txt, TXT_CAP);
    const size_t plist_len = bplist_build_update_info(
        plist, PLIST_CAP, device_id, device_name, AIRPLAY_MODEL,
        hap_get_public_key(), 32, features, AIRPLAY_VV,
        airplay_identity_flags(), airplay_identity_pi(),
        airplay_identity_psi(), features_ex, sender_address, txt, txt_len);
    if (plist_len > 0) {
      diag_log_bplist("Event TX updateInfo plist", plist, plist_len);
    }
    if (fex_len == 0) {
      ESP_LOGE(TAG, "Event channel: could not derive HomePod featuresEx");
    }
    const int hdr_len = snprintf(
        (char *)msg, HDR_CAP,
        "POST /command RTSP/1.0\r\nContent-Length: %u\r\n"
        "Content-Type: application/x-apple-binary-plist\r\n\r\n",
        (unsigned)plist_len);
    if (fex_len > 0 && txt_len > 0 && plist_len > 0 && hdr_len > 0 &&
        hdr_len < HDR_CAP) {
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

static bool event_send_update_info(event_ctx_t *ctx, int client) {
  if (!ctx->have_keys) {
    ESP_LOGW(TAG, "Event channel: no pairing keys, updateInfo not sent");
    return false;
  }
  if (!ctx->update_info) {
    ESP_LOGW(TAG, "Event channel: updateInfo body unavailable");
    return false;
  }

  ESP_LOGI(TAG,
           "Event TX: POST /command type=updateInfo len=%u nonce=%" PRIu64,
           (unsigned)ctx->update_info_len, ctx->enc_nonce);
  if (rtsp_crypto_seal_send(client, ctx->enc_key, &ctx->enc_nonce,
                            ctx->update_info, ctx->update_info_len) != 0) {
    ESP_LOGW(TAG, "Event channel: sending updateInfo failed");
    return false;
  }
  ESP_LOGI(TAG, "Event TX: updateInfo sent");
  return true;
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
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
    trace_event_message(plain, (size_t)plain_len);
#endif
    /* The sender normally answers our POST /command with an RTSP response.
     * Keep this concise diagnostic always on so we can prove that updateInfo
     * was accepted even when full protocol tracing is disabled. */
    if (plain_len >= 5 && memcmp(plain, "RTSP/", 5) == 0) {
      size_t first_line = 0;
      while (first_line < (size_t)plain_len && first_line < 96 &&
             plain[first_line] != '\r' && plain[first_line] != '\n') {
        first_line++;
      }
      ESP_LOGI(TAG, "Event RX: %.*s (%dB)", (int)first_line,
               (const char *)plain, plain_len);
    } else {
      ESP_LOGI(TAG, "Event RX: decrypted message %dB", plain_len);
    }
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
       * sender does.  Do NOT advertise updateInfo yet: the sender has not
       * completed RECORD, so defer it until handle_record() acknowledges the
       * control-plane transition. */
      ctx->enc_nonce = 0;
      ctx->dec_nonce = 0;
      event_update_info_requested = false;
      event_update_info_sent = false;
      ESP_LOGI(TAG, "Event: updateInfo deferred until RECORD");

      size_t rx_len = 0;
      bool cipher_ok = ctx->have_keys;
      uint32_t discarded = 0;

      while (event_client_socket >= 0 && !event_task_should_stop) {
        if (event_update_info_requested && !event_update_info_sent) {
          /* Consume the request before sending so a failed TX cannot spin and
           * flood the log.  A new event connection resets both flags. */
          event_update_info_requested = false;
          event_update_info_sent =
              event_send_update_info(ctx, event_client_socket);
        }

        fd_set cfds;
        FD_ZERO(&cfds);
        FD_SET(event_client_socket, &cfds);
        /* Before RECORD/updateInfo, poll briefly so the deferred send happens
         * before the sender advances to type-130 setup.  Afterwards return to
         * the low-wakeup 1 s event wait used during normal playback. */
        struct timeval ctv = event_update_info_sent
                                 ? (struct timeval){.tv_sec = 1, .tv_usec = 0}
                                 : (struct timeval){.tv_sec = 0, .tv_usec = 10000};

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
                                     const hap_session_t *session,
                                     uint32_t client_ip,
                                     uint16_t client_rtsp_port) {
  event_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    return ESP_ERR_NO_MEM;
  }
  ctx->listen_socket = listen_socket;
  if (session && session->event_keys_valid) {
    memcpy(ctx->enc_key, session->event_encrypt_key, sizeof(ctx->enc_key));
    memcpy(ctx->dec_key, session->event_decrypt_key, sizeof(ctx->dec_key));
    ctx->have_keys = true;
    ctx->update_info = event_build_update_info(&ctx->update_info_len, client_ip,
                                               client_rtsp_port);
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
  event_update_info_requested = false;
  event_update_info_sent = false;
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
static void handle_loudnessnormalization(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         const uint8_t *raw, size_t raw_len);
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
static void mdc_on_message(rtsp_datastream_t *stream,
                           const char *type, const char *command,
                           const uint8_t *payload, size_t payload_len,
                           uint8_t *reply, size_t reply_cap, size_t *reply_len,
                           void *user);
static size_t mdc_anchor_reply(rtsp_conn_t *conn, uint8_t *reply,
                               size_t reply_cap);
static bool read_mdc_rate(const uint8_t *body, size_t len, double *rate);
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
    {"LOUDNESSNORMALIZATION", handle_loudnessnormalization},
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


/* v4.1.91 session-lifecycle diagnostics. Keep this intentionally compact and
 * always enabled in the diagnostic build: unlike AIRPLAY_PROTOCOL_TRACE it
 * does not dump payloads or run expensive formatting on every feedback frame. */
static bool diag_copy_raw_header(const uint8_t *raw, size_t raw_len,
                                 const char *name, char *out, size_t out_cap) {
  if (!raw || !name || !out || out_cap == 0) return false;
  out[0] = '\0';
  const uint8_t *header_end = rtsp_find_header_end(raw, raw_len);
  if (!header_end) return false;

  const char *line = (const char *)raw;
  const char *end = (const char *)header_end;
  const size_t name_len = strlen(name);
  while (line < end) {
    const char *line_end = line;
    while (line_end < end && *line_end != '\r' && *line_end != '\n') line_end++;
    if ((size_t)(line_end - line) >= name_len &&
        strncasecmp(line, name, name_len) == 0) {
      const char *v = line + name_len;
      while (v < line_end && (*v == ' ' || *v == '\t')) v++;
      size_t n = (size_t)(line_end - v);
      if (n >= out_cap) n = out_cap - 1U;
      memcpy(out, v, n);
      out[n] = '\0';
      return true;
    }
    line = line_end;
    while (line < end && (*line == '\r' || *line == '\n')) line++;
  }
  return false;
}

static bool diag_is_session_method(const rtsp_request_t *req) {
  if (!req) return false;
  return strcasecmp(req->method, "SETUP") == 0 ||
         strcasecmp(req->method, "RECORD") == 0 ||
         strcasecmp(req->method, "TEARDOWN") == 0 ||
         strcasecmp(req->method, "SETPEERS") == 0 ||
         strcasecmp(req->method, "SETPEERSX") == 0 ||
         strcasecmp(req->method, "SETRATEANCHORTIME") == 0;
}

static void diag_log_session_plist(const rtsp_conn_t *conn,
                                   const rtsp_request_t *req) {
  if (!conn || !req || !req->body || req->body_len < 8 ||
      memcmp(req->body, "bplist00", 8) != 0) return;

  char session_uuid[96] = "-";
  char corr_uuid[96] = "-";
  char group_uuid[96] = "-";
  char timing[32] = "-";
  char channel_id[96] = "-";
  char client_uuid[96] = "-";
  char client_type_uuid[96] = "-";
  char client_id[96] = "-";
  bool remote_only = false, have_remote_only = false;
  bool dedicated = false, have_dedicated = false;
  int64_t control_type = -1, seed = -1, stream_connection_id = -1;
  bool have_control_type = false, have_seed = false;
  bool have_stream_connection_id = false;
  size_t stream_count = 0;
  const bool have_streams =
      bplist_get_streams_count(req->body, req->body_len, &stream_count);

  (void)bplist_find_string_deep(req->body, req->body_len, "sessionUUID",
                           session_uuid, sizeof(session_uuid));
  (void)bplist_find_string_deep(req->body, req->body_len, "sessionCorrelationUUID",
                           corr_uuid, sizeof(corr_uuid));
  (void)bplist_find_string_deep(req->body, req->body_len, "groupUUID",
                           group_uuid, sizeof(group_uuid));
  (void)bplist_find_string_deep(req->body, req->body_len, "timingProtocol",
                           timing, sizeof(timing));
  have_remote_only = bplist_find_bool_deep(req->body, req->body_len,
                                           "isRemoteControlOnly", &remote_only);
  (void)bplist_find_string_deep(req->body, req->body_len, "channelID",
                                channel_id, sizeof(channel_id));
  (void)bplist_find_string_deep(req->body, req->body_len, "clientUUID",
                                client_uuid, sizeof(client_uuid));
  (void)bplist_find_string_deep(req->body, req->body_len, "clientTypeUUID",
                                client_type_uuid, sizeof(client_type_uuid));
  (void)bplist_find_string_deep(req->body, req->body_len, "clientID",
                                client_id, sizeof(client_id));
  have_dedicated = bplist_find_bool_deep(req->body, req->body_len,
                                         "wantsDedicatedSocket", &dedicated);
  have_control_type = bplist_find_int_deep(req->body, req->body_len,
                                           "controlType", &control_type);
  have_seed = bplist_find_int_deep(req->body, req->body_len, "seed", &seed);
  have_stream_connection_id = bplist_find_int_deep(
      req->body, req->body_len, "streamConnectionID", &stream_connection_id);

  ESP_LOGI(TAG,
           "RSESSION PLIST id=%u cseq=%d streams=%s/%u timing=%s remoteOnly=%s sessionUUID=%s correlationUUID=%s groupUUID=%s",
           (unsigned)conn->diag_conn_id, req->cseq,
           have_streams ? "yes" : "no", (unsigned)stream_count, timing,
           have_remote_only ? (remote_only ? "true" : "false") : "-",
           session_uuid, corr_uuid, group_uuid);

  if (channel_id[0] != '-' || client_uuid[0] != '-' ||
      client_type_uuid[0] != '-' || client_id[0] != '-' ||
      have_control_type || have_seed || have_dedicated ||
      have_stream_connection_id) {
    ESP_LOGI(TAG,
             "RSESSION IDS id=%u cseq=%d channelID=%s clientUUID=%s clientTypeUUID=%s clientID=%s controlType=%lld seed=%lld dedicated=%s streamConnectionID=%lld",
             (unsigned)conn->diag_conn_id, req->cseq, channel_id, client_uuid,
             client_type_uuid, client_id,
             (long long)(have_control_type ? control_type : -1),
             (long long)(have_seed ? seed : -1),
             have_dedicated ? (dedicated ? "true" : "false") : "-",
             (long long)(have_stream_connection_id ? stream_connection_id : -1));
  }

  if (have_streams) {
    const size_t shown = stream_count < 4U ? stream_count : 4U;
    for (size_t i = 0; i < shown; ++i) {
      int64_t type = -1;
      size_t ekey_len = 0, eiv_len = 0, shk_len = 0;
      if (bplist_get_stream_info(req->body, req->body_len, i, &type,
                                 &ekey_len, &eiv_len, &shk_len)) {
        ESP_LOGI(TAG,
                 "RSESSION STREAM id=%u cseq=%d index=%u type=%lld ekey=%u eiv=%u shk=%u",
                 (unsigned)conn->diag_conn_id, req->cseq, (unsigned)i,
                 (long long)type, (unsigned)ekey_len, (unsigned)eiv_len,
                 (unsigned)shk_len);
      }
    }
  }
}

static void diag_log_session_request(int socket, rtsp_conn_t *conn,
                                     const rtsp_request_t *req,
                                     const uint8_t *raw, size_t raw_len) {
  if (!conn || !req) return;
  const uint32_t n = ++conn->diag_request_seq;
  if (!diag_is_session_method(req)) return;

  char session[128] = "-";
  char x_session[128] = "-";
  char connection[48] = "-";
  char user_agent[96] = "-";
  char dacp_id[64] = "-";
  char active_remote[64] = "-";
  (void)diag_copy_raw_header(raw, raw_len, "Session:", session, sizeof(session));
  (void)diag_copy_raw_header(raw, raw_len, "X-Apple-Session-ID:", x_session,
                             sizeof(x_session));
  (void)diag_copy_raw_header(raw, raw_len, "Connection:", connection,
                             sizeof(connection));
  (void)diag_copy_raw_header(raw, raw_len, "User-Agent:", user_agent,
                             sizeof(user_agent));
  (void)diag_copy_raw_header(raw, raw_len, "DACP-ID:", dacp_id, sizeof(dacp_id));
  (void)diag_copy_raw_header(raw, raw_len, "Active-Remote:", active_remote,
                             sizeof(active_remote));

  ESP_LOGI(TAG,
           "RSESSION RX id=%u req=%u fd=%d cseq=%d %s %s owner=%d enc=%d active=%d streamType=%lld body=%u ct=%s",
           (unsigned)conn->diag_conn_id, (unsigned)n, socket, req->cseq,
           req->method, req->path, conn->play_owner ? 1 : 0,
           conn->encrypted_mode ? 1 : 0, conn->stream_active ? 1 : 0,
           (long long)conn->stream_type, (unsigned)req->body_len,
           req->content_type[0] ? req->content_type : "-");
  ESP_LOGI(TAG,
           "RSESSION HDR id=%u cseq=%d Session=%s XAppleSessionID=%s Connection=%s UserAgent=%s DACPID=%s ActiveRemote=%s",
           (unsigned)conn->diag_conn_id, req->cseq, session, x_session,
           connection, user_agent, dacp_id, active_remote);
  diag_log_session_plist(conn, req);
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
    esp_err_t mr_err = media_remote_state_init();
    if (mr_err != ESP_OK) return mr_err;
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
    rtsp_protocol_trace_features();
#endif
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
      /* RECORD is deliberately not play-lock gated.  In AirPlay 2 the
       * RemoteControl-only bootstrap sends RECORD before the audio stream
       * SETUP; handle_record() is side-effect free and returns the expected
       * Audio-Latency / Audio-Jack-Status headers. */
      "SET_PARAMETER", "PAUSE", "FLUSH", "FLUSHBUFFERED", "TEARDOWN",
      "SETRATEANCHORTIME", "SETPEERS", "SETPEERSX", NULL};
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
  if (bplist_get_streams_count(body, body_len, &streams)) {
    /* A RemoteControlOnly type-130 DataStream must never steal the global
     * audio play lock.  Audio stream SETUPs (96/103) still do. */
    if (streams == 1) {
      int64_t type = -1;
      size_t ekey_len = 0, eiv_len = 0, shk_len = 0;
      if (bplist_get_stream_info(body, body_len, 0, &type, &ekey_len,
                                 &eiv_len, &shk_len) &&
          type == 130) {
        return false;
      }
    }
    return true;
  }
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
    /* Non-principal TEARDOWN is normally a RemoteControlOnly conversation.
     * It owns only its dedicated type-130 DataStream, never the audio engine. */
    rtsp_datastream_stop(&conn->remote_control_datastream);
    conn->remote_control_data_port = 0;
    if (!bplist_get_streams_count(req->body, req->body_len, &streams)) {
      ESP_LOGI(TAG,
               "RSESSION TX id=%u cseq=%d non-owner TEARDOWN without streams -> 200 Connection:close",
               (unsigned)conn->diag_conn_id, req->cseq);
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
  diag_log_session_request(socket, conn, req, raw_request, raw_len);
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
  trace_request(conn, req, raw_request, raw_len);
#endif
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
      "OPTIONS, POST, GET, SET_PARAMETER, GET_PARAMETER, SETPEERS, SETPEERSX, "
      "SETRATEANCHORTIME, LOUDNESSNORMALIZATION\r\n";

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

    int64_t protocol_version = AIRPLAY_VV;

    if (request_uses_rtsp(req)) {
      /* "qualifier": ["txtAirPlay"] asks for the TXT record too (Shairport
       * generateInfoPlist(); every iOS 27 sender sends it). */
      bool want_txt = false;
      if (req->body && req->body_len >= 8 &&
          memcmp(req->body, "bplist00", 8) == 0) {
        static const char kTxt[] = "txtAirPlay";
        for (size_t i = 0; i + sizeof(kTxt) - 1 <= req->body_len; ++i) {
          if (memcmp(req->body + i, kTxt, sizeof(kTxt) - 1) == 0) {
            want_txt = true;
            break;
          }
        }
      }
      static uint8_t txt[512];
      const size_t txt_len =
          want_txt ? mdns_airplay_txt_record_data(txt, sizeof(txt)) : 0;
      static uint8_t body[1536];
      size_t body_len = bplist_build_info_response(
          body, sizeof(body), device_id, device_name, AIRPLAY_MODEL, pk, 32,
          features, protocol_version, airplay_identity_flags(),
          airplay_identity_pi(), txt_len ? txt : NULL, txt_len);
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
      trace_body("GET /info reply", body, body_len, NULL);
#endif
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
    plist_dict_string(&p, "protovers", AIRPLAY_PROTOVERS);
    plist_dict_string(&p, "srcvers", AIRPLAY_SOURCE_VERSION);
    plist_dict_int(&p, "vv", protocol_version);
    plist_dict_int(&p, "statusFlags", (int64_t)airplay_identity_flags());
    plist_dict_data(&p, "pk", pk, 32);
    plist_dict_string(&p, "pi", airplay_identity_pi());
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
    /* MediaRemote state is context only.  It never starts/stops/flushes audio;
     * buffered timeline authority comes from negotiated MediaDataControl
     * (srat/fshb), with legacy RTSP timing accepted only if explicitly sent. */
    media_remote_state_handle_command(body, body_len);
    rtsp_send_ok(socket, conn, req->cseq);

  } else if (strstr(req->path, "/feedback")) {
    const uint32_t feedback_no = ++conn->diag_feedback_rx;
    const uint64_t elapsed_ms = diag_session_elapsed_ms(conn);
    ESP_LOGI(TAG,
             "FEEDBACK RX #%u +%llums body=%uB cseq=%d stream=%lld active=%d",
             (unsigned)feedback_no, (unsigned long long)elapsed_ms,
             (unsigned)body_len, req->cseq, (long long)conn->stream_type,
             conn->stream_active ? 1 : 0);
    diag_log_bplist("FEEDBACK request plist", body, body_len);

    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        ESP_LOGI(TAG, "/feedback has networkTimeSecs=%lld", (long long)value);
      }
    }

    /* RemoteControl-only sessions use /feedback as a two-second keepalive
     * before an audio stream exists.  The AirPlay 2 reference flow answers
     * those requests with the bplist {streams:[]}; a bare 200 OK is not the
     * same protocol response and can make the sender abandon the RC endpoint. */
    const bool remote_control_keepalive =
        conn->remote_control_datastream != NULL && conn->stream_type == 0;

    if (remote_control_keepalive) {
      uint8_t response[96];
      const size_t response_len =
          bplist_build_feedback_empty_streams(response, sizeof(response));
      if (response_len > 0) {
        diag_log_bplist("FEEDBACK RC keepalive response plist", response,
                        response_len);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                           "Content-Type: application/x-apple-binary-plist\r\n",
                           (const char *)response, response_len);
        const uint32_t tx_no = ++conn->diag_feedback_tx;
        ESP_LOGI(TAG,
                 "FEEDBACK TX #%u +%llums 200 RC-KEEPALIVE bplist=%uB streams=[]",
                 (unsigned)tx_no,
                 (unsigned long long)diag_session_elapsed_ms(conn),
                 (unsigned)response_len);
      } else {
        rtsp_send_ok(socket, conn, req->cseq);
        const uint32_t tx_no = ++conn->diag_feedback_tx;
        ESP_LOGW(TAG,
                 "FEEDBACK TX #%u +%llums 200 EMPTY (RC keepalive plist build failed)",
                 (unsigned)tx_no,
                 (unsigned long long)diag_session_elapsed_ms(conn));
      }
    // Buffered streams (type 103): always answer with the stream status; it
    // acts as a keepalive that stops the iPhone sending TEARDOWN during a long
    // pause. Realtime AP2 (type 96) answers the same way while its stream is
    // set up, as Shairport Sync handle_feedback() does for 96 and 103.
    } else if (conn->stream_type == 103 ||
               (conn->stream_type == 96 && conn->stream_active)) {
      uint8_t response[128];
      size_t response_len = bplist_build_feedback_response(
          response, sizeof(response), conn->stream_type, 44100.0);

      if (response_len > 0) {
        diag_log_bplist("FEEDBACK response plist", response, response_len);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                           "Content-Type: application/x-apple-binary-plist\r\n",
                           (const char *)response, response_len);
        const uint32_t tx_no = ++conn->diag_feedback_tx;
        ESP_LOGI(TAG,
                 "FEEDBACK TX #%u +%llums 200 bplist=%uB type=%lld sr=44100",
                 (unsigned)tx_no,
                 (unsigned long long)diag_session_elapsed_ms(conn),
                 (unsigned)response_len, (long long)conn->stream_type);
      } else {
        // Fallback to simple OK if response build fails.
        rtsp_send_ok(socket, conn, req->cseq);
        const uint32_t tx_no = ++conn->diag_feedback_tx;
        ESP_LOGW(TAG,
                 "FEEDBACK TX #%u +%llums 200 EMPTY (plist build failed)",
                 (unsigned)tx_no,
                 (unsigned long long)diag_session_elapsed_ms(conn));
      }
    } else {
      // For non-buffered streams, simple OK is fine.
      rtsp_send_ok(socket, conn, req->cseq);
      const uint32_t tx_no = ++conn->diag_feedback_tx;
      ESP_LOGI(TAG, "FEEDBACK TX #%u +%llums 200 EMPTY non-audio",
               (unsigned)tx_no,
               (unsigned long long)diag_session_elapsed_ms(conn));
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

/*
 * Type-130 RemoteControl is a MediaRemote Protocol (MRP) channel carried in
 * an encrypted AirPlay DataStream.  The sender's first sync/comm message is a
 * bplist wrapper whose params.data contains one or more protobuf
 * ProtocolMessage envelopes (normally DEVICE_INFO_MESSAGE first).
 *
 * v4.1.81 only acknowledged this message at the DataStream framing layer.
 * Because type-130 had no on_message callback, the reply contained no payload
 * at all.  Apple receivers instead answer this request with a valid bplist
 * payload; an empty dictionary is 42 bytes on the wire.  Keep this handler
 * deliberately small: it diagnoses the nested MRP handshake and returns the
 * protocol-level empty dictionary without touching audio/MDC state.
 */

#define RCS_MRP_CAPTURE_MAX 4096U
#define RCS_HEX_CHUNK       32U

static const uint8_t k_rcs_empty_bplist_dict[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, /* bplist00 */
    0xd0,                                           /* empty dict */
    0x08,                                           /* offset table */
    /* trailer */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, /* offset size */
    0x01, /* object ref size */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, /* object count */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* top object */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09  /* offset table */
};

static bool rcs_read_varint(const uint8_t *buf, size_t len, size_t *pos,
                            uint64_t *out) {
  uint64_t value = 0;
  unsigned shift = 0;
  size_t p = *pos;
  while (p < len && shift < 64U) {
    const uint8_t b = buf[p++];
    value |= (uint64_t)(b & 0x7fU) << shift;
    if ((b & 0x80U) == 0) {
      *pos = p;
      *out = value;
      return true;
    }
    shift += 7U;
  }
  return false;
}

static const char *rcs_mrp_type_name(uint64_t type) {
  switch (type) {
    case 15: return "DEVICE_INFO_MESSAGE";
    case 16: return "CLIENT_UPDATES_CONFIG_MESSAGE";
    case 36: return "SET_READY_STATE_MESSAGE";
    case 37: return "DEVICE_INFO_UPDATE_MESSAGE";
    case 38: return "SET_CONNECTION_STATE_MESSAGE";
    case 42: return "GENERIC_MESSAGE";
    case 46: return "SET_NOW_PLAYING_CLIENT_MESSAGE";
    case 47: return "SET_NOW_PLAYING_PLAYER_MESSAGE";
    case 72: return "SET_DEFAULT_SUPPORTED_COMMANDS_MESSAGE";
    case 120: return "CONFIGURE_CONNECTION_MESSAGE";
    default: return "unmapped";
  }
}

static void rcs_hex_dump(const uint8_t *data, size_t len) {
  if (!data || len == 0) return;
  for (size_t off = 0; off < len; off += RCS_HEX_CHUNK) {
    const size_t n = (len - off < RCS_HEX_CHUNK) ? len - off : RCS_HEX_CHUNK;
    char line[RCS_HEX_CHUNK * 3U + 1U];
    size_t p = 0;
    for (size_t i = 0; i < n && p + 3U < sizeof(line); ++i) {
      const int w = snprintf(line + p, sizeof(line) - p, "%02X%s",
                             data[off + i], i + 1U == n ? "" : " ");
      if (w <= 0) break;
      p += (size_t)w;
    }
    line[sizeof(line) - 1U] = '\0';
    ESP_LOGI(TAG, "RCS params.data +0x%04x: %s", (unsigned)off, line);
  }
}

/* Parse just enough of the protobuf ProtocolMessage envelope to identify the
 * message. Field 1 is type and field 2 is the textual identifier. */
static bool rcs_log_one_mrp(const uint8_t *buf, size_t len, unsigned index) {
  size_t pos = 0;
  bool have_type = false;
  uint64_t type = 0;
  char identifier[96] = {0};
  unsigned fields = 0;

  while (pos < len && fields++ < 64U) {
    uint64_t key = 0;
    if (!rcs_read_varint(buf, len, &pos, &key) || key == 0) return false;
    const uint32_t field = (uint32_t)(key >> 3);
    const uint8_t wire = (uint8_t)(key & 7U);

    if (wire == 0) {
      uint64_t value = 0;
      if (!rcs_read_varint(buf, len, &pos, &value)) return false;
      if (field == 1) {
        type = value;
        have_type = true;
      }
    } else if (wire == 1) {
      if (len - pos < 8U) return false;
      pos += 8U;
    } else if (wire == 2) {
      uint64_t item_len64 = 0;
      if (!rcs_read_varint(buf, len, &pos, &item_len64) ||
          item_len64 > len - pos) {
        return false;
      }
      const size_t item_len = (size_t)item_len64;
      if (field == 2 && item_len > 0) {
        const size_t copy = item_len < sizeof(identifier) - 1U
                                ? item_len : sizeof(identifier) - 1U;
        bool printable = true;
        for (size_t i = 0; i < copy; ++i) {
          if (buf[pos + i] < 0x20 || buf[pos + i] > 0x7e) {
            printable = false;
            break;
          }
        }
        if (printable) {
          memcpy(identifier, buf + pos, copy);
          identifier[copy] = '\0';
        }
      }
      pos += item_len;
    } else if (wire == 5) {
      if (len - pos < 4U) return false;
      pos += 4U;
    } else {
      return false;
    }
  }

  if (!have_type) return false;
  ESP_LOGI(TAG, "RCS MRP[%u]: type=%llu (%s) id=\"%s\" bytes=%u",
           index, (unsigned long long)type, rcs_mrp_type_name(type),
           identifier, (unsigned)len);
  return true;
}

static bool rcs_log_mrp_blob(const uint8_t *data, size_t len) {
  if (!data || len == 0) return false;

  size_t pos = 0;
  unsigned index = 0;
  bool any = false;
  while (pos < len && index < 32U) {
    const size_t prefix = pos;
    uint64_t msg_len64 = 0;
    if (!rcs_read_varint(data, len, &pos, &msg_len64) || msg_len64 == 0 ||
        msg_len64 > len - pos) {
      pos = prefix;
      break;
    }
    if (!rcs_log_one_mrp(data + pos, (size_t)msg_len64, index)) {
      pos = prefix;
      break;
    }
    any = true;
    ++index;
    pos += (size_t)msg_len64;
  }

  if (any && pos == len) return true;
  if (!any && rcs_log_one_mrp(data, len, 0)) return true;
  return any;
}

static size_t rcs_make_uuid4(char out[37]) {
  if (!out) return 0;
  uint8_t u[16];
  for (size_t i = 0; i < sizeof(u); i += 4) {
    const uint32_t r = esp_random();
    u[i + 0] = (uint8_t)(r >> 24);
    u[i + 1] = (uint8_t)(r >> 16);
    u[i + 2] = (uint8_t)(r >> 8);
    u[i + 3] = (uint8_t)r;
  }
  u[6] = (uint8_t)((u[6] & 0x0fU) | 0x40U); /* UUIDv4 */
  u[8] = (uint8_t)((u[8] & 0x3fU) | 0x80U);
  snprintf(out, 37,
           "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9],
           u[10], u[11], u[12], u[13], u[14], u[15]);
  return 36;
}

static void remote_control_on_message(rtsp_datastream_t *stream,
                                      const char *type, const char *command,
                                      const uint8_t *payload,
                                      size_t payload_len, uint8_t *reply,
                                      size_t reply_cap, size_t *reply_len,
                                      void *user) {
  (void)stream;
  (void)user;
  if (reply_len) *reply_len = 0;
  bool saw_device_info = false;

  ESP_LOGI(TAG, "RCS message: %s/%s payload=%uB",
           type ? type : "?", command ? command : "?",
           (unsigned)payload_len);

  if (payload && payload_len >= 8U &&
      memcmp(payload, "bplist00", 8U) == 0) {
    uint8_t *nested = heap_caps_malloc(RCS_MRP_CAPTURE_MAX,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (nested) {
      size_t nested_len = 0;
      if (bplist_find_data_deep(payload, payload_len, "data", nested,
                                RCS_MRP_CAPTURE_MAX, &nested_len) &&
          nested_len > 0) {
        ESP_LOGI(TAG, "RCS handshake params.data=%uB", (unsigned)nested_len);
        rcs_hex_dump(nested, nested_len);
        if (nested_len >= 8U && memcmp(nested, "bplist00", 8U) == 0) {
          char *desc = heap_caps_malloc(2048,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (desc) {
            if (bplist_describe(nested, nested_len, desc, 2048) > 0) {
              ESP_LOGI(TAG, "RCS nested bplist: %s", desc);
            }
            heap_caps_free(desc);
          }
        } else {
          if (!rcs_log_mrp_blob(nested, nested_len)) {
            ESP_LOGW(TAG, "RCS params.data did not decode as MRP envelope");
          }
          size_t rp = 0;
          uint64_t ml = 0;
          if (rcs_read_varint(nested, nested_len, &rp, &ml) &&
              ml <= nested_len - rp && ml >= 2U &&
              nested[rp] == 0x08 && nested[rp + 1] == 0x0F) {
            saw_device_info = true;
          }
        }
      } else {
        ESP_LOGW(TAG, "RCS bplist has no readable params.data");
      }
      heap_caps_free(nested);
    } else {
      ESP_LOGW(TAG, "RCS params.data capture allocation failed");
    }
  }

  /* A sync/comm request expects a protocol-level plist response. Returning a
   * valid empty dictionary is intentionally different from a zero-byte rply. */
  if (type && command && strcmp(type, "sync") == 0 &&
      strcmp(command, "comm") == 0) {
    if (reply && reply_len && reply_cap >= sizeof(k_rcs_empty_bplist_dict)) {
      memcpy(reply, k_rcs_empty_bplist_dict, sizeof(k_rcs_empty_bplist_dict));
      *reply_len = sizeof(k_rcs_empty_bplist_dict);
      ESP_LOGI(TAG, "RCS sync/comm -> bplist {} reply (%uB)",
               (unsigned)*reply_len);
      if (saw_device_info) {
        ESP_LOGI(TAG,
                 "RCS receiver role: DEVICE_INFO ACKed; no receiver-originated MRP queued; waiting for sender follow-up");
      }
    } else {
      ESP_LOGW(TAG, "RCS sync/comm: reply buffer too small for bplist {}");
    }
  }
}

static void handle_type130_setup(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req,
                                 const uint8_t *body, size_t body_len) {
  bplist_kv_info_t kv[20];
  size_t kv_count = 0;
  int64_t seed_signed = 0;
  bool have_seed = false;
  if (bplist_get_stream_kv_info(body, body_len, 0, kv,
                                sizeof(kv) / sizeof(kv[0]), &kv_count)) {
    for (size_t i = 0; i < kv_count; ++i) {
      if (kv[i].value_type == BPLIST_VALUE_INT &&
          strcmp(kv[i].key, "seed") == 0) {
        seed_signed = kv[i].int_value;
        have_seed = true;
        break;
      }
    }
  }

  bool include_data_port = false;
  conn->remote_control_data_port = 0;
  if (have_seed) {
    const uint64_t seed = (uint64_t)seed_signed;
    esp_err_t err = rtsp_datastream_start(
        conn->hap_session, seed, conn->client_ip, "RemoteControl",
        remote_control_on_message, conn,
        &conn->remote_control_datastream, &conn->remote_control_data_port);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "SETUP type=130: DataStream start failed: %s",
               esp_err_to_name(err));
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq,
                         NULL, NULL, 0);
      return;
    }
    include_data_port = true;
  } else {
    /* Shairport answers type 130 even if seed is absent, but omits dataPort. */
    rtsp_datastream_stop(&conn->remote_control_datastream);
    ESP_LOGW(TAG, "SETUP type=130: no DataStream seed; response has no dataPort");
  }

  uint8_t plist_body[256];
  const size_t plist_len = bplist_build_datastream_setup(
      plist_body, sizeof(plist_body), conn->remote_control_data_port, 1,
      include_data_port);
  if (plist_len == 0) {
    rtsp_datastream_stop(&conn->remote_control_datastream);
    conn->remote_control_data_port = 0;
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq,
                       NULL, NULL, 0);
    return;
  }
  ESP_LOGI(TAG,
           "SETUP response: type=130 streamID=1 dataPort=%u seed=%s",
           conn->remote_control_data_port, have_seed ? "yes" : "no");
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Content-Type: application/x-apple-binary-plist\r\n",
                     (const char *)plist_body, plist_len);
}

static void handle_setup(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  bool is_bplist =
      strstr(req->content_type, "application/x-apple-binary-plist") != NULL;

  bool supports_dynamic_stream_id = false;
  bool stream_connection_rtp = false;
  bool stream_connection_rtcp = false;
  bool stream_connection_apap = false;
  bool stream_connection_apap_uses_stream_key = false;
  bool stream_connection_mdc = false;
  bool stream_connection_mdc_seed_valid = false;
  uint64_t stream_connection_mdc_seed = 0;
  /* APAP smoke build: retain the negotiated stream key locally so the
   * APAP endpoint can authenticate/decrypt the compressed access units. */
  uint8_t apap_stream_key[32] = {0};
  size_t apap_stream_key_len = 0;

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

  if (request_has_streams && stream_count > 0) {
    int64_t requested_type = -1;
    size_t ekey_len = 0, eiv_len = 0, shk_len = 0;
    if (bplist_get_stream_info(body, body_len, 0, &requested_type, &ekey_len,
                               &eiv_len, &shk_len) &&
        requested_type == 130) {
      ESP_LOGI(TAG, "SETUP: Remote Control dedicated DataStream type=130");
      handle_type130_setup(socket, conn, req, body, body_len);
      return;
    }
  }

  /* Remote-control-only initial SETUP (timingProtocol "None") must still
   * establish the AirPlay 2 event channel.  The sender connects to eventPort,
   * sends RECORD, then creates the type-130 DataStream.  Returning eventPort=0
   * leaves the RCS tunnel only half-established and modern senders tear it
   * down a few seconds later.  Do not acquire the audio play lock here: this
   * is control-plane state only and must not disturb an existing stream.
   *
   * The current embedded event receiver is global.  Never replace an event
   * task that already belongs to another active playback connection; in that
   * coexistence case retain the old non-invasive behaviour rather than
   * stealing its channel. */
  if (!conn->play_owner) {
    if (conn->event_port == 0 && event_task_handle == NULL) {
      conn->event_socket = rtsp_create_event_socket(&conn->event_port);
      if (conn->event_socket >= 0) {
        if (rtsp_start_event_port_task(conn->event_socket, conn->hap_session,
                                   conn->client_ip, conn->client_rtsp_port) ==
            ESP_OK) {
          ESP_LOGI(TAG,
                   "SETUP: RemoteControl event port %u created before RECORD",
                   conn->event_port);
        } else {
          close(conn->event_socket);
          conn->event_socket = -1;
          conn->event_port = 0;
          rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq,
                             NULL, NULL, 0);
          return;
        }
      }
    } else if (conn->event_port == 0) {
      ESP_LOGW(TAG,
               "SETUP: RemoteControl event channel busy; preserving active "
               "playback event task");
    }

    ESP_LOGI(TAG,
             "SETUP: remote-control-only session; playback untouched "
             "eventPort=%u",
             conn->event_port);
    uint8_t plist_body[128];
    const size_t plist_len = bplist_build_initial_setup(
        plist_body, sizeof(plist_body), conn->event_port, NULL);
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

    rtsp_connection_audio_setup_t conn_model;
    if (!rtsp_connection_model_parse_audio_setup(
            body, body_len, 0, stream_type, &conn_model)) {
      ESP_LOGE(TAG, "SETUP: missing AirPlay 2 audio format dictionary");
      rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    const int64_t codec_type = conn_model.codec_type;
    const int64_t sample_rate = conn_model.sample_rate;
    const int64_t samples_per_frame = conn_model.samples_per_frame;
    supports_dynamic_stream_id = conn_model.supports_dynamic_stream_id;
    if (conn_model.client_control_port_valid) {
      conn->client_control_port = conn_model.client_control_port;
    }

    if (conn_model.has_stream_connections) {
      stream_connection_rtp = conn_model.stream_connection_rtp;
      stream_connection_rtcp = conn_model.stream_connection_rtcp;
      stream_connection_apap = conn_model.stream_connection_apap;
      stream_connection_apap_uses_stream_key =
          conn_model.apap_use_stream_encryption_key;
      stream_connection_mdc = conn_model.stream_connection_media_data_control;
      stream_connection_mdc_seed_valid =
          conn_model.media_data_control_seed_valid;
      stream_connection_mdc_seed = conn_model.media_data_control_seed;
      ESP_LOGI(TAG,
               "SETUP: streamConnections rtp=%d rtcp=%d apap=%d "
               "apapStreamKey=%d mediaDataControl=%d mdcSeed=%d dynamicID=%d",
               stream_connection_rtp, stream_connection_rtcp,
               stream_connection_apap,
               stream_connection_apap_uses_stream_key,
               stream_connection_mdc, stream_connection_mdc_seed_valid,
               supports_dynamic_stream_id);
      /* Transport and timeline control are negotiated independently. Current
       * traces show Apple TV using RTP+MediaDataControl while direct iPhone
       * 27.2 chooses APAP+MediaDataControl for the HomePod-27 profile. Older
       * RTP-only senders may still use RTSP SETRATEANCHORTIME/FLUSHBUFFERED. */
      ESP_LOGI(TAG, "SETUP: buffered transport=%s control=%s",
               stream_connection_apap ? "APAP"
                   : (stream_connection_rtp ? "RTP" : "legacy"),
               stream_connection_mdc
                   ? "MediaDataControl (srat/fshb)"
                   : "RTSP (SETRATEANCHORTIME/FLUSHBUFFERED)");
      if (stream_connection_mdc && !stream_connection_mdc_seed_valid) {
        ESP_LOGE(TAG,
                 "SETUP: MediaDataControl requested without encryption seed");
        rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq,
                           NULL, NULL, 0);
        return;
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
      memcpy(apap_stream_key, audio_encrypt.key, sizeof(apap_stream_key));
      apap_stream_key_len = audio_encrypt.key_len;
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
        memcpy(apap_stream_key, audio_encrypt.key, sizeof(apap_stream_key));
        apap_stream_key_len = audio_encrypt.key_len;
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
        memcpy(apap_stream_key, audio_encrypt.key, sizeof(apap_stream_key));
        apap_stream_key_len = audio_encrypt.key_len;
      }
    }
  }

  // Create event port if needed
  if (conn->event_port == 0) {
    conn->event_socket = rtsp_create_event_socket(&conn->event_port);
    if (conn->event_socket >= 0) {
      if (rtsp_start_event_port_task(conn->event_socket, conn->hap_session,
                                   conn->client_ip, conn->client_rtsp_port) ==
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
      char timing_protocol[16] = {0};
      const bool setup_ptp =
          bplist_find_string_deep(body, body_len, "timingProtocol",
                                  timing_protocol, sizeof(timing_protocol)) &&
          strcmp(timing_protocol, "PTP") == 0;
      char timing_peer_addr[32] = {0};
      const char *timing_peer = NULL;
      if (setup_ptp &&
          wifi_get_ip_str(timing_peer_addr, sizeof(timing_peer_addr)) == ESP_OK) {
        timing_peer = timing_peer_addr;
      }

      uint8_t plist_body[384];
      size_t plist_len = bplist_build_initial_setup(
          plist_body, sizeof(plist_body), conn->event_port, timing_peer);
      if (setup_ptp) {
        ESP_LOGI(TAG,
                 "SETUP response: PTP timingPeerInfo Addresses=[%s] ID=%s eventPort=%u timingPort=0",
                 timing_peer ? timing_peer : "<unavailable>",
                 timing_peer ? timing_peer : "<unavailable>", conn->event_port);
      }
      if (plist_len == 0) {
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
      trace_body("SETUP reply", plist_body, plist_len, NULL);
#endif
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

  bool buffered = (stream_type == AUDIO_STREAM_APAP);

  /* A fresh audio stream must not inherit the previous APAP strt media point. */
  conn->mdc_start_valid = false;
  conn->mdc_media_time_value = 0;
  conn->mdc_media_time_scale = 0;
  conn->mdc_anchor_valid = false;
  conn->mdc_anchor_clock_id = 0;
  conn->mdc_anchor_ptp_ns = 0;
  conn->diag_audio_setup_us = (uint64_t)esp_timer_get_time();
  conn->diag_feedback_rx = 0;
  conn->diag_feedback_tx = 0;
  ESP_LOGI(TAG, "CONTROL DIAG: audio session timer started");

  /* Only AirPlay 2 realtime ALAC uses the NQPTP-style GM estimator plus
   * local presentation-anchor path. Buffered AAC stays on the PTP anchor
   * (audio_receiver_set_anchor_time) timing path.
   * The RTSP peer IP, not D7 clockIdentity, selects the PTP packet source. */
  ptp_clock_set_realtime_mode(!buffered,
                              !buffered ? conn->client_ip : 0);
  /* PTP belongs to the control session, not to an individual audio stream.
   * The normal path resets it when this RTSP connection acquires the play
   * lock, before SETPEERS/SETPEERSX.  Therefore a first stream SETUP must NOT
   * throw away the qualified samples collected since SETPEERS.  Keep this
   * fallback only for synthetic/tests or an unusual direct-handler path that
   * bypassed normal play-lock acquisition. */
  if (!conn->ptp_session_fresh) {
    ptp_clock_clear();
    conn->ptp_session_fresh = true;
    ESP_LOGW(TAG, "SETUP: PTP session-start fallback reset (play-lock reset missing)");
  }

  /* A genuine HomePod-27 profile advertises fex bit 72
   * (SupportsBufferedAPAP). Direct iPhone 27.2 therefore requests APAP rather
   * than RTP for type-103 audio. APAP owns framing/decryption/AAC decode, then
   * publishes timed PCM into the common EQ/PTP/I2S pipeline. This branch has
   * no legacy buffered-RTP byte FIFO. */
  rtsp_apap_audio_stop(&conn->apap_audio);
  conn->apap_port = 0;
  if (buffered) {
    /* This branch intentionally implements only the modern Buffered APAP
     * transport. A sender that asks for legacy type-103 RTP/TCP is rejected
     * instead of reviving the removed raw-byte FIFO path. */
    if (!stream_connection_apap || !stream_connection_mdc ||
        !stream_connection_apap_uses_stream_key || apap_stream_key_len != 32U) {
      ESP_LOGW(TAG,
               "SETUP: type-103 requires APAP + MediaDataControl + 32B stream key "
               "(apap=%d mdc=%d keyFlag=%d key=%uB)",
               stream_connection_apap ? 1 : 0, stream_connection_mdc ? 1 : 0,
               stream_connection_apap_uses_stream_key ? 1 : 0,
               (unsigned)apap_stream_key_len);
      rtsp_send_response(socket, conn, 461, "Unsupported Transport", req->cseq,
                         NULL, NULL, 0);
      return;
    }
    /* APAP is the only buffered transport on this branch. If a sender also
     * advertises legacy RTP/RTCP alternatives, do not mirror them back or
     * allocate runtime state for them. Realtime type-96 RTP is unaffected. */
    stream_connection_rtp = false;
    stream_connection_rtcp = false;
    esp_err_t apap_audio_err = audio_receiver_start_apap();
    if (apap_audio_err != ESP_OK) {
      ESP_LOGE(TAG, "SETUP: APAP audio pipeline start failed: %s",
               esp_err_to_name(apap_audio_err));
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq,
                         NULL, NULL, 0);
      return;
    }
    esp_err_t apap_err = rtsp_apap_audio_start(
        conn->client_ip, apap_stream_key, apap_stream_key_len,
        &conn->apap_audio, &conn->apap_port);
    if (apap_err != ESP_OK) {
      ESP_LOGE(TAG, "SETUP: APAP receiver start failed: %s",
               esp_err_to_name(apap_err));
      audio_receiver_stop();
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq,
                         NULL, NULL, 0);
      return;
    }
  }

  /* iOS currently accepts APAP with the conventional top-level controlPort
   * still present. Keep that bound UDP compatibility endpoint until wire
   * traces prove it can be omitted; no legacy buffered-RTP engine consumes it. */
  if (!buffered) {
    if (conn->apap_control_socket >= 0) {
      rtsp_conn_close_apap_control(conn);
      conn->apap_control_socket = -1;
    }
    ensure_stream_ports(conn);
  } else {
    conn->data_port = 0;
    if (conn->apap_control_socket >= 0) {
      rtsp_conn_close_apap_control(conn);
      conn->apap_control_socket = -1;
      conn->control_port = 0;
    }
    conn->apap_control_socket = rtsp_create_udp_socket(&conn->control_port);
    if (conn->apap_control_socket >= 0 && conn->control_port != 0)
      ap2_control_watch(conn->apap_control_socket, conn->control_port);
    if (conn->apap_control_socket < 0 || conn->control_port == 0) {
      ESP_LOGE(TAG, "SETUP: could not allocate buffered AP2 control port");
      if (conn->apap_control_socket >= 0) {
        rtsp_conn_close_apap_control(conn);
        conn->apap_control_socket = -1;
      }
      conn->control_port = 0;
      audio_receiver_stop();
      rtsp_apap_audio_stop(&conn->apap_audio);
      conn->apap_port = 0;
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }
  }

  uint16_t response_data_port = buffered ? conn->apap_port : conn->data_port;

  /* APAP owns buffered AAC transport end-to-end. Its decoded PCM enters the
   * common scheduler; only realtime ALAC uses the separate RTP path. */
  if (!buffered &&
      !start_realtime_receiver_or_fail(socket, conn, req)) {
    if (buffered && conn->apap_control_socket >= 0) {
      rtsp_conn_close_apap_control(conn);
      conn->apap_control_socket = -1;
      conn->control_port = 0;
    }
    return;
  }

  /* Bit 60: the MediaDataControl endpoint is a dedicated encrypted TCP
   * DataStream.  The request provides streamConnectionKeyEncryptionSeed;
   * derive DataStream-Salt<seed> keys from pair-verify and return the bound
   * streamConnectionKeyPort in the SETUP response. */
  rtsp_datastream_stop(&conn->media_data_control);
  conn->media_data_control_port = 0;
  if (stream_connection_mdc) {
    esp_err_t mdc_err = rtsp_datastream_start(
        conn->hap_session, stream_connection_mdc_seed, conn->client_ip,
        "MediaDataControl", mdc_on_message, conn, &conn->media_data_control,
        &conn->media_data_control_port);
    if (mdc_err != ESP_OK) {
      ESP_LOGE(TAG, "SETUP: MediaDataControl start failed: %s",
               esp_err_to_name(mdc_err));
      audio_receiver_stop();
      rtsp_apap_audio_stop(&conn->apap_audio);
      conn->apap_port = 0;
      if (buffered && conn->apap_control_socket >= 0) {
        rtsp_conn_close_apap_control(conn);
        conn->apap_control_socket = -1;
      }
      conn->control_port = 0;
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq,
                         NULL, NULL, 0);
      return;
    }
  }

  // Playout latency.  For REALTIME streams (type 96) the timing anchor
  // maps an RTP timestamp onto the sender's source
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

  const bool include_stream_id =
      supports_dynamic_stream_id || stream_connection_rtp ||
      stream_connection_rtcp || stream_connection_apap ||
      stream_connection_mdc;
  if (include_stream_id) {
    conn->stream_id = esp_random();
    if (conn->stream_id == 0) conn->stream_id = 1;
  } else {
    conn->stream_id = 0;
  }

  uint8_t plist_body[512];
  uint32_t audio_buffer_size = (stream_type == AUDIO_STREAM_APAP)
      ? (uint32_t)APAP_FRAME_QUEUE_BYTES : 0U;
  size_t plist_len = bplist_build_stream_setup(
      plist_body, sizeof(plist_body), stream_type, response_data_port,
      conn->control_port, audio_buffer_size, conn->stream_id,
      include_stream_id, stream_connection_rtp, stream_connection_rtcp,
      stream_connection_apap, conn->apap_port, stream_connection_mdc,
      conn->media_data_control_port, stream_connection_mdc_seed);
  if (plist_len == 0) {
    audio_receiver_stop();
    rtsp_datastream_stop(&conn->media_data_control);
    rtsp_apap_audio_stop(&conn->apap_audio);
    conn->media_data_control_port = 0;
    conn->apap_port = 0;
    if (buffered && conn->apap_control_socket >= 0) {
      rtsp_conn_close_apap_control(conn);
      conn->apap_control_socket = -1;
      conn->control_port = 0;
    }
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                       NULL, 0);
    return;
  }
  ESP_LOGI(TAG,
           "SETUP response: type=%lld dataPort=%u controlPort=%u audioBufferSize=%u "
           "streamID=%" PRIu32
           " scRTP=%d scRTCP=%d scAPAP=%d apapPort=%u scMDC=%d mdcPort=%u",
           (long long)stream_type, response_data_port, conn->control_port,
           (unsigned)audio_buffer_size, conn->stream_id,
           stream_connection_rtp, stream_connection_rtcp,
           stream_connection_apap, conn->apap_port,
           stream_connection_mdc, conn->media_data_control_port);
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Content-Type: application/x-apple-binary-plist\r\n",
                     (const char *)plist_body, plist_len);
  log_memory("after stream SETUP");
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
  trace_body("SETUP reply", plist_body, plist_len, NULL);
#endif

  // Enable NACK retransmission if we know the client's control port
  if (conn->client_control_port > 0 && conn->client_ip != 0) {
    audio_receiver_set_client_control(conn->client_ip,
                                      conn->client_control_port);
  }

  conn->stream_active = true;
  amp_session_activate_once(conn);

  if (stream_type == AUDIO_STREAM_REALTIME) {
    /* AP2 realtime ALAC (type 96) needs the play gate open here: in this
     * transport the presentation mapping can arrive through D7. Buffered AP2
     * AAC (type 103) stays gated until negotiated rate/anchor control (observed
     * as MediaDataControl srat on the HomePod-27 profile) opens playout. */
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
   * opened by negotiated rate/anchor control (MDC srat in the observed modern
   * path), while realtime AP2 is opened by stream SETUP and remains blocked
   * downstream until D7/PTP timing becomes valid. */
  ESP_LOGI(TAG, "RECORD: acknowledged eventPort=%u playOwner=%d",
           conn->event_port, conn->play_owner ? 1 : 0);
  char headers[128];
  snprintf(headers, sizeof(headers),
           "Audio-Latency: 0\r\n"
           "Audio-Jack-Status: connected\r\n");
  rtsp_send_response(socket, conn, 200, "OK", req->cseq, headers, NULL, 0);

  /* AirPlay 2 receiver state advertisement belongs after RECORD.  Keep the
   * encrypted event TX in event_port_task (it owns enc_nonce); only signal it
   * here after the 200 OK has gone out on the RTSP control connection. */
  if (conn->event_port != 0 && event_task_handle != NULL &&
      event_client_socket >= 0 && !event_update_info_sent) {
    event_update_info_requested = true;
    ESP_LOGI(TAG,
             "Event: RECORD completed -> schedule updateInfo on eventPort=%u",
             conn->event_port);
  } else if (conn->event_port != 0 && !event_update_info_sent) {
    ESP_LOGW(TAG,
             "Event: RECORD completed but event client is not ready; "
             "updateInfo not scheduled");
  }
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

static void handle_loudnessnormalization(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  bool enabled = false;
  if (req->body && req->body_len >= 8 &&
      memcmp(req->body, "bplist00", 8) == 0 &&
      bplist_find_bool(req->body, req->body_len,
                       "loudnessNormalizationEnabled", &enabled)) {
    const bool changed =
        (conn->loudness_normalization_enabled != enabled);
    conn->loudness_normalization_enabled = enabled;
    ESP_LOGI(TAG,
             "LOUDNESSNORMALIZATION: enabled=%d changed=%d body=%uB "
             "(state accepted; DSP unchanged)",
             enabled, changed, (unsigned)req->body_len);
  } else {
    ESP_LOGW(TAG, "LOUDNESSNORMALIZATION: missing/invalid enabled flag");
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

  // Stop the audio consumer but leave the buffer filling.  A fresh sender
  // rate/anchor update (observed as MDC srat on the modern path) re-aligns the
  // buffered frames to the correct wall-clock position on resume.
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
    /* Modern buffered audio is APAP-only. Without an explicit fshb boundary,
     * invalidate the timed PCM cache and wait for the next strt/anch map. */
    audio_receiver_apap_flush();
  }
  rtsp_send_ok(socket, conn, req->cseq);
}

/* Apply a FLUSHBUFFERED plist: the RTSP method body, or the same keys carried
 * as MediaDataControl "fshb".  `src` labels the log lines. */
static void apply_flushbuffered(rtsp_conn_t *conn, const uint8_t *body,
                                size_t body_len, const char *src) {
  // AirPlay 2 FLUSHBUFFERED belongs to timeline control, not connection
  // setup. Parse it into a typed timeline command first; the audio receiver
  // remains the sole owner of discard/cursor side effects.
  rtsp_flushbuffered_t flush = {0};
  if (rtsp_timeline_parse_flushbuffered(body, body_len, &flush)) {
    if (conn && conn->apap_audio) {
      ESP_LOGI(TAG,
               "%s APAP: fromSeq=%s%" PRId64 " untilSeq=%s%" PRId64
               " fromMedia=%s%" PRId64 "/%" PRId64
               " untilMedia=%s%" PRId64 "/%" PRId64,
               src, flush.have_from_seq ? "" : "missing/", flush.from_seq,
               flush.have_until_seq ? "" : "missing/", flush.until_seq,
               flush.have_from_media_time ? "" : "missing/",
               flush.from_media_time_value, flush.from_media_time_scale,
               flush.have_until_media_time ? "" : "missing/",
               flush.until_media_time_value, flush.until_media_time_scale);
      const bool have_from_media =
          flush.have_from_media_time &&
          flush.from_media_time_scale > 0 &&
          (uint64_t)flush.from_media_time_scale <= UINT32_MAX;
      const bool have_until_media =
          flush.have_until_media_time &&
          flush.until_media_time_scale > 0 &&
          (uint64_t)flush.until_media_time_scale <= UINT32_MAX;
      rtsp_apap_audio_flush(
          conn->apap_audio,
          flush.have_from_seq, (uint32_t)flush.from_seq,
          flush.have_until_seq, (uint32_t)flush.until_seq,
          have_from_media, flush.from_media_time_value,
          have_from_media ? (uint32_t)flush.from_media_time_scale : 0U,
          have_until_media, flush.until_media_time_value,
          have_until_media ? (uint32_t)flush.until_media_time_scale : 0U);
      return;
    }
    ESP_LOGW(TAG, "%s ignored: no active APAP transport", src);
  }

  /* Shairport: a FLUSHBUFFERED without a plist does nothing and returns 200. */
}

static void handle_flushbuffered(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
  (void)raw;
  (void)raw_len;
  apply_flushbuffered(conn, req->body, req->body_len, "FLUSHBUFFERED");
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
  log_memory("at TEARDOWN");
  ESP_LOGI(TAG,
           "CONTROL DIAG at TEARDOWN: +%llums feedback rx=%u tx=%u",
           (unsigned long long)diag_session_elapsed_ms(conn),
           (unsigned)conn->diag_feedback_rx, (unsigned)conn->diag_feedback_tx);

  /* A type-130 RemoteControl DataStream is a separate logical stream. iPhone
   * routinely tears it down while type-103 APAP audio is still active. The
   * old generic stream TEARDOWN path incorrectly stopped audio_receiver, MDC
   * and APAP here, which is exactly the short disconnect seen in the smoke
   * trace. Classify the listed streams before touching the audio engine. */
  bool teardown_remote_control = false;
  bool teardown_audio = false;
  bool teardown_unknown = false;
  if (has_streams) {
    for (size_t i = 0; i < stream_count; ++i) {
      int64_t type = 0;
      size_t ekey_len = 0, eiv_len = 0, shk_len = 0;
      if (!bplist_get_stream_info(body, body_len, i, &type, &ekey_len,
                                  &eiv_len, &shk_len)) {
        teardown_unknown = true;
        continue;
      }
      if (type == 130) {
        teardown_remote_control = true;
      } else if (type == AUDIO_STREAM_APAP ||
                 type == AUDIO_STREAM_REALTIME) {
        teardown_audio = true;
      } else {
        teardown_unknown = true;
        ESP_LOGW(TAG, "TEARDOWN: unclassified stream type=%lld",
                 (long long)type);
      }
    }
  }

  if (has_streams && teardown_remote_control && !teardown_audio &&
      !teardown_unknown) {
    ESP_LOGI(TAG,
             "TEARDOWN: type-130 RemoteControl only -> audio/APAP/MDC preserved");
    rtsp_datastream_stop(&conn->remote_control_datastream);
    conn->remote_control_data_port = 0;
    ESP_LOGI(TAG,
             "RSESSION TX id=%u cseq=%d TEARDOWN type130 -> 200 keep-control-connection-open owner=%d active=%d streamType=%lld",
             (unsigned)conn->diag_conn_id, req->cseq,
             conn->play_owner ? 1 : 0, conn->stream_active ? 1 : 0,
             (long long)conn->stream_type);
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }

  // Stream-level audio teardown is a pause: freeze playout immediately so audio
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
  if (teardown_remote_control) {
    rtsp_datastream_stop(&conn->remote_control_datastream);
    conn->remote_control_data_port = 0;
  }
  rtsp_datastream_stop(&conn->media_data_control);
  rtsp_apap_audio_stop(&conn->apap_audio);
  conn->media_data_control_port = 0;
  conn->apap_port = 0;
  conn->mdc_start_valid = false;
  conn->mdc_media_time_value = 0;
  conn->mdc_media_time_scale = 0;
  if (conn->apap_control_socket >= 0) {
    rtsp_conn_close_apap_control(conn);
    conn->apap_control_socket = -1;
  }
  conn->data_port = 0;
  conn->control_port = 0;
  conn->stream_id = 0;
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
    ESP_LOGI(TAG,
             "RSESSION TX id=%u cseq=%d full TEARDOWN -> 200 Connection:close owner=%d active=%d streamType=%lld",
             (unsigned)conn->diag_conn_id, req->cseq,
             conn->play_owner ? 1 : 0, conn->stream_active ? 1 : 0,
             (long long)conn->stream_type);
    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                       "Connection: close\r\n", NULL, 0);
    conn->close_after_response = true;
    return;
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

/* Apply a sender rate/anchor: the RTSP SETRATEANCHORTIME body, or the same
 * plist carried as MediaDataControl "srat".  `src` labels the log lines. */
static void apply_rate_anchor(rtsp_conn_t *conn, const uint8_t *body,
                              size_t body_len, const char *src) {
  rtsp_rate_anchor_t anchor = {0};
  const bool have_anchor_plist =
      rtsp_timeline_parse_rate_anchor(body, body_len, &anchor);

  /* Keep local aliases so the application of the timeline command remains
   * byte-for-byte equivalent in intent to the pre-refactor handler. */
  const double rate = anchor.rate;
  const bool have_rate = anchor.have_rate;
  const uint64_t clock_id = anchor.clock_id;
  const uint64_t network_time_secs = anchor.network_time_secs;
  const uint64_t network_time_frac = anchor.network_time_frac;
  const uint64_t rtp_time = anchor.rtp_time;
  const bool have_network_time_secs = anchor.have_network_time_secs;

  if (have_anchor_plist) {
    ESP_LOGI(TAG,
             "%s: secs=%llu frac=0x%016llx rtp=%llu"
             " clock=%016llx rate=%.1f stream=%lld",
             src, (unsigned long long)network_time_secs,
             (unsigned long long)network_time_frac,
             (unsigned long long)rtp_time,
             (unsigned long long)clock_id, rate,
             (long long)conn->stream_type);

    /* Pause first.  Do not publish an anchor from a rate=0 message and wake a
     * processor immediately before closing the play gate. */
    if (have_rate && rate == 0.0) {
      ESP_LOGI(TAG, "%s: rate=0 -> PAUSING", src);
      note_stream_pause_started(conn);
      conn->stream_paused = true;
      audio_receiver_pause();
      rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);
      return;
    }

    /* RTP is a wrapping 32-bit timeline. rtpTime==0 is therefore perfectly
     * valid; validity comes from key presence, not from the numeric value. */
    /* Shairport Sync 5.5.2: the anchor is set whenever networkTimeSecs is
     * present; rtpTime defaults to 0 if missing. */
    if (have_network_time_secs) {
      const uint64_t network_time_ns =
          rtsp_timeline_network_time_ns(&anchor);
      ESP_LOGI(TAG,
               "%s MAP: clock=%016llx ptp=%llu rtp=%llu", src,
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
                   "%s RT local raw=%llu local=%llu gm=%016llx age=%lums",
                   src, (unsigned long long)network_time_ns,
                   (unsigned long long)local_ns,
                   (unsigned long long)ps.master_clock_id,
                   (unsigned long)ps.mastership_age_ms);
          audio_realtime_anchor_result_t anchor_result = {0};
          if (!audio_receiver_set_realtime_anchor_local(
                  clock_id, ps.gm_change_count, ps.mastership_age_ms,
                  network_time_ns, local_ns, (uint32_t)rtp_time,
                  &anchor_result)) {
            ESP_LOGI(TAG,
                     "%s RT media rebase deferred clock=%016llx gm=%016llx epoch=%lu age=%lums",
                     src, (unsigned long long)clock_id,
                     (unsigned long long)ps.master_clock_id,
                     (unsigned long)ps.gm_change_count,
                     (unsigned long)ps.mastership_age_ms);
          }
        } else {
          ESP_LOGI(TAG,
                   "%s RT deferred clock=%016llx gm=%016llx ready=%d age=%lums",
                   src, (unsigned long long)clock_id,
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
    ESP_LOGI(TAG, "%s: no rate -> play state unchanged", src);
    return;
  }
  ESP_LOGI(TAG, "%s: rate=%.1f -> RESUMING (was_paused=%d)", src,
           rate, conn->stream_paused);
  if (conn->stream_paused) notify_timing_resume(conn);
  conn->stream_paused = false;
  audio_receiver_set_playing(true);
  rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);
}

static void handle_setrateanchortime(int socket, rtsp_conn_t *conn,
                                     const rtsp_request_t *req,
                                     const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
  apply_rate_anchor(conn, req->body, req->body_len, "SETRATEANCHORTIME");
  rtsp_send_ok(socket, conn, req->cseq);
}

/* MediaDataControl (streamConnections bit 60) messages, called from the
 * DataStream task.  `srat/fshb` are the RTP-style buffered dialect.  A real
 * HomePod feature profile makes direct iPhone 27.2 select Buffered APAP, where
 * we observe `strt` followed by `anch`; the latter requires a payload in the
 * encrypted DataStream rply rather than the old empty acknowledgement. */
#define MDC_ANCHOR_LEAD_MS 1000U
#define MDC_ANCHOR_MIN_MASTERSHIP_MS 1000U
#define MDC_ANCHOR_RELAX_MS 1500U
#define MDC_ANCHOR_GIVE_UP_MS 3000U

static bool read_mdc_rate(const uint8_t *body, size_t len, double *rate) {
  if (!rate) return false;
  if (bplist_find_real(body, len, "rate", rate)) return true;
  int64_t v = 0;
  if (bplist_find_int(body, len, "rate", &v)) {
    *rate = (double)v;
    return true;
  }
  return false;
}

static size_t mdc_anchor_reply(rtsp_conn_t *conn, uint8_t *reply,
                               size_t reply_cap) {
  if (!conn || !reply || reply_cap == 0) return 0;

  const bool choose_new_anchor = !conn->mdc_anchor_valid;
  uint64_t remote_ns = conn->mdc_anchor_ptp_ns;
  uint64_t anchor_clock_id = conn->mdc_anchor_clock_id;
  bool qualified = conn->mdc_anchor_valid;
  uint32_t waited_ms = 0;

  if (choose_new_anchor) {
    const uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    ptp_clock_snapshot_t ps = {0};
    for (;;) {
      ptp_clock_get_snapshot(&ps);
      waited_ms = (uint32_t)(esp_timer_get_time() / 1000LL) - start_ms;
      const bool usable = !ps.realtime_mode && ps.valid &&
                          ps.grandmaster_clock_id != 0 &&
                          ps.sample_age_ms != UINT32_MAX &&
                          ps.sample_age_ms <= 1500U;
      qualified = usable && ps.locked &&
                  ps.mastership_age_ms >= MDC_ANCHOR_MIN_MASTERSHIP_MS;
      if (usable && (qualified || waited_ms >= MDC_ANCHOR_RELAX_MS)) break;
      if (waited_ms >= MDC_ANCHOR_GIVE_UP_MS) {
        ESP_LOGW(TAG,
                 "MDC anch: no usable PTP after %u ms (valid=%d locked=%d "
                 "gm=%016llx age=%u sampleAge=%u) -> empty rply",
                 (unsigned)waited_ms, ps.valid, ps.locked,
                 (unsigned long long)ps.grandmaster_clock_id,
                 (unsigned)ps.mastership_age_ms, (unsigned)ps.sample_age_ms);
        return 0;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    const uint64_t local_ns =
        (uint64_t)esp_timer_get_time() * 1000ULL +
        (uint64_t)MDC_ANCHOR_LEAD_MS * 1000000ULL;
    if (!ptp_clock_engine_local_to_remote(local_ns, ps.filtered_offset_ns,
                                          &remote_ns) ||
        remote_ns == 0) {
      ESP_LOGW(TAG, "MDC anch: local->PTP conversion failed");
      return 0;
    }
    anchor_clock_id = ps.grandmaster_clock_id;
    conn->mdc_anchor_ptp_ns = remote_ns;
    conn->mdc_anchor_clock_id = anchor_clock_id;
    conn->mdc_anchor_valid = true;
  }

  const uint64_t secs = remote_ns / 1000000000ULL;
  const uint64_t ns = remote_ns % 1000000000ULL;
  const uint64_t frac32 = (ns << 32) / 1000000000ULL;
  const uint64_t frac = frac32 << 32;
  static const char *const keys[] = {
      "rate", "networkTimeSecs", "networkTimeFrac", "networkTimeFlags",
      "networkTimeTimelineID", "mediaTimeValue", "mediaTimeScale"};
  const uint64_t values[] = {
      1, secs, frac, 0, anchor_clock_id,
      (uint64_t)(conn->mdc_start_valid ? conn->mdc_media_time_value : 0),
      (uint64_t)(conn->mdc_start_valid ? conn->mdc_media_time_scale
                                       : 1000000000LL)};

  const size_t len = bplist_build_int_dict(
      reply, reply_cap, keys, values, sizeof(keys) / sizeof(keys[0]));
  ESP_LOGI(TAG,
           "MDC anch: reply clock=%016llx ptp=%llu (%s; waited %u ms) "
           "mediaTime=%lld/%lld%s body=%uB",
           (unsigned long long)anchor_clock_id,
           (unsigned long long)remote_ns,
           choose_new_anchor ? (qualified ? "chosen/locked" : "chosen/valid")
                             : "reused",
           (unsigned)waited_ms,
           (long long)conn->mdc_media_time_value,
           (long long)conn->mdc_media_time_scale,
           conn->mdc_start_valid ? "" : " no-strt", (unsigned)len);

  if (len && choose_new_anchor && conn->mdc_start_valid &&
      conn->apap_audio) {
    (void)audio_receiver_set_media_anchor(
        anchor_clock_id, remote_ns, conn->mdc_media_time_value,
        (uint32_t)conn->mdc_media_time_scale);
  }
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
  if (len) trace_body("MDC anch reply", reply, len, NULL);
#endif
  return len;
}

static void mdc_on_message(rtsp_datastream_t *stream,
                           const char *type, const char *command,
                           const uint8_t *payload, size_t payload_len,
                           uint8_t *reply, size_t reply_cap, size_t *reply_len,
                           void *user) {
  rtsp_conn_t *conn = (rtsp_conn_t *)user;
  (void)stream;
  if (!conn || !command) return;
  if (reply_len) *reply_len = 0;

  if (strcmp(command, "srat") == 0) {
    apply_rate_anchor(conn, payload, payload_len, "MDC srat");
  } else if (strcmp(command, "fshb") == 0) {
    apply_flushbuffered(conn, payload, payload_len, "MDC fshb");
  } else if (strcmp(command, "strt") == 0) {
    int64_t value = 0;
    int64_t scale = 0;
    double rate = 1.0;
    const bool have_value =
        bplist_find_int(payload, payload_len, "mediaTimeValue", &value);
    const bool have_scale =
        bplist_find_int(payload, payload_len, "mediaTimeScale", &scale);
    const bool have_rate = read_mdc_rate(payload, payload_len, &rate);
    conn->mdc_start_valid = have_value && have_scale && scale > 0;
    conn->mdc_media_time_value = value;
    conn->mdc_media_time_scale = scale;
    conn->mdc_anchor_valid = false;
    conn->mdc_anchor_clock_id = 0;
    conn->mdc_anchor_ptp_ns = 0;
    if (have_rate && rate == 0.0) audio_receiver_pause();
    ESP_LOGI(TAG, "MDC strt: mediaTime=%lld/%lld (%.6f s) rate=%.3f%s",
             (long long)value, (long long)scale,
             scale > 0 ? (double)value / (double)scale : 0.0, rate,
             have_rate ? "" : " no-rate");
  } else if (strcmp(command, "anch") == 0) {
    if (reply && reply_len) {
      *reply_len = mdc_anchor_reply(conn, reply, reply_cap);
    }
  } else if (strcmp(command, "magc") == 0) {
    uint8_t cookie[64];
    size_t cookie_len = 0;
    if (bplist_find_data(payload, payload_len, "magicCookie", cookie,
                         sizeof(cookie), &cookie_len)) {
      char hex[2 * sizeof(cookie) + 1];
      for (size_t i = 0; i < cookie_len; ++i) {
        snprintf(hex + 2 * i, 3, "%02x", cookie[i]);
      }
      hex[2 * cookie_len] = '\0';
      ESP_LOGI(TAG, "MDC magc: magicCookie %uB %s", (unsigned)cookie_len, hex);
    } else {
      ESP_LOGI(TAG, "MDC magc: magicCookie not readable");
    }
  } else if (strcmp(command, "amsm") != 0) {
    ESP_LOGI(TAG, "MDC %s/%s: no handler yet (logged only)", type ? type : "?",
             command);
  }
}

/* Receiver-chosen anchor is implemented only on the observed encrypted MDC
 * `anch` command.  We still do not advertise or invent RTSP GETANCHOR/SETRATE
 * verbs. */

static void handle_setpeers(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  const bool extended = strcasecmp(req->method, "SETPEERSX") == 0;

  /* Connection/setup metadata boundary: SETPEERS/SETPEERSX updates which PTP
   * peers are admitted/identified, but it never creates or replaces an audio
   * RTP<->time anchor. Timeline authority remains D7 for realtime streams
   * and negotiated rate/anchor control (MDC srat on the observed buffered path). */

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
