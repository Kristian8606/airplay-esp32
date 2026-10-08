#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * Start the AirPlay RTSP server on port 7000
 * Handles initial connection requests from iOS devices
 */
esp_err_t rtsp_server_start(void);

/**
 * Stop the RTSP server
 */
void rtsp_server_stop(void);

/* Stop timeout is not completion. Check before releasing shared resources. */
bool rtsp_server_is_idle(void);


