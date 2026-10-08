#pragma once

/* Small general-purpose binary plist ("bplist00") writer.
 *
 * Build the tree with the add/begin/end calls, then serialise it with
 * bpw_finish(). Strings and data are referenced, not copied: they must stay
 * valid until bpw_finish() returns. Any overflow (too many objects or
 * nesting) is sticky and makes bpw_finish() return 0. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BPW_MAX_OBJECTS 192
#define BPW_MAX_PENDING 192
#define BPW_MAX_DEPTH   8

typedef struct {
  uint8_t kind;
  union {
    int64_t i;
    double r;
    bool b;
    struct {
      const void *ptr;
      size_t len;
    } bytes;
    struct {
      uint16_t first; /* index into refs[] */
      uint16_t count; /* children (dict: key/value pairs) */
    } c;
  } v;
} bpw_obj_t;

typedef struct {
  bpw_obj_t objs[BPW_MAX_OBJECTS];
  uint16_t refs[BPW_MAX_OBJECTS * 2];
  uint16_t pending[BPW_MAX_PENDING];
  uint16_t frame_start[BPW_MAX_DEPTH];
  uint16_t frame_obj[BPW_MAX_DEPTH];
  uint16_t nobj;
  uint16_t nrefs;
  uint16_t npending;
  uint8_t depth;
  bool overflow;
} bpw_t;

void bpw_init(bpw_t *w);

/* Containers. The root must be a dict or an array. Inside a dict, add the
 * key first (bpw_key) and then exactly one value. */
void bpw_dict_begin(bpw_t *w);
void bpw_array_begin(bpw_t *w);
void bpw_end(bpw_t *w);

void bpw_key(bpw_t *w, const char *key);
void bpw_string(bpw_t *w, const char *utf8);
void bpw_int(bpw_t *w, int64_t value);
void bpw_uint(bpw_t *w, uint64_t value); /* stored as 8-byte two's complement */
void bpw_real(bpw_t *w, double value);
void bpw_bool(bpw_t *w, bool value);
void bpw_data(bpw_t *w, const void *data, size_t len);

/* Convenience: key + value in one call. */
void bpw_kv_string(bpw_t *w, const char *key, const char *utf8);
void bpw_kv_int(bpw_t *w, const char *key, int64_t value);
void bpw_kv_uint(bpw_t *w, const char *key, uint64_t value);
void bpw_kv_real(bpw_t *w, const char *key, double value);
void bpw_kv_bool(bpw_t *w, const char *key, bool value);
void bpw_kv_data(bpw_t *w, const char *key, const void *data, size_t len);

/* Serialise. Returns the number of bytes written, 0 on any error. */
size_t bpw_finish(bpw_t *w, uint8_t *out, size_t capacity);
