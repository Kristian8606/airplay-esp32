#include <string.h>

#include "base64.h"

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encoded_length(size_t input_len) {
  return ((input_len + 2) / 3) * 4;
}

int base64_encode(const uint8_t *input, size_t input_len, char *output,
                  size_t output_capacity) {
  if (!input || !output) {
    return -1;
  }

  size_t out_len = base64_encoded_length(input_len);
  if (out_len > output_capacity) {
    return -1;
  }

  size_t pos = 0;
  for (size_t i = 0; i < input_len; i += 3) {
    uint32_t octet_a = i < input_len ? input[i] : 0;
    uint32_t octet_b = i + 1 < input_len ? input[i + 1] : 0;
    uint32_t octet_c = i + 2 < input_len ? input[i + 2] : 0;

    uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

    output[pos++] = b64_table[(triple >> 18) & 0x3F];
    output[pos++] = b64_table[(triple >> 12) & 0x3F];
    output[pos++] =
        (char)((i + 1 < input_len) ? b64_table[(triple >> 6) & 0x3F] : '=');
    output[pos++] =
        (char)((i + 2 < input_len) ? b64_table[triple & 0x3F] : '=');
  }

  return (int)out_len;
}

