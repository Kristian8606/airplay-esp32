#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AAC_DECODER_INPUT_HEADROOM 7U

typedef struct aac_decoder aac_decoder_t;

typedef struct {
  int sample_rate;
  int channels;
  int bits_per_sample; /* Announced AirPlay metadata; ignored by AAC decode. */
} aac_decoder_config_t;

typedef struct {
  int channels;
} aac_decode_info_t;

aac_decoder_t *aac_decoder_create(const aac_decoder_config_t *config);
void aac_decoder_destroy(aac_decoder_t *decoder);
/* input points to payload with AAC_DECODER_INPUT_HEADROOM writable bytes
 * immediately before it. Raw AU input receives an ADTS header there; payload
 * is never moved. Input that already has ADTS is passed through unchanged.
 * Output is stereo float32 (mono duplicated), normalized from FDK signed32
 * by 2^31 without a 16-bit intermediate. Capacity counts stereo frames. */
int aac_decoder_decode(aac_decoder_t *decoder, uint8_t *input,
                       size_t input_len, float *output,
                       size_t output_capacity_frames,
                       aac_decode_info_t *info);
