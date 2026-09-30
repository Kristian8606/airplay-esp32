#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "hap.h"

typedef struct rtsp_datastream rtsp_datastream_t;

/**
 * Start an AirPlay 2 encrypted DataStream listener.
 *
 * The sender connects to the returned TCP port. Keys are derived from the HAP
 * pair-verify shared secret with DataStream-Salt<seed>. The task understands
 * the 32-byte AirPlay DataStream header and acknowledges "sync" messages with
 * an encrypted "rply" carrying the same sequence number.
 */
esp_err_t rtsp_datastream_start(const hap_session_t *session, uint64_t seed,
                                uint32_t expected_client_ip,
                                const char *label,
                                rtsp_datastream_t **out_stream,
                                uint16_t *out_port);

/** Stop, close and free a DataStream created by rtsp_datastream_start(). */
void rtsp_datastream_stop(rtsp_datastream_t **stream);
