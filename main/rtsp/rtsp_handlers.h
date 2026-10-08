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

#include "airplay_identity.h"

// Include for audio_format_t and the shared buffered transport capacity.
#include "audio_receiver.h"

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


// Event port task management
// Successful start transfers listening descriptor ownership to the task.
// Stop requests cancellation; is_idle reports actual close/completion.
esp_err_t rtsp_start_event_port_task(int listen_socket, rtsp_conn_t *conn);
void rtsp_stop_event_port_task(void);
bool rtsp_event_port_is_idle(void);
