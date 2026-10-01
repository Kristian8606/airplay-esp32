#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * AirPlay connection/setup model.
 *
 * This module only interprets what transport the sender negotiated in SETUP.
 * It must not touch PTP, FLUSH state, RTP presentation anchors or audio
 * playout.  Those belong to rtsp_timeline_control + the audio receiver.
 */
typedef struct {
  int64_t codec_type;
  int64_t sample_rate;
  int64_t samples_per_frame;

  bool client_control_port_valid;
  uint16_t client_control_port;

  bool supports_dynamic_stream_id;

  bool has_stream_connections;
  bool stream_connection_rtp;
  bool stream_connection_rtcp;
  bool stream_connection_apap;
  bool apap_use_stream_encryption_key;
  bool stream_connection_media_data_control;
  bool media_data_control_seed_valid;
  uint64_t media_data_control_seed;
} rtsp_connection_audio_setup_t;

/* Parse the connection-related fields of one AirPlay 2 audio stream SETUP.
 * Defaults intentionally match the existing receiver behaviour: 44.1 kHz,
 * 1024 samples for buffered type 103 and 352 for realtime type 96. */
bool rtsp_connection_model_parse_audio_setup(
    const uint8_t *plist, size_t plist_len, size_t stream_index,
    int64_t stream_type, rtsp_connection_audio_setup_t *out);
