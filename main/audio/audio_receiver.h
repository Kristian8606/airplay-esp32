#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Raw AirPlay 2 buffered-audio FIFO. SETUP advertises the actual allocated
 * capacity, so sender buffering follows the receiver capacity. */
#define AP2_BUFFERED_AUDIO_BUFFER_REQUEST_BYTES (1U * 1024U * 1024U)

/* AirPlay 2 audio receiver: buffered AAC plus realtime ALAC. */
typedef struct {
  char codec[32];
  int sample_rate;
  int channels;
  int bits_per_sample;
  int frame_size; /* observed AP2 AAC path: 1024 PCM frames/AU */
} audio_format_t;

typedef enum {
  AUDIO_ENCRYPT_NONE = 0,
  AUDIO_ENCRYPT_AES_CBC,
  AUDIO_ENCRYPT_CHACHA20_POLY1305
} audio_encrypt_type_t;

typedef struct {
  audio_encrypt_type_t type;
  uint8_t key[32];
  uint8_t iv[16];
  size_t key_len;
} audio_encrypt_t;

typedef struct {
  uint32_t packets_received;
  uint32_t packets_decoded;
  uint32_t packets_dropped;
  uint32_t decrypt_errors;
  uint32_t buffer_underruns;
  uint32_t buffer_overruns;
  uint32_t late_frames;
  uint16_t last_seq;
  uint32_t last_timestamp;
} audio_stats_t;

typedef enum {
  AUDIO_STREAM_NONE = 0,
  AUDIO_STREAM_REALTIME = 96,  /* AP2 realtime UDP ALAC */
  AUDIO_STREAM_BUFFERED = 103  /* AP2 buffered TCP AAC */
} audio_stream_type_t;

esp_err_t audio_receiver_init(void);
void audio_receiver_set_format(const audio_format_t *format);
void audio_receiver_set_encryption(const audio_encrypt_t *encrypt);
void audio_receiver_set_stream_type(audio_stream_type_t type);

esp_err_t audio_receiver_start(uint16_t data_port, uint16_t control_port);
esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port);
esp_err_t audio_receiver_start_buffered(uint16_t tcp_port);
void audio_receiver_stop(void);
void audio_receiver_stop_buffered_only(void);
uint16_t audio_receiver_get_stream_port(void);
uint16_t audio_receiver_get_buffered_port(void);
size_t audio_receiver_get_buffered_audio_buffer_size(void);

void audio_receiver_get_stats(audio_stats_t *stats);

/* Software output volume. Q15: 0=mute, 32768=0 dB/full scale. */
void audio_receiver_set_volume_q15(int32_t volume_q15);
int32_t audio_receiver_get_volume_q15(void);

/* Timeline/generation control. These invalidate old PCM in O(1), no scan. */
void audio_receiver_flush(void);
void audio_receiver_seek_flush(void);
/* AP2 realtime FLUSH with RTP-Info: discard audio older than the sender's
 * RTP boundary while preserving the validated D7/SETRATE RTP<->PTP map. */
void audio_receiver_realtime_flush_to_rtp(uint32_t flush_rtp);
void audio_receiver_realtime_flush_wait_sender_anchor(void);
void audio_receiver_set_deferred_flush_range(uint32_t from_seq, uint32_t from_ts,
                                              uint32_t until_seq, uint32_t until_ts);
void audio_receiver_set_immediate_flush(uint32_t until_seq, uint32_t until_ts,
                                        bool has_endpoint);
void audio_receiver_pause(void);
void audio_receiver_set_playing(bool playing);
bool audio_receiver_is_playing(void);
void audio_receiver_reset_timing(void);

void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                    uint32_t rtp_time);
void audio_receiver_set_client_control(uint32_t client_ip,
                                       uint16_t client_control_port);

/* RTSP compatibility: buffered AP2 uses zero extra playout latency here. */
void audio_receiver_set_playout_latency_samples(uint32_t latency_samples);
uint32_t audio_receiver_get_hardware_latency_us(void);
