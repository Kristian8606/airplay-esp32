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
  uint32_t limiter_delay_frames;
  uint32_t output_rtp; /* RTP of output[0], corrected for PCM lookahead. */
} aac_decode_info_t;
uint32_t aac_decoder_limiter_delay_frames(const aac_decoder_t *decoder);
/* Tail is needed within the queued-DMA horizon, even if TCP stays open. */
bool aac_decoder_tail_due(const aac_decoder_t *decoder,
                            uint32_t wanted_rtp, uint32_t lead_frames);
uint32_t aac_decoder_tail_wait_ms(const aac_decoder_t *decoder,
                                   uint32_t requested_ms, uint32_t maximum_ms);
/* Publish a valid final tail at EOS/forward gap; not for cancelled audio. */
int aac_decoder_drain(aac_decoder_t *decoder, float *output,
                       size_t capacity_frames, aac_decode_info_t *info);

aac_decoder_t *aac_decoder_create(const aac_decoder_config_t *config);
void aac_decoder_destroy(aac_decoder_t *decoder);
/* input points to payload with AAC_DECODER_INPUT_HEADROOM writable bytes
 * immediately before it. Raw AU input receives an ADTS header there; payload
 * is never moved. Input that already has ADTS is passed through unchanged.
 * Output is stereo float32 (mono duplicated), normalized from FDK signed32
 * by 2^31 without a 16-bit intermediate. Capacity counts stereo frames.
 * input_rtp identifies this AU; info.output_rtp compensates actual lookahead.
 * mute_output advances AAC history but clears output and limiter tail on a
 * new play sequence. Contiguous pause/resume preserves limiter history. */
int aac_decoder_decode(aac_decoder_t *decoder, uint8_t *input,
                       size_t input_len, float *output,
                       size_t output_capacity_frames,
                       uint32_t input_rtp, bool mute_output,
                       aac_decode_info_t *info);
