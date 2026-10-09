#include "rtsp_handlers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sodium.h"

#include "audio_receiver.h"
#include "audio_diag.h"
#include "amp_control.h"
#include "hap.h"
#include "hap_pairings.h"
#include "ptp_clock.h"
#include "plist.h"
#include "rtsp_fairplay.h"
#include "settings.h"
#include "socket_utils.h"
#include "tlv8.h"

#include "rtsp_events.h"
#include "rtsp_server.h"
#include "bplist_writer.h"
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
// To add a new codec, add an entry to codec_registry[] below.

typedef struct {
  const char *name; // Codec name: "ALAC", "AAC", "OPUS"
  int64_t type_id;  // bplist "ct" value (2=ALAC, 4=AAC, 8=AAC-ELD, 64=OPUS)
} rtsp_codec_t;

static void configure_codec(audio_format_t *fmt, const char *name, int64_t sr,
                            int64_t spf) {
  strcpy(fmt->codec, name);
  fmt->sample_rate = (int)sr;
  fmt->channels = 2;
  fmt->bits_per_sample = 16;
  fmt->frame_size = (int)spf;
}

// Codec registry - add new codecs here
// ct values: 2=ALAC, 4=AAC, 8=AAC-ELD, 64=OPUS (based on AirPlay 2 protocol)
static const rtsp_codec_t codec_registry[] = {
    {"ALAC", 2}, {"AAC", 4}, {"AAC-ELD", 8}, {"OPUS", 64}, {NULL, 0}};

static bool rtsp_codec_configure(int64_t type_id, audio_format_t *fmt,
                                 int64_t sample_rate,
                                 int64_t samples_per_frame) {
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

// Default playout latency for realtime (type 96) streams when SETUP does not
// carry a usable latencyMin: 11025 samples = 250 ms at 44.1 kHz.  This is the
// standard AirPlay realtime-stream minimum latency; senders transmit audio
// ~2 s (88200 samples) ahead of this deadline.
#define AIRPLAY_RT_LATENCY_DEFAULT_SAMPLES 11025
static void rtsp_get_device_id(char *device_id, size_t len) {
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

/* Realtime (type 96) only: reserve free UDP data/control port numbers for the
 * SETUP response. The realtime receiver binds them when it starts. */
static void ensure_realtime_ports(rtsp_conn_t *conn) {
  int temp_socket;
  if (conn->data_port == 0) {
    temp_socket = rtsp_create_udp_socket(&conn->data_port);
    if (temp_socket >= 0) {
      close(temp_socket);
    }
  }
  if (conn->control_port == 0) {
    temp_socket = rtsp_create_udp_socket(&conn->control_port);
    if (temp_socket >= 0) {
      close(temp_socket);
    }
  }
}

static bool start_audio_receiver_or_fail(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         int64_t stream_type) {
  audio_receiver_set_stream_type((audio_stream_type_t)stream_type);
  esp_err_t err = audio_receiver_start_stream(
      conn->data_port, conn->control_port, conn->buffered_port);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start audio receiver: %s", esp_err_to_name(err));
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                       NULL, 0);
    return false;
  }
  return true;
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
static void handle_loudnessnormalization(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         const uint8_t *raw, size_t raw_len);

// Dispatch table
static const rtsp_method_handler_t method_handlers[] = {
    {"OPTIONS", handle_options},
    {"GET", handle_get},
    {"POST", handle_post},
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
    {"LOUDNESSNORMALIZATION", handle_loudnessnormalization},
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

static bool audio_owner_method(const char *method) {
  static const char *const methods[] = {
      "SET_PARAMETER", "PAUSE", "FLUSH", "FLUSHBUFFERED",
      "SETRATEANCHORTIME", "SETPEERS", "SETPEERSX", NULL};
  for (int i = 0; methods[i]; i++) {
    if (strcasecmp(method, methods[i]) == 0) return true;
  }
  return false;
}

int rtsp_dispatch(int socket, rtsp_conn_t *conn, const uint8_t *raw_request,
                  size_t raw_len) {
  rtsp_request_t req;
  if (rtsp_request_parse(raw_request, raw_len, &req) < 0) {
    ESP_LOGW(TAG, "Failed to parse RTSP request");
    return -1;
  }

  AUDIO_DIAG_FLUSH_RTSP_BEGIN(socket, req.method);

  /* One line per request. The session URL (rtsp://<ip>/<session>) is left
   * out; frequent requests that log their own content (SET_PARAMETER volume,
   * progress, metadata) and the /feedback keepalive only at debug. */
  {
    const bool session_url = strncmp(req.path, "rtsp://", 7) == 0;
    const char *path = session_url ? "" : req.path;
    const bool quiet = strstr(req.path, "/feedback") ||
                       strcasecmp(req.method, "SET_PARAMETER") == 0 ||
                       strcasecmp(req.method, "GET_PARAMETER") == 0;
    char extra[24] = "";
    if (req.body_len) snprintf(extra, sizeof(extra), " (%u B)", (unsigned)req.body_len);
    if (quiet) {
      ESP_LOGD(TAG, "RTSP <- %s %s cseq=%d%s", req.method, path, req.cseq, extra);
    } else {
      ESP_LOGI(TAG, "RTSP <- %s%s%s cseq=%d%s%s", req.method, path[0] ? " " : "", path,
               req.cseq, extra, conn->encrypted_mode ? "" : " [not encrypted]");
    }
  }

  /* Requests that change the global audio / PTP state are only taken from
   * the connection that owns audio. A remote-control or management
   * connection (e.g. the Home hub) gets 200 OK and the playing session is
   * left alone. */
  if (!conn->owns_audio && audio_owner_method(req.method)) {
    ESP_LOGI(TAG, "%s ignored: not the audio session%s", req.method,
             conn->rc_only ? " (remote control connection)" : "");
    rtsp_send_ok(socket, conn, req.cseq);
    AUDIO_DIAG_FLUSH_RTSP_END(socket, req.method);
    return 0;
  }

  // Find handler in dispatch table
  for (const rtsp_method_handler_t *h = method_handlers; h->method; h++) {
    if (strcasecmp(req.method, h->method) == 0) {
      h->handler(socket, conn, &req, raw_request, raw_len);
      AUDIO_DIAG_FLUSH_RTSP_END(socket, req.method);
      return 0;
    }
  }

  ESP_LOGW(TAG, "Unknown method: %s", req.method);
  if (request_uses_rtsp(&req)) {
    rtsp_send_response(socket, conn, 501, "Not Implemented", req.cseq,
                       "Content-Type: text/plain\r\n", "Not Implemented", 15);
  } else {
    rtsp_send_http_response(socket, conn, 501, "Not Implemented", "text/plain",
                            "Not Implemented", 15);
  }
  AUDIO_DIAG_FLUSH_RTSP_END(socket, req.method);
  return 0;
}

// ============================================================================
// Handler implementations
// ============================================================================

static void handle_options(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req, const uint8_t *raw,
                           size_t raw_len) {
  (void)raw;
  (void)raw_len;
  static const char public_methods[] =
      "Public: SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, "
      "OPTIONS, POST, GET, SET_PARAMETER, GET_PARAMETER, SETPEERS, "
      "SETPEERSX, SETRATEANCHORTIME, LOUDNESSNORMALIZATION\r\n";
  rtsp_send_response(socket, conn, 200, "OK", req->cseq, public_methods, NULL,
                     0);
}

#ifdef CONFIG_AIRPLAY_HOMEKIT
/* GET /info laid out as an AirPort Express answers it (same key names and
 * value types; plus the keys earlier senders of this firmware use). */
static size_t build_info_airport_style(uint8_t *out, size_t cap, const char *device_id,
                                       const char *device_name, const uint8_t *pk,
                                       uint64_t features) {
  bpw_t *w = heap_caps_malloc(sizeof(*w), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!w) w = malloc(sizeof(*w));
  if (!w) return 0;
  char pi[AIRPLAY_PAIRING_ID_LEN], serial[13], firmware[32];
  airplay_get_pairing_id(pi, sizeof(pi));
  airplay_get_serial_number(serial, sizeof(serial));
  airplay_get_firmware_revision(firmware, sizeof(firmware));
  static const int64_t stream_types[] = {AUDIO_STREAM_REALTIME, AUDIO_STREAM_BUFFERED};

  bpw_init(w);
  bpw_dict_begin(w);
  bpw_key(w, "audioFormats");
  bpw_array_begin(w);
  bpw_dict_begin(w);
  bpw_kv_int(w, "audioInputFormats", 0x1000000);
  bpw_kv_int(w, "audioOutputFormats", 0x1000000);
  bpw_kv_int(w, "type", AUDIO_STREAM_REALTIME);
  bpw_end(w);
  bpw_end(w);
  bpw_key(w, "audioLatencies");
  bpw_array_begin(w);
  for (size_t i = 0; i < sizeof(stream_types) / sizeof(stream_types[0]); i++) {
    bpw_dict_begin(w);
    bpw_kv_string(w, "audioType", "default");
    bpw_kv_int(w, "inputLatencyMicros", 0);
    bpw_kv_int(w, "outputLatencyMicros", 0);
    bpw_kv_int(w, "type", stream_types[i]);
    bpw_end(w);
  }
  bpw_end(w);
  bpw_kv_string(w, "build", firmware);
  bpw_kv_string(w, "deviceID", device_id);
  bpw_kv_string(w, "deviceid", device_id);
  bpw_kv_uint(w, "features", features);
  bpw_kv_string(w, "firmwareBuildDate", __DATE__);
  bpw_kv_string(w, "firmwareRevision", firmware);
  bpw_kv_bool(w, "keepAliveLowPower", true);
  bpw_kv_bool(w, "keepAliveSendStatsAsBody", true);
  bpw_kv_string(w, "manufacturer", CONFIG_AIRPLAY_HOMEKIT_MANUFACTURER);
  bpw_kv_string(w, "model", AIRPLAY_MODEL);
  bpw_kv_string(w, "name", device_name);
  bpw_kv_string(w, "pi", pi);
  bpw_kv_data(w, "pk", pk, 32);
  bpw_kv_string(w, "protocolVersion", AIRPLAY_PROTOVERS);
  bpw_kv_string(w, "protovers", AIRPLAY_PROTOVERS);
  bpw_kv_string(w, "sdk", "AirPlay;2.0.2");
  bpw_kv_string(w, "serialNumber", serial);
  bpw_kv_string(w, "sourceVersion", AIRPLAY_SOURCE_VERSION);
  bpw_kv_string(w, "srcvers", AIRPLAY_SOURCE_VERSION);
  bpw_kv_uint(w, "statusFlags", airplay_status_flags());
  bpw_kv_int(w, "vv", AIRPLAY_PROTOCOL_VERSION);
  bpw_end(w);
  const size_t n = bpw_finish(w, out, cap);
  free(w);
  return n;
}
#endif

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

    /* The reply buffers are only needed while answering: allocate them per
     * request and free them right after sending. */
    if (request_uses_rtsp(req)) {
      const size_t body_cap = 1536;
      uint8_t *body = malloc(body_cap);
#ifdef CONFIG_AIRPLAY_HOMEKIT
      size_t body_len =
          body ? build_info_airport_style(body, body_cap, device_id, device_name, pk,
                                          features)
               : 0;
#else
      size_t body_len =
          body ? bplist_build_info_response(body, body_cap, device_id,
                                            device_name, pk, 32, features)
               : 0;
#endif
      if (body_len == 0) {
        ESP_LOGE(TAG, "Failed to build binary /info response");
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        free(body);
        return;
      }
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/x-apple-binary-plist\r\n",
                         (const char *)body, body_len);
      free(body);
      return;
    }

    const size_t body_cap = 4096;
    char *body = malloc(body_cap);
    if (!body) {
      rtsp_send_http_response(socket, conn, 500, "Internal Error",
                              "text/plain", "Out of memory", 13);
      return;
    }
    plist_t p;
    char pairing_id[AIRPLAY_PAIRING_ID_LEN];
    airplay_get_pairing_id(pairing_id, sizeof(pairing_id));

    plist_init(&p, body, body_cap);
    plist_begin(&p);
    plist_dict_begin(&p);

    plist_dict_string(&p, "deviceid", device_id);
    plist_dict_uint(&p, "features", features);
    plist_dict_string(&p, "model", AIRPLAY_MODEL);
    plist_dict_string(&p, "protovers", AIRPLAY_PROTOVERS);
    plist_dict_string(&p, "srcvers", AIRPLAY_SOURCE_VERSION);
#ifdef CONFIG_AIRPLAY_HOMEKIT
    /* Apple key names (AirPort Express, shairport-sync), next to the above. */
    plist_dict_string(&p, "deviceID", device_id);
    plist_dict_string(&p, "protocolVersion", AIRPLAY_PROTOVERS);
    plist_dict_string(&p, "sourceVersion", AIRPLAY_SOURCE_VERSION);
#endif
    plist_dict_int(&p, "vv", AIRPLAY_PROTOCOL_VERSION);
    plist_dict_int(&p, "statusFlags", (int64_t)airplay_status_flags());
    plist_dict_data(&p, "pk", pk, 32);
    plist_dict_string(&p, "pi", pairing_id);
    plist_dict_string(&p, "name", device_name);
#ifdef CONFIG_AIRPLAY_HOMEKIT
    {
      char serial[13], firmware[32];
      airplay_get_serial_number(serial, sizeof(serial));
      airplay_get_firmware_revision(firmware, sizeof(firmware));
      plist_dict_string(&p, "manufacturer", CONFIG_AIRPLAY_HOMEKIT_MANUFACTURER);
      plist_dict_string(&p, "serialNumber", serial);
      plist_dict_string(&p, "firmwareRevision", firmware);
    }
#endif

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
    // anchor timing; the receiver compensates its own output latency
    // (I2S DMA + configured DAC/DSP latency) internally. Report 0 so the
    // sender does NOT also adjust its anchor, otherwise the delay would be
    // compensated twice and the ESP would play ahead of other speakers.
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
    free(body);
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

/* A binary plist body, one key per log line. debug_only: only when this
 * tag logs at debug level (bodies of requests that repeat in every session,
 * such as the Home hub's SETUP and /configure). */
static void log_body_plist(const char *what, const uint8_t *b, size_t n, bool debug_only) {
  if (debug_only && esp_log_level_get(TAG) < ESP_LOG_DEBUG) return;
  const size_t cap = 4096;
  char *text = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!text) text = malloc(cap);
  if (!text) return;
  if (bplist_dump(b, n, text, cap, true) > 0) {
    if (debug_only) ESP_LOGD(TAG, "    %s:", what);
    else ESP_LOGI(TAG, "    %s:", what);
    char *line = text;
    while (line && *line) {
      char *nl = strchr(line, '\n');
      if (nl) *nl = '\0';
      if (debug_only) ESP_LOGD(TAG, "      %s", line);
      else ESP_LOGI(TAG, "      %s", line);
      line = nl ? nl + 1 : NULL;
    }
  }
  free(text);
}

/* First bytes of a body in hex, for requests we do not understand yet. */
static void log_body_hex(const char *what, const uint8_t *b, size_t n) {
  char hex[2 * 48 + 1];
  const size_t k = n < 48 ? n : 48;
  for (size_t i = 0; i < k; i++) snprintf(hex + 2 * i, 3, "%02x", b[i]);
  hex[2 * k] = '\0';
  ESP_LOGI(TAG, "    %s %u B: %s%s", what, (unsigned)n, hex, n > 48 ? "..." : "");
}

/* HomeKit pairings management (HAP methods 3 add, 4 remove, 5 list) on
 * /pair-add, /pair-remove, /pair-list (or /pairings with a method TLV).
 * Only a verified admin controller may use them, with one exception: the
 * Home app adds an AirPlay speaker over a transient pairing (code 3939) and
 * then sends /pair-add with the home's controller. While the receiver has no
 * pairing at all, that first /pair-add makes its controller the owner
 * (admin), and this connection acts as that admin from then on. */
static void handle_pairings(int socket, rtsp_conn_t *conn, const rtsp_request_t *req) {
  enum { M_ADD = 3, M_REMOVE = 4, M_LIST = 5 };
  static const uint8_t TLV_PERMISSIONS = 0x0B;
  const uint8_t *body = req->body;
  const size_t body_len = req->body_len;
  size_t len = 0;
  const uint8_t *m = body ? tlv8_find(body, body_len, TLV_TYPE_METHOD, &len) : NULL;
  int method = (m && len == 1) ? m[0] : -1;
  if (method < 0) {
    if (strstr(req->path, "/pair-add")) method = M_ADD;
    else if (strstr(req->path, "/pair-remove")) method = M_REMOVE;
    else if (strstr(req->path, "/pair-list")) method = M_LIST;
  }
  hap_session_t *s = conn->hap_session;
  const bool admin = s && s->controller_verified && (s->controller_perm & HAP_PERM_ADMIN);
  bool first_owner = !admin && method == M_ADD && s && conn->encrypted_mode &&
                     hap_pairings_count() == 0;
  /* The Home app retries a failed add the same way (transient + /pair-add).
   * If it re-adds a stored admin with the very same long-term key, accept it
   * as that owner again instead of refusing until the flash is erased. */
  if (!admin && !first_owner && method == M_ADD && s && conn->encrypted_mode && body) {
    size_t il = 0, kl = 0;
    const uint8_t *i = tlv8_find(body, body_len, TLV_TYPE_IDENTIFIER, &il);
    const uint8_t *k = tlv8_find(body, body_len, TLV_TYPE_PUBLIC_KEY, &kl);
    uint8_t stored[32], perm = 0;
    if (i && k && kl == 32 && il <= HAP_PAIRING_ID_MAX &&
        hap_pairings_find(i, il, stored, &perm) && (perm & HAP_PERM_ADMIN) &&
        memcmp(stored, k, 32) == 0) {
      first_owner = true;
      ESP_LOGW(TAG, "HomeKit: owner %.*s adds itself again (Home app retry)", (int)il,
               (const char *)i);
    }
  }
  ESP_LOGI(TAG, "HomeKit %s (method %d) from %s%.*s", req->path, method,
           admin ? "admin " : (s && s->controller_verified ? "user " : "unverified controller"),
           s ? (int)s->controller_id_len : 0, s ? s->controller_id : "");
  if (body && body_len) {
    size_t il = 0, pl = 0;
    const uint8_t *i = tlv8_find(body, body_len, TLV_TYPE_IDENTIFIER, &il);
    const uint8_t *p = tlv8_find(body, body_len, TLV_PERMISSIONS, &pl);
    if (i && il)
      ESP_LOGI(TAG, "    identifier %.*s permissions %d", (int)(il > 64 ? 64 : il),
               (const char *)i, (p && pl == 1) ? p[0] : -1);
  }

  uint8_t *out = malloc(1024);
  if (!out) {
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL, NULL, 0);
    return;
  }
  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, out, 1024);
  size_t out_len = 0;

#ifndef CONFIG_AIRPLAY_HOMEKIT
  const bool enabled = false;
#else
  const bool enabled = true;
#endif
  if (!enabled || (!admin && !first_owner)) {
    ESP_LOGW(TAG, "HomeKit %s refused: %s", req->path,
             enabled ? "not an admin controller" : "HomeKit disabled in menuconfig");
    tlv8_encode_byte(&enc, TLV_TYPE_STATE, 2);
    tlv8_encode_byte(&enc, TLV_TYPE_ERROR, TLV_ERROR_AUTHENTICATION);
    out_len = tlv8_encoder_size(&enc);
  } else if (method == M_LIST) {
    out_len = hap_pairings_list_tlv(out, 1024);
    hap_pairings_log("pair-list", false);
  } else if (method == M_ADD || method == M_REMOVE) {
    size_t id_len = 0, key_len = 0, perm_len = 0;
    const uint8_t *id = tlv8_find(body, body_len, TLV_TYPE_IDENTIFIER, &id_len);
    const uint8_t *key = tlv8_find(body, body_len, TLV_TYPE_PUBLIC_KEY, &key_len);
    const uint8_t *perm = tlv8_find(body, body_len, TLV_PERMISSIONS, &perm_len);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (id && id_len > 0 && id_len <= HAP_PAIRING_ID_MAX) {
      uint8_t p = (perm && perm_len == 1) ? perm[0] : 0;
      if (first_owner) p |= HAP_PERM_ADMIN; /* the first pairing is the owner */
      if (method == M_ADD && key && key_len == 32)
        err = hap_pairings_add(id, id_len, key, p);
      else if (method == M_REMOVE)
        err = hap_pairings_remove(id, id_len);
      if (err == ESP_OK && first_owner) {
        memcpy(s->controller_id, id, id_len);
        s->controller_id_len = (uint8_t)id_len;
        s->controller_perm = HAP_PERM_ADMIN;
        s->controller_verified = true;
        ESP_LOGI(TAG, "HomeKit: %.*s is the owner (admin), added over a %s session",
                 (int)id_len, (const char *)id,
                 s->pair_setup_transient ? "transient" : "verified");
      }
    }
    if (method == M_REMOVE && err == ESP_ERR_NOT_FOUND) err = ESP_OK; /* idempotent */
    tlv8_encode_byte(&enc, TLV_TYPE_STATE, 2);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "HomeKit %s failed: %s", method == M_ADD ? "pair-add" : "pair-remove",
               esp_err_to_name(err));
      tlv8_encode_byte(&enc, TLV_TYPE_ERROR,
                       err == ESP_ERR_NO_MEM ? TLV_ERROR_MAX_PEERS : TLV_ERROR_UNKNOWN);
    }
    out_len = tlv8_encoder_size(&enc);
    hap_pairings_log(method == M_ADD ? "pair-add" : "pair-remove", false);
  } else {
    ESP_LOGW(TAG, "HomeKit %s: unknown method %d", req->path, method);
    if (body && body_len) log_body_hex("pairings body", body, body_len);
    tlv8_encode_byte(&enc, TLV_TYPE_STATE, 2);
    tlv8_encode_byte(&enc, TLV_TYPE_ERROR, TLV_ERROR_UNKNOWN);
    out_len = tlv8_encoder_size(&enc);
  }
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Content-Type: application/octet-stream\r\n", (const char *)out,
                     out_len);
  free(out);
}

/* POST /configure (HomeKit): the Home app tells the speaker how access is
 * controlled. The reply carries the accessory's pairing identifier ("pi")
 * and long-term public key; adding over transient pairing gives the Home app
 * no other way to learn them, and without them the add fails. */
static void handle_configure(int socket, rtsp_conn_t *conn, const rtsp_request_t *req) {
  const uint8_t *body = req->body;
  const size_t body_len = req->body_len;
  if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0)
    log_body_plist("configure", body, body_len, true);
  int64_t acl = 0, hkac = 1;
  char name[65] = "", pw[65] = "";
  (void)bplist_find_any_int(body, body_len, "Access_Control_Level", &acl);
  (void)bplist_find_any_int(body, body_len, "Enable_HK_Access_Control", &hkac);
  const bool got_name = bplist_find_any_string(body, body_len, "Device_Name", name, sizeof(name));
  (void)bplist_find_any_string(body, body_len, "Password", pw, sizeof(pw));
  /* A Device_Name in the request is echoed back; otherwise our own name. */
  if (!got_name) settings_get_device_name(name, sizeof(name));
  ESP_LOGI(TAG, "configure: access control level %lld (0 everyone, 1 home members, 2 admins), "
           "HomeKit access control %s, password %s", (long long)acl, hkac ? "on" : "off",
           pw[0] ? "set" : "none");

  char pi[AIRPLAY_PAIRING_ID_LEN];
  airplay_get_pairing_id(pi, sizeof(pi));
  uint8_t out[384];
  size_t n = bplist_build_configure_response(out, sizeof(out), pi, hkac != 0,
                                             hap_get_public_key(), name, acl, pw);
  if (!n) {
    ESP_LOGE(TAG, "configure: reply build failed");
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL, NULL, 0);
    return;
  }
  log_body_plist("configure reply", out, n, true);
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Content-Type: application/x-apple-binary-plist\r\n", (const char *)out, n);
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
    bool start_encryption = false;

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
          // TLV8 pair-verify M3 establishes RTSP channel encryption, starting
          // with the request after M4: M4 itself goes out in plain text.
          start_encryption = err == ESP_OK &&
              conn->hap_session->pair_verify_state == PAIR_VERIFY_STATE_M4;
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
      if (start_encryption) {
        conn->encrypted_mode = true;
        ESP_LOGI(TAG, "RTSP encryption enabled (TLV8 pair-verify)");
      }
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
      char cmd_type[64];
      if (bplist_find_string(body, body_len, "type", cmd_type, sizeof(cmd_type))) {
        ESP_LOGI(TAG, "Sender /command type=%s", cmd_type);
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

    // For buffered audio streams (type 103), send a proper feedback response
    // with stream status. This acts as a keepalive to prevent iPhone from
    // sending TEARDOWN during extended pause.
    if (conn->stream_type == 103) {
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

  } else if (strstr(req->path, "/configure")) {
    handle_configure(socket, conn, req);

  } else if (strstr(req->path, "/audioMode")) {
    /* Sent before each stream start ({"audioMode": "default"}); nothing to do. */
    char mode[32] = "?";
    (void)bplist_find_any_string(body, body_len, "audioMode", mode, sizeof(mode));
    ESP_LOGI(TAG, "audioMode: %s", mode);
    rtsp_send_ok(socket, conn, req->cseq);

  } else if (strstr(req->path, "/pair-add") || strstr(req->path, "/pair-remove") ||
             strstr(req->path, "/pair-list") || strstr(req->path, "/pairings")) {
    handle_pairings(socket, conn, req);

  } else {
    ESP_LOGW(TAG, "Unhandled POST %s (%u bytes), answered 200 OK", req->path,
             (unsigned)body_len);
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0)
      log_body_plist("POST body", body, body_len, false);
    else if (body && body_len)
      log_body_hex("POST body", body, body_len);
    rtsp_send_ok(socket, conn, req->cseq);
  }
}

/* SETUP on a remote-control-only connection. The session SETUP gets an
 * event port of its own (a listener owned by this connection, not the audio
 * event channel); streams are not offered on such a connection. */
static void handle_setup_remote_control(int socket, rtsp_conn_t *conn,
                                        const rtsp_request_t *req,
                                        bool has_streams) {
  if (has_streams) {
    /* Expected while status bit 11 (relay) is advertised: senders keep asking
     * for the type 130 remote-control channel. Refusing is harmless, so keep
     * it out of the normal log. */
    ESP_LOGD(TAG, "SETUP: stream on a remote-control-only connection refused");
    rtsp_send_response(socket, conn, 455, "Method Not Valid In This State",
                       req->cseq, NULL, NULL, 0);
    return;
  }
  if (conn->rc_event_socket < 0) {
    conn->rc_event_socket = rtsp_create_event_socket(&conn->rc_event_port);
    if (conn->rc_event_socket < 0) {
      conn->rc_event_port = 0;
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL, NULL, 0);
      return;
    }
  }
  uint8_t reply[128];
  const size_t reply_len =
      bplist_build_initial_setup(reply, sizeof(reply), conn->rc_event_port);
  if (!reply_len) {
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL, NULL, 0);
    return;
  }
  ESP_LOGI(TAG, "SETUP: remote control session (audio untouched), event port %u",
           conn->rc_event_port);
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Content-Type: application/x-apple-binary-plist\r\n",
                     (const char *)reply, reply_len);
}

static void handle_setup(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw;
  (void)raw_len;

  audio_format_t proposed_format = {0};
  audio_encrypt_t proposed_encryption = conn->setup_encryption;
  int64_t proposed_type = conn->stream_type;
  uint16_t proposed_control_port = conn->client_control_port;
  uint32_t proposed_latency = conn->setup_latency;
  bool has_format = false;
  bool has_encryption = false;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  /* AirPlay 2 SETUP always carries a binary plist: the session SETUP without
   * streams[] and the stream SETUP with streams[]. Anything else is a classic
   * RAOP (AirPlay 1) request, which this receiver does not support. */
  if (!body || body_len < 8 || memcmp(body, "bplist00", 8) != 0) {
    ESP_LOGW(TAG, "SETUP without binary plist rejected (AirPlay 1 not supported)");
    rtsp_send_response(socket, conn, 461, "Unsupported Transport", req->cseq,
                       NULL, NULL, 0);
    return;
  }

  // Check for streams array
  size_t stream_count = 0;
  const bool request_has_streams =
      bplist_get_streams_count(body, body_len, &stream_count);

  ESP_LOGI(TAG, "SETUP: has_streams=%d, stream_count=%zu", request_has_streams,
           stream_count);
  if (!request_has_streams) {
    /* One line per session SETUP; the whole body at debug level. */
    char sender[48] = "?", model[32] = "?";
    (void)bplist_find_any_string(body, body_len, "name", sender, sizeof(sender));
    (void)bplist_find_any_string(body, body_len, "model", model, sizeof(model));
    ESP_LOGI(TAG, "SETUP: session from \"%s\" (%s)", sender, model);
    log_body_plist("SETUP body", body, body_len, true);
  }

  /* A session SETUP with isRemoteControlOnly (the Home hub managing HomeKit
   * pairings, a remote control) never touches the global audio / PTP /
   * event state: it must not stop a session that is playing. Every session
   * SETUP decides again, so a connection can still become an audio session
   * with a later normal session SETUP. */
  if (!request_has_streams) {
    int64_t rc_flag = 0;
    conn->rc_only = bplist_find_any_int(body, body_len, "isRemoteControlOnly", &rc_flag) &&
                    rc_flag != 0;
  }
  if (conn->rc_only) {
    handle_setup_remote_control(socket, conn, req, request_has_streams);
    return;
  }
  /* Audio session: stop the previous audio owner (another sender) first. */
  if (rtsp_server_claim_audio(conn) != ESP_OK) {
    rtsp_send_response(socket, conn, 503, "Service Unavailable", req->cseq,
                       NULL, NULL, 0);
    return;
  }
  if (!request_has_streams) {
    /* Advertise the sender's group (TXT gid/gcgl), as AirPlay 2 speakers do:
     * iOS and the Home app use it to tie this speaker to the group's
     * now-playing info (artwork, controls). */
    char group[48] = "";
    int64_t leader = 0;
    (void)bplist_find_any_string(body, body_len, "groupUUID", group, sizeof(group));
    (void)bplist_find_any_int(body, body_len, "groupContainsGroupLeader", &leader);
    if (group[0]) mdns_airplay_set_group(group, leader != 0);
  }

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
    int64_t latency_min = 0;
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
        if (kv[k].int_value < 0 || kv[k].int_value > UINT16_MAX) {
          rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL, NULL, 0);
          return;
        }
        proposed_control_port = (uint16_t)kv[k].int_value;
      } else if (strcmp(kv[k].key, "latencyMin") == 0) {
        latency_min = kv[k].int_value;
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

    proposed_type = stream_type;
    proposed_format = format;
    has_format = true;
    proposed_latency = stream_type == AUDIO_STREAM_BUFFERED ? 0 :
        (latency_min > 0 && latency_min <= 5 * 44100 ?
         (uint32_t)latency_min : AIRPLAY_RT_LATENCY_DEFAULT_SAMPLES);
  }

  // Process encryption keys
  {
    uint8_t ekey_encrypted[64];
    size_t ekey_len = 0;
    uint8_t eiv[16];
    size_t eiv_len = 0;
    uint8_t shk[crypto_aead_chacha20poly1305_ietf_KEYBYTES];
    size_t shk_len = 0;

    int64_t crypto_stream_type = proposed_type > 0 ? proposed_type : 96;
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

    if (eiv_len != 0 && eiv_len != 16) {
      rtsp_send_response(socket, conn, 400, "Invalid Audio IV", req->cseq, NULL, NULL, 0);
      return;
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
      proposed_encryption = audio_encrypt;
      has_encryption = true;
      encryption_set = true;
    } else if (shk_len != 0) {
      rtsp_send_response(socket, conn, 400, "Invalid Audio Key", req->cseq, NULL, NULL, 0);
      return;
    } else if (ekey_len > 16 && conn->hap_session &&
               conn->hap_session->session_established) {
      uint8_t nonce[12] = {0};
      /* AEAD may output every ciphertext byte except the authentication tag. */
      uint8_t decrypted_key[sizeof(ekey_encrypted)];
      unsigned long long decrypted_len;

      if (crypto_aead_chacha20poly1305_ietf_decrypt(
              decrypted_key, &decrypted_len, NULL, ekey_encrypted, ekey_len,
              NULL, 0, nonce, conn->hap_session->shared_secret) == 0 &&
          decrypted_len == sizeof(audio_encrypt.key)) {
        audio_encrypt.type = AUDIO_ENCRYPT_CHACHA20_POLY1305;
        memcpy(audio_encrypt.key, decrypted_key, sizeof(audio_encrypt.key));
        audio_encrypt.key_len = sizeof(audio_encrypt.key);
        if (eiv_len >= 16) {
          memcpy(audio_encrypt.iv, eiv, 16);
        }
        proposed_encryption = audio_encrypt;
        has_encryption = true;
        encryption_set = true;
      }
    }

    if (ekey_len && !encryption_set && !has_encryption) {
      rtsp_send_response(socket, conn, 400, "Invalid Audio Key", req->cseq, NULL, NULL, 0);
      return;
    }

    if (!encryption_set && conn->setup_encryption.type != AUDIO_ENCRYPT_NONE) {
      /* Omitted keys retain the committed session key, including retries. */
      if (eiv_len == 16) {
        memcpy(proposed_encryption.iv, eiv, 16);
        has_encryption = true;
      }
    } else if (!encryption_set && conn->hap_session &&
               conn->hap_session->session_established) {
      audio_encrypt.type = AUDIO_ENCRYPT_CHACHA20_POLY1305;
      if (hap_derive_audio_key(conn->hap_session, audio_encrypt.key,
                               sizeof(audio_encrypt.key)) == ESP_OK) {
        audio_encrypt.key_len = 32;
        if (eiv_len >= 16) {
          memcpy(audio_encrypt.iv, eiv, 16);
        }
        proposed_encryption = audio_encrypt;
        has_encryption = true;
      }
    }
  }

  if (request_has_streams && !has_format) {
    rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL, NULL, 0);
    return;
  }

  /* Validate the entire request before publishing format/key. A live stream
   * only accepts the same negotiated configuration and returns its ports. */
  if (conn->stream_active) {
    const audio_format_t *old = &conn->setup_format;
    bool same_format = !has_format || (conn->setup_format_valid &&
        proposed_type == conn->stream_type &&
        strcmp(proposed_format.codec, old->codec) == 0 &&
        proposed_format.sample_rate == old->sample_rate &&
        proposed_format.channels == old->channels &&
        proposed_format.bits_per_sample == old->bits_per_sample &&
        proposed_format.frame_size == old->frame_size &&
        proposed_control_port == conn->client_control_port &&
        proposed_latency == conn->setup_latency);
    const audio_encrypt_t *old_key = &conn->setup_encryption;
    bool same_key = !has_encryption || (
        proposed_encryption.type == old_key->type &&
        proposed_encryption.key_len == old_key->key_len &&
        memcmp(proposed_encryption.key, old_key->key, sizeof(old_key->key)) == 0 &&
        memcmp(proposed_encryption.iv, old_key->iv, sizeof(old_key->iv)) == 0);
    if (!same_format || !same_key) {
      rtsp_send_response(socket, conn, 455, "Method Not Valid In This State",
                         req->cseq, NULL, NULL, 0);
      return;
    }
    uint8_t reply[256];
    size_t reply_len;
    if (request_has_streams) {
      bool buffered = conn->stream_type == AUDIO_STREAM_BUFFERED;
      reply_len = bplist_build_stream_setup(reply, sizeof(reply), conn->stream_type,
          buffered ? conn->buffered_port : conn->data_port, conn->control_port,
          buffered ? (uint32_t)AP2_BUFFERED_AUDIO_ADVERTISED_BYTES : 0U);
    } else {
      reply_len = bplist_build_initial_setup(reply, sizeof(reply), conn->event_port);
    }
    if (!reply_len) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL, NULL, 0);
      return;
    }
    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
        "Content-Type: application/x-apple-binary-plist\r\n", (const char *)reply, reply_len);
    return;
  }
  if ((has_format || has_encryption) && !audio_receiver_is_idle()) {
    rtsp_send_response(socket, conn, 455, "Method Not Valid In This State",
                       req->cseq, NULL, NULL, 0);
    return;
  }
  if (has_format) {
    conn->stream_type = proposed_type;
    conn->client_control_port = proposed_control_port;
    conn->setup_format = proposed_format;
    conn->setup_format_valid = true;
    conn->setup_latency = proposed_latency;
    audio_receiver_set_stream_type((audio_stream_type_t)proposed_type);
    audio_receiver_set_format(&proposed_format);
  }
  if (has_encryption) {
    conn->setup_encryption = proposed_encryption;
    audio_receiver_set_encryption(&proposed_encryption);
  }

  // Create event port if needed
  if (conn->event_port == 0) {
    conn->event_socket = rtsp_create_event_socket(&conn->event_port);
    if (conn->event_socket < 0) {
      conn->event_port = 0;
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL, NULL, 0);
      return;
    }
    if (conn->event_socket >= 0) {
      if (rtsp_start_event_port_task(conn->event_socket, conn) == ESP_OK) {
        ESP_LOGI(TAG, "SETUP: Created event port %u", conn->event_port);
        conn->event_socket = -1; /* Ownership transferred to event task. */
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
   * local presentation-anchor path. Buffered AAC uses the PTP clock snapshot
   * directly. For realtime, the RTSP peer IP (not the D7 clockIdentity)
   * selects the PTP packet source. */
  ptp_clock_set_realtime_mode(!buffered, !buffered ? conn->client_ip : 0);
  /* PTP is a control-session clock, not an audio-stream buffer. Preserve a
   * healthy same-session estimator across stream TEARDOWN/SETUP, exactly as
   * the sender preserves its clock timeline. set_realtime_mode() already
   * resets the estimator when the timing mode/peer really changes; a full
   * RTSP disconnect still clears it in connection cleanup.
   *
   * The FIRST stream of a connection must not inherit what the PTP task
   * collected before this session existed (boot, other devices on the
   * network); that stale state kept the first session after boot silent.
   * Clear exactly once per connection, after SETPEERS and mode selection. */
  if (!conn->ptp_session_fresh) {
    ptp_clock_clear();
    conn->ptp_session_fresh = true;
    ESP_LOGI(TAG, "SETUP: first stream of this connection, PTP estimator reset");
  }

  if (buffered) {
    esp_err_t err = audio_receiver_start_buffered(0);
    if (err != ESP_OK) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }
    conn->buffered_port = audio_receiver_get_buffered_port();
  }

  // Buffered audio (type 103) uses only the TCP dataPort plus PTP; realtime
  // (type 96) needs UDP data and control (retransmit) ports.
  if (!buffered) {
    ensure_realtime_ports(conn);
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
    audio_receiver_set_playout_latency_samples(conn->setup_latency);
    ESP_LOGI(TAG, "Realtime playout latency: %u samples", (unsigned)conn->setup_latency);
  }

  uint8_t plist_body[256];
  uint32_t audio_buffer_size = buffered
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

  // Realtime: enable NACK retransmission requests to the sender's control port
  if (conn->client_control_port > 0 && conn->client_ip != 0) {
    audio_receiver_set_client_control(conn->client_ip,
                                      conn->client_control_port);
  }

  conn->stream_active = true;
  amp_session_activate_once(conn);

  if (stream_type == AUDIO_STREAM_REALTIME) {
    /* Realtime ALAC (type 96) needs the play gate open here: in this
     * transport the presentation mapping can arrive through D7 and the
     * sender may never send SETRATEANCHORTIME rate=1. Buffered AAC
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
   * and remains blocked downstream until D7/PTP timing becomes valid.
   * Output latency is compensated internally, so report 0. */
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Audio-Latency: 0\r\n"
                     "Audio-Jack-Status: connected\r\n",
                     NULL, 0);
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

    if (start > UINT32_MAX || current > UINT32_MAX || end > UINT32_MAX) return;
    uint32_t duration = (uint32_t)end - (uint32_t)start;
    uint32_t position = (uint32_t)current - (uint32_t)start;
    /* RTP arithmetic is modulo 2^32; half-range intervals are ambiguous. */
    if (duration >= UINT32_C(0x80000000) || position > duration) return;
    meta->position_secs = position / sample_rate;
    meta->duration_secs = duration / sample_rate;

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
          char *end = NULL;
          errno = 0;
          float volume = strtof(vol + 7, &end);
          const bool parsed = end != vol + 7 && errno != ERANGE && isfinite(volume);
          while (*end == ' ' || *end == '\t') ++end;
          if (!parsed || (*end != '\0' && *end != '\r' && *end != '\n')) {
            rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq,
                               NULL, NULL, 0);
            return;
          }
          rtsp_conn_set_volume(conn, volume);
          ESP_LOGD(TAG, "Volume %.1f dB%s", volume, volume <= -144.0f ? " (mute)" : "");
        }
      }
      // Progress may also arrive in the text/parameters body
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
    // DMAP-tagged track metadata (title/artist/album/genre)
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
    // Artwork - log and flag in metadata
    ESP_LOGI(TAG, "Received artwork: %s (%zu bytes)", req->content_type,
             body_len);
    event_data.metadata.has_artwork = true;
    has_metadata = true;
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
                             "volume: %.2f\r\n", rtsp_conn_get_volume_db(conn));
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: text/parameters\r\n", vol_response,
                         vol_len);
      return;
    }
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

/* LOUDNESSNORMALIZATION: newer iOS/tvOS senders send this during SETUP with
 * a bplist { loudnessNormalizationEnabled: <bool> }. Accept it with 200 OK
 * like a real speaker instead of 501; the setting is not applied. */
static void handle_loudnessnormalization(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
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
    /* No buffered 23-bit endpoint: stop old media and wait for a real anchor. */
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
    bool got_from_ts =
        bplist_find_int(body, body_len, "flushFromTS", &flush_from_ts);
    bool got_until_seq =
        bplist_find_int(body, body_len, "flushUntilSeq", &flush_until_seq);
    bool got_until_ts =
        bplist_find_int(body, body_len, "flushUntilTS", &flush_until_ts);

    /* Key presence is retained in diagnostics. A deferred zero endpoint may
     * be real sequence wrap; only immediate no-boundary recovery is special. */
    if (got_until_seq && flush_until_seq == 0) {
      ESP_LOGW(TAG,
               "FLUSHBUFFERED explicit untilSeq=0 (%s); "
               "%s",
               got_from_seq ? "deferred" : "immediate",
               got_from_seq
                   ? "deferred request keeps normal sequence semantics"
                   : "consumer will wait for the next anchor, then resync RTP");
    }

    /* Shairport Sync 5.5.2 handle_flushbuffered(): deferred iff flushFromSeq is
     * present (the other fields default to 0 when missing). */
    if (got_from_seq) {
      ESP_LOGI(TAG,
               "FLUSHBUFFERED deferred: fromSeq=%" PRId64 " fromTS=%" PRId64
               " untilSeq=%" PRId64 " untilTS=%" PRId64,
               flush_from_seq, flush_from_ts, flush_until_seq, flush_until_ts);
      esp_err_t flush_err = audio_receiver_set_deferred_flush_range(
          (uint32_t)flush_from_seq, (uint32_t)flush_from_ts,
          (uint32_t)flush_until_seq, (uint32_t)flush_until_ts,
          got_from_ts && got_until_ts);
      if (flush_err != ESP_OK) {
        /* Shairport: "no more room for deferred flush request records" is
         * only logged; the reply is still 200. */
        ESP_LOGW(TAG, "FLUSHBUFFERED deferred not stored (%s)",
                 esp_err_to_name(flush_err));
      }
    } else {
      /* The absence of flushFromSeq selects immediate mode. Non-zero
       * flushUntilSeq follows Shairport sequence semantics. An explicit zero
       * uses the receiver's experimental no-boundary policy: wait for the new
       * anchor, then sequentially classify compressed packets for admission. */
      if (!got_until_seq) {
        ESP_LOGW(TAG,
                 "FLUSHBUFFERED immediate without flushUntilSeq; "
                 "no sequence boundary; waiting for fresh anchor");
      }
      ESP_LOGI(TAG,
               "FLUSHBUFFERED immediate: untilSeq=%" PRId64
               " untilTS=%" PRId64,
               flush_until_seq, flush_until_ts);
      audio_receiver_set_immediate_flush((uint32_t)flush_until_seq,
                                         (uint32_t)flush_until_ts);
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
  if (!conn->owns_audio) {
    /* Remote-control / management connection: nothing of the audio session
     * (this or another connection's) is touched. */
    if (!has_streams) {
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Connection: close\r\n", NULL, 0);
      conn->close_after_response = true;
      return;
    }
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }
  // Stream-level teardown is a pause: close the play gate immediately and
  // emit PAUSED (LED state) before the slower receiver/decoder teardown below
  // (audio_receiver_stop can block ~1 s waiting for the listener task).
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
  /* Client cleanup remains the final reset owner when stop times out. */
  if (!has_streams && audio_receiver_is_idle()) {
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

    // Full teardown — server cleanup will emit RTSP_EVENT_DISCONNECTED
    // when the TCP connection closes.
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
  bool have_rtp_time = false;

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
      have_rtp_time = true;
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
    /* A real RTP key and timeline ID are required to commit a new map.
     * Missing/default RTP must not release zero-seq recovery at timestamp 0. */
    if (have_network_time_secs && have_rtp_time && clock_id != 0U) {
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

  /* The peer tables (~4.6 KB) live on the heap, not on the RTSP client task
   * stack: SETPEERS runs at the deepest point of the request path (dispatch +
   * encrypted response send) and overflowed the 8 KB stack. */
  struct setpeers_work {
    bplist_peer_info_t parsed[PTP_CLOCK_MAX_PEERS];
    ptp_clock_peer_t tracked[PTP_CLOCK_MAX_PEERS];
  } *work = heap_caps_calloc(1, sizeof(*work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!work) work = calloc(1, sizeof(*work));
  if (!work) {
    ESP_LOGE(TAG, "%s: out of memory, peer list kept", req->method);
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }
  bplist_peer_info_t *parsed = work->parsed;
  ptp_clock_peer_t *tracked = work->tracked;

  size_t advertised_count = 0;
  if (!bplist_get_peer_list(body, body_len, extended, parsed,
                            PTP_CLOCK_MAX_PEERS, &advertised_count)) {
    ESP_LOGW(TAG, "%s: invalid peer-list bplist", req->method);
    free(work);
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }

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

  ptp_clock_set_peers(tracked, tracked_count);

  ESP_LOGI(TAG,
           "%s: peers=%zu tracked=%zu ipv4=%zu ipv6=%zu clocks=%zu%s%s",
           req->method, advertised_count, tracked_count, ipv4_count,
           ipv6_count, clock_count,
           advertised_count > PTP_CLOCK_MAX_PEERS ? " truncated" : "",
           invalid_addr_count ? " invalid-address" : "");
  free(work);

  /* Do NOT reset audio timing here. Buffered AAC keeps its existing RTP<->PTP
   * map; realtime ALAC keeps its RTP<->ESP-local anchor. A new admitted PTP
   * source/GM is qualified by the existing handover logic. */

  rtsp_send_ok(socket, conn, req->cseq);
}
