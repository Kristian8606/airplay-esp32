#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct aac_decoder aac_decoder_t;

typedef struct {
  int sample_rate;
  int channels;
  int bits_per_sample;
} aac_decoder_config_t;

typedef struct {
  int channels;
} aac_decode_info_t;

aac_decoder_t *aac_decoder_create(const aac_decoder_config_t *config);
void aac_decoder_destroy(aac_decoder_t *decoder);
/* Reset codec overlap/history without reallocating the decoder object or its
 * input scratch buffer.  Used only for a real media-cursor discontinuity, not
 * for a presentation-clock/anchor update. */
bool aac_decoder_reset(aac_decoder_t *decoder);
int aac_decoder_decode(aac_decoder_t *decoder, const uint8_t *input,
                       size_t input_len, int16_t *output,
                       size_t output_capacity_frames,
                       aac_decode_info_t *info);
