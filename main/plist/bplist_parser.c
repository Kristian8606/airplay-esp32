#include <stdio.h>
#include <string.h>

#include "plist.h"

// Binary plist object types (high nibble of marker byte)
#define BPLIST_INT     0x10
#define BPLIST_REAL    0x20
#define BPLIST_DATA    0x40
#define BPLIST_STRING  0x50
#define BPLIST_UNICODE 0x60
#define BPLIST_UID     0x80
#define BPLIST_ARRAY   0xA0
#define BPLIST_SET     0xC0
#define BPLIST_DICT    0xD0

/* Upper bound on objects examined by one bounded deep-search call. */
#define BPLIST_DEEP_SEARCH_MAX_VISITS 512U

static uint64_t read_be_int(const uint8_t *data, size_t bytes) {
  uint64_t val = 0;
  for (size_t i = 0; i < bytes; i++) {
    val = (val << 8) | data[i];
  }
  return val;
}

/* Every count/length in a binary plist is a wire value of up to 64 bits, but
 * size_t is only 32 bits on the ESP32. A check written as
 * `pos + count * ref_size > plist_len` can wrap (on the ESP32 already for
 * count >= 2^31, and even on 64-bit hosts for an 8-byte count), pass, and let
 * the reference loop walk far outside the request buffer or spin for billions
 * of iterations. Any LAN client can send such a plist to /command, /feedback
 * or SETUP before pairing, so all spans are checked in this overflow-free form
 * (same hardening as Shairport Sync 5.1/5.5). */
static inline bool bplist_span_ok(uint64_t pos, uint64_t count,
                                  uint64_t elem_size, size_t plist_len) {
  if (pos > (uint64_t)plist_len) return false;
  if (elem_size == 0) return true;
  return count <= ((uint64_t)plist_len - pos) / elem_size;
}

/* Extended length/count that follows a 0x?F marker: an INT object of 1, 2, 4
 * or 8 bytes. Rejects wider integers (read_be_int would silently drop their
 * high bytes) and values that do not fit in size_t. */
static bool bplist_read_ext_len(const uint8_t *plist, size_t plist_len,
                                size_t *pos, size_t *out) {
  size_t p = *pos;
  if (p >= plist_len) return false;
  const uint8_t len_marker = plist[p++];
  if ((len_marker & 0xF0) != BPLIST_INT) return false;
  const unsigned pow2 = len_marker & 0x0FU;
  if (pow2 > 3U) return false;
  const size_t len_bytes = (size_t)1U << pow2;
  if (len_bytes > plist_len - p) return false;
  const uint64_t v = read_be_int(plist + p, len_bytes);
  if (v > (uint64_t)SIZE_MAX) return false;
  *out = (size_t)v;
  *pos = p + len_bytes;
  return true;
}

static bool bplist_parse_trailer(const uint8_t *plist, size_t plist_len,
                                 uint8_t *offset_size, uint8_t *ref_size,
                                 uint64_t *num_objects, uint64_t *top_object,
                                 uint64_t *offset_table_offset) {
  if (plist_len < 32) {
    return false;
  }

  const uint8_t *trailer = plist + plist_len - 32;

  *offset_size = trailer[6];
  *ref_size = trailer[7];
  *num_objects = read_be_int(trailer + 8, 8);
  *top_object = read_be_int(trailer + 16, 8);
  *offset_table_offset = read_be_int(trailer + 24, 8);

  if (*offset_size == 0 || *offset_size > 8 || *ref_size == 0 ||
      *ref_size > 8 || *num_objects == 0 || *top_object >= *num_objects) {
    return false;
  }
  const uint64_t trailer_offset = (uint64_t)plist_len - 32U;
  if (*offset_table_offset > trailer_offset ||
      *num_objects > (UINT64_MAX - *offset_table_offset) / *offset_size) {
    return false;
  }
  const uint64_t table_end =
      *offset_table_offset + *num_objects * (uint64_t)*offset_size;
  return table_end <= trailer_offset;
}

static uint64_t bplist_get_offset(const uint8_t *plist, size_t plist_len,
                                  uint64_t offset_table_offset,
                                  uint8_t offset_size, uint64_t obj_idx) {
  if (!plist || plist_len < 32 || offset_size == 0 || offset_size > 8) {
    return UINT64_MAX;
  }

  const size_t trailer_offset = plist_len - 32U;
  const uint8_t *trailer = plist + trailer_offset;
  const uint64_t num_objects = read_be_int(trailer + 8, 8);
  if (obj_idx >= num_objects || offset_table_offset > trailer_offset) {
    return UINT64_MAX;
  }

  if (obj_idx > (UINT64_MAX - offset_table_offset) / offset_size) {
    return UINT64_MAX;
  }
  const uint64_t entry_offset =
      offset_table_offset + obj_idx * (uint64_t)offset_size;
  if (entry_offset > trailer_offset ||
      (uint64_t)offset_size > (uint64_t)trailer_offset - entry_offset) {
    return UINT64_MAX;
  }

  const uint64_t object_offset =
      read_be_int(plist + (size_t)entry_offset, offset_size);
  if (object_offset >= offset_table_offset || object_offset >= trailer_offset) {
    return UINT64_MAX;
  }
  return object_offset;
}

static bool bplist_read_string(const uint8_t *plist, size_t plist_len,
                               uint64_t offset, char *out,
                               size_t out_capacity) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;
  size_t len = marker & 0x0F;
  size_t pos = offset + 1;

  if (len == 0x0F && !bplist_read_ext_len(plist, plist_len, &pos, &len)) {
    return false;
  }

  if (type == BPLIST_STRING) {
    if (len > plist_len - pos || len >= out_capacity) {
      return false;
    }
    memcpy(out, plist + pos, len);
    out[len] = '\0';
    return true;
  }

  if (type == BPLIST_UNICODE) {
    if (len > (plist_len - pos) / 2U || len >= out_capacity) {
      return false;
    }
    for (size_t i = 0; i < len; i++) {
      uint16_t code =
          (uint16_t)(plist[pos + i * 2] << 8) | plist[pos + i * 2 + 1];
      if (code > 0x7F) {
        return false;
      }
      out[i] = (char)code;
    }
    out[len] = '\0';
    return true;
  }

  return false;
}

static bool bplist_read_data(const uint8_t *plist, size_t plist_len,
                             uint64_t offset, uint8_t *out, size_t out_capacity,
                             size_t *out_len) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;
  size_t len = marker & 0x0F;
  size_t pos = offset + 1;

  if (len == 0x0F && !bplist_read_ext_len(plist, plist_len, &pos, &len)) {
    return false;
  }

  if (type == BPLIST_DATA) {
    if (len > plist_len - pos || len > out_capacity) {
      return false;
    }
    memcpy(out, plist + pos, len);
    *out_len = len;
    return true;
  }

  return false;
}

static bool bplist_read_data_len(const uint8_t *plist, size_t plist_len,
                                 uint64_t offset, size_t *out_len) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;
  size_t len = marker & 0x0F;
  size_t pos = offset + 1;

  if (len == 0x0F && !bplist_read_ext_len(plist, plist_len, &pos, &len)) {
    return false;
  }

  if (type == BPLIST_DATA) {
    if (len > plist_len - pos) {
      return false;
    }
    *out_len = len;
    return true;
  }

  return false;
}

static bool bplist_read_string_len(const uint8_t *plist, size_t plist_len,
                                   uint64_t offset, size_t *out_len) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;
  size_t len = marker & 0x0F;
  size_t pos = offset + 1;

  if (len == 0x0F && !bplist_read_ext_len(plist, plist_len, &pos, &len)) {
    return false;
  }

  if (type == BPLIST_STRING) {
    if (len > plist_len - pos) {
      return false;
    }
    *out_len = len;
    return true;
  }
  if (type == BPLIST_UNICODE) {
    if (len > (plist_len - pos) / 2U) {
      return false;
    }
    *out_len = len;
    return true;
  }

  return false;
}

static bool bplist_read_int(const uint8_t *plist, size_t plist_len,
                            uint64_t offset, int64_t *out) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;

  if (type == BPLIST_INT) {
    size_t len = 1 << (marker & 0x0F);
    if (offset + 1 + len > plist_len) {
      return false;
    }
    *out = (int64_t)read_be_int(plist + offset + 1, len);
    return true;
  }

  return false;
}

static bool bplist_read_real(const uint8_t *plist, size_t plist_len,
                             uint64_t offset, double *out) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;

  if (type == BPLIST_REAL) {
    size_t len = 1 << (marker & 0x0F);
    if (offset + 1 + len > plist_len) {
      return false;
    }

    if (len == 4) {
      uint32_t bits = (uint32_t)read_be_int(plist + offset + 1, 4);
      float f;
      memcpy(&f, &bits, sizeof(f));
      *out = (double)f;
      return true;
    } else if (len == 8) {
      uint64_t bits = read_be_int(plist + offset + 1, 8);
      memcpy(out, &bits, sizeof(*out));
      return true;
    }
  }

  return false;
}

static bool bplist_parse_count(const uint8_t *plist, size_t plist_len,
                               uint64_t offset, size_t *count,
                               size_t *header_len) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  size_t info = marker & 0x0F;
  size_t pos = offset + 1;

  if (info == 0x0F) {
    if (!bplist_read_ext_len(plist, plist_len, &pos, count)) {
      return false;
    }
  } else {
    *count = info;
  }

  *header_len = pos - offset;
  return true;
}

/* Locate a value in one streams[index] dictionary.  Keep this small helper
 * private so feature-specific parsers (e.g. streamConnections) don't need to
 * duplicate the full top-dict -> streams array -> stream-dict walk. */
static bool bplist_get_stream_value_offset(
    const uint8_t *plist, size_t plist_len, size_t index, const char *wanted_key,
    uint64_t *value_offset, uint8_t *offset_size_out, uint8_t *ref_size_out,
    uint64_t *offset_table_offset_out) {
  if (!plist || !wanted_key || !value_offset || plist_len < 40 ||
      memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  const uint64_t top_offset = bplist_get_offset(
      plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len || (plist[top_offset] & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t top_count = 0, top_header = 0;
  if (!bplist_parse_count(plist, plist_len, top_offset, &top_count,
                          &top_header)) {
    return false;
  }
  const uint64_t top_pos64 = top_offset + top_header;
  if (!bplist_span_ok(top_pos64, top_count, 2U * (uint64_t)ref_size,
                      plist_len)) {
    return false;
  }
  const size_t top_pos = (size_t)top_pos64;
  const uint8_t *top_keys = plist + top_pos;
  const uint8_t *top_vals = plist + top_pos + top_count * ref_size;

  uint64_t streams_array_offset = UINT64_MAX;
  for (size_t i = 0; i < top_count; ++i) {
    const uint64_t key_idx = read_be_int(top_keys + i * ref_size, ref_size);
    const uint64_t key_off = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, key_idx);
    char key[32];
    if (!bplist_read_string(plist, plist_len, key_off, key, sizeof(key)) ||
        strcmp(key, "streams") != 0) {
      continue;
    }
    const uint64_t val_idx = read_be_int(top_vals + i * ref_size, ref_size);
    streams_array_offset = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, val_idx);
    break;
  }

  if (streams_array_offset >= plist_len ||
      (plist[streams_array_offset] & 0xF0) != BPLIST_ARRAY) {
    return false;
  }

  size_t array_count = 0, array_header = 0;
  if (!bplist_parse_count(plist, plist_len, streams_array_offset, &array_count,
                          &array_header) ||
      index >= array_count) {
    return false;
  }
  const uint64_t array_pos64 = streams_array_offset + array_header;
  if (!bplist_span_ok(array_pos64, array_count, ref_size, plist_len)) {
    return false;
  }
  const size_t array_pos = (size_t)array_pos64;
  const uint64_t stream_idx =
      read_be_int(plist + array_pos + index * ref_size, ref_size);
  const uint64_t stream_offset = bplist_get_offset(
      plist, plist_len, offset_table_offset, offset_size, stream_idx);
  if (stream_offset >= plist_len ||
      (plist[stream_offset] & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t stream_count = 0, stream_header = 0;
  if (!bplist_parse_count(plist, plist_len, stream_offset, &stream_count,
                          &stream_header)) {
    return false;
  }
  const uint64_t stream_pos64 = stream_offset + stream_header;
  if (!bplist_span_ok(stream_pos64, stream_count, 2U * (uint64_t)ref_size,
                      plist_len)) {
    return false;
  }
  const size_t stream_pos = (size_t)stream_pos64;
  const uint8_t *stream_keys = plist + stream_pos;
  const uint8_t *stream_vals = plist + stream_pos + stream_count * ref_size;

  for (size_t i = 0; i < stream_count; ++i) {
    const uint64_t key_idx =
        read_be_int(stream_keys + i * ref_size, ref_size);
    const uint64_t key_off = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, key_idx);
    char key[64];
    if (!bplist_read_string(plist, plist_len, key_off, key, sizeof(key)) ||
        strcmp(key, wanted_key) != 0) {
      continue;
    }
    const uint64_t val_idx =
        read_be_int(stream_vals + i * ref_size, ref_size);
    const uint64_t val_off = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, val_idx);
    if (val_off >= plist_len) return false;
    *value_offset = val_off;
    if (offset_size_out) *offset_size_out = offset_size;
    if (ref_size_out) *ref_size_out = ref_size;
    if (offset_table_offset_out)
      *offset_table_offset_out = offset_table_offset;
    return true;
  }

  return false;
}

static bool bplist_read_u64_flexible(const uint8_t *plist, size_t plist_len,
                                     uint64_t offset, uint64_t *out) {
  if (!plist || !out || offset >= plist_len) return false;

  int64_t int_value = 0;
  if (bplist_read_int(plist, plist_len, offset, &int_value)) {
    *out = (uint64_t)int_value;
    return true;
  }

  uint8_t data[8];
  size_t data_len = 0;
  if (bplist_read_data(plist, plist_len, offset, data, sizeof(data),
                       &data_len) &&
      data_len > 0 && data_len <= sizeof(data)) {
    *out = read_be_int(data, data_len);
    return true;
  }

  char text[40];
  if (!bplist_read_string(plist, plist_len, offset, text, sizeof(text)))
    return false;

  const char *p = text;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

  uint64_t value = 0;
  size_t digits = 0;
  for (; *p; ++p) {
    unsigned nibble;
    if (*p >= '0' && *p <= '9')
      nibble = (unsigned)(*p - '0');
    else if (*p >= 'a' && *p <= 'f')
      nibble = (unsigned)(*p - 'a' + 10);
    else if (*p >= 'A' && *p <= 'F')
      nibble = (unsigned)(*p - 'A' + 10);
    else if (*p == ':' || *p == '-' || *p == ' ')
      continue;
    else
      return false;

    if (++digits > 16U) return false;
    value = (value << 4) | nibble;
  }
  if (digits == 0) return false;
  *out = value;
  return true;
}

static bool bplist_read_string_array(const uint8_t *plist, size_t plist_len,
                                     uint64_t array_offset,
                                     uint64_t offset_table_offset,
                                     uint8_t offset_size, uint8_t ref_size,
                                     char out[][BPLIST_PEER_ADDRESS_MAX],
                                     size_t out_capacity,
                                     size_t *out_count) {
  if (!plist || !out_count || array_offset >= plist_len || ref_size == 0)
    return false;
  if ((plist[array_offset] & 0xF0) != BPLIST_ARRAY) return false;

  size_t count = 0, header_len = 0;
  if (!bplist_parse_count(plist, plist_len, array_offset, &count, &header_len))
    return false;
  const uint64_t refs_offset = array_offset + header_len;
  if (refs_offset > plist_len ||
      count > ((uint64_t)plist_len - refs_offset) / ref_size)
    return false;

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint64_t ref_pos = refs_offset + i * (uint64_t)ref_size;
    const uint64_t obj_idx = read_be_int(plist + (size_t)ref_pos, ref_size);
    const uint64_t obj_offset = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, obj_idx);
    if (obj_offset == UINT64_MAX) return false;

    char scratch[BPLIST_PEER_ADDRESS_MAX];
    char *dst = written < out_capacity ? out[written] : scratch;
    size_t dst_capacity = written < out_capacity ? BPLIST_PEER_ADDRESS_MAX
                                                 : sizeof(scratch);

    // Shairport Sync 5.5+: malformed/non-string elements in
    // timingPeerInfo.Addresses are ignored individually. A bad entry must
    // not invalidate the remaining usable timing peers.
    if (!bplist_read_string(plist, plist_len, obj_offset, dst, dst_capacity)) {
      continue;
    }
    if (written < out_capacity) {
      written++;
    }
  }
  *out_count = written;
  return true;
}

static bool bplist_read_peer_dict(const uint8_t *plist, size_t plist_len,
                                  uint64_t dict_offset,
                                  uint64_t offset_table_offset,
                                  uint8_t offset_size, uint8_t ref_size,
                                  bplist_peer_info_t *peer) {
  if (!plist || !peer || dict_offset >= plist_len || ref_size == 0)
    return false;
  if ((plist[dict_offset] & 0xF0) != BPLIST_DICT) return false;

  memset(peer, 0, sizeof(*peer));

  size_t dict_size = 0, header_len = 0;
  if (!bplist_parse_count(plist, plist_len, dict_offset, &dict_size,
                          &header_len))
    return false;
  const uint64_t refs_offset = dict_offset + header_len;
  if (refs_offset > plist_len ||
      dict_size > ((uint64_t)plist_len - refs_offset) / (2U * ref_size))
    return false;

  const uint8_t *key_refs = plist + (size_t)refs_offset;
  const uint8_t *val_refs = key_refs + dict_size * ref_size;
  for (size_t i = 0; i < dict_size; ++i) {
    const uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    const uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
    const uint64_t key_offset = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, key_idx);
    const uint64_t val_offset = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, val_idx);
    if (key_offset == UINT64_MAX || val_offset == UINT64_MAX) return false;

    char key[48];
    if (!bplist_read_string(plist, plist_len, key_offset, key, sizeof(key)))
      return false;

    if (strcmp(key, "Addresses") == 0) {
      size_t address_count = 0;
      if (!bplist_read_string_array(
              plist, plist_len, val_offset, offset_table_offset, offset_size,
              ref_size, peer->addresses, BPLIST_PEER_MAX_ADDRESSES,
              &address_count))
        return false;
      peer->address_count = address_count > BPLIST_PEER_MAX_ADDRESSES
                                ? BPLIST_PEER_MAX_ADDRESSES
                                : address_count;
    } else if (strcmp(key, "ClockID") == 0) {
      uint64_t clock_id = 0;
      if (bplist_read_u64_flexible(plist, plist_len, val_offset, &clock_id)) {
        peer->clock_id = clock_id;
        peer->has_clock_id = clock_id != 0;
      }
    }
  }
  return true;
}

bool bplist_get_peer_list(const uint8_t *plist, size_t plist_len,
                          bool extended, bplist_peer_info_t *out,
                          size_t out_capacity, size_t *out_count) {
  if (!plist || !out_count || (out_capacity > 0 && !out) || plist_len < 40 ||
      memcmp(plist, "bplist00", 8) != 0)
    return false;

  uint8_t offset_size = 0, ref_size = 0;
  uint64_t num_objects = 0, top_object = 0, offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset))
    return false;

  const uint64_t top_offset = bplist_get_offset(
      plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset == UINT64_MAX ||
      (plist[top_offset] & 0xF0) != BPLIST_ARRAY)
    return false;

  size_t count = 0, header_len = 0;
  if (!bplist_parse_count(plist, plist_len, top_offset, &count, &header_len))
    return false;
  const uint64_t refs_offset = top_offset + header_len;
  if (refs_offset > plist_len ||
      count > ((uint64_t)plist_len - refs_offset) / ref_size)
    return false;

  /* *out_count reports every usable peer (it may exceed out_capacity; only the
   * first out_capacity are stored). Shairport Sync handle_setpeers() skips a
   * non-string SETPEERS element individually, exactly like the Addresses
   * array inside SETPEERSX: one malformed entry must not discard the rest. */
  size_t usable = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint64_t ref_pos = refs_offset + i * (uint64_t)ref_size;
    const uint64_t obj_idx = read_be_int(plist + (size_t)ref_pos, ref_size);
    const uint64_t obj_offset = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, obj_idx);
    if (obj_offset == UINT64_MAX) return false;

    bplist_peer_info_t scratch;
    bplist_peer_info_t *peer = usable < out_capacity ? &out[usable] : &scratch;
    memset(peer, 0, sizeof(*peer));

    if (!extended) {
      if (!bplist_read_string(plist, plist_len, obj_offset,
                              peer->addresses[0],
                              BPLIST_PEER_ADDRESS_MAX))
        continue;
      peer->address_count = 1;
    } else if (!bplist_read_peer_dict(plist, plist_len, obj_offset,
                                      offset_table_offset, offset_size,
                                      ref_size, peer)) {
      return false;
    }
    usable++;
  }

  /* A non-empty list with no usable entry at all is still an invalid list;
   * the caller keeps its current peer set rather than clearing it. */
  if (count != 0 && usable == 0) return false;
  *out_count = usable;
  return true;
}

static bool bplist_find_data_in_dict(const uint8_t *plist, size_t plist_len,
                                     uint64_t dict_offset,
                                     uint64_t offset_table_offset,
                                     uint8_t offset_size, uint8_t ref_size,
                                     const char *key, uint8_t *out_data,
                                     size_t out_capacity, size_t *out_len) {
  if (dict_offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[dict_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0;
  size_t header_len = 0;
  if (!bplist_parse_count(plist, plist_len, dict_offset, &dict_size,
                          &header_len)) {
    return false;
  }

  size_t pos = dict_offset + header_len;
  if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, key_idx);

    char found_key[64];
    if (bplist_read_string(plist, plist_len, key_offset, found_key,
                           sizeof(found_key))) {
      if (strcmp(found_key, key) == 0) {
        uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
        uint64_t val_offset =
            bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, val_idx);
        return bplist_read_data(plist, plist_len, val_offset, out_data,
                                out_capacity, out_len);
      }
    }
  }

  return false;
}

static bool bplist_find_data_recursive(const uint8_t *plist, size_t plist_len,
                                       uint64_t obj_idx,
                                       uint64_t offset_table_offset,
                                       uint8_t offset_size, uint8_t ref_size,
                                       const char *key, uint8_t *out_data,
                                       size_t out_capacity, size_t *out_len,
                                       int depth, uint32_t *visits_left) {
  /* The depth limit alone does not bound the work. Objects are
   * referenced by index, so a dict whose N values all point back to itself is
   * visited N^10 times (Shairport 5.1: "potential infinite loop" in a parser);
   * the RTSP task would spin forever. Real SETUP plists have a few dozen
   * objects, so a global visit budget is far above any legitimate need. */
  if (depth > 10 || *visits_left == 0U) {
    return false;
  }
  --*visits_left;

  uint64_t offset =
      bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, obj_idx);
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;

  if (type == BPLIST_DICT) {
    size_t dict_size = 0;
    size_t header_len = 0;
    if (!bplist_parse_count(plist, plist_len, offset, &dict_size,
                            &header_len)) {
      return false;
    }

    size_t pos = offset + header_len;
    if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
      return false;
    }

    const uint8_t *key_refs = plist + pos;
    const uint8_t *val_refs = plist + pos + dict_size * ref_size;

    for (size_t i = 0; i < dict_size; i++) {
      uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
      uint64_t key_offset =
          bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, key_idx);

      char found_key[64];
      if (bplist_read_string(plist, plist_len, key_offset, found_key,
                             sizeof(found_key))) {
        if (strcmp(found_key, key) == 0) {
          uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
          uint64_t val_offset = bplist_get_offset(plist, plist_len, offset_table_offset,
                                                  offset_size, val_idx);
          return bplist_read_data(plist, plist_len, val_offset, out_data,
                                  out_capacity, out_len);
        }
      }
    }

    for (size_t i = 0; i < dict_size; i++) {
      uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
      if (bplist_find_data_recursive(
              plist, plist_len, val_idx, offset_table_offset, offset_size,
              ref_size, key, out_data, out_capacity, out_len, depth + 1,
                                     visits_left)) {
        return true;
      }
    }
  } else if (type == BPLIST_ARRAY || type == BPLIST_SET) {
    size_t count = 0;
    size_t header_len = 0;
    if (!bplist_parse_count(plist, plist_len, offset, &count, &header_len)) {
      return false;
    }

    size_t pos = offset + header_len;
    if (!bplist_span_ok(pos, count, ref_size, plist_len)) {
      return false;
    }

    for (size_t i = 0; i < count; i++) {
      uint64_t idx = read_be_int(plist + pos + i * ref_size, ref_size);
      if (bplist_find_data_recursive(plist, plist_len, idx, offset_table_offset,
                                     offset_size, ref_size, key, out_data,
                                     out_capacity, out_len, depth + 1,
                                     visits_left)) {
        return true;
      }
    }
  }

  return false;
}

bool bplist_find_data(const uint8_t *plist, size_t plist_len, const char *key,
                      uint8_t *out_data, size_t out_capacity, size_t *out_len) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  return bplist_find_data_in_dict(plist, plist_len, top_offset,
                                  offset_table_offset, offset_size, ref_size,
                                  key, out_data, out_capacity, out_len);
}

bool bplist_find_data_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, uint8_t *out_data,
                           size_t out_capacity, size_t *out_len) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  (void)num_objects;
  uint32_t visits_left = BPLIST_DEEP_SEARCH_MAX_VISITS;
  return bplist_find_data_recursive(plist, plist_len, top_object,
                                    offset_table_offset, offset_size, ref_size,
                                    key, out_data, out_capacity, out_len, 0,
                                    &visits_left);
}

/* Locate a value object by key anywhere below the top object.  This is the
 * small generic counterpart to bplist_find_data_recursive(), used by the
 * readable AirPlay 2 metadata trace.  Keep the same depth/visit budget so a
 * malformed self-referential plist cannot pin the RTSP task. */
static bool bplist_find_value_offset_recursive(
    const uint8_t *plist, size_t plist_len, uint64_t obj_idx,
    uint64_t offset_table_offset, uint8_t offset_size, uint8_t ref_size,
    const char *key, uint64_t *out_value_offset, int depth,
    uint32_t *visits_left) {
  if (depth > 10 || *visits_left == 0U) return false;
  --*visits_left;

  const uint64_t offset = bplist_get_offset(
      plist, plist_len, offset_table_offset, offset_size, obj_idx);
  if (offset >= plist_len) return false;

  const uint8_t type = plist[offset] & 0xF0;
  if (type == BPLIST_DICT) {
    size_t dict_size = 0;
    size_t header_len = 0;
    if (!bplist_parse_count(plist, plist_len, offset, &dict_size,
                            &header_len)) {
      return false;
    }

    const uint64_t pos = offset + header_len;
    if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
      return false;
    }
    const uint8_t *key_refs = plist + (size_t)pos;
    const uint8_t *val_refs = key_refs + dict_size * ref_size;

    for (size_t i = 0; i < dict_size; ++i) {
      const uint64_t key_idx =
          read_be_int(key_refs + i * ref_size, ref_size);
      const uint64_t key_offset = bplist_get_offset(
          plist, plist_len, offset_table_offset, offset_size, key_idx);
      char found_key[160];
      if (!bplist_read_string(plist, plist_len, key_offset, found_key,
                              sizeof(found_key)) ||
          strcmp(found_key, key) != 0) {
        continue;
      }
      const uint64_t val_idx =
          read_be_int(val_refs + i * ref_size, ref_size);
      const uint64_t val_offset = bplist_get_offset(
          plist, plist_len, offset_table_offset, offset_size, val_idx);
      if (val_offset >= plist_len) return false;
      *out_value_offset = val_offset;
      return true;
    }

    for (size_t i = 0; i < dict_size; ++i) {
      const uint64_t val_idx =
          read_be_int(val_refs + i * ref_size, ref_size);
      if (bplist_find_value_offset_recursive(
              plist, plist_len, val_idx, offset_table_offset, offset_size,
              ref_size, key, out_value_offset, depth + 1, visits_left)) {
        return true;
      }
    }
  } else if (type == BPLIST_ARRAY || type == BPLIST_SET) {
    size_t count = 0;
    size_t header_len = 0;
    if (!bplist_parse_count(plist, plist_len, offset, &count, &header_len)) {
      return false;
    }
    const uint64_t pos = offset + header_len;
    if (!bplist_span_ok(pos, count, ref_size, plist_len)) return false;
    for (size_t i = 0; i < count; ++i) {
      const uint64_t child_idx =
          read_be_int(plist + (size_t)pos + i * ref_size, ref_size);
      if (bplist_find_value_offset_recursive(
              plist, plist_len, child_idx, offset_table_offset, offset_size,
              ref_size, key, out_value_offset, depth + 1, visits_left)) {
        return true;
      }
    }
  }
  return false;
}

static bool bplist_find_value_offset_deep(const uint8_t *plist,
                                          size_t plist_len,
                                          const char *key,
                                          uint64_t *out_value_offset) {
  if (!plist || !key || !out_value_offset || plist_len < 40 ||
      memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }
  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object,
                            &offset_table_offset)) {
    return false;
  }
  (void)num_objects;
  uint32_t visits_left = BPLIST_DEEP_SEARCH_MAX_VISITS;
  return bplist_find_value_offset_recursive(
      plist, plist_len, top_object, offset_table_offset, offset_size, ref_size,
      key, out_value_offset, 0, &visits_left);
}

bool bplist_find_int_deep(const uint8_t *plist, size_t plist_len,
                          const char *key, int64_t *out_value) {
  uint64_t offset = 0;
  return out_value &&
         bplist_find_value_offset_deep(plist, plist_len, key, &offset) &&
         bplist_read_int(plist, plist_len, offset, out_value);
}

bool bplist_find_bool_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, bool *out_value) {
  uint64_t offset = 0;
  if (!out_value ||
      !bplist_find_value_offset_deep(plist, plist_len, key, &offset) ||
      offset >= plist_len) {
    return false;
  }
  if (plist[offset] == 0x08) {
    *out_value = false;
    return true;
  }
  if (plist[offset] == 0x09) {
    *out_value = true;
    return true;
  }
  return false;
}

bool bplist_find_real_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, double *out_value) {
  uint64_t offset = 0;
  if (!out_value ||
      !bplist_find_value_offset_deep(plist, plist_len, key, &offset)) {
    return false;
  }
  if (bplist_read_real(plist, plist_len, offset, out_value)) return true;
  int64_t int_value = 0;
  if (bplist_read_int(plist, plist_len, offset, &int_value)) {
    *out_value = (double)int_value;
    return true;
  }
  return false;
}

bool bplist_find_string_deep(const uint8_t *plist, size_t plist_len,
                             const char *key, char *out_str,
                             size_t out_capacity) {
  uint64_t offset = 0;
  return out_str && out_capacity > 0 &&
         bplist_find_value_offset_deep(plist, plist_len, key, &offset) &&
         bplist_read_string(plist, plist_len, offset, out_str, out_capacity);
}

bool bplist_find_data_len_deep(const uint8_t *plist, size_t plist_len,
                               const char *key, size_t *out_len) {
  uint64_t offset = 0;
  return out_len &&
         bplist_find_value_offset_deep(plist, plist_len, key, &offset) &&
         bplist_read_data_len(plist, plist_len, offset, out_len);
}


static bool bplist_find_data_array_recursive(
    const uint8_t *plist, size_t plist_len, uint64_t obj_idx,
    uint64_t offset_table_offset, uint8_t offset_size, uint8_t ref_size,
    const char *key, size_t item_index, uint8_t *out_data,
    size_t out_capacity, size_t *out_len, size_t *out_count, int depth,
    uint32_t *visits_left) {
  if (depth > 10 || *visits_left == 0U) return false;
  --*visits_left;

  const uint64_t offset = bplist_get_offset(
      plist, plist_len, offset_table_offset, offset_size, obj_idx);
  if (offset >= plist_len) return false;

  const uint8_t type = plist[offset] & 0xF0;
  if (type == BPLIST_DICT) {
    size_t dict_size = 0;
    size_t header_len = 0;
    if (!bplist_parse_count(plist, plist_len, offset, &dict_size,
                            &header_len)) {
      return false;
    }

    const uint64_t pos = offset + header_len;
    if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
      return false;
    }

    const uint8_t *key_refs = plist + pos;
    const uint8_t *val_refs = key_refs + dict_size * ref_size;
    for (size_t i = 0; i < dict_size; ++i) {
      const uint64_t key_idx =
          read_be_int(key_refs + i * ref_size, ref_size);
      const uint64_t key_offset = bplist_get_offset(
          plist, plist_len, offset_table_offset, offset_size, key_idx);
      char found_key[64];
      if (!bplist_read_string(plist, plist_len, key_offset, found_key,
                              sizeof(found_key)) ||
          strcmp(found_key, key) != 0) {
        continue;
      }

      const uint64_t val_idx =
          read_be_int(val_refs + i * ref_size, ref_size);
      const uint64_t val_offset = bplist_get_offset(
          plist, plist_len, offset_table_offset, offset_size, val_idx);
      if (val_offset >= plist_len) return false;
      const uint8_t val_type = plist[val_offset] & 0xF0;
      if (val_type != BPLIST_ARRAY && val_type != BPLIST_SET) return false;

      size_t count = 0;
      size_t array_header_len = 0;
      if (!bplist_parse_count(plist, plist_len, val_offset, &count,
                              &array_header_len)) {
        return false;
      }
      *out_count = count;
      if (item_index >= count) return false;

      const uint64_t array_pos = val_offset + array_header_len;
      if (!bplist_span_ok(array_pos, count, ref_size, plist_len)) return false;
      const uint64_t item_idx =
          read_be_int(plist + array_pos + item_index * ref_size, ref_size);
      const uint64_t item_offset = bplist_get_offset(
          plist, plist_len, offset_table_offset, offset_size, item_idx);
      return bplist_read_data(plist, plist_len, item_offset, out_data,
                              out_capacity, out_len);
    }

    for (size_t i = 0; i < dict_size; ++i) {
      const uint64_t val_idx =
          read_be_int(val_refs + i * ref_size, ref_size);
      if (bplist_find_data_array_recursive(
              plist, plist_len, val_idx, offset_table_offset, offset_size,
              ref_size, key, item_index, out_data, out_capacity, out_len,
              out_count, depth + 1, visits_left)) {
        return true;
      }
    }
  } else if (type == BPLIST_ARRAY || type == BPLIST_SET) {
    size_t count = 0;
    size_t header_len = 0;
    if (!bplist_parse_count(plist, plist_len, offset, &count, &header_len)) {
      return false;
    }
    const uint64_t pos = offset + header_len;
    if (!bplist_span_ok(pos, count, ref_size, plist_len)) return false;

    for (size_t i = 0; i < count; ++i) {
      const uint64_t idx =
          read_be_int(plist + pos + i * ref_size, ref_size);
      if (bplist_find_data_array_recursive(
              plist, plist_len, idx, offset_table_offset, offset_size,
              ref_size, key, item_index, out_data, out_capacity, out_len,
              out_count, depth + 1, visits_left)) {
        return true;
      }
    }
  }
  return false;
}

bool bplist_get_data_array_item_deep(const uint8_t *plist, size_t plist_len,
                                     const char *key, size_t item_index,
                                     uint8_t *out_data, size_t out_capacity,
                                     size_t *out_len, size_t *out_count) {
  if (!plist || !key || !out_data || !out_len || !out_count ||
      plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }
  *out_len = 0;
  *out_count = 0;

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object,
                            &offset_table_offset)) {
    return false;
  }

  (void)num_objects;
  uint32_t visits_left = BPLIST_DEEP_SEARCH_MAX_VISITS;
  return bplist_find_data_array_recursive(
      plist, plist_len, top_object, offset_table_offset, offset_size, ref_size,
      key, item_index, out_data, out_capacity, out_len, out_count, 0,
      &visits_left);
}

bool bplist_find_int(const uint8_t *plist, size_t plist_len, const char *key,
                     int64_t *out_value) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[top_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0;
  size_t dict_header_len = 0;
  if (!bplist_parse_count(plist, plist_len, top_offset, &dict_size,
                          &dict_header_len)) {
    return false;
  }
  size_t pos = top_offset + dict_header_len;

  if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, key_idx);

    char found_key[64];
    if (bplist_read_string(plist, plist_len, key_offset, found_key,
                           sizeof(found_key))) {
      if (strcmp(found_key, key) == 0) {
        uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
        uint64_t val_offset =
            bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, val_idx);
        return bplist_read_int(plist, plist_len, val_offset, out_value);
      }
    }
  }

  return false;
}

bool bplist_find_bool(const uint8_t *plist, size_t plist_len,
                      const char *key, bool *out_value) {
  if (!out_value || plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }
  (void)num_objects;

  const uint64_t top_offset = bplist_get_offset(
      plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len || (plist[top_offset] & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0, header_len = 0;
  if (!bplist_parse_count(plist, plist_len, top_offset, &dict_size,
                          &header_len)) {
    return false;
  }
  const uint64_t pos64 = top_offset + header_len;
  if (!bplist_span_ok(pos64, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }
  const size_t pos = (size_t)pos64;
  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; ++i) {
    const uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    const uint64_t key_off = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, key_idx);
    char found_key[64];
    if (!bplist_read_string(plist, plist_len, key_off, found_key,
                            sizeof(found_key)) ||
        strcmp(found_key, key) != 0) {
      continue;
    }
    const uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
    const uint64_t val_off = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, val_idx);
    if (val_off >= plist_len) return false;
    if (plist[val_off] == 0x08) {
      *out_value = false;
      return true;
    }
    if (plist[val_off] == 0x09) {
      *out_value = true;
      return true;
    }
    return false;
  }
  return false;
}

bool bplist_find_real(const uint8_t *plist, size_t plist_len, const char *key,
                      double *out_value) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[top_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0;
  size_t dict_header_len = 0;
  if (!bplist_parse_count(plist, plist_len, top_offset, &dict_size,
                          &dict_header_len)) {
    return false;
  }
  size_t pos = top_offset + dict_header_len;

  if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, key_idx);

    char found_key[64];
    if (bplist_read_string(plist, plist_len, key_offset, found_key,
                           sizeof(found_key))) {
      if (strcmp(found_key, key) == 0) {
        uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
        uint64_t val_offset =
            bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, val_idx);
        if (bplist_read_real(plist, plist_len, val_offset, out_value)) {
          return true;
        }
        int64_t int_val = 0;
        if (bplist_read_int(plist, plist_len, val_offset, &int_val)) {
          *out_value = (double)int_val;
          return true;
        }
        return false;
      }
    }
  }

  return false;
}

bool bplist_find_string(const uint8_t *plist, size_t plist_len, const char *key,
                        char *out_str, size_t out_capacity) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[top_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0;
  size_t dict_header_len = 0;
  if (!bplist_parse_count(plist, plist_len, top_offset, &dict_size,
                          &dict_header_len)) {
    return false;
  }
  size_t pos = top_offset + dict_header_len;

  if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, key_idx);

    char found_key[64];
    if (bplist_read_string(plist, plist_len, key_offset, found_key,
                           sizeof(found_key))) {
      if (strcmp(found_key, key) == 0) {
        uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
        uint64_t val_offset =
            bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, val_idx);
        return bplist_read_string(plist, plist_len, val_offset, out_str,
                                  out_capacity);
      }
    }
  }

  return false;
}

bool bplist_get_streams_count(const uint8_t *plist, size_t plist_len,
                              size_t *count) {
  if (!count) {
    return false;
  }

  *count = 0;

  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  size_t streams_key_len = 0;
  uint64_t streams_key_offset = 0;
  for (uint64_t i = 0; i < num_objects; i++) {
    uint64_t offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, i);
    if (offset >= plist_len) {
      continue;
    }
    char key[16];
    if (bplist_read_string(plist, plist_len, offset, key, sizeof(key))) {
      if (strcmp(key, "streams") == 0) {
        streams_key_offset = offset;
        if (!bplist_read_string_len(plist, plist_len, offset,
                                    &streams_key_len)) {
          return false;
        }
        break;
      }
    }
  }

  if (streams_key_len == 0) {
    return false;
  }

  uint64_t top_dict_offset = top_offset;
  uint8_t marker = plist[top_dict_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0;
  size_t dict_header_len = 0;
  if (!bplist_parse_count(plist, plist_len, top_dict_offset, &dict_size,
                          &dict_header_len)) {
    return false;
  }
  size_t pos = top_dict_offset + dict_header_len;

  if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, key_idx);

    if (key_offset == streams_key_offset) {
      uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
      uint64_t val_offset =
          bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, val_idx);

      if (val_offset >= plist_len) return false;
      uint8_t val_marker = plist[val_offset];
      if ((val_marker & 0xF0) != BPLIST_ARRAY) {
        return false;
      }

      size_t array_count = 0;
      size_t header_len = 0;
      if (!bplist_parse_count(plist, plist_len, val_offset, &array_count,
                              &header_len)) {
        return false;
      }
      /* A count whose references cannot fit in the body is bogus, and
       * callers loop over it (bplist_find_stream_crypto). */
      if (!bplist_span_ok(val_offset + header_len, array_count, ref_size,
                          plist_len)) {
        return false;
      }

      *count = array_count;
      return true;
    }
  }

  return false;
}

bool bplist_get_stream_info(const uint8_t *plist, size_t plist_len,
                            size_t index, int64_t *type, size_t *ekey_len,
                            size_t *eiv_len, size_t *shk_len) {
  if (!type) {
    return false;
  }

  *type = -1;
  if (ekey_len) {
    *ekey_len = 0;
  }
  if (eiv_len) {
    *eiv_len = 0;
  }
  if (shk_len) {
    *shk_len = 0;
  }

  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  size_t streams_key_len = 0;
  uint64_t streams_key_offset = 0;
  for (uint64_t i = 0; i < num_objects; i++) {
    uint64_t offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, i);
    if (offset >= plist_len) {
      continue;
    }
    char key[16];
    if (bplist_read_string(plist, plist_len, offset, key, sizeof(key))) {
      if (strcmp(key, "streams") == 0) {
        streams_key_offset = offset;
        if (!bplist_read_string_len(plist, plist_len, offset,
                                    &streams_key_len)) {
          return false;
        }
        break;
      }
    }
  }

  if (streams_key_len == 0) {
    return false;
  }

  uint64_t top_dict_offset = top_offset;
  uint8_t marker = plist[top_dict_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0;
  size_t dict_header_len = 0;
  if (!bplist_parse_count(plist, plist_len, top_dict_offset, &dict_size,
                          &dict_header_len)) {
    return false;
  }
  size_t pos = top_dict_offset + dict_header_len;

  if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, key_idx);

    if (key_offset == streams_key_offset) {
      uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
      uint64_t val_offset =
          bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, val_idx);

      if (val_offset >= plist_len) return false;
      uint8_t val_marker = plist[val_offset];
      if ((val_marker & 0xF0) != BPLIST_ARRAY) {
        return false;
      }

      size_t array_count = 0;
      size_t header_len = 0;
      if (!bplist_parse_count(plist, plist_len, val_offset, &array_count,
                              &header_len)) {
        return false;
      }

      if (index >= array_count) {
        return false;
      }

      size_t array_pos = val_offset + header_len;
      if (!bplist_span_ok(array_pos, array_count, ref_size, plist_len)) {
        return false;
      }

      uint64_t stream_idx =
          read_be_int(plist + array_pos + index * ref_size, ref_size);
      uint64_t stream_offset = bplist_get_offset(plist, plist_len, offset_table_offset,
                                                 offset_size, stream_idx);

      if (stream_offset >= plist_len) return false;
      uint8_t stream_marker = plist[stream_offset];
      if ((stream_marker & 0xF0) != BPLIST_DICT) {
        return false;
      }

      size_t stream_dict_size = 0;
      size_t stream_header_len = 0;
      if (!bplist_parse_count(plist, plist_len, stream_offset,
                              &stream_dict_size, &stream_header_len)) {
        return false;
      }

      size_t stream_pos = stream_offset + stream_header_len;
      if (!bplist_span_ok(stream_pos, stream_dict_size, 2U * (uint64_t)ref_size, plist_len)) {
        return false;
      }

      const uint8_t *stream_key_refs = plist + stream_pos;
      const uint8_t *stream_val_refs =
          plist + stream_pos + stream_dict_size * ref_size;

      for (size_t j = 0; j < stream_dict_size; j++) {
        uint64_t stream_key_idx =
            read_be_int(stream_key_refs + j * ref_size, ref_size);
        uint64_t stream_key_offset = bplist_get_offset(
            plist, plist_len, offset_table_offset, offset_size, stream_key_idx);

        char stream_key[32];
        if (!bplist_read_string(plist, plist_len, stream_key_offset, stream_key,
                                sizeof(stream_key))) {
          continue;
        }

        uint64_t stream_val_idx =
            read_be_int(stream_val_refs + j * ref_size, ref_size);
        uint64_t stream_val_offset = bplist_get_offset(
            plist, plist_len, offset_table_offset, offset_size, stream_val_idx);

        if (strcmp(stream_key, "type") == 0) {
          int64_t type_val = 0;
          if (bplist_read_int(plist, plist_len, stream_val_offset, &type_val)) {
            *type = type_val;
          }
        } else if (strcmp(stream_key, "ekey") == 0 && ekey_len) {
          bplist_read_data_len(plist, plist_len, stream_val_offset, ekey_len);
        } else if (strcmp(stream_key, "eiv") == 0 && eiv_len) {
          bplist_read_data_len(plist, plist_len, stream_val_offset, eiv_len);
        } else if (strcmp(stream_key, "shk") == 0 && shk_len) {
          bplist_read_data_len(plist, plist_len, stream_val_offset, shk_len);
        }
      }

      return (*type != -1);
    }
  }

  return false;
}

bool bplist_get_stream_kv_info(const uint8_t *plist, size_t plist_len,
                               size_t index, bplist_kv_info_t *out,
                               size_t out_capacity, size_t *out_count) {
  if (!out || out_capacity == 0 || !out_count) {
    return false;
  }

  *out_count = 0;

  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  size_t streams_key_len = 0;
  uint64_t streams_key_offset = 0;
  for (uint64_t i = 0; i < num_objects; i++) {
    uint64_t offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, i);
    if (offset >= plist_len) {
      continue;
    }
    char key[16];
    if (bplist_read_string(plist, plist_len, offset, key, sizeof(key))) {
      if (strcmp(key, "streams") == 0) {
        streams_key_offset = offset;
        if (!bplist_read_string_len(plist, plist_len, offset,
                                    &streams_key_len)) {
          return false;
        }
        break;
      }
    }
  }

  if (streams_key_len == 0) {
    return false;
  }

  uint64_t top_dict_offset = top_offset;
  uint8_t marker = plist[top_dict_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0;
  size_t dict_header_len = 0;
  if (!bplist_parse_count(plist, plist_len, top_dict_offset, &dict_size,
                          &dict_header_len)) {
    return false;
  }
  size_t pos = top_dict_offset + dict_header_len;

  if (!bplist_span_ok(pos, dict_size, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, key_idx);

    if (key_offset == streams_key_offset) {
      uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
      uint64_t val_offset =
          bplist_get_offset(plist, plist_len, offset_table_offset, offset_size, val_idx);

      if (val_offset >= plist_len) return false;
      uint8_t val_marker = plist[val_offset];
      if ((val_marker & 0xF0) != BPLIST_ARRAY) {
        return false;
      }

      size_t array_count = 0;
      size_t header_len = 0;
      if (!bplist_parse_count(plist, plist_len, val_offset, &array_count,
                              &header_len)) {
        return false;
      }

      if (index >= array_count) {
        return false;
      }

      size_t array_pos = val_offset + header_len;
      if (!bplist_span_ok(array_pos, array_count, ref_size, plist_len)) {
        return false;
      }

      uint64_t stream_idx =
          read_be_int(plist + array_pos + index * ref_size, ref_size);
      uint64_t stream_offset = bplist_get_offset(plist, plist_len, offset_table_offset,
                                                 offset_size, stream_idx);

      if (stream_offset >= plist_len) return false;
      uint8_t stream_marker = plist[stream_offset];
      if ((stream_marker & 0xF0) != BPLIST_DICT) {
        return false;
      }

      size_t stream_dict_size = 0;
      size_t stream_header_len = 0;
      if (!bplist_parse_count(plist, plist_len, stream_offset,
                              &stream_dict_size, &stream_header_len)) {
        return false;
      }

      size_t stream_pos = stream_offset + stream_header_len;
      if (!bplist_span_ok(stream_pos, stream_dict_size, 2U * (uint64_t)ref_size, plist_len)) {
        return false;
      }

      const uint8_t *stream_key_refs = plist + stream_pos;
      const uint8_t *stream_val_refs =
          plist + stream_pos + stream_dict_size * ref_size;

      for (size_t j = 0; j < stream_dict_size && *out_count < out_capacity;
           j++) {
        uint64_t stream_key_idx =
            read_be_int(stream_key_refs + j * ref_size, ref_size);
        uint64_t stream_key_offset = bplist_get_offset(
            plist, plist_len, offset_table_offset, offset_size, stream_key_idx);

        char stream_key[64];
        if (!bplist_read_string(plist, plist_len, stream_key_offset, stream_key,
                                sizeof(stream_key))) {
          continue;
        }

        uint64_t stream_val_idx =
            read_be_int(stream_val_refs + j * ref_size, ref_size);
        uint64_t stream_val_offset = bplist_get_offset(
            plist, plist_len, offset_table_offset, offset_size, stream_val_idx);

        bplist_kv_info_t *info = &out[*out_count];
        memset(info, 0, sizeof(*info));
        strlcpy(info->key, stream_key, sizeof(info->key));

        if (stream_val_offset >= plist_len) return false;
        uint8_t stream_val_marker = plist[stream_val_offset];
        uint8_t stream_val_type = stream_val_marker & 0xF0;

        if (stream_val_type == BPLIST_INT) {
          info->value_type = BPLIST_VALUE_INT;
          int64_t int_val = 0;
          if (bplist_read_int(plist, plist_len, stream_val_offset, &int_val)) {
            info->int_value = int_val;
          }
        } else if (stream_val_type == BPLIST_DATA) {
          info->value_type = BPLIST_VALUE_DATA;
          size_t len = 0;
          if (bplist_read_data_len(plist, plist_len, stream_val_offset, &len)) {
            info->value_len = len;
          }
        } else if (stream_val_type == BPLIST_STRING ||
                   stream_val_type == BPLIST_UNICODE) {
          info->value_type = BPLIST_VALUE_STRING;
          size_t len = 0;
          if (bplist_read_string_len(plist, plist_len, stream_val_offset,
                                     &len)) {
            info->value_len = len;
          }
        } else if (stream_val_type == BPLIST_UID) {
          info->value_type = BPLIST_VALUE_UID;
        } else if (stream_val_type == BPLIST_ARRAY) {
          info->value_type = BPLIST_VALUE_ARRAY;
        } else if (stream_val_type == BPLIST_DICT) {
          info->value_type = BPLIST_VALUE_DICT;
        }

        (*out_count)++;
      }

      return (*out_count > 0);
    }
  }

  return false;
}

bool bplist_get_stream_connection_info(
    const uint8_t *plist, size_t plist_len, size_t index,
    bplist_stream_connection_info_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));

  uint64_t sc_offset = 0;
  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_get_stream_value_offset(
          plist, plist_len, index, "streamConnections", &sc_offset,
          &offset_size, &ref_size, &offset_table_offset)) {
    return false;
  }
  if (sc_offset >= plist_len || (plist[sc_offset] & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t count = 0, header_len = 0;
  if (!bplist_parse_count(plist, plist_len, sc_offset, &count, &header_len)) {
    return false;
  }
  const uint64_t pos64 = sc_offset + header_len;
  if (!bplist_span_ok(pos64, count, 2U * (uint64_t)ref_size, plist_len)) {
    return false;
  }
  const size_t pos = (size_t)pos64;
  const uint8_t *keys = plist + pos;
  const uint8_t *vals = plist + pos + count * ref_size;

  for (size_t i = 0; i < count; ++i) {
    const uint64_t key_idx = read_be_int(keys + i * ref_size, ref_size);
    const uint64_t key_off = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, key_idx);
    char key[64];
    if (!bplist_read_string(plist, plist_len, key_off, key, sizeof(key))) {
      continue;
    }

    if (strcmp(key, "streamConnectionTypeRTP") == 0) {
      out->has_rtp = true;
      continue;
    }
    if (strcmp(key, "streamConnectionTypeRTCP") == 0) {
      out->has_rtcp = true;
      continue;
    }
    if (strcmp(key, "streamConnectionTypeMediaDataControl") != 0) {
      continue;
    }

    out->has_media_data_control = true;
    const uint64_t val_idx = read_be_int(vals + i * ref_size, ref_size);
    const uint64_t val_off = bplist_get_offset(
        plist, plist_len, offset_table_offset, offset_size, val_idx);
    if (val_off >= plist_len || (plist[val_off] & 0xF0) != BPLIST_DICT) {
      continue;
    }

    size_t mdc_count = 0, mdc_header_len = 0;
    if (!bplist_parse_count(plist, plist_len, val_off, &mdc_count,
                            &mdc_header_len)) {
      continue;
    }
    const uint64_t mdc_pos64 = val_off + mdc_header_len;
    if (!bplist_span_ok(mdc_pos64, mdc_count,
                        2U * (uint64_t)ref_size, plist_len)) {
      continue;
    }
    const size_t mdc_pos = (size_t)mdc_pos64;
    const uint8_t *mdc_keys = plist + mdc_pos;
    const uint8_t *mdc_vals = plist + mdc_pos + mdc_count * ref_size;
    for (size_t j = 0; j < mdc_count; ++j) {
      const uint64_t mk_idx = read_be_int(mdc_keys + j * ref_size, ref_size);
      const uint64_t mk_off = bplist_get_offset(
          plist, plist_len, offset_table_offset, offset_size, mk_idx);
      char mkey[64];
      if (!bplist_read_string(plist, plist_len, mk_off, mkey, sizeof(mkey)) ||
          strcmp(mkey, "streamConnectionKeyEncryptionSeed") != 0) {
        continue;
      }
      const uint64_t mv_idx = read_be_int(mdc_vals + j * ref_size, ref_size);
      const uint64_t mv_off = bplist_get_offset(
          plist, plist_len, offset_table_offset, offset_size, mv_idx);
      int64_t signed_seed = 0;
      if (bplist_read_int(plist, plist_len, mv_off, &signed_seed)) {
        out->media_data_control_seed = (uint64_t)signed_seed;
        out->has_media_data_control_seed = true;
      }
      break;
    }
  }

  return true;
}

bool bplist_get_stream_connection_types(const uint8_t *plist,
                                        size_t plist_len, size_t index,
                                        bool *has_rtp, bool *has_rtcp,
                                        bool *has_media_data_control) {
  if (has_rtp) *has_rtp = false;
  if (has_rtcp) *has_rtcp = false;
  if (has_media_data_control) *has_media_data_control = false;

  bplist_stream_connection_info_t info;
  if (!bplist_get_stream_connection_info(plist, plist_len, index, &info)) {
    return false;
  }
  if (has_rtp) *has_rtp = info.has_rtp;
  if (has_rtcp) *has_rtcp = info.has_rtcp;
  if (has_media_data_control) {
    *has_media_data_control = info.has_media_data_control;
  }
  return true;
}

bool bplist_find_stream_crypto(const uint8_t *plist, size_t plist_len,
                               int64_t stream_type, uint8_t *ekey,
                               size_t ekey_capacity, size_t *ekey_len,
                               uint8_t *eiv, size_t eiv_capacity,
                               size_t *eiv_len, uint8_t *shk,
                               size_t shk_capacity, size_t *shk_len) {
  bool found = false;

  if (ekey_len) {
    *ekey_len = 0;
  }
  if (eiv_len) {
    *eiv_len = 0;
  }
  if (shk_len) {
    *shk_len = 0;
  }

  size_t stream_count = 0;
  if (!bplist_get_streams_count(plist, plist_len, &stream_count)) {
    return false;
  }

  for (size_t i = 0; i < stream_count; i++) {
    int64_t type = -1;
    size_t local_ekey_len = 0;
    size_t local_eiv_len = 0;
    size_t local_shk_len = 0;

    if (!bplist_get_stream_info(plist, plist_len, i, &type, &local_ekey_len,
                                &local_eiv_len, &local_shk_len)) {
      continue;
    }

    if (type != stream_type) {
      continue;
    }

    uint8_t temp_buf[512];
    size_t temp_len = 0;

    if (ekey && local_ekey_len > 0 && ekey_len) {
      if (bplist_find_data(plist, plist_len, "ekey", temp_buf, sizeof(temp_buf),
                           &temp_len)) {
        size_t copy_len = temp_len < ekey_capacity ? temp_len : ekey_capacity;
        memcpy(ekey, temp_buf, copy_len);
        *ekey_len = copy_len;
        found = true;
      }
    }

    if (eiv && local_eiv_len > 0 && eiv_len) {
      if (bplist_find_data(plist, plist_len, "eiv", temp_buf, sizeof(temp_buf),
                           &temp_len)) {
        size_t copy_len = temp_len < eiv_capacity ? temp_len : eiv_capacity;
        memcpy(eiv, temp_buf, copy_len);
        *eiv_len = copy_len;
        found = true;
      }
    }

    if (shk && local_shk_len > 0 && shk_len) {
      if (bplist_find_data(plist, plist_len, "shk", temp_buf, sizeof(temp_buf),
                           &temp_len)) {
        size_t copy_len = temp_len < shk_capacity ? temp_len : shk_capacity;
        memcpy(shk, temp_buf, copy_len);
        // Report the actual plist data length, not the truncated copy length.
        // The SETUP handler must be able to reject an shk that is not exactly
        // 32 bytes, including an oversized value.
        *shk_len = temp_len;
        found = true;
      }
    }

    break;
  }

  return found;
}

/* ---- Protocol trace: compact text rendering of a bplist -------------------
 * Used by bounded diagnostics and MediaRemote state parsing.
 * Same bounds as the lookups above: depth limit, global visit budget and
 * overflow-free span checks; output is always NUL-terminated and truncated
 * at out_capacity. */

#define BPLIST_DESCRIBE_MAX_DEPTH 6

typedef struct {
  char *out;
  size_t cap;
  size_t pos;
} bplist_text_t;

static bool text_full(const bplist_text_t *t) { return t->pos + 1 >= t->cap; }

static void text_put(bplist_text_t *t, const char *s) {
  while (*s && !text_full(t)) t->out[t->pos++] = *s++;
  t->out[t->pos] = '\0';
}

static void bplist_describe_obj(const uint8_t *plist, size_t plist_len,
                                uint64_t obj_idx, uint64_t offset_table_offset,
                                uint8_t offset_size, uint8_t ref_size,
                                bplist_text_t *t, int depth,
                                uint32_t *visits_left) {
  if (text_full(t)) return;
  if (depth > BPLIST_DESCRIBE_MAX_DEPTH || *visits_left == 0U) {
    text_put(t, "...");
    return;
  }
  --*visits_left;

  const uint64_t offset = bplist_get_offset(plist, plist_len,
                                            offset_table_offset, offset_size,
                                            obj_idx);
  if (offset >= plist_len) {
    text_put(t, "?");
    return;
  }
  const uint8_t marker = plist[offset];
  /* Protocol discovery includes long MediaRemote option-key names (50+ chars).
   * Keep enough room to render them instead of replacing them with "?". */
  char buf[128];

  switch (marker & 0xF0) {
    case 0x00:
      text_put(t, marker == 0x09 ? "true" : marker == 0x08 ? "false" : "null");
      return;
    case BPLIST_INT: {
      int64_t v = 0;
      if (bplist_read_int(plist, plist_len, offset, &v)) {
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        text_put(t, buf);
      } else {
        text_put(t, "?int");
      }
      return;
    }
    case BPLIST_REAL: {
      double d = 0.0;
      if (bplist_read_real(plist, plist_len, offset, &d)) {
        snprintf(buf, sizeof(buf), "%g", d);
        text_put(t, buf);
      } else {
        text_put(t, "?real");
      }
      return;
    }
    case BPLIST_DATA: {
      size_t n = 0;
      if (bplist_read_data_len(plist, plist_len, offset, &n)) {
        snprintf(buf, sizeof(buf), "<data %u>", (unsigned)n);
        text_put(t, buf);
      } else {
        text_put(t, "?data");
      }
      return;
    }
    case BPLIST_STRING:
    case BPLIST_UNICODE: {
      size_t n = 0;
      if (bplist_read_string(plist, plist_len, offset, buf, sizeof(buf))) {
        text_put(t, "\"");
        text_put(t, buf);
        text_put(t, "\"");
      } else if (bplist_read_string_len(plist, plist_len, offset, &n)) {
        snprintf(buf, sizeof(buf), "<str %u>", (unsigned)n);
        text_put(t, buf);
      } else {
        text_put(t, "?str");
      }
      return;
    }
    case BPLIST_ARRAY:
    case BPLIST_SET: {
      size_t count = 0;
      size_t header_len = 0;
      if (!bplist_parse_count(plist, plist_len, offset, &count, &header_len) ||
          !bplist_span_ok(offset + header_len, count, ref_size, plist_len)) {
        text_put(t, "?array");
        return;
      }
      const uint8_t *refs = plist + offset + header_len;
      text_put(t, "[");
      for (size_t i = 0; i < count && !text_full(t); i++) {
        if (i) text_put(t, ", ");
        bplist_describe_obj(plist, plist_len,
                            read_be_int(refs + i * ref_size, ref_size),
                            offset_table_offset, offset_size, ref_size, t,
                            depth + 1, visits_left);
      }
      text_put(t, "]");
      return;
    }
    case BPLIST_DICT: {
      size_t count = 0;
      size_t header_len = 0;
      if (!bplist_parse_count(plist, plist_len, offset, &count, &header_len) ||
          !bplist_span_ok(offset + header_len, count, 2U * (uint64_t)ref_size,
                          plist_len)) {
        text_put(t, "?dict");
        return;
      }
      const uint8_t *key_refs = plist + offset + header_len;
      const uint8_t *val_refs = key_refs + count * ref_size;
      text_put(t, "{");
      for (size_t i = 0; i < count && !text_full(t); i++) {
        if (i) text_put(t, ", ");
        const uint64_t key_offset = bplist_get_offset(
            plist, plist_len, offset_table_offset, offset_size,
            read_be_int(key_refs + i * ref_size, ref_size));
        text_put(t, bplist_read_string(plist, plist_len, key_offset, buf,
                                       sizeof(buf))
                        ? buf
                        : "?");
        text_put(t, "=");
        bplist_describe_obj(plist, plist_len,
                            read_be_int(val_refs + i * ref_size, ref_size),
                            offset_table_offset, offset_size, ref_size, t,
                            depth + 1, visits_left);
      }
      text_put(t, "}");
      return;
    }
    default:
      snprintf(buf, sizeof(buf), "<0x%02x>", (unsigned)marker);
      text_put(t, buf);
      return;
  }
}

size_t bplist_describe(const uint8_t *plist, size_t plist_len, char *out,
                       size_t out_capacity) {
  if (!out || out_capacity == 0) return 0;
  out[0] = '\0';
  if (!plist || plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return 0;
  }
  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return 0;
  }
  uint32_t visits_left = BPLIST_DEEP_SEARCH_MAX_VISITS;
  bplist_text_t t = {out, out_capacity, 0};
  bplist_describe_obj(plist, plist_len, top_object, offset_table_offset,
                      offset_size, ref_size, &t, 0, &visits_left);
  return t.pos;
}
