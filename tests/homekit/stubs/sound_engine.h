#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define SOUND_STREAM_NONE 0
#define SOUND_STREAM_REALTIME 96
#define SOUND_STREAM_BUFFERED 103
#define SOUND_CIPHER_NONE 0
#define SOUND_CIPHER_CHACHA20_POLY1305 1
#define AP2_BUFFERED_AUDIO_ADVERTISED_BYTES 6291456
typedef int sound_stream_t;
typedef struct{char codec[16];int sample_rate,channels,bits_per_sample,frame_size;} sound_format_t;
typedef struct{int type;uint8_t key[32];unsigned key_len;} sound_cipher_t;
extern int audio_calls, playing_starts, buffered_starts, stop_calls;
extern bool engine_playing;
#define AUDIO_STUB(name,args) static inline void name args { audio_calls++; }
AUDIO_STUB(sound_engine_set_volume_q15,(int32_t v))
AUDIO_STUB(sound_engine_set_encryption,(const sound_cipher_t *c))
AUDIO_STUB(sound_engine_set_format,(const sound_format_t *f))
AUDIO_STUB(sound_engine_set_stream_type,(sound_stream_t t))
static inline void sound_engine_set_playing(bool p){audio_calls++;engine_playing=p;if(p)playing_starts++;}
AUDIO_STUB(sound_engine_set_client_control,(uint32_t i,uint16_t p))
AUDIO_STUB(sound_engine_set_playout_latency_samples,(uint32_t s))
AUDIO_STUB(sound_engine_realtime_flush_to_rtp,(uint32_t t))
AUDIO_STUB(sound_engine_set_immediate_flush,(uint32_t s,uint32_t t))
static inline void sound_engine_pause(void){audio_calls++;engine_playing=false;}
static inline void sound_engine_stop(void){audio_calls++;stop_calls++;engine_playing=false;}
AUDIO_STUB(sound_engine_set_anchor_time,(uint64_t c,uint64_t n,uint32_t t))
static inline esp_err_t sound_engine_start_stream(uint16_t a,uint16_t b,uint16_t c){audio_calls++;return ESP_OK;}
static inline esp_err_t sound_engine_start_buffered(uint16_t p){audio_calls++;buffered_starts++;return ESP_OK;}
static inline uint16_t sound_engine_get_buffered_port(void){return 9001;}
static inline esp_err_t sound_engine_set_deferred_flush_range(uint32_t a,uint32_t b,uint32_t c,uint32_t d,bool e){audio_calls++;return ESP_OK;}
