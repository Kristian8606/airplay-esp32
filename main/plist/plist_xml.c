#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "plist.h"

static void plist_append(plist_t *p, const char *str) {
  size_t len = strlen(str);
  if (p->size + len < p->capacity) {
    memcpy(p->buffer + p->size, str, len);
    p->size += len;
    p->buffer[p->size] = '\0';
  }
}

void plist_init(plist_t *p, char *buffer, size_t capacity) {
  p->buffer = buffer;
  p->size = 0;
  p->capacity = capacity;
  p->buffer[0] = '\0';
}

void plist_begin(plist_t *p) {
  plist_append(p, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  plist_append(p, "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                  "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
  plist_append(p, "<plist version=\"1.0\">\n");
}

void plist_dict_begin(plist_t *p) {
  plist_append(p, "<dict>\n");
}

/* Length of s with the XML special characters escaped. */
static size_t xml_escaped_len(const char *s) {
  size_t n = 0;
  for (; *s; s++) {
    switch (*s) {
      case '&': n += 5; break;  /* &amp; */
      case '<':
      case '>': n += 4; break;  /* &lt; &gt; */
      case '"': n += 6; break;  /* &quot; */
      default: n += 1; break;
    }
  }
  return n;
}

/* Append s escaped; the caller has checked that it fits. */
static void xml_append_escaped(plist_t *p, const char *s) {
  for (; *s; s++) {
    const char *rep = NULL;
    switch (*s) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      default: break;
    }
    if (rep) {
      const size_t k = strlen(rep);
      memcpy(p->buffer + p->size, rep, k);
      p->size += k;
    } else {
      p->buffer[p->size++] = *s;
    }
  }
  p->buffer[p->size] = '\0';
}

void plist_dict_string(plist_t *p, const char *key, const char *value) {
  /* Whole entry or nothing, as before; key and value are escaped (a device
   * name such as "Kitchen & Living Room" must not break the XML). */
  static const char k_open[] = "<key>", k_close[] = "</key>\n<string>",
                    s_close[] = "</string>\n";
  const size_t need = (sizeof(k_open) - 1) + xml_escaped_len(key) + (sizeof(k_close) - 1) +
                      xml_escaped_len(value) + (sizeof(s_close) - 1);
  if (p->size + need >= p->capacity) return;
  plist_append(p, k_open);
  xml_append_escaped(p, key);
  plist_append(p, k_close);
  xml_append_escaped(p, value);
  plist_append(p, s_close);
}

void plist_dict_int(plist_t *p, const char *key, int64_t value) {
  size_t remaining = p->capacity - p->size;
  int len =
      snprintf(p->buffer + p->size, remaining,
               "<key>%s</key>\n<integer>%" PRId64 "</integer>\n", key, value);
  if (len > 0 && (size_t)len < remaining) {
    p->size += (size_t)len;
  }
}

void plist_dict_uint(plist_t *p, const char *key, uint64_t value) {
  size_t remaining = p->capacity - p->size;
  int len =
      snprintf(p->buffer + p->size, remaining,
               "<key>%s</key>\n<integer>%" PRIu64 "</integer>\n", key, value);
  if (len > 0 && (size_t)len < remaining) {
    p->size += (size_t)len;
  }
}

void plist_dict_data(plist_t *p, const char *key, const uint8_t *data,
                     size_t len) {
  size_t b64_len = base64_encoded_length(len);
  size_t remaining = p->capacity - p->size;

  if (remaining < strlen(key) + b64_len + 50) {
    return;
  }

  int written =
      snprintf(p->buffer + p->size, remaining, "<key>%s</key>\n<data>", key);
  if (written > 0) {
    p->size += (size_t)written;
  }

  int encoded =
      base64_encode(data, len, p->buffer + p->size, p->capacity - p->size);
  if (encoded < 0) {
    return;
  }
  p->size += (size_t)encoded;
  p->buffer[p->size] = '\0';

  plist_append(p, "</data>\n");
}

void plist_dict_end(plist_t *p) {
  plist_append(p, "</dict>\n");
}

void plist_dict_array_begin(plist_t *p, const char *key) {
  size_t remaining = p->capacity - p->size;
  int len =
      snprintf(p->buffer + p->size, remaining, "<key>%s</key>\n<array>\n", key);
  if (len > 0 && (size_t)len < remaining) {
    p->size += (size_t)len;
  }
}

void plist_array_end(plist_t *p) {
  plist_append(p, "</array>\n");
}

size_t plist_end(plist_t *p) {
  plist_append(p, "</plist>\n");
  return p->size;
}
