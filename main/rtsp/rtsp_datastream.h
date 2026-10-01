#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hap.h"

typedef struct rtsp_datastream rtsp_datastream_t;

/**
 * Called from the DataStream task for every complete message whose payload
 * was captured in full (type "sync"/"comm", command e.g. "srat", "amsm").
 * Runs before the "rply" is sent.  The payload is only valid during the call.
 * The callback may return a bplist (or other command-specific body) in
 * reply[0..reply_cap) and set *reply_len.  For sync messages it is appended
 * to the encrypted DataStream rply header.
 */
typedef void (*rtsp_datastream_msg_cb)(const char *type, const char *command,
                                       const uint8_t *payload,
                                       size_t payload_len, uint8_t *reply,
                                       size_t reply_cap, size_t *reply_len,
                                       void *user);

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
                                rtsp_datastream_msg_cb on_message,
                                void *on_message_user,
                                rtsp_datastream_t **out_stream,
                                uint16_t *out_port);

/** Stop, close and free a DataStream created by rtsp_datastream_start(). */
void rtsp_datastream_stop(rtsp_datastream_t **stream);
