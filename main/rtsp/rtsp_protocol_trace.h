#pragma once

#include <stddef.h>
#include <stdint.h>

/* Optional protocol-only diagnostics. These helpers never change AirPlay
 * state; they only render sender data already received by RTSP/Event/DataStream
 * paths. Functional MediaRemote state lives in media_remote_state.c. */
void rtsp_protocol_trace_features(void);
void rtsp_protocol_trace_command(const uint8_t *plist, size_t plist_len);
void rtsp_protocol_trace_datastream_payload(const char *label,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            size_t full_payload_len);
