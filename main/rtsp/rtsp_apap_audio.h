#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct rtsp_apap_audio rtsp_apap_audio_t;

/* Modern Buffered APAP transport.
 *
 * Owns BufferedAPAP framing, stream-key authentication/decryption and the
 * metadata-aware compressed-frame queue. AAC is decoded on a separate task
 * and PCM is published into audio_receiver's common EQ/PTP/PCM-ring path.
 */
esp_err_t rtsp_apap_audio_start(uint32_t expected_client_ip,
                                const uint8_t *stream_key,
                                size_t stream_key_len,
                                rtsp_apap_audio_t **out_audio,
                                uint16_t *out_port);

void rtsp_apap_audio_stop(rtsp_apap_audio_t **audio);

/* Apply MediaDataControl fshb boundaries. APAP sequence numbers are preferred
 * when present; mediaTime is retained as an independent boundary/fallback and
 * is normalized to the 44.1-kHz sample domain used by the common PCM engine. */
void rtsp_apap_audio_flush(rtsp_apap_audio_t *audio,
                           bool have_from_seq, uint32_t from_seq,
                           bool have_until_seq, uint32_t until_seq,
                           bool have_from_media_time,
                           int64_t from_media_time_value,
                           uint32_t from_media_time_scale,
                           bool have_until_media_time,
                           int64_t until_media_time_value,
                           uint32_t until_media_time_scale);
