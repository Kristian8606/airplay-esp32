#pragma once

#include <stddef.h>
#include <stdint.h>

size_t base64_encoded_length(size_t input_len);
int base64_encode(const uint8_t *input, size_t input_len, char *output,
                  size_t output_capacity);
