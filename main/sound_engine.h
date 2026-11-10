#pragma once

/* The audio engine under the names the RTSP server (ap2_core) and
 * the web server (components/web_server) call: this speaker's own audio path,
 * audio/audio_receiver.c. The types are the engine's own. */
#include <stdbool.h>
#include <stdint.h>

#include "audio_receiver.h"
#include "esp_err.h"

typedef audio_format_t sound_format_t;
typedef audio_encrypt_t sound_cipher_t;
typedef audio_stream_type_t sound_stream_t;

#define SOUND_CIPHER_NONE AUDIO_ENCRYPT_NONE
#define SOUND_CIPHER_CHACHA20_POLY1305 AUDIO_ENCRYPT_CHACHA20_POLY1305
#define SOUND_STREAM_NONE AUDIO_STREAM_NONE
#define SOUND_STREAM_REALTIME AUDIO_STREAM_REALTIME
#define SOUND_STREAM_BUFFERED AUDIO_STREAM_BUFFERED

static inline esp_err_t sound_engine_init(void) {
  return audio_receiver_init();
}
static inline bool sound_engine_is_initialized(void) {
  return audio_receiver_is_initialized();
}
static inline bool sound_engine_is_idle(void) {
  return audio_receiver_is_idle();
}
static inline esp_err_t sound_engine_release_for_wifi_scan(void) {
  return audio_receiver_release_for_wifi_scan();
}
static inline void sound_engine_set_format(const sound_format_t *format) {
  audio_receiver_set_format(format);
}
static inline void sound_engine_set_encryption(const sound_cipher_t *cipher) {
  audio_receiver_set_encryption(cipher);
}
static inline void sound_engine_set_stream_type(sound_stream_t type) {
  audio_receiver_set_stream_type(type);
}
static inline esp_err_t sound_engine_start_stream(uint16_t data_port,
                                                  uint16_t control_port,
                                                  uint16_t tcp_port) {
  return audio_receiver_start_stream(data_port, control_port, tcp_port);
}
static inline esp_err_t sound_engine_start_buffered(uint16_t tcp_port) {
  return audio_receiver_start_buffered(tcp_port);
}
static inline void sound_engine_stop(void) { audio_receiver_stop(); }
static inline uint16_t sound_engine_get_buffered_port(void) {
  return audio_receiver_get_buffered_port();
}
static inline void sound_engine_set_volume_q15(int32_t volume_q15) {
  audio_receiver_set_volume_q15(volume_q15);
}
static inline void sound_engine_realtime_flush_to_rtp(uint32_t flush_rtp) {
  audio_receiver_realtime_flush_to_rtp(flush_rtp);
}
static inline esp_err_t sound_engine_set_deferred_flush_range(
    uint32_t from_seq, uint32_t from_ts, uint32_t until_seq,
    uint32_t until_ts, bool timestamps_valid) {
  return audio_receiver_set_deferred_flush_range(from_seq, from_ts, until_seq,
                                                 until_ts, timestamps_valid);
}
static inline void sound_engine_set_immediate_flush(uint32_t until_seq,
                                                    uint32_t until_ts) {
  audio_receiver_set_immediate_flush(until_seq, until_ts);
}
static inline void sound_engine_pause(void) { audio_receiver_pause(); }
static inline void sound_engine_set_playing(bool playing) {
  audio_receiver_set_playing(playing);
}
static inline void sound_engine_set_client_control(uint32_t client_ip,
                                                   uint16_t client_control_port) {
  audio_receiver_set_client_control(client_ip, client_control_port);
}
static inline void sound_engine_set_playout_latency_samples(
    uint32_t latency_samples) {
  audio_receiver_set_playout_latency_samples(latency_samples);
}

/* SETRATEANCHORTIME. Buffered: the engine keeps the anchor in sender time.
 * Realtime: converted once into ESP time with the current clock snapshot and
 * handed to the engine as a local anchor (sound_engine.c). */
void sound_engine_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                  uint32_t rtp_time);
