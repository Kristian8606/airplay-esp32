#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct rtsp_apap_observer rtsp_apap_observer_t;

/* Buffered APAP transport receiver.
 *
 * It owns APTransport framing, the stream-key cryptor and AAC decoding. Decoded
 * PCM is published into audio_receiver's normal EQ/PTP/PCM-ring pipeline.
 */
esp_err_t rtsp_apap_observer_start(uint32_t expected_client_ip,
                                   bool uses_stream_encryption_key,
                                   const uint8_t *stream_key,
                                   size_t stream_key_len,
                                   rtsp_apap_observer_t **out_observer,
                                   uint16_t *out_port);

void rtsp_apap_observer_stop(rtsp_apap_observer_t **observer);

/* Apply the sequence boundaries from MediaDataControl fshb. Deferred ranges
 * keep publishing until fromSeq; immediate ranges drain to untilSeq now. */
void rtsp_apap_observer_flush(rtsp_apap_observer_t *observer,
                              bool have_from_seq, uint32_t from_seq,
                              bool have_until_seq, uint32_t until_seq);
