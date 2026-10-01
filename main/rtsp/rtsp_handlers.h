#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "rtsp_conn.h"
#include "rtsp_message.h"

/**
 * RTSP Method Handler Dispatch Table
 * Inspired by shairport-sync's method_handlers pattern
 */

// AirPlay feature flags (AIRPLAY_FEATURES*), see airplay_features.h.
#include "airplay_features.h"

// Model identifier in mDNS ("model", "am"), /info and updateInfo:
// AudioAccessory5,1 = HomePod mini.
#define AIRPLAY_MODEL "AudioAccessory5,1"

// Include for audio_format_t and the shared buffered transport capacity.
#include "audio_receiver.h"


/**
 * Codec registry entry
 */
typedef struct {
  const char *name; // Codec name: "ALAC", "AAC", "AAC-ELD", "OPUS"
  int64_t type_id;  // bplist "ct" value (2=ALAC, 4=AAC, 8=AAC-ELD, 64=OPUS)
} rtsp_codec_t;

/**
 * Configure audio format from codec type ID
 * Looks up the codec in the registry and fills fmt (2 ch, 16 bit, given
 * sample rate and samples per frame).
 * @param type_id Codec type from bplist "ct" field
 * @param fmt Audio format struct to configure
 * @param sample_rate Sample rate from bplist
 * @param samples_per_frame Samples per frame from bplist
 * @return true if codec found and configured, false otherwise
 */
bool rtsp_codec_configure(int64_t type_id, audio_format_t *fmt,
                          int64_t sample_rate, int64_t samples_per_frame);

/**
 * Handler function type
 */
typedef void (*rtsp_handler_fn)(int socket, rtsp_conn_t *conn,
                                const rtsp_request_t *req,
                                const uint8_t *raw_request, size_t raw_len);

/**
 * Method handler entry
 */
typedef struct {
  const char *method;
  rtsp_handler_fn handler;
} rtsp_method_handler_t;

/**
 * Dispatch RTSP request to appropriate handler
 * @param socket Client socket
 * @param conn Connection state
 * @param raw_request Raw request data
 * @param raw_len Length of raw request
 * @return 0 on success, -1 on error
 */
int rtsp_dispatch(int socket, rtsp_conn_t *conn, const uint8_t *raw_request,
                  size_t raw_len);

/**
 * Create the handler mutex. Call once before the RTSP server accepts
 * connections (rtsp_server_start() does).
 */
esp_err_t rtsp_handlers_init(void);

/**
 * Serialise work on the shared RTSP/audio state across client tasks.
 * rtsp_dispatch() takes it around every handler; rtsp_server takes it around
 * the play owner's disconnect cleanup. Never acquire the play lock while
 * holding it.
 */
void rtsp_handlers_lock(void);
void rtsp_handlers_unlock(void);

/**
 * Get device ID string (MAC address format)
 * @param device_id Output buffer (at least 18 bytes)
 * @param len Buffer size
 */
void rtsp_get_device_id(char *device_id, size_t len);

// Event port task management
// session: the play owner's HAP session (event channel keys); may be NULL.
esp_err_t rtsp_start_event_port_task(int listen_socket,
                                     const hap_session_t *session,
                                     uint32_t client_ip,
                                     uint16_t client_rtsp_port);
void rtsp_stop_event_port_task(void);
