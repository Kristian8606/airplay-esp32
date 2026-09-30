#include <string.h>

#include "audio_receiver.h"
#include "plist.h"
#include "airplay_version.h"

static bool bplist_has_room(size_t pos, size_t need, size_t capacity) {
  return pos <= capacity && need <= capacity - pos;
}

static bool bplist_write_u64(uint8_t *out, size_t capacity, size_t *pos,
                             uint64_t value) {
  if (!bplist_has_room(*pos, 8, capacity)) {
    return false;
  }
  for (int i = 7; i >= 0; i--) {
    out[(*pos)++] = (uint8_t)(value >> (i * 8));
  }
  return true;
}

static bool bplist_write_length(uint8_t *out, size_t capacity, size_t *pos,
                                uint8_t marker_base, size_t length) {
  if (length < 15) {
    if (!bplist_has_room(*pos, 1, capacity)) {
      return false;
    }
    out[(*pos)++] = marker_base | (uint8_t)length;
    return true;
  }
  if (length <= UINT8_MAX) {
    if (!bplist_has_room(*pos, 3, capacity)) {
      return false;
    }
    out[(*pos)++] = marker_base | 0x0F;
    out[(*pos)++] = 0x10;
    out[(*pos)++] = (uint8_t)length;
    return true;
  }
  if (length > UINT16_MAX || !bplist_has_room(*pos, 4, capacity)) {
    return false;
  }
  out[(*pos)++] = marker_base | 0x0F;
  out[(*pos)++] = 0x11;
  out[(*pos)++] = (uint8_t)(length >> 8);
  out[(*pos)++] = (uint8_t)length;
  return true;
}

static bool bplist_write_ascii_string(uint8_t *out, size_t capacity,
                                      size_t *pos, const char *value) {
  size_t len = strlen(value);
  if (!bplist_write_length(out, capacity, pos, 0x50, len) ||
      !bplist_has_room(*pos, len, capacity)) {
    return false;
  }
  memcpy(out + *pos, value, len);
  *pos += len;
  return true;
}

static bool bplist_write_data(uint8_t *out, size_t capacity, size_t *pos,
                              const uint8_t *data, size_t len) {
  if (!bplist_write_length(out, capacity, pos, 0x40, len) ||
      !bplist_has_room(*pos, len, capacity)) {
    return false;
  }
  memcpy(out + *pos, data, len);
  *pos += len;
  return true;
}

static bool bplist_write_int(uint8_t *out, size_t capacity, size_t *pos,
                             uint64_t value) {
  uint8_t marker;
  size_t bytes;
  if (value <= UINT8_MAX) {
    marker = 0x10;
    bytes = 1;
  } else if (value <= UINT16_MAX) {
    marker = 0x11;
    bytes = 2;
  } else if (value <= UINT32_MAX) {
    marker = 0x12;
    bytes = 4;
  } else {
    marker = 0x13;
    bytes = 8;
  }

  if (!bplist_has_room(*pos, 1 + bytes, capacity)) {
    return false;
  }
  out[(*pos)++] = marker;
  for (int i = (int)bytes - 1; i >= 0; i--) {
    out[(*pos)++] = (uint8_t)(value >> (i * 8));
  }
  return true;
}

static bool bplist_write_refs(uint8_t *out, size_t capacity, size_t *pos,
                              const uint8_t *refs, size_t count) {
  if (!bplist_has_room(*pos, count, capacity)) {
    return false;
  }
  memcpy(out + *pos, refs, count);
  *pos += count;
  return true;
}

static bool bplist_write_array(uint8_t *out, size_t capacity, size_t *pos,
                               const uint8_t *refs, size_t count) {
  return bplist_write_length(out, capacity, pos, 0xA0, count) &&
         bplist_write_refs(out, capacity, pos, refs, count);
}

static bool bplist_write_dict(uint8_t *out, size_t capacity, size_t *pos,
                              const uint8_t *keys, const uint8_t *values,
                              size_t count) {
  return bplist_write_length(out, capacity, pos, 0xD0, count) &&
         bplist_write_refs(out, capacity, pos, keys, count) &&
         bplist_write_refs(out, capacity, pos, values, count);
}

static bool bplist_finish(uint8_t *out, size_t capacity, size_t *pos,
                          const size_t *offsets, size_t object_count,
                          size_t top_object) {
  size_t offset_table_offset = *pos;
  for (size_t i = 0; i < object_count; i++) {
    if (offsets[i] > UINT16_MAX || !bplist_has_room(*pos, 2, capacity)) {
      return false;
    }
    out[(*pos)++] = (uint8_t)(offsets[i] >> 8);
    out[(*pos)++] = (uint8_t)offsets[i];
  }

  if (!bplist_has_room(*pos, 32, capacity)) {
    return false;
  }
  memset(out + *pos, 0, 6);
  *pos += 6;
  out[(*pos)++] = 2; // offset size
  out[(*pos)++] = 1; // object ref size

  if (!bplist_write_u64(out, capacity, pos, object_count) ||
      !bplist_write_u64(out, capacity, pos, top_object) ||
      !bplist_write_u64(out, capacity, pos, offset_table_offset)) {
    return false;
  }
  return true;
}

size_t bplist_build_initial_setup(uint8_t *out, size_t capacity,
                                  uint16_t event_port) {
  if (capacity < 100) {
    return 0;
  }

  size_t pos = 0;
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  size_t offsets[10];
  size_t obj = 0;

  offsets[obj++] = pos;
  out[pos++] = 0x59;
  memcpy(out + pos, "eventPort", 9);
  pos += 9;

  offsets[obj++] = pos;
  out[pos++] = 0x5A;
  memcpy(out + pos, "timingPort", 10);
  pos += 10;

  offsets[obj++] = pos;
  out[pos++] = 0x11;
  out[pos++] = (event_port >> 8) & 0xFF;
  out[pos++] = event_port & 0xFF;

  offsets[obj++] = pos;
  out[pos++] = 0x10;
  out[pos++] = 0;

  offsets[obj++] = pos;
  out[pos++] = 0xD2;
  out[pos++] = 0;
  out[pos++] = 1;
  out[pos++] = 2;
  out[pos++] = 3;

  size_t offset_table_offset = pos;
  for (size_t i = 0; i < obj; i++) {
    if (offsets[i] > 0xFF) {
      return 0;
    }
    out[pos++] = (uint8_t)offsets[i];
  }

  memset(out + pos, 0, 6);
  pos += 6;
  out[pos++] = 1;
  out[pos++] = 1;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)obj;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = 4;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)offset_table_offset;

  return pos;
}

size_t bplist_build_stream_setup(uint8_t *out, size_t capacity,
                                 int64_t stream_type, uint16_t data_port,
                                 uint16_t control_port,
                                 uint32_t audio_buffer_size,
                                 uint32_t stream_id, bool include_stream_id,
                                 bool stream_connection_rtp,
                                 bool stream_connection_rtcp,
                                 bool stream_connection_mdc,
                                 uint16_t media_data_control_port,
                                 uint64_t media_data_control_seed) {
  if (!out || capacity < 256 || stream_type < 0 || stream_type > UINT8_MAX) {
    return 0;
  }

  /* Bit 59 (SupportsAudioStreamConnectionSetup) changes the SETUP contract:
   * the sender may include streamConnections and supportsDynamicStreamID.  In
   * that path it expects the receiver to return a streamID and to mirror each
   * requested RTP/RTCP connection with streamConnectionKeyPort.  Build the
   * response as a small generic binary plist rather than relying on the old
   * fixed object numbers. */
  size_t pos = 0;
  if (!bplist_has_room(pos, 8, capacity)) return 0;
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  size_t offsets[32];
  size_t obj = 0;

#define ADD_STRING(var, text)                                                   \
  uint8_t var = (uint8_t)obj;                                                   \
  do {                                                                          \
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;                  \
    offsets[obj++] = pos;                                                       \
    if (!bplist_write_ascii_string(out, capacity, &pos, (text))) return 0;      \
  } while (0)
#define ADD_INT(var, value)                                                     \
  uint8_t var = (uint8_t)obj;                                                   \
  do {                                                                          \
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;                  \
    offsets[obj++] = pos;                                                       \
    if (!bplist_write_int(out, capacity, &pos, (uint64_t)(value))) return 0;    \
  } while (0)

  ADD_STRING(k_streams, "streams");
  ADD_STRING(k_type, "type");
  ADD_STRING(k_data_port, "dataPort");
  ADD_STRING(k_control_port, "controlPort");

  uint8_t k_buffer_size = 0;
  const bool buffered = stream_type == AUDIO_STREAM_BUFFERED;
  if (buffered) {
    ADD_STRING(tmp_k_buffer_size, "audioBufferSize");
    k_buffer_size = tmp_k_buffer_size;
  }

  uint8_t k_stream_id = 0;
  if (include_stream_id) {
    ADD_STRING(tmp_k_stream_id, "streamID");
    k_stream_id = tmp_k_stream_id;
  }

  const bool include_stream_connections =
      stream_connection_rtp || stream_connection_rtcp || stream_connection_mdc;
  uint8_t k_stream_connections = 0;
  uint8_t k_sc_rtp = 0;
  uint8_t k_sc_rtcp = 0;
  uint8_t k_sc_mdc = 0;
  uint8_t k_sc_port = 0;
  uint8_t k_sc_seed = 0;
  if (include_stream_connections) {
    ADD_STRING(tmp_k_sc, "streamConnections");
    k_stream_connections = tmp_k_sc;
    if (stream_connection_rtp) {
      ADD_STRING(tmp_k_sc_rtp, "streamConnectionTypeRTP");
      k_sc_rtp = tmp_k_sc_rtp;
    }
    if (stream_connection_rtcp) {
      ADD_STRING(tmp_k_sc_rtcp, "streamConnectionTypeRTCP");
      k_sc_rtcp = tmp_k_sc_rtcp;
    }
    if (stream_connection_mdc) {
      ADD_STRING(tmp_k_sc_mdc, "streamConnectionTypeMediaDataControl");
      k_sc_mdc = tmp_k_sc_mdc;
    }
    ADD_STRING(tmp_k_sc_port, "streamConnectionKeyPort");
    k_sc_port = tmp_k_sc_port;
    if (stream_connection_mdc) {
      ADD_STRING(tmp_k_sc_seed, "streamConnectionKeyEncryptionSeed");
      k_sc_seed = tmp_k_sc_seed;
    }
  }

  ADD_INT(v_type, (uint8_t)stream_type);
  ADD_INT(v_data_port, data_port);
  ADD_INT(v_control_port, control_port);

  uint8_t v_buffer_size = 0;
  if (buffered) {
    ADD_INT(tmp_v_buffer_size, audio_buffer_size);
    v_buffer_size = tmp_v_buffer_size;
  }

  uint8_t v_stream_id = 0;
  if (include_stream_id) {
    ADD_INT(tmp_v_stream_id, stream_id);
    v_stream_id = tmp_v_stream_id;
  }

  uint8_t d_sc_rtp = 0;
  if (stream_connection_rtp) {
    ADD_INT(v_sc_rtp_port, data_port);
    d_sc_rtp = (uint8_t)obj;
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;
    offsets[obj++] = pos;
    const uint8_t keys[] = {k_sc_port};
    const uint8_t vals[] = {v_sc_rtp_port};
    if (!bplist_write_dict(out, capacity, &pos, keys, vals, 1)) return 0;
  }

  uint8_t d_sc_rtcp = 0;
  if (stream_connection_rtcp) {
    ADD_INT(v_sc_rtcp_port, control_port);
    d_sc_rtcp = (uint8_t)obj;
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;
    offsets[obj++] = pos;
    const uint8_t keys[] = {k_sc_port};
    const uint8_t vals[] = {v_sc_rtcp_port};
    if (!bplist_write_dict(out, capacity, &pos, keys, vals, 1)) return 0;
  }

  uint8_t d_sc_mdc = 0;
  if (stream_connection_mdc) {
    ADD_INT(v_sc_mdc_port, media_data_control_port);
    ADD_INT(v_sc_mdc_seed, media_data_control_seed);
    d_sc_mdc = (uint8_t)obj;
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;
    offsets[obj++] = pos;
    const uint8_t keys[] = {k_sc_port, k_sc_seed};
    const uint8_t vals[] = {v_sc_mdc_port, v_sc_mdc_seed};
    if (!bplist_write_dict(out, capacity, &pos, keys, vals, 2)) return 0;
  }

  uint8_t d_stream_connections = 0;
  if (include_stream_connections) {
    uint8_t keys[3];
    uint8_t vals[3];
    size_t count = 0;
    if (stream_connection_rtp) {
      keys[count] = k_sc_rtp;
      vals[count] = d_sc_rtp;
      count++;
    }
    if (stream_connection_rtcp) {
      keys[count] = k_sc_rtcp;
      vals[count] = d_sc_rtcp;
      count++;
    }
    if (stream_connection_mdc) {
      keys[count] = k_sc_mdc;
      vals[count] = d_sc_mdc;
      count++;
    }
    d_stream_connections = (uint8_t)obj;
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;
    offsets[obj++] = pos;
    if (!bplist_write_dict(out, capacity, &pos, keys, vals, count)) return 0;
  }

  uint8_t stream_keys[7];
  uint8_t stream_vals[7];
  size_t stream_count = 0;
  stream_keys[stream_count] = k_type;
  stream_vals[stream_count++] = v_type;
  stream_keys[stream_count] = k_data_port;
  stream_vals[stream_count++] = v_data_port;
  stream_keys[stream_count] = k_control_port;
  stream_vals[stream_count++] = v_control_port;
  if (buffered) {
    stream_keys[stream_count] = k_buffer_size;
    stream_vals[stream_count++] = v_buffer_size;
  }
  if (include_stream_id) {
    stream_keys[stream_count] = k_stream_id;
    stream_vals[stream_count++] = v_stream_id;
  }
  if (include_stream_connections) {
    stream_keys[stream_count] = k_stream_connections;
    stream_vals[stream_count++] = d_stream_connections;
  }

  const uint8_t d_stream = (uint8_t)obj;
  if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;
  offsets[obj++] = pos;
  if (!bplist_write_dict(out, capacity, &pos, stream_keys, stream_vals,
                         stream_count)) {
    return 0;
  }

  const uint8_t a_streams = (uint8_t)obj;
  if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;
  offsets[obj++] = pos;
  const uint8_t stream_refs[] = {d_stream};
  if (!bplist_write_array(out, capacity, &pos, stream_refs, 1)) return 0;

  const uint8_t top = (uint8_t)obj;
  if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;
  offsets[obj++] = pos;
  const uint8_t top_keys[] = {k_streams};
  const uint8_t top_vals[] = {a_streams};
  if (!bplist_write_dict(out, capacity, &pos, top_keys, top_vals, 1)) return 0;

#undef ADD_STRING
#undef ADD_INT

  if (!bplist_finish(out, capacity, &pos, offsets, obj, top)) return 0;
  return pos;
}

size_t bplist_build_datastream_setup(uint8_t *out, size_t capacity,
                                     uint16_t data_port, uint32_t stream_id,
                                     bool include_data_port) {
  if (!out || capacity < 160) return 0;

  size_t pos = 0;
  memcpy(out + pos, "bplist00", 8);
  pos += 8;
  size_t offsets[12];
  size_t obj = 0;

#define DS_ADD_STRING(var, text)                                               \
  uint8_t var = (uint8_t)obj;                                                  \
  do {                                                                         \
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;                 \
    offsets[obj++] = pos;                                                      \
    if (!bplist_write_ascii_string(out, capacity, &pos, (text))) return 0;     \
  } while (0)
#define DS_ADD_INT(var, value)                                                  \
  uint8_t var = (uint8_t)obj;                                                   \
  do {                                                                          \
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) return 0;                  \
    offsets[obj++] = pos;                                                       \
    if (!bplist_write_int(out, capacity, &pos, (uint64_t)(value))) return 0;   \
  } while (0)

  DS_ADD_STRING(k_streams, "streams");
  DS_ADD_STRING(k_type, "type");
  DS_ADD_STRING(k_stream_id, "streamID");
  uint8_t k_data_port = 0;
  if (include_data_port) {
    DS_ADD_STRING(tmp_k_data_port, "dataPort");
    k_data_port = tmp_k_data_port;
  }
  DS_ADD_INT(v_type, 130);
  DS_ADD_INT(v_stream_id, stream_id);
  uint8_t v_data_port = 0;
  if (include_data_port) {
    DS_ADD_INT(tmp_v_data_port, data_port);
    v_data_port = tmp_v_data_port;
  }

  uint8_t stream_keys[3];
  uint8_t stream_vals[3];
  size_t stream_count = 0;
  stream_keys[stream_count] = k_type;
  stream_vals[stream_count++] = v_type;
  stream_keys[stream_count] = k_stream_id;
  stream_vals[stream_count++] = v_stream_id;
  if (include_data_port) {
    stream_keys[stream_count] = k_data_port;
    stream_vals[stream_count++] = v_data_port;
  }

  const uint8_t d_stream = (uint8_t)obj;
  offsets[obj++] = pos;
  if (!bplist_write_dict(out, capacity, &pos, stream_keys, stream_vals,
                         stream_count)) return 0;
  const uint8_t a_streams = (uint8_t)obj;
  offsets[obj++] = pos;
  const uint8_t refs[] = {d_stream};
  if (!bplist_write_array(out, capacity, &pos, refs, 1)) return 0;
  const uint8_t top = (uint8_t)obj;
  offsets[obj++] = pos;
  const uint8_t top_keys[] = {k_streams};
  const uint8_t top_vals[] = {a_streams};
  if (!bplist_write_dict(out, capacity, &pos, top_keys, top_vals, 1)) return 0;

#undef DS_ADD_STRING
#undef DS_ADD_INT

  if (!bplist_finish(out, capacity, &pos, offsets, obj, top)) return 0;
  return pos;
}

size_t bplist_build_feedback_response(uint8_t *out, size_t capacity,
                                      int64_t stream_type, double sample_rate) {
  // Feedback response for buffered audio streams (type 103)
  // Format: { streams: [ { type: 103, sr: 44100.0 } ] }
  // This acts as a keepalive mechanism to prevent iPhone from
  // sending TEARDOWN during extended pause
  if (capacity < 100) {
    return 0;
  }

  size_t pos = 0;
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  size_t offsets[10];
  size_t obj = 0;

  // Object 0: "streams" key string
  offsets[obj++] = pos;
  out[pos++] = 0x57; // String, length 7
  memcpy(out + pos, "streams", 7);
  pos += 7;

  // Object 1: "type" key string
  offsets[obj++] = pos;
  out[pos++] = 0x54; // String, length 4
  memcpy(out + pos, "type", 4);
  pos += 4;

  // Object 2: "sr" key string (sample rate)
  offsets[obj++] = pos;
  out[pos++] = 0x52; // String, length 2
  memcpy(out + pos, "sr", 2);
  pos += 2;

  // Object 3: type value (103 for buffered audio)
  offsets[obj++] = pos;
  out[pos++] = 0x10; // Int, 1 byte
  out[pos++] = (uint8_t)stream_type;

  // Object 4: sample rate value as double
  // IEEE 754 double-precision big-endian
  offsets[obj++] = pos;
  out[pos++] = 0x23; // Real, 8 bytes (double)
  union {
    double d;
    uint8_t bytes[8];
  } sr;
  sr.d = sample_rate;
  // Convert to big-endian
  for (int i = 7; i >= 0; i--) {
    out[pos++] = sr.bytes[i];
  }

  // Object 5: stream dict { type: 3, sr: 4 }
  offsets[obj++] = pos;
  out[pos++] = 0xD2; // Dict, 2 key-value pairs
  out[pos++] = 1;    // Key: object 1 (type)
  out[pos++] = 2;    // Key: object 2 (sr)
  out[pos++] = 3;    // Value: object 3 (type value)
  out[pos++] = 4;    // Value: object 4 (sr value)

  // Object 6: streams array [ object 5 ]
  offsets[obj++] = pos;
  out[pos++] = 0xA1; // Array, 1 element
  out[pos++] = 5;    // Contains object 5 (stream dict)

  // Object 7: top-level dict { streams: 6 }
  offsets[obj++] = pos;
  out[pos++] = 0xD1; // Dict, 1 key-value pair
  out[pos++] = 0;    // Key: object 0 (streams)
  out[pos++] = 6;    // Value: object 6 (streams array)

  // Offset table
  size_t offset_table_offset = pos;
  for (size_t i = 0; i < obj; i++) {
    if (offsets[i] > 0xFF) {
      return 0;
    }
    out[pos++] = (uint8_t)offsets[i];
  }

  // Trailer: 6 unused bytes, then metadata
  memset(out + pos, 0, 6);
  pos += 6;
  out[pos++] = 1; // Offset size
  out[pos++] = 1; // Object ref size

  // Number of objects (8 bytes big-endian)
  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)obj;

  // Top object index (8 bytes big-endian)
  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = 7; // Top object is object 7

  // Offset table offset (8 bytes big-endian)
  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)offset_table_offset;

  return pos;
}

/* Object indices shared by the /info body and the updateInfo event. */
#define INFO_OBJ_TYPE_KEY 21 /* "type" */
#define INFO_OBJ_COUNT 39    /* objects 0..38, 38 = info dict */
#define INFO_MAX_OBJECTS (INFO_OBJ_COUNT + 5)

/* Writes the /info objects starting at object 0. With txt != NULL the info
 * dict also carries "txtAirPlay" (Shairport generateInfoPlist() +
 * generateTxtDataValueInfo()). On success *info_dict is the index of the info
 * dict. The caller writes the header before and the trailer after. */
static bool bplist_write_info_objects(uint8_t *out, size_t capacity,
                                      size_t *pos_io, size_t *offsets,
                                      size_t *obj_io, const char *device_id,
                                      const char *device_name, const char *model,
                                      const uint8_t *public_key,
                                      size_t public_key_len, uint64_t features,
                                      int64_t protocol_version,
                                      const uint8_t *txt, size_t txt_len,
                                      size_t *info_dict) {
  size_t pos = *pos_io;
  size_t obj = *obj_io;

#define ADD_OFFSET()                    \
  do {                                  \
    if (obj >= INFO_MAX_OBJECTS) {      \
      return false;                     \
    }                                   \
    offsets[obj++] = pos;               \
  } while (0)

  ADD_OFFSET(); // 0: "deviceid"
  if (!bplist_write_ascii_string(out, capacity, &pos, "deviceid")) {
    return false;
  }
  ADD_OFFSET(); // 1: device id
  if (!bplist_write_ascii_string(out, capacity, &pos, device_id)) {
    return false;
  }
  ADD_OFFSET(); // 2: "features"
  if (!bplist_write_ascii_string(out, capacity, &pos, "features")) {
    return false;
  }
  ADD_OFFSET(); // 3: features
  if (!bplist_write_int(out, capacity, &pos, features)) {
    return false;
  }
  ADD_OFFSET(); // 4: "model"
  if (!bplist_write_ascii_string(out, capacity, &pos, "model")) {
    return false;
  }
  ADD_OFFSET(); // 5: model
  if (!bplist_write_ascii_string(out, capacity, &pos, model)) {
    return false;
  }
  ADD_OFFSET(); // 6: "protovers"
  if (!bplist_write_ascii_string(out, capacity, &pos, "protovers")) {
    return false;
  }
  ADD_OFFSET(); // 7: protocol version string
  if (!bplist_write_ascii_string(out, capacity, &pos, "1.1")) {
    return false;
  }
  ADD_OFFSET(); // 8: "srcvers"
  if (!bplist_write_ascii_string(out, capacity, &pos, "srcvers")) {
    return false;
  }
  ADD_OFFSET(); // 9: source version string
  if (!bplist_write_ascii_string(out, capacity, &pos, AIRPLAY_SOURCE_VERSION)) {
    return false;
  }
  ADD_OFFSET(); // 10: "vv"
  if (!bplist_write_ascii_string(out, capacity, &pos, "vv")) {
    return false;
  }
  ADD_OFFSET(); // 11: vv value
  if (!bplist_write_int(out, capacity, &pos, (uint64_t)protocol_version)) {
    return false;
  }
  ADD_OFFSET(); // 12: "statusFlags"
  if (!bplist_write_ascii_string(out, capacity, &pos, "statusFlags")) {
    return false;
  }
  ADD_OFFSET(); // 13: statusFlags value
  if (!bplist_write_int(out, capacity, &pos, 4)) {
    return false;
  }
  ADD_OFFSET(); // 14: "pk"
  if (!bplist_write_ascii_string(out, capacity, &pos, "pk")) {
    return false;
  }
  ADD_OFFSET(); // 15: public key
  if (!bplist_write_data(out, capacity, &pos, public_key, public_key_len)) {
    return false;
  }
  ADD_OFFSET(); // 16: "pi"
  if (!bplist_write_ascii_string(out, capacity, &pos, "pi")) {
    return false;
  }
  ADD_OFFSET(); // 17: pairing identifier
  if (!bplist_write_ascii_string(out, capacity, &pos,
                                 "00000000-0000-0000-0000-000000000000")) {
    return false;
  }
  ADD_OFFSET(); // 18: "name"
  if (!bplist_write_ascii_string(out, capacity, &pos, "name")) {
    return false;
  }
  ADD_OFFSET(); // 19: device name
  if (!bplist_write_ascii_string(out, capacity, &pos, device_name)) {
    return false;
  }
  ADD_OFFSET(); // 20: "audioFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioFormats")) {
    return false;
  }
  ADD_OFFSET(); // 21: "type"
  if (!bplist_write_ascii_string(out, capacity, &pos, "type")) {
    return false;
  }
  ADD_OFFSET(); // 22: "audioInputFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioInputFormats")) {
    return false;
  }
  ADD_OFFSET(); // 23: "audioOutputFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioOutputFormats")) {
    return false;
  }
  ADD_OFFSET(); // 24: stream type 96
  if (!bplist_write_int(out, capacity, &pos, 96)) {
    return false;
  }
  ADD_OFFSET(); // 25: format mask
  if (!bplist_write_int(out, capacity, &pos, 0x01000000)) {
    return false;
  }
  ADD_OFFSET(); // 26: audio format dict
  {
    const uint8_t keys[] = {21, 22, 23};
    const uint8_t values[] = {24, 25, 25};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 3)) {
      return false;
    }
  }
  ADD_OFFSET(); // 27: audioFormats array
  {
    const uint8_t refs[] = {26};
    if (!bplist_write_array(out, capacity, &pos, refs, 1)) {
      return false;
    }
  }
  ADD_OFFSET(); // 28: "audioLatencies"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioLatencies")) {
    return false;
  }
  ADD_OFFSET(); // 29: "audioType"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioType")) {
    return false;
  }
  ADD_OFFSET(); // 30: "inputLatencyMicros"
  if (!bplist_write_ascii_string(out, capacity, &pos, "inputLatencyMicros")) {
    return false;
  }
  ADD_OFFSET(); // 31: "outputLatencyMicros"
  if (!bplist_write_ascii_string(out, capacity, &pos, "outputLatencyMicros")) {
    return false;
  }
  ADD_OFFSET(); // 32: stream type 103
  if (!bplist_write_int(out, capacity, &pos, 103)) {
    return false;
  }
  ADD_OFFSET(); // 33: audio type
  if (!bplist_write_int(out, capacity, &pos, 0x64)) {
    return false;
  }
  ADD_OFFSET(); // 34: zero latency
  if (!bplist_write_int(out, capacity, &pos, 0)) {
    return false;
  }
  ADD_OFFSET(); // 35: latency dict for realtime stream
  {
    const uint8_t keys[] = {21, 29, 30, 31};
    const uint8_t values[] = {24, 33, 34, 34};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 4)) {
      return false;
    }
  }
  ADD_OFFSET(); // 36: latency dict for buffered stream
  {
    const uint8_t keys[] = {21, 29, 30, 31};
    const uint8_t values[] = {32, 33, 34, 34};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 4)) {
      return false;
    }
  }
  ADD_OFFSET(); // 37: audioLatencies array
  {
    const uint8_t refs[] = {35, 36};
    if (!bplist_write_array(out, capacity, &pos, refs, 2)) {
      return false;
    }
  }
  ADD_OFFSET(); // 38: top-level info dict
  {
    uint8_t keys[13] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 28, 0};
    uint8_t values[13] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 27, 37, 0};
    size_t count = 12;
    if (txt) {
      /* Objects 39/40 are written after the dict; references may point
       * forward in a bplist. */
      keys[count] = INFO_OBJ_COUNT;
      values[count] = INFO_OBJ_COUNT + 1;
      count++;
    }
    if (!bplist_write_dict(out, capacity, &pos, keys, values, count)) {
      return false;
    }
  }
  if (txt) {
    ADD_OFFSET(); // 39: "txtAirPlay"
    if (!bplist_write_ascii_string(out, capacity, &pos, "txtAirPlay")) {
      return false;
    }
    ADD_OFFSET(); // 40: TXT record data (length-prefixed "key=value" strings)
    if (!bplist_write_data(out, capacity, &pos, txt, txt_len)) {
      return false;
    }
  }

#undef ADD_OFFSET

  *pos_io = pos;
  *obj_io = obj;
  *info_dict = INFO_OBJ_COUNT - 1;
  return true;
}

size_t bplist_build_info_response(uint8_t *out, size_t capacity,
                                  const char *device_id,
                                  const char *device_name, const char *model,
                                  const uint8_t *public_key,
                                  size_t public_key_len, uint64_t features,
                                  int64_t protocol_version) {
  if (!out || !device_id || !device_name || !model || !public_key ||
      public_key_len == 0 || capacity < 512) {
    return 0;
  }
  size_t pos = 0;
  size_t offsets[INFO_MAX_OBJECTS];
  size_t obj = 0;
  size_t info_dict = 0;
  if (!bplist_has_room(pos, 8, capacity)) {
    return 0;
  }
  memcpy(out + pos, "bplist00", 8);
  pos += 8;
  if (!bplist_write_info_objects(out, capacity, &pos, offsets, &obj, device_id,
                                 device_name, model, public_key, public_key_len,
                                 features, protocol_version, NULL, 0,
                                 &info_dict) ||
      obj != INFO_OBJ_COUNT ||
      !bplist_finish(out, capacity, &pos, offsets, obj, info_dict)) {
    return 0;
  }
  return pos;
}

size_t bplist_build_update_info(uint8_t *out, size_t capacity,
                                const char *device_id, const char *device_name,
                                const char *model, const uint8_t *public_key,
                                size_t public_key_len, uint64_t features,
                                int64_t protocol_version, const uint8_t *txt,
                                size_t txt_len) {
  if (!out || !device_id || !device_name || !model || !public_key ||
      public_key_len == 0 || !txt || txt_len == 0 || capacity < 1024) {
    return 0;
  }
  size_t pos = 0;
  size_t offsets[INFO_MAX_OBJECTS];
  size_t obj = 0;
  size_t info_dict = 0;
  if (!bplist_has_room(pos, 8, capacity)) {
    return 0;
  }
  memcpy(out + pos, "bplist00", 8);
  pos += 8;
  if (!bplist_write_info_objects(out, capacity, &pos, offsets, &obj, device_id,
                                 device_name, model, public_key, public_key_len,
                                 features, protocol_version, txt, txt_len,
                                 &info_dict) ||
      obj != INFO_OBJ_COUNT + 2) {
    return 0;
  }
  /* { "type": "updateInfo", "value": <info dict> } - Shairport
   * ap2_event_send_update_info(). "type" reuses the /info key object. */
  const size_t type_value = obj;
  offsets[obj++] = pos;
  if (!bplist_write_ascii_string(out, capacity, &pos, "updateInfo")) {
    return 0;
  }
  const size_t value_key = obj;
  offsets[obj++] = pos;
  if (!bplist_write_ascii_string(out, capacity, &pos, "value")) {
    return 0;
  }
  const size_t top = obj;
  offsets[obj++] = pos;
  {
    const uint8_t keys[] = {INFO_OBJ_TYPE_KEY, (uint8_t)value_key};
    const uint8_t values[] = {(uint8_t)type_value, (uint8_t)info_dict};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 2)) {
      return 0;
    }
  }
  if (!bplist_finish(out, capacity, &pos, offsets, obj, top)) {
    return 0;
  }
  return pos;
}

size_t bplist_build_anchor(uint8_t *out, size_t capacity, uint64_t rate,
                           uint64_t rtp_time, uint64_t network_time_secs,
                           uint64_t network_time_frac, uint64_t flags,
                           uint64_t timeline_id) {
  static const char *const keys[] = {"rate", "rtpTime", "networkTimeSecs",
                                     "networkTimeFrac", "networkTimeFlags",
                                     "networkTimeTimelineID"};
  const uint64_t values[] = {rate, rtp_time, network_time_secs,
                             network_time_frac, flags, timeline_id};
  enum { N = 6 };
  if (!out || capacity < 8) return 0;
  size_t pos = 0;
  size_t offsets[2 * N + 1];
  uint8_t key_refs[N];
  uint8_t value_refs[N];
  memcpy(out, "bplist00", 8);
  pos = 8;
  size_t obj = 0;
  for (size_t i = 0; i < N; ++i) {
    offsets[obj] = pos;
    key_refs[i] = (uint8_t)obj++;
    if (!bplist_write_ascii_string(out, capacity, &pos, keys[i])) return 0;
  }
  for (size_t i = 0; i < N; ++i) {
    offsets[obj] = pos;
    value_refs[i] = (uint8_t)obj++;
    if (!bplist_write_int(out, capacity, &pos, values[i])) return 0;
  }
  offsets[obj] = pos;
  const size_t top = obj++;
  if (!bplist_write_dict(out, capacity, &pos, key_refs, value_refs, N) ||
      !bplist_finish(out, capacity, &pos, offsets, obj, top)) {
    return 0;
  }
  return pos;
}
