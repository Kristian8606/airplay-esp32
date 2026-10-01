#include "mrp_inspect.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "plist.h"

static const char *TAG = "mrp_inspect";

#define MRP_DATA_MAX 16384U    /* DataStream capture limit */
#define MRP_HEX_DUMP_MAX 2048U /* full hex for the handshake-sized blobs */
#define MRP_MAX_DEPTH 5U
#define MRP_MAX_FIELDS 160U
#define MRP_STR_SHOW 160U
#define MRP_MAX_FIELD_NUMBER 4096U

/* ---- protobuf primitives ------------------------------------------------ */

static bool pb_varint(const uint8_t *b, size_t n, size_t *pos, uint64_t *out) {
  uint64_t v = 0;
  unsigned shift = 0;
  size_t p = *pos;
  while (p < n && shift < 64U) {
    const uint8_t c = b[p++];
    v |= (uint64_t)(c & 0x7fU) << shift;
    if ((c & 0x80U) == 0) {
      *pos = p;
      *out = v;
      return true;
    }
    shift += 7U;
  }
  return false;
}

/* One protobuf field. `data/len` are set for wire types 1, 2 and 5. */
typedef struct {
  uint32_t field;
  uint8_t wire;
  uint64_t varint;
  const uint8_t *data;
  size_t len;
} pb_field_t;

static bool pb_next(const uint8_t *b, size_t n, size_t *pos, pb_field_t *f) {
  uint64_t key = 0;
  if (!pb_varint(b, n, pos, &key)) return false;
  f->field = (uint32_t)(key >> 3);
  f->wire = (uint8_t)(key & 7U);
  f->varint = 0;
  f->data = NULL;
  f->len = 0;
  if (f->field == 0 || (key >> 3) > 0x1fffffffULL) return false;
  switch (f->wire) {
    case 0:
      return pb_varint(b, n, pos, &f->varint);
    case 1:
      if (n - *pos < 8U) return false;
      f->data = b + *pos;
      f->len = 8U;
      *pos += 8U;
      return true;
    case 2: {
      uint64_t l = 0;
      if (!pb_varint(b, n, pos, &l) || l > n - *pos) return false;
      f->data = b + *pos;
      f->len = (size_t)l;
      *pos += (size_t)l;
      return true;
    }
    case 5:
      if (n - *pos < 4U) return false;
      f->data = b + *pos;
      f->len = 4U;
      *pos += 4U;
      return true;
    default:
      return false;
  }
}

/* True when the whole buffer is a sequence of well-formed fields. */
static bool pb_valid(const uint8_t *b, size_t n) {
  if (!b || n == 0) return false;
  size_t pos = 0;
  unsigned count = 0;
  pb_field_t f;
  while (pos < n) {
    if (!pb_next(b, n, &pos, &f) || ++count > MRP_MAX_FIELDS) return false;
    /* Real MediaRemote field numbers are small; random bytes (UUIDs, MAC
     * addresses) that happen to parse produce huge ones. */
    if (f.field > MRP_MAX_FIELD_NUMBER) return false;
  }
  return pos == n;
}

static bool printable(const uint8_t *b, size_t n) {
  if (!b || n == 0) return false;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = b[i];
    if (c < 0x20 || c == 0x7f) return false; /* allow UTF-8 >= 0x80 */
  }
  return true;
}

static const char *mrp_type_name(uint64_t t) {
  switch (t) {
    case 1: return "SEND_COMMAND";
    case 2: return "SEND_COMMAND_RESULT";
    case 3: return "GET_STATE";
    case 4: return "SET_STATE";
    case 5: return "SET_ARTWORK";
    case 11: return "NOTIFICATION";
    case 15: return "DEVICE_INFO";
    case 16: return "CLIENT_UPDATES_CONFIG";
    case 17: return "VOLUME_CONTROL_AVAILABILITY";
    case 32: return "PLAYBACK_QUEUE_REQUEST";
    case 33: return "TRANSACTION";
    case 34: return "CRYPTO_PAIRING";
    case 36: return "SET_READY_STATE";
    case 37: return "DEVICE_INFO_UPDATE";
    case 38: return "SET_CONNECTION_STATE";
    case 42: return "GENERIC";
    case 46: return "SET_NOW_PLAYING_CLIENT";
    case 47: return "SET_NOW_PLAYING_PLAYER";
    case 49: return "GET_VOLUME";
    case 50: return "GET_VOLUME_RESULT";
    case 51: return "SET_VOLUME";
    case 52: return "VOLUME_DID_CHANGE";
    case 55: return "UPDATE_CLIENT";
    case 56: return "UPDATE_CONTENT_ITEM";
    case 58: return "UPDATE_PLAYER";
    case 62: return "GET_VOLUME_CONTROL_CAPABILITIES";
    case 63: return "GET_VOLUME_CONTROL_CAPABILITIES_RESULT";
    case 65: return "UPDATE_OUTPUT_DEVICE";
    case 77: return "UPDATE_ACTIVE_SYSTEM_ENDPOINT";
    case 120: return "CONFIGURE_CONNECTION";
    default: return "?";
  }
}

/* pyatv's DeviceInfoMessage numbering for the fields we are confident about.
 * Shown only as a hint next to the field number. */
static const char *devinfo_hint(uint32_t field) {
  switch (field) {
    case 1: return "uniqueIdentifier";
    case 2: return "name";
    case 3: return "localizedModelName";
    case 4: return "systemBuildVersion";
    case 5: return "applicationBundleIdentifier";
    case 6: return "applicationBundleVersion";
    case 7: return "protocolVersion";
    default: return NULL;
  }
}

static void hex_text(const uint8_t *b, size_t n, char *out, size_t cap) {
  size_t p = 0;
  out[0] = '\0';
  for (size_t i = 0; i < n && p + 4U < cap; ++i) {
    const int w = snprintf(out + p, cap - p, "%02x%s", b[i],
                           i + 1U < n ? " " : "");
    if (w <= 0) break;
    p += (size_t)w;
  }
}

static void hex_dump(const char *label, const uint8_t *b, size_t n) {
  const size_t lim = n > MRP_HEX_DUMP_MAX ? MRP_HEX_DUMP_MAX : n;
  ESP_LOGI(TAG, "%s hex %u bytes%s:", label, (unsigned)n,
           n > lim ? " (prefix shown)" : "");
  char line[32U * 3U + 1U];
  for (size_t off = 0; off < lim; off += 32U) {
    const size_t c = lim - off > 32U ? 32U : lim - off;
    hex_text(b + off, c, line, sizeof(line));
    ESP_LOGI(TAG, "  %04x: %s", (unsigned)off, line);
  }
}

/* Recursive generic walk. `ctx_devinfo` marks a nested message that sits in a
 * DEVICE_INFO ProtocolMessage extension, to add field-name hints. */
static unsigned pb_dump(const char *label, const uint8_t *b, size_t n,
                        unsigned depth, bool top, bool ctx_devinfo,
                        uint64_t msg_type) {
  static const char pad[] = "                    ";
  const char *ind = pad + (sizeof(pad) - 1U - (depth * 2U > 18U ? 18U : depth * 2U));
  unsigned lines = 0;
  size_t pos = 0;
  pb_field_t f;
  while (pos < n) {
    if (!pb_next(b, n, &pos, &f)) {
      ESP_LOGW(TAG, "%s %s<malformed at byte %u>", label, ind, (unsigned)pos);
      return lines + 1U;
    }
    const char *hint = ctx_devinfo ? devinfo_hint(f.field) : NULL;
    char hint_buf[40] = {0};
    if (top && f.field == 1 && f.wire == 0) {
      snprintf(hint_buf, sizeof(hint_buf), " (type %s)", mrp_type_name(f.varint));
    } else if (top && f.field == 2 && f.wire == 2) {
      snprintf(hint_buf, sizeof(hint_buf), " (identifier)");
    } else if (hint) {
      snprintf(hint_buf, sizeof(hint_buf), " (%s)", hint);
    }
    switch (f.wire) {
      case 0:
        ESP_LOGI(TAG, "%s %s#%" PRIu32 " varint=%" PRIu64 "%s", label, ind,
                 f.field, f.varint, hint_buf);
        ++lines;
        break;
      case 1: {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | f.data[i];
        double d;
        memcpy(&d, &v, sizeof(d));
        ESP_LOGI(TAG, "%s %s#%" PRIu32 " fixed64=0x%016" PRIx64 " (double %.6f)%s",
                 label, ind, f.field, v, d, hint_buf);
        ++lines;
        break;
      }
      case 5: {
        uint32_t v = (uint32_t)f.data[0] | ((uint32_t)f.data[1] << 8) |
                     ((uint32_t)f.data[2] << 16) | ((uint32_t)f.data[3] << 24);
        float fl;
        memcpy(&fl, &v, sizeof(fl));
        ESP_LOGI(TAG, "%s %s#%" PRIu32 " fixed32=0x%08" PRIx32 " (float %.4f)%s",
                 label, ind, f.field, v, (double)fl, hint_buf);
        ++lines;
        break;
      }
      case 2:
        if (f.len == 0) {
          ESP_LOGI(TAG, "%s %s#%" PRIu32 " bytes[0]%s", label, ind, f.field,
                   hint_buf);
          ++lines;
        } else if (printable(f.data, f.len)) {
          const size_t show = f.len > MRP_STR_SHOW ? MRP_STR_SHOW : f.len;
          ESP_LOGI(TAG, "%s %s#%" PRIu32 " str[%u]=\"%.*s\"%s%s", label, ind,
                   f.field, (unsigned)f.len, (int)show, (const char *)f.data,
                   f.len > show ? "..." : "", hint_buf);
          ++lines;
        } else if (depth + 1U < MRP_MAX_DEPTH && pb_valid(f.data, f.len)) {
          /* A nested message inside a top-level DEVICE_INFO envelope (any
           * extension field >= 6) is the DeviceInfo body. */
          const bool child_devinfo =
              top && msg_type == MRP_TYPE_DEVICE_INFO && f.field >= 6U;
          ESP_LOGI(TAG, "%s %s#%" PRIu32 " msg[%u] {%s", label, ind, f.field,
                   (unsigned)f.len, child_devinfo ? " (DeviceInfo body)" : "");
          lines += 1U + pb_dump(label, f.data, f.len, depth + 1U, false,
                                child_devinfo, 0);
          ESP_LOGI(TAG, "%s %s}", label, ind);
          ++lines;
        } else {
          char hx[48U * 3U + 1U];
          const size_t show = f.len > 48U ? 48U : f.len;
          hex_text(f.data, show, hx, sizeof(hx));
          ESP_LOGI(TAG, "%s %s#%" PRIu32 " bytes[%u]=%s%s%s", label, ind,
                   f.field, (unsigned)f.len, hx, f.len > show ? " ..." : "",
                   hint_buf);
          ++lines;
        }
        break;
      default:
        break;
    }
  }
  return lines;
}

static uint64_t pb_message_type(const uint8_t *b, size_t n) {
  size_t pos = 0;
  pb_field_t f;
  while (pos < n && pb_next(b, n, &pos, &f)) {
    if (f.field == 1 && f.wire == 0) return f.varint;
  }
  return 0;
}

unsigned mrp_inspect_protobuf(const char *label, const uint8_t *buf,
                              size_t len) {
  if (!pb_valid(buf, len)) return 0;
  const uint64_t type = pb_message_type(buf, len);
  return pb_dump(label, buf, len, 0, true, false, type);
}

/* ---- params.data framing ------------------------------------------------ */

/* Calls `fn` for every varint-length-prefixed ProtocolMessage in `data`.
 * Falls back to treating the whole blob as one message. Returns the count. */
typedef void (*mrp_msg_fn)(const uint8_t *msg, size_t len, unsigned index,
                           void *user);

static unsigned for_each_message(const uint8_t *data, size_t len, mrp_msg_fn fn,
                                 void *user, bool *framed) {
  size_t pos = 0;
  unsigned count = 0;
  *framed = false;
  while (pos < len && count < 32U) {
    const size_t start = pos;
    uint64_t n = 0;
    if (!pb_varint(data, len, &pos, &n) || n == 0 || n > len - pos ||
        !pb_valid(data + pos, (size_t)n)) {
      pos = start;
      break;
    }
    if (fn) fn(data + pos, (size_t)n, count, user);
    ++count;
    pos += (size_t)n;
  }
  if (count > 0 && pos == len) {
    *framed = true;
    return count;
  }
  if (pb_valid(data, len)) {
    if (fn) fn(data, len, 0, user);
    return 1;
  }
  return 0;
}

typedef struct {
  const char *label;
} inspect_ctx_t;

static void inspect_one(const uint8_t *msg, size_t len, unsigned index,
                        void *user) {
  const inspect_ctx_t *c = (const inspect_ctx_t *)user;
  const uint64_t type = pb_message_type(msg, len);
  char lbl[48];
  snprintf(lbl, sizeof(lbl), "%s MRP[%u]", c->label, index);
  ESP_LOGI(TAG, "%s ProtocolMessage %u bytes type=%" PRIu64 " (%s)", lbl,
           (unsigned)len, type, mrp_type_name(type));
  (void)pb_dump(lbl, msg, len, 0, true, false, type);
}

void mrp_inspect_comm(const char *label, const uint8_t *plist,
                      size_t plist_len) {
  if (!plist || plist_len < 8) return;
  uint8_t *data = heap_caps_malloc(MRP_DATA_MAX,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!data) return;
  size_t data_len = 0;
  if (!bplist_find_data_deep(plist, plist_len, "data", data, MRP_DATA_MAX,
                             &data_len) ||
      data_len == 0) {
    ESP_LOGI(TAG, "%s comm: no params.data (plist %uB)", label ? label : "?",
             (unsigned)plist_len);
    heap_caps_free(data);
    return;
  }
  char lbl[32];
  snprintf(lbl, sizeof(lbl), "%s", label ? label : "comm");
  hex_dump(lbl, data, data_len);
  inspect_ctx_t c = {.label = lbl};
  bool framed = false;
  const unsigned count = for_each_message(data, data_len, inspect_one, &c,
                                          &framed);
  ESP_LOGI(TAG, "%s params.data %uB -> %u ProtocolMessage(s), %s", lbl,
           (unsigned)data_len, count,
           count == 0 ? "NOT protobuf" : framed ? "varint-framed" : "unframed");
  heap_caps_free(data);
}

/* ---- experimental DEVICE_INFO reply ------------------------------------- */

static size_t pb_put_varint(uint8_t *o, size_t cap, size_t p, uint64_t v) {
  do {
    if (p >= cap) return 0;
    uint8_t c = (uint8_t)(v & 0x7fU);
    v >>= 7;
    if (v) c |= 0x80U;
    o[p++] = c;
  } while (v);
  return p;
}

static size_t pb_put_key(uint8_t *o, size_t cap, size_t p, uint32_t field,
                         uint8_t wire) {
  return pb_put_varint(o, cap, p, ((uint64_t)field << 3) | wire);
}

static size_t pb_put_bytes(uint8_t *o, size_t cap, size_t p, uint32_t field,
                           const uint8_t *b, size_t n) {
  p = pb_put_key(o, cap, p, field, 2);
  if (!p) return 0;
  p = pb_put_varint(o, cap, p, n);
  if (!p || n > cap - p) return 0;
  if (n) memcpy(o + p, b, n);
  return p + n;
}

static size_t pb_put_str(uint8_t *o, size_t cap, size_t p, uint32_t field,
                         const char *s) {
  if (!s) return p;
  return pb_put_bytes(o, cap, p, field, (const uint8_t *)s, strlen(s));
}

static size_t pb_put_uint(uint8_t *o, size_t cap, size_t p, uint32_t field,
                          uint64_t v) {
  p = pb_put_key(o, cap, p, field, 0);
  if (!p) return 0;
  return pb_put_varint(o, cap, p, v);
}

typedef struct {
  bool found;
  char identifier[96];
  uint32_t ext_field;
  char bundle_id[96];
  char bundle_ver[48];
  bool have_proto;
  uint64_t proto;
} devinfo_request_t;

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t n) {
  const size_t c = n < cap - 1U ? n : cap - 1U;
  memcpy(dst, src, c);
  dst[c] = '\0';
}

static void find_devinfo(const uint8_t *msg, size_t len, unsigned index,
                         void *user) {
  devinfo_request_t *r = (devinfo_request_t *)user;
  (void)index;
  if (r->found || pb_message_type(msg, len) != MRP_TYPE_DEVICE_INFO) return;
  size_t pos = 0;
  pb_field_t f;
  const uint8_t *body = NULL;
  size_t body_len = 0;
  while (pos < len && pb_next(msg, len, &pos, &f)) {
    if (f.field == 2 && f.wire == 2 && printable(f.data, f.len)) {
      copy_str(r->identifier, sizeof(r->identifier), f.data, f.len);
    } else if (f.field >= 6U && f.wire == 2 && !body &&
               pb_valid(f.data, f.len)) {
      r->ext_field = f.field;
      body = f.data;
      body_len = f.len;
    }
  }
  if (!body) return;
  r->found = true;
  pos = 0;
  while (pos < body_len && pb_next(body, body_len, &pos, &f)) {
    if (f.field == 5 && f.wire == 2 && printable(f.data, f.len)) {
      copy_str(r->bundle_id, sizeof(r->bundle_id), f.data, f.len);
    } else if (f.field == 6 && f.wire == 2 && printable(f.data, f.len)) {
      copy_str(r->bundle_ver, sizeof(r->bundle_ver), f.data, f.len);
    } else if (f.field == 7 && f.wire == 0) {
      r->have_proto = true;
      r->proto = f.varint;
    }
  }
}

/* Minimal binary plist: { params = { data = <blob> } }. */
static size_t bplist_params_data(const uint8_t *blob, size_t n, uint8_t *o,
                                 size_t cap) {
  if (n > 0xffffU || cap < 64U + n) return 0;
  size_t p = 0;
  size_t off[5];
  memcpy(o, "bplist00", 8);
  p = 8;
  off[0] = p; /* dict { obj1 : obj2 } */
  o[p++] = 0xD1;
  o[p++] = 1;
  o[p++] = 2;
  off[1] = p; /* "params" */
  o[p++] = 0x56;
  memcpy(o + p, "params", 6);
  p += 6;
  off[2] = p; /* dict { obj3 : obj4 } */
  o[p++] = 0xD1;
  o[p++] = 3;
  o[p++] = 4;
  off[3] = p; /* "data" */
  o[p++] = 0x54;
  memcpy(o + p, "data", 4);
  p += 4;
  off[4] = p; /* data blob */
  if (n < 15U) {
    o[p++] = (uint8_t)(0x40U | n);
  } else if (n <= 0xffU) {
    o[p++] = 0x4F;
    o[p++] = 0x10;
    o[p++] = (uint8_t)n;
  } else {
    o[p++] = 0x4F;
    o[p++] = 0x11;
    o[p++] = (uint8_t)(n >> 8);
    o[p++] = (uint8_t)n;
  }
  if (n > cap - p) return 0;
  memcpy(o + p, blob, n);
  p += n;
  const size_t table = p;
  if (cap - p < 5U * 2U + 32U) return 0;
  for (int i = 0; i < 5; ++i) {
    o[p++] = (uint8_t)(off[i] >> 8);
    o[p++] = (uint8_t)off[i];
  }
  memset(o + p, 0, 32);
  o[p + 6] = 2;  /* offset int size */
  o[p + 7] = 1;  /* object ref size */
  o[p + 15] = 5; /* number of objects */
  o[p + 23] = 0; /* top object */
  o[p + 30] = (uint8_t)(table >> 8);
  o[p + 31] = (uint8_t)table;
  return p + 32;
}

size_t mrp_build_device_info_reply(const uint8_t *request_plist,
                                   size_t request_len,
                                   const mrp_device_identity_t *id,
                                   uint8_t *out, size_t out_cap) {
  if (!request_plist || !id || !out) return 0;
  uint8_t *data = heap_caps_malloc(MRP_DATA_MAX,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!data) return 0;
  size_t data_len = 0;
  devinfo_request_t req = {0};
  bool framed = false;
  if (bplist_find_data_deep(request_plist, request_len, "data", data,
                            MRP_DATA_MAX, &data_len) && data_len > 0) {
    (void)for_each_message(data, data_len, find_devinfo, &req, &framed);
  }
  heap_caps_free(data);
  if (!req.found) return 0;

  uint8_t body[512];
  size_t b = 0;
  b = pb_put_str(body, sizeof(body), b, 1, id->unique_identifier);
  if (b) b = pb_put_str(body, sizeof(body), b, 2, id->name);
  if (b) b = pb_put_str(body, sizeof(body), b, 3, id->localized_model);
  if (b) b = pb_put_str(body, sizeof(body), b, 4, id->system_build);
  if (b) b = pb_put_str(body, sizeof(body), b, 5,
                        req.bundle_id[0] ? req.bundle_id
                                         : "com.apple.mediaremoted");
  if (b && req.bundle_ver[0]) b = pb_put_str(body, sizeof(body), b, 6,
                                             req.bundle_ver);
  if (b) b = pb_put_uint(body, sizeof(body), b, 7,
                         req.have_proto ? req.proto : 1U);
  if (!b) return 0;

  uint8_t msg[640];
  size_t m = 0;
  m = pb_put_uint(msg, sizeof(msg), m, 1, MRP_TYPE_DEVICE_INFO);
  if (m && req.identifier[0]) m = pb_put_str(msg, sizeof(msg), m, 2,
                                             req.identifier);
  if (m) m = pb_put_bytes(msg, sizeof(msg), m, req.ext_field, body, b);
  if (!m) return 0;

  uint8_t framed_msg[660];
  size_t fm = pb_put_varint(framed_msg, sizeof(framed_msg), 0, m);
  if (!fm || m > sizeof(framed_msg) - fm) return 0;
  memcpy(framed_msg + fm, msg, m);
  fm += m;

  ESP_LOGI(TAG,
           "DEVICE_INFO reply built: id=\"%s\" ext=#%" PRIu32
           " bundle=\"%s\" proto=%" PRIu64 " msg=%uB",
           req.identifier, req.ext_field,
           req.bundle_id[0] ? req.bundle_id : "com.apple.mediaremoted",
           req.have_proto ? req.proto : (uint64_t)1U, (unsigned)m);
  return bplist_params_data(framed_msg, fm, out, out_cap);
}
