#include <string.h>
#include <math.h>

#include "airplay_identity.h"
#include "audio_receiver.h"
#include "plist.h"

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
  if (length > UINT8_MAX || !bplist_has_room(*pos, 3, capacity)) {
    return false;
  }
  out[(*pos)++] = marker_base | 0x0F;
  out[(*pos)++] = 0x10;
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
                                 uint32_t audio_buffer_size) {
  if (capacity < 200) {
    return 0;
  }

  size_t pos = 0;
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  size_t offsets[16];
  size_t obj = 0;

  offsets[obj++] = pos;
  out[pos++] = 0x57;
  memcpy(out + pos, "streams", 7);
  pos += 7;

  offsets[obj++] = pos;
  out[pos++] = 0x54;
  memcpy(out + pos, "type", 4);
  pos += 4;

  offsets[obj++] = pos;
  out[pos++] = 0x58;
  memcpy(out + pos, "dataPort", 8);
  pos += 8;

  offsets[obj++] = pos;
  out[pos++] = 0x5B;
  memcpy(out + pos, "controlPort", 11);
  pos += 11;

  offsets[obj++] = pos;
  out[pos++] = 0x5F;
  out[pos++] = 0x10;
  out[pos++] = 15;
  memcpy(out + pos, "audioBufferSize", 15);
  pos += 15;

  offsets[obj++] = pos;
  out[pos++] = 0x10;
  out[pos++] = (uint8_t)stream_type;

  offsets[obj++] = pos;
  out[pos++] = 0x11;
  out[pos++] = (data_port >> 8) & 0xFF;
  out[pos++] = data_port & 0xFF;

  offsets[obj++] = pos;
  out[pos++] = 0x11;
  out[pos++] = (control_port >> 8) & 0xFF;
  out[pos++] = control_port & 0xFF;

  offsets[obj++] = pos;
  out[pos++] = 0x12;
  out[pos++] = (audio_buffer_size >> 24) & 0xFF;
  out[pos++] = (audio_buffer_size >> 16) & 0xFF;
  out[pos++] = (audio_buffer_size >> 8) & 0xFF;
  out[pos++] = audio_buffer_size & 0xFF;

  offsets[obj++] = pos;
  if ((stream_type == AUDIO_STREAM_BUFFERED)) {
    out[pos++] = 0xD4;
    out[pos++] = 1;
    out[pos++] = 2;
    out[pos++] = 4;
    out[pos++] = 3;
    out[pos++] = 5;
    out[pos++] = 6;
    out[pos++] = 8;
    out[pos++] = 7;
  } else {
    out[pos++] = 0xD3;
    out[pos++] = 1;
    out[pos++] = 2;
    out[pos++] = 3;
    out[pos++] = 5;
    out[pos++] = 6;
    out[pos++] = 7;
  }

  offsets[obj++] = pos;
  out[pos++] = 0xA1;
  out[pos++] = 9;

  offsets[obj++] = pos;
  out[pos++] = 0xD1;
  out[pos++] = 0;
  out[pos++] = 10;

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
  out[pos++] = 11;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)offset_table_offset;

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

size_t bplist_build_info_response(uint8_t *out, size_t capacity,
                                  const char *device_id,
                                  const char *device_name,
                                  const uint8_t *public_key,
                                  size_t public_key_len, uint64_t features) {
  if (!out || !device_id || !device_name || !public_key ||
      public_key_len == 0 || capacity < 512) {
    return 0;
  }

  size_t pos = 0;
#ifdef CONFIG_AIRPLAY_HOMEKIT
  size_t offsets[45]; /* + manufacturer, serialNumber, firmwareRevision */
#else
  size_t offsets[39];
#endif
  size_t obj = 0;

#define ADD_OFFSET()                                   \
  do {                                                 \
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) { \
      return 0;                                        \
    }                                                  \
    offsets[obj++] = pos;                              \
  } while (0)

  if (!bplist_has_room(pos, 8, capacity)) {
    return 0;
  }
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  ADD_OFFSET(); // 0: "deviceid"
  if (!bplist_write_ascii_string(out, capacity, &pos, "deviceid")) {
    return 0;
  }
  ADD_OFFSET(); // 1: device id
  if (!bplist_write_ascii_string(out, capacity, &pos, device_id)) {
    return 0;
  }
  ADD_OFFSET(); // 2: "features"
  if (!bplist_write_ascii_string(out, capacity, &pos, "features")) {
    return 0;
  }
  ADD_OFFSET(); // 3: features
  if (!bplist_write_int(out, capacity, &pos, features)) {
    return 0;
  }
  ADD_OFFSET(); // 4: "model"
  if (!bplist_write_ascii_string(out, capacity, &pos, "model")) {
    return 0;
  }
  ADD_OFFSET(); // 5: model
  if (!bplist_write_ascii_string(out, capacity, &pos, AIRPLAY_MODEL)) {
    return 0;
  }
  ADD_OFFSET(); // 6: "protovers"
  if (!bplist_write_ascii_string(out, capacity, &pos, "protovers")) {
    return 0;
  }
  ADD_OFFSET(); // 7: protocol version string
  if (!bplist_write_ascii_string(out, capacity, &pos, AIRPLAY_PROTOVERS)) {
    return 0;
  }
  ADD_OFFSET(); // 8: "srcvers"
  if (!bplist_write_ascii_string(out, capacity, &pos, "srcvers")) {
    return 0;
  }
  ADD_OFFSET(); // 9: source version string
  if (!bplist_write_ascii_string(out, capacity, &pos, AIRPLAY_SOURCE_VERSION)) {
    return 0;
  }
  ADD_OFFSET(); // 10: "vv"
  if (!bplist_write_ascii_string(out, capacity, &pos, "vv")) {
    return 0;
  }
  ADD_OFFSET(); // 11: vv value
  if (!bplist_write_int(out, capacity, &pos, AIRPLAY_PROTOCOL_VERSION)) {
    return 0;
  }
  ADD_OFFSET(); // 12: "statusFlags"
  if (!bplist_write_ascii_string(out, capacity, &pos, "statusFlags")) {
    return 0;
  }
  ADD_OFFSET(); // 13: statusFlags value
  if (!bplist_write_int(out, capacity, &pos, (int64_t)airplay_status_flags())) {
    return 0;
  }
  ADD_OFFSET(); // 14: "pk"
  if (!bplist_write_ascii_string(out, capacity, &pos, "pk")) {
    return 0;
  }
  ADD_OFFSET(); // 15: public key
  if (!bplist_write_data(out, capacity, &pos, public_key, public_key_len)) {
    return 0;
  }
  ADD_OFFSET(); // 16: "pi"
  if (!bplist_write_ascii_string(out, capacity, &pos, "pi")) {
    return 0;
  }
  ADD_OFFSET(); // 17: pairing identifier
  char pairing_id[AIRPLAY_PAIRING_ID_LEN];
  airplay_get_pairing_id(pairing_id, sizeof(pairing_id));
  if (!bplist_write_ascii_string(out, capacity, &pos, pairing_id)) {
    return 0;
  }
  ADD_OFFSET(); // 18: "name"
  if (!bplist_write_ascii_string(out, capacity, &pos, "name")) {
    return 0;
  }
  ADD_OFFSET(); // 19: device name
  if (!bplist_write_ascii_string(out, capacity, &pos, device_name)) {
    return 0;
  }
  ADD_OFFSET(); // 20: "audioFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioFormats")) {
    return 0;
  }
  ADD_OFFSET(); // 21: "type"
  if (!bplist_write_ascii_string(out, capacity, &pos, "type")) {
    return 0;
  }
  ADD_OFFSET(); // 22: "audioInputFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioInputFormats")) {
    return 0;
  }
  ADD_OFFSET(); // 23: "audioOutputFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioOutputFormats")) {
    return 0;
  }
  ADD_OFFSET(); // 24: stream type 96
  if (!bplist_write_int(out, capacity, &pos, 96)) {
    return 0;
  }
  ADD_OFFSET(); // 25: format mask
  if (!bplist_write_int(out, capacity, &pos, 0x01000000)) {
    return 0;
  }
  ADD_OFFSET(); // 26: audio format dict
  {
    const uint8_t keys[] = {21, 22, 23};
    const uint8_t values[] = {24, 25, 25};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 3)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 27: audioFormats array
  {
    const uint8_t refs[] = {26};
    if (!bplist_write_array(out, capacity, &pos, refs, 1)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 28: "audioLatencies"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioLatencies")) {
    return 0;
  }
  ADD_OFFSET(); // 29: "audioType"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioType")) {
    return 0;
  }
  ADD_OFFSET(); // 30: "inputLatencyMicros"
  if (!bplist_write_ascii_string(out, capacity, &pos, "inputLatencyMicros")) {
    return 0;
  }
  ADD_OFFSET(); // 31: "outputLatencyMicros"
  if (!bplist_write_ascii_string(out, capacity, &pos, "outputLatencyMicros")) {
    return 0;
  }
  ADD_OFFSET(); // 32: stream type 103
  if (!bplist_write_int(out, capacity, &pos, 103)) {
    return 0;
  }
  ADD_OFFSET(); // 33: audio type
  if (!bplist_write_int(out, capacity, &pos, 0x64)) {
    return 0;
  }
  ADD_OFFSET(); // 34: zero latency
  if (!bplist_write_int(out, capacity, &pos, 0)) {
    return 0;
  }
  ADD_OFFSET(); // 35: latency dict for realtime stream
  {
    const uint8_t keys[] = {21, 29, 30, 31};
    const uint8_t values[] = {24, 33, 34, 34};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 4)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 36: latency dict for buffered stream
  {
    const uint8_t keys[] = {21, 29, 30, 31};
    const uint8_t values[] = {32, 33, 34, 34};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 4)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 37: audioLatencies array
  {
    const uint8_t refs[] = {35, 36};
    if (!bplist_write_array(out, capacity, &pos, refs, 2)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 38: top-level info dict
  {
#ifdef CONFIG_AIRPLAY_HOMEKIT
    /* Accessory details the Home app shows (objects 39..44 below). */
    const uint8_t keys[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 28, 39, 41, 43};
    const uint8_t values[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 27, 37, 40, 42, 44};
#else
    const uint8_t keys[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 28};
    const uint8_t values[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 27, 37};
#endif
    if (!bplist_write_dict(out, capacity, &pos, keys, values, sizeof(keys))) {
      return 0;
    }
  }
#ifdef CONFIG_AIRPLAY_HOMEKIT
  {
    char serial[13], firmware[32];
    airplay_get_serial_number(serial, sizeof(serial));
    airplay_get_firmware_revision(firmware, sizeof(firmware));
    ADD_OFFSET(); // 39
    if (!bplist_write_ascii_string(out, capacity, &pos, "manufacturer")) return 0;
    ADD_OFFSET(); // 40
    if (!bplist_write_ascii_string(out, capacity, &pos, CONFIG_AIRPLAY_HOMEKIT_MANUFACTURER))
      return 0;
    ADD_OFFSET(); // 41
    if (!bplist_write_ascii_string(out, capacity, &pos, "serialNumber")) return 0;
    ADD_OFFSET(); // 42
    if (!bplist_write_ascii_string(out, capacity, &pos, serial)) return 0;
    ADD_OFFSET(); // 43
    if (!bplist_write_ascii_string(out, capacity, &pos, "firmwareRevision")) return 0;
    ADD_OFFSET(); // 44
    if (!bplist_write_ascii_string(out, capacity, &pos, firmware)) return 0;
  }
#endif

#undef ADD_OFFSET

  if (obj != sizeof(offsets) / sizeof(offsets[0]) ||
      !bplist_finish(out, capacity, &pos, offsets, obj, 38)) {
    return 0;
  }

  return pos;
}

size_t bplist_build_media_remote_command(uint8_t *out, size_t capacity,
                                        uint8_t command) {
  if (!out || capacity < 8 || command > 5) return 0;
  memcpy(out, "bplist00", 8);
  size_t pos = 8;
  size_t offsets[5];
  offsets[0] = pos;
  if (!bplist_write_ascii_string(out, capacity, &pos, "type")) return 0;
  offsets[1] = pos;
  if (!bplist_write_ascii_string(out, capacity, &pos, "modernMediaRemoteCommand")) return 0;
  offsets[2] = pos;
  if (!bplist_write_ascii_string(out, capacity, &pos, "sendMediaRemoteCommand")) return 0;
  offsets[3] = pos;
  if (!bplist_write_int(out, capacity, &pos, command)) return 0;
  offsets[4] = pos;
  const uint8_t keys[] = {0, 1}, values[] = {2, 3};
  if (!bplist_write_dict(out, capacity, &pos, keys, values, 2) ||
      !bplist_finish(out, capacity, &pos, offsets, 5, 4)) return 0;
  return pos;
}

size_t bplist_build_media_remote_volume(uint8_t *out, size_t capacity,
                                       double volume) {
  if (!out || capacity < 8 || !isfinite(volume) || volume < 0.0 || volume > 1.0)
    return 0;
  memcpy(out, "bplist00", 8);
  size_t pos = 8;
  size_t offsets[9];
  const char *strings[] = {"type", "value", "volume", "params",
                           "sendMediaRemoteCommand", "dvlc"};
  for (size_t i = 0; i < 6; i++) {
    offsets[i] = pos;
    if (!bplist_write_ascii_string(out, capacity, &pos, strings[i])) return 0;
  }
  offsets[6] = pos;
  if (!bplist_has_room(pos, 9, capacity)) return 0;
  out[pos++] = 0x23; /* IEEE-754 double, big endian. */
  union { double value; uint64_t bits; } real = {.value = volume};
  if (!bplist_write_u64(out, capacity, &pos, real.bits)) return 0;
  offsets[7] = pos;
  const uint8_t param_keys[] = {2}, param_values[] = {6};
  if (!bplist_write_dict(out, capacity, &pos, param_keys, param_values, 1)) return 0;
  offsets[8] = pos;
  const uint8_t keys[] = {0, 1, 2, 3}, values[] = {4, 5, 6, 7};
  if (!bplist_write_dict(out, capacity, &pos, keys, values, 4) ||
      !bplist_finish(out, capacity, &pos, offsets, 9, 8)) return 0;
  return pos;
}

/* A string object: ASCII as is, anything else as UTF-16BE (BMP only). */
static bool bplist_write_text(uint8_t *out, size_t capacity, size_t *pos,
                              const char *value) {
  bool ascii = true;
  for (const char *c = value; *c; c++)
    if ((uint8_t)*c >= 0x80) ascii = false;
  if (ascii) return bplist_write_ascii_string(out, capacity, pos, value);
  uint16_t u16[128];
  size_t n = 0;
  const uint8_t *s = (const uint8_t *)value;
  while (*s && n < 128) {
    uint32_t cp;
    if (s[0] < 0x80) { cp = s[0]; s += 1; }
    else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1FU) << 6) | (s[1] & 0x3FU); s += 2; }
    else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
      cp = ((s[0] & 0x0FU) << 12) | ((s[1] & 0x3FU) << 6) | (s[2] & 0x3FU); s += 3;
    } else { cp = '?'; s += 1; while ((*s & 0xC0) == 0x80) s++; }
    u16[n++] = (uint16_t)cp;
  }
  if (!bplist_write_length(out, capacity, pos, 0x60, n) ||
      !bplist_has_room(*pos, 2 * n, capacity))
    return false;
  for (size_t i = 0; i < n; i++) {
    out[(*pos)++] = (uint8_t)(u16[i] >> 8);
    out[(*pos)++] = (uint8_t)u16[i];
  }
  return true;
}

size_t bplist_build_configure_response(uint8_t *out, size_t capacity,
                                       const char *identifier, bool hk_access_control,
                                       const uint8_t public_key[32], const char *device_name,
                                       int64_t access_control_level, const char *password) {
  if (!out || !identifier || !public_key || !device_name || capacity < 256) return 0;
  const bool with_pw = password && password[0];
  size_t pos = 0;
  size_t offsets[13];
  size_t obj = 0;
  memcpy(out, "bplist00", 8);
  pos = 8;
#define ADD() offsets[obj++] = pos
  ADD(); if (!bplist_write_ascii_string(out, capacity, &pos, "Identifier")) return 0;
  ADD(); if (!bplist_write_text(out, capacity, &pos, identifier)) return 0;
  ADD(); if (!bplist_write_ascii_string(out, capacity, &pos, "Enable_HK_Access_Control")) return 0;
  ADD(); if (!bplist_has_room(pos, 1, capacity)) return 0;
  out[pos++] = hk_access_control ? 0x09 : 0x08;
  ADD(); if (!bplist_write_ascii_string(out, capacity, &pos, "PublicKey")) return 0;
  ADD(); if (!bplist_write_data(out, capacity, &pos, public_key, 32)) return 0;
  ADD(); if (!bplist_write_ascii_string(out, capacity, &pos, "Device_Name")) return 0;
  ADD(); if (!bplist_write_text(out, capacity, &pos, device_name)) return 0;
  ADD(); if (!bplist_write_ascii_string(out, capacity, &pos, "Access_Control_Level")) return 0;
  ADD(); if (!bplist_write_int(out, capacity, &pos,
                               (uint64_t)(access_control_level < 0 ? 0 : access_control_level)))
    return 0;
  if (with_pw) {
    ADD(); if (!bplist_write_ascii_string(out, capacity, &pos, "Password")) return 0;
    ADD(); if (!bplist_write_text(out, capacity, &pos, password)) return 0;
  }
  const size_t top = obj;
  ADD();
  {
    const uint8_t keys[] = {0, 2, 4, 6, 8, 10};
    const uint8_t values[] = {1, 3, 5, 7, 9, 11};
    const size_t n = with_pw ? 6 : 5;
    if (!bplist_write_dict(out, capacity, &pos, keys, values, n)) return 0;
  }
#undef ADD
  if (!bplist_finish(out, capacity, &pos, offsets, obj, top)) return 0;
  return pos;
}
