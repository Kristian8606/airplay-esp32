#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Raw AirPlay-2 buffered TCP FIFO. The receiver stores bytes exactly in TCP
 * order; packet framing and FLUSH interpretation happen only in the single
 * sequential AAC consumer, following Shairport Sync's buffered path. */
#define AP2_BUFFERED_AUDIO_BUFFER_REQUEST_BYTES (6U * 1024U * 1024U)

/* AirPlay type-103 capacity advertised to the sender ("audioBufferSize").
 * Equal to the physical raw FIFO, so normal TCP backpressure bounds the
 * sender's preload. */
#define AP2_BUFFERED_AUDIO_ADVERTISED_BYTES AP2_BUFFERED_AUDIO_BUFFER_REQUEST_BYTES

/* AirPlay 2 audio receiver: buffered AAC plus realtime ALAC. */
typedef struct {
  char codec[32];
  int sample_rate;
  int channels;
  int bits_per_sample;
  int frame_size; /* PCM frames per packet: AAC 1024, ALAC 352 */
} audio_format_t;

typedef enum {
  AUDIO_ENCRYPT_NONE = 0,
  AUDIO_ENCRYPT_CHACHA20_POLY1305 = 2
} audio_encrypt_type_t;

typedef struct {
  audio_encrypt_type_t type;
  uint8_t key[32];
  uint8_t iv[16];
  size_t key_len;
} audio_encrypt_t;

typedef enum {
  AUDIO_STREAM_NONE = 0,
  AUDIO_STREAM_REALTIME = 96,  /* AP2 realtime UDP ALAC */
  AUDIO_STREAM_BUFFERED = 103  /* AP2 buffered TCP AAC */
} audio_stream_type_t;

esp_err_t audio_receiver_init(void);
bool audio_receiver_is_initialized(void);
/* Wi-Fi scans need temporary heap headroom. This is a full audio-engine
 * memory release (except for the already-created I2S driver itself): current
 * media is stopped, audio worker tasks exit, all large codec/PCM stores are
 * freed, and audio_receiver_init() may be called afterwards to restore the
 * engine. Call only after the RTSP server has stopped accepting clients. */
esp_err_t audio_receiver_release_for_wifi_scan(void);
void audio_receiver_set_format(const audio_format_t *format);
void audio_receiver_set_encryption(const audio_encrypt_t *encrypt);
void audio_receiver_set_stream_type(audio_stream_type_t type);

esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port);
esp_err_t audio_receiver_start_buffered(uint16_t tcp_port);


/* Buffered APAP uses the receiver's existing PCM/EQ/PTP playout pipeline but
 * supplies already-framed/decrypted AAC externally instead of the legacy raw
 * RTP/TCP byte FIFO. */
esp_err_t audio_receiver_start_external_buffered(void);

/* Publish one decoded APAP PCM access unit using its media timestamp as the
 * PCM-ring address. The buffer is modified in place by the common EQ path.
 * Returns true when PCM was published, false when it was deliberately dropped
 * (late/stale/timeline changed) or the external buffered path is inactive. */
bool audio_receiver_publish_timed_pcm(int64_t media_time_value,
                                      uint32_t media_time_scale,
                                      int16_t *pcm, size_t frames,
                                      int channels);

/* Receiver-chosen APAP anchor. mediaTime is converted into the stream sample
 * domain and then uses the normal buffered RTP<->PTP scheduler unchanged. */
bool audio_receiver_set_media_anchor(uint64_t clock_id, uint64_t network_time_ns,
                                     int64_t media_time_value,
                                     uint32_t media_time_scale);

/* APAP FLUSH/track boundary: invalidate current presentation timing and the
 * finite PCM cache. The actual AAC decoder remains transport-owned so codec
 * history can survive ordinary continuity where appropriate. */
void audio_receiver_external_buffered_flush(void);
void audio_receiver_stop(void);
uint16_t audio_receiver_get_buffered_port(void);

/* Software output volume. Q15: 0=mute, 32768=0 dB/full scale. */
void audio_receiver_set_volume_q15(int32_t volume_q15);

/* Timeline control: invalidate the presentation anchor (old PCM can no
 * longer be scheduled) until the sender publishes a new one. O(1), no scan. */
void audio_receiver_seek_flush(void);
/* AP2 realtime FLUSH with RTP-Info: discard audio older than the sender's
 * RTP boundary while preserving the validated D7/SETRATE RTP<->PTP map. */
void audio_receiver_realtime_flush_to_rtp(uint32_t flush_rtp);
void audio_receiver_realtime_flush_wait_sender_anchor(void);
esp_err_t audio_receiver_set_deferred_flush_range(uint32_t from_seq, uint32_t from_ts,
                                                   uint32_t until_seq, uint32_t until_ts);
/* Immediate FLUSHBUFFERED. until_seq_valid=false (flushUntilSeq 0 or missing)
 * ends the flush by timestamp instead of by sequence number. */
void audio_receiver_set_immediate_flush(uint32_t until_seq, uint32_t until_ts,
                                        bool until_seq_valid);
void audio_receiver_pause(void);
void audio_receiver_set_playing(bool playing);

void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                    uint32_t rtp_time);
typedef struct {
  uint64_t effective_local_ns;
  int64_t rebase_step_ns;
  int64_t rebase_bias_ns;
  bool rebased;
  bool deferred;
} audio_realtime_anchor_result_t;

/* Realtime-only D7/SETRATE anchor already converted into ESP monotonic time.
 *
 * Initial startup accepts the current ready GM immediately. While audio is
 * already running, a new PTP mastership epoch is intentionally kept out of
 * the media phase until it has been stable for the handover settle period.
 * The first accepted anchor from that epoch is then rebased onto the existing
 * RTP<->ESP-local timeline; the resulting media-domain bias is applied to
 * later anchors from the same epoch and retired at 50 us/s. PTP itself is never biased or
 * slewed, and buffered/AAC timing is untouched.
 *
 * Returns true when the anchor was published. False means it was deliberately
 * deferred (typically because a new GM is still settling) or not applicable.
 */
bool audio_receiver_set_realtime_anchor_local(
    uint64_t clock_id, uint32_t gm_epoch, uint32_t mastership_age_ms,
    uint64_t remote_ptp_ns, uint64_t candidate_local_ns, uint32_t rtp_time,
    audio_realtime_anchor_result_t *result);
void audio_receiver_set_client_control(uint32_t client_ip,
                                       uint16_t client_control_port);

/* Extra sender-side playout latency in samples (realtime ALAC: SETUP
 * latencyMin, default 11025). Buffered AAC schedules from the anchor directly
 * and sets 0. */
void audio_receiver_set_playout_latency_samples(uint32_t latency_samples);

/* Output latency after the ESP (DAC/DSP), microseconds. Positive =
 * the chain delays the sound, so the ESP plays that much earlier. Applied
 * live; persist=true also stores it in NVS. Range -100000..150000. */
#include "latency_cal.h"
int32_t audio_receiver_get_output_latency_us(void);
esp_err_t audio_receiver_set_output_latency_us(int32_t us, bool persist);
/* Wired ADC loopback measurement. The caller must first stop RTSP and call
 * audio_receiver_release_for_wifi_scan() (exactly like the Wi-Fi scan) and
 * restore AirPlay afterwards. Blocks ~2-3 s. Does NOT apply the result. */
esp_err_t audio_receiver_measure_output_latency(latency_cal_result_t *res,
                                                bool audible);
