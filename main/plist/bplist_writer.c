#include "bplist_writer.h"

#include <string.h>

enum {
  K_INT = 1,
  K_UINT,
  K_REAL,
  K_BOOL,
  K_STRING,
  K_DATA,
  K_ARRAY,
  K_DICT,
};

void bpw_init(bpw_t *w) { memset(w, 0, sizeof(*w)); }

static int add_obj(bpw_t *w, uint8_t kind) {
  if (w->overflow || w->nobj >= BPW_MAX_OBJECTS) {
    w->overflow = true;
    return -1;
  }
  int idx = w->nobj++;
  memset(&w->objs[idx], 0, sizeof(w->objs[idx]));
  w->objs[idx].kind = kind;
  return idx;
}

/* Every non-root object becomes a child of the innermost open container. */
static void attach(bpw_t *w, int idx) {
  if (idx < 0) return;
  if (w->depth == 0) {
    if (idx != 0) w->overflow = true; /* only the root may be top level */
    return;
  }
  if (w->npending >= BPW_MAX_PENDING) {
    w->overflow = true;
    return;
  }
  w->pending[w->npending++] = (uint16_t)idx;
}

static void begin(bpw_t *w, uint8_t kind) {
  int idx = add_obj(w, kind);
  if (idx < 0) return;
  attach(w, idx);
  if (w->depth >= BPW_MAX_DEPTH) {
    w->overflow = true;
    return;
  }
  w->frame_start[w->depth] = w->npending;
  w->frame_obj[w->depth] = (uint16_t)idx;
  w->depth++;
}

void bpw_dict_begin(bpw_t *w) { begin(w, K_DICT); }
void bpw_array_begin(bpw_t *w) { begin(w, K_ARRAY); }

void bpw_end(bpw_t *w) {
  if (w->overflow) return;
  if (w->depth == 0) {
    w->overflow = true;
    return;
  }
  w->depth--;
  const uint16_t start = w->frame_start[w->depth];
  const uint16_t n = (uint16_t)(w->npending - start);
  bpw_obj_t *o = &w->objs[w->frame_obj[w->depth]];
  if (w->nrefs + n > (uint16_t)(sizeof(w->refs) / sizeof(w->refs[0]))) {
    w->overflow = true;
    return;
  }
  o->v.c.first = w->nrefs;
  if (o->kind == K_DICT) {
    if (n % 2U) {
      w->overflow = true; /* key without value */
      return;
    }
    /* bplist dicts store all key refs, then all value refs. */
    for (uint16_t i = 0; i < n; i += 2) w->refs[w->nrefs++] = w->pending[start + i];
    for (uint16_t i = 1; i < n; i += 2) w->refs[w->nrefs++] = w->pending[start + i];
    o->v.c.count = (uint16_t)(n / 2U);
  } else {
    for (uint16_t i = 0; i < n; i++) w->refs[w->nrefs++] = w->pending[start + i];
    o->v.c.count = n;
  }
  w->npending = start;
}

static void add_bytes(bpw_t *w, uint8_t kind, const void *p, size_t len) {
  int idx = add_obj(w, kind);
  if (idx < 0) return;
  w->objs[idx].v.bytes.ptr = p;
  w->objs[idx].v.bytes.len = len;
  attach(w, idx);
}

void bpw_key(bpw_t *w, const char *key) { bpw_string(w, key); }
void bpw_string(bpw_t *w, const char *s) {
  add_bytes(w, K_STRING, s ? s : "", s ? strlen(s) : 0);
}
void bpw_data(bpw_t *w, const void *d, size_t len) {
  add_bytes(w, K_DATA, d, d ? len : 0);
}

void bpw_int(bpw_t *w, int64_t v) {
  int idx = add_obj(w, K_INT);
  if (idx < 0) return;
  w->objs[idx].v.i = v;
  attach(w, idx);
}
void bpw_uint(bpw_t *w, uint64_t v) {
  int idx = add_obj(w, K_UINT);
  if (idx < 0) return;
  w->objs[idx].v.i = (int64_t)v;
  attach(w, idx);
}
void bpw_real(bpw_t *w, double v) {
  int idx = add_obj(w, K_REAL);
  if (idx < 0) return;
  w->objs[idx].v.r = v;
  attach(w, idx);
}
void bpw_bool(bpw_t *w, bool v) {
  int idx = add_obj(w, K_BOOL);
  if (idx < 0) return;
  w->objs[idx].v.b = v;
  attach(w, idx);
}

void bpw_kv_string(bpw_t *w, const char *k, const char *s) { bpw_key(w, k); bpw_string(w, s); }
void bpw_kv_int(bpw_t *w, const char *k, int64_t v) { bpw_key(w, k); bpw_int(w, v); }
void bpw_kv_uint(bpw_t *w, const char *k, uint64_t v) { bpw_key(w, k); bpw_uint(w, v); }
void bpw_kv_real(bpw_t *w, const char *k, double v) { bpw_key(w, k); bpw_real(w, v); }
void bpw_kv_bool(bpw_t *w, const char *k, bool v) { bpw_key(w, k); bpw_bool(w, v); }
void bpw_kv_data(bpw_t *w, const char *k, const void *d, size_t n) {
  bpw_key(w, k);
  bpw_data(w, d, n);
}

/* ---- serialisation ---- */

typedef struct {
  uint8_t *out;
  size_t cap;
  size_t pos;
  bool err;
} sink_t;

static void put(sink_t *s, const void *p, size_t n) {
  if (s->err || s->cap - s->pos < n) {
    s->err = true;
    return;
  }
  memcpy(s->out + s->pos, p, n);
  s->pos += n;
}
static void put_u8(sink_t *s, uint8_t b) { put(s, &b, 1); }
static void put_be(sink_t *s, uint64_t v, unsigned bytes) {
  uint8_t b[8];
  for (unsigned i = 0; i < bytes; i++) b[i] = (uint8_t)(v >> (8U * (bytes - 1U - i)));
  put(s, b, bytes);
}

static void put_int(sink_t *s, int64_t v) {
  if (v >= 0 && v <= 0xFF) {
    put_u8(s, 0x10);
    put_be(s, (uint64_t)v, 1);
  } else if (v >= 0 && v <= 0xFFFF) {
    put_u8(s, 0x11);
    put_be(s, (uint64_t)v, 2);
  } else if (v >= 0 && v <= 0xFFFFFFFFLL) {
    put_u8(s, 0x12);
    put_be(s, (uint64_t)v, 4);
  } else {
    put_u8(s, 0x13);
    put_be(s, (uint64_t)v, 8);
  }
}

static void put_marker_len(sink_t *s, uint8_t marker, size_t len) {
  if (len < 15) {
    put_u8(s, (uint8_t)(marker | len));
  } else {
    put_u8(s, (uint8_t)(marker | 0x0F));
    put_int(s, (int64_t)len);
  }
}

/* Decode one UTF-8 code point; invalid input yields U+FFFD. */
static uint32_t utf8_next(const uint8_t *p, size_t len, size_t *i) {
  uint8_t c = p[*i];
  uint32_t cp;
  unsigned extra;
  if (c < 0x80) {
    (*i)++;
    return c;
  } else if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    extra = 1;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    extra = 2;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    extra = 3;
  } else {
    (*i)++;
    return 0xFFFD;
  }
  size_t j = *i + 1;
  for (unsigned k = 0; k < extra; k++, j++) {
    if (j >= len || (p[j] & 0xC0) != 0x80) {
      *i = j;
      return 0xFFFD;
    }
    cp = (cp << 6) | (p[j] & 0x3F);
  }
  *i = j;
  return cp > 0x10FFFF ? 0xFFFD : cp;
}

static void put_string(sink_t *s, const uint8_t *p, size_t len) {
  bool ascii = true;
  for (size_t i = 0; i < len; i++) {
    if (p[i] >= 0x80) {
      ascii = false;
      break;
    }
  }
  if (ascii) {
    put_marker_len(s, 0x50, len);
    put(s, p, len);
    return;
  }
  /* Non-ASCII: UTF-16BE, length counted in 16-bit units. */
  size_t units = 0;
  for (size_t i = 0; i < len;) units += utf8_next(p, len, &i) > 0xFFFF ? 2 : 1;
  put_marker_len(s, 0x60, units);
  for (size_t i = 0; i < len;) {
    uint32_t cp = utf8_next(p, len, &i);
    if (cp > 0xFFFF) {
      cp -= 0x10000;
      put_be(s, 0xD800 + (cp >> 10), 2);
      put_be(s, 0xDC00 + (cp & 0x3FF), 2);
    } else {
      put_be(s, cp, 2);
    }
  }
}

size_t bpw_finish(bpw_t *w, uint8_t *out, size_t capacity) {
  if (!w || !out || w->overflow || w->depth != 0 || w->nobj == 0) return 0;
  const uint8_t root_kind = w->objs[0].kind;
  if (root_kind != K_DICT && root_kind != K_ARRAY) return 0;

  uint32_t offsets[BPW_MAX_OBJECTS];
  const unsigned ref_size = w->nobj <= 0xFF ? 1U : 2U;
  sink_t s = {.out = out, .cap = capacity, .pos = 0, .err = false};
  put(&s, "bplist00", 8);

  for (uint16_t i = 0; i < w->nobj && !s.err; i++) {
    const bpw_obj_t *o = &w->objs[i];
    offsets[i] = (uint32_t)s.pos;
    switch (o->kind) {
      case K_INT:
        put_int(&s, o->v.i);
        break;
      case K_UINT:
        if (o->v.i >= 0) {
          put_int(&s, o->v.i);
        } else { /* > INT64_MAX: 128-bit form, high half zero */
          put_u8(&s, 0x14);
          put_be(&s, 0, 8);
          put_be(&s, (uint64_t)o->v.i, 8);
        }
        break;
      case K_REAL: {
        uint64_t bits;
        memcpy(&bits, &o->v.r, sizeof(bits));
        put_u8(&s, 0x23);
        put_be(&s, bits, 8);
        break;
      }
      case K_BOOL:
        put_u8(&s, o->v.b ? 0x09 : 0x08);
        break;
      case K_STRING:
        put_string(&s, (const uint8_t *)o->v.bytes.ptr, o->v.bytes.len);
        break;
      case K_DATA:
        put_marker_len(&s, 0x40, o->v.bytes.len);
        put(&s, o->v.bytes.ptr, o->v.bytes.len);
        break;
      case K_ARRAY:
      case K_DICT: {
        const unsigned per = o->kind == K_DICT ? 2U : 1U;
        put_marker_len(&s, o->kind == K_DICT ? 0xD0 : 0xA0, o->v.c.count);
        for (unsigned r = 0; r < (unsigned)o->v.c.count * per; r++)
          put_be(&s, w->refs[o->v.c.first + r], ref_size);
        break;
      }
      default:
        s.err = true;
        break;
    }
  }
  if (s.err) return 0;

  const size_t table_offset = s.pos;
  const unsigned off_size = table_offset <= 0xFF ? 1U : (table_offset <= 0xFFFF ? 2U : 4U);
  for (uint16_t i = 0; i < w->nobj; i++) put_be(&s, offsets[i], off_size);

  static const uint8_t pad[6] = {0};
  put(&s, pad, sizeof(pad));
  put_u8(&s, (uint8_t)off_size);
  put_u8(&s, (uint8_t)ref_size);
  put_be(&s, w->nobj, 8);
  put_be(&s, 0, 8); /* top object */
  put_be(&s, table_offset, 8);
  return s.err ? 0 : s.pos;
}
