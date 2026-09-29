#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#include "rtsp_conn.h"

/**
 * Start the AirPlay RTSP server on port 7000
 * Handles initial connection requests from iOS devices
 */
esp_err_t rtsp_server_start(void);

/**
 * Stop the RTSP server
 */
void rtsp_server_stop(void);

/**
 * Take the play lock (Shairport "principal_conn") for this connection.
 * Called by the connection's own task when it starts to play. If another
 * connection holds the lock, that connection is stopped first and this call
 * waits (bounded) until its global audio/PTP cleanup has finished.
 * @return true when conn now owns the audio engine.
 */
bool rtsp_server_acquire_play_lock(rtsp_conn_t *conn);
