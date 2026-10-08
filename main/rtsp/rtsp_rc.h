#pragma once

/* AirPlay 2 remote control channels (lab).
 *
 * A sender that sees the HomePod feature bits opens a separate
 * "remote control only" RTSP connection (initial SETUP with
 * isRemoteControlOnly) and asks for a stream of type 130 on it. That stream
 * is a "DataStream": an encrypted TCP connection from the sender carrying
 * MediaRemote (MRP) messages. Every "sync" message must be answered with a
 * "rply" carrying the same sequence number.
 *
 * Framing and key derivation follow pyatv (sender side):
 *   keys: HKDF-SHA512(pair secret, salt "DataStream-Salt<seed>",
 *         info "DataStream-Output-Encryption-Key" = sender -> receiver,
 *         info "DataStream-Input-Encryption-Key"  = receiver -> sender)
 *   transport frames: 2-byte LE length, ChaCha20-Poly1305 (64-bit counter
 *         nonce), the length bytes as AAD
 *   messages: 32-byte big-endian header {size, type[12], command[4],
 *         seqno u64, padding u32} followed by a binary plist payload
 *         {"params": {"data": <MRP protobufs>}} */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "rtsp_conn.h"

/* Event port for a remote-control-only connection: accepted and drained,
 * never used to send events. */
esp_err_t rtsp_rc_open_events(rtsp_conn_t *conn, uint16_t *port);

/* Data port for a type 130 stream. seed is the stream's "seed" value as the
 * low 64 bits of the integer the sender sent. */
esp_err_t rtsp_rc_open_data(rtsp_conn_t *conn, uint64_t seed, uint16_t *port);

/* MediaDataControl (lab): the DataStream stream connection of a type 103
 * audio stream with streamConnectionTypeMediaDataControl. Same transport as
 * the type 130 DataStream; each "sync" message is passed to cb, which may
 * return a reply payload (written to reply, at most cap bytes) that goes back
 * in the "rply". cmd is the 4-character command ("srat", "fshb", "strt",
 * "anch", "magc", ...). cb runs on the remote control task. */
typedef size_t (*rtsp_rc_mdc_cb)(rtsp_conn_t *conn, const char *cmd,
                                 const uint8_t *payload, size_t len,
                                 uint8_t *reply, size_t cap);
/* udp_fd: the stream's UDP controlPort socket (or -1). Its ownership moves
 * to the remote control task, which logs what arrives and closes it together
 * with the MediaDataControl channel (also when this call fails). */
esp_err_t rtsp_rc_open_mdc(rtsp_conn_t *conn, uint64_t seed, rtsp_rc_mdc_cb cb,
                           int udp_fd, uint16_t *port);

/* Close only the MediaDataControl channel (audio stream TEARDOWN). */
void rtsp_rc_close_mdc(rtsp_conn_t *conn);

/* Close only the type 130 DataStream (its stream TEARDOWN). The event channel
 * stays: the sender keeps using it for the whole connection, also after the
 * connection became an audio session. MediaDataControl stays open too. */
void rtsp_rc_close_remote(rtsp_conn_t *conn);

/* Close all remote control channels of this connection (idempotent). */
void rtsp_rc_stop(rtsp_conn_t *conn);
