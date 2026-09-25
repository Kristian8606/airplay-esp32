#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AAC_DECODER_INPUT_HEADROOM 7U

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
/* input points to payload with AAC_DECODER_INPUT_HEADROOM writable bytes
 * immediately before it. Raw AU input receives an ADTS header there; payload
 * is never moved. Input that already has ADTS is passed through unchanged. */
int aac_decoder_decode(aac_decoder_t *decoder, uint8_t *input,
                       size_t input_len, int16_t *output,
                       size_t output_capacity_frames,
                       aac_decode_info_t *info);
