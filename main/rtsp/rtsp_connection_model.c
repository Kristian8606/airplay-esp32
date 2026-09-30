#include "rtsp_connection_model.h"

#include <string.h>

#include "plist.h"

bool rtsp_connection_model_parse_audio_setup(
    const uint8_t *plist, size_t plist_len, size_t stream_index,
    int64_t stream_type, rtsp_connection_audio_setup_t *out) {
  if (!plist || !out) return false;

  memset(out, 0, sizeof(*out));
  out->codec_type = -1;
  out->sample_rate = 44100;
  /* AirPlay 2 stream type 103 is buffered AAC; type 96 is realtime ALAC.
   * Keep this parser independent from the audio engine headers. */
  out->samples_per_frame =
      stream_type == 103 ? 1024 : (stream_type == 96 ? 352 : -1);

  bplist_kv_info_t kv[16];
  size_t kv_count = 0;
  if (!bplist_get_stream_kv_info(plist, plist_len, stream_index, kv,
                                 sizeof(kv) / sizeof(kv[0]), &kv_count)) {
    return false;
  }

  for (size_t k = 0; k < kv_count; ++k) {
    if (strcmp(kv[k].key, "supportsDynamicStreamID") == 0) {
      /* Apple only includes this key when dynamic stream IDs are negotiated;
       * the compact KV summary does not need to decode the boolean payload. */
      out->supports_dynamic_stream_id = true;
    }

    if (kv[k].value_type != BPLIST_VALUE_INT) continue;

    if (strcmp(kv[k].key, "ct") == 0) {
      out->codec_type = kv[k].int_value;
    } else if (strcmp(kv[k].key, "sr") == 0) {
      out->sample_rate = kv[k].int_value;
    } else if (strcmp(kv[k].key, "spf") == 0) {
      out->samples_per_frame = kv[k].int_value;
    } else if (strcmp(kv[k].key, "controlPort") == 0) {
      out->client_control_port_valid = true;
      out->client_control_port = (uint16_t)kv[k].int_value;
    }
  }

  bplist_stream_connection_info_t sc_info;
  if (bplist_get_stream_connection_info(plist, plist_len, stream_index,
                                        &sc_info)) {
    out->has_stream_connections = true;
    out->stream_connection_rtp = sc_info.has_rtp;
    out->stream_connection_rtcp = sc_info.has_rtcp;
    out->stream_connection_media_data_control = sc_info.has_media_data_control;
    out->media_data_control_seed_valid = sc_info.has_media_data_control_seed;
    out->media_data_control_seed = sc_info.media_data_control_seed;
  }

  return true;
}
