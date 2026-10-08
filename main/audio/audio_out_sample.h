#pragma once

/* Final I2S output sample format.
 *
 * Decoders, EQ and the PCM ring stay 16-bit. Only the very last stage, the
 * output volume in the playout task, writes this format before I2S.
 *
 * 32-bit slots (default): 24-bit audio, left-justified in a 32-bit I2S slot
 * (low 8 bits zero), 64 BCLK per stereo frame. Volume is applied without
 * truncating back to 16 bits, so attenuation keeps the full 16-bit detail
 * and no dither is needed (rounding error ~ -144 dBFS).
 *
 * 16-bit slots (menuconfig fallback): previous behaviour, 32 BCLK per frame,
 * volume requantised to 16 bits with TPDF dither.
 *
 * This header is pure C (no IDF calls) so the math can be host-tested. */

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if defined(CONFIG_AIRPLAY_I2S_SLOT_16) && CONFIG_AIRPLAY_I2S_SLOT_16
#define AUDIO_OUT_SLOT_BITS 16
typedef int16_t audio_out_sample_t;
#else
#define AUDIO_OUT_SLOT_BITS 32
typedef int32_t audio_out_sample_t;
#endif

/* 16-bit PCM sample -> output slot at unity gain (bit-exact). */
static inline audio_out_sample_t audio_out_from_pcm16(int16_t x) {
#if AUDIO_OUT_SLOT_BITS == 32
  return (int32_t)x * 65536;
#else
  return x;
#endif
}

static inline void audio_out_from_pcm16_block(const int16_t *in,
                                              audio_out_sample_t *out,
                                              size_t samples) {
  for (size_t i = 0; i < samples; ++i) out[i] = audio_out_from_pcm16(in[i]);
}

/* 32-bit slot -> 16-bit view (LED/VU meter only). */
static inline int16_t audio_out_to_pcm16(audio_out_sample_t x) {
#if AUDIO_OUT_SLOT_BITS == 32
  return (int16_t)(x >> 16);
#else
  return x;
#endif
}

#if AUDIO_OUT_SLOT_BITS == 32
/* x * gain with gain in Q15 (0..32768, 32768 = 0 dB). The result is rounded
 * to 24 bits and left-justified in a 32-bit slot. gain 32768 gives exactly
 * x << 16; x == 0 or gain == 0 gives exactly 0. */
static inline int32_t audio_out_scale_q15(int16_t x, int32_t gain_q15) {
  if (gain_q15 <= 0 || x == 0) return 0;
  if (gain_q15 > 32768) gain_q15 = 32768;
  /* x (Q15) * gain (Q15) = Q30; << 1 = Q31 full scale of the 32-bit slot. */
  const int64_t y = (int64_t)x * (int64_t)gain_q15 * 2;
  /* Round half up to a multiple of 256 (24-bit resolution). */
  int64_t r = (y + 128) & ~(int64_t)255;
  if (r > (int64_t)INT32_MAX) r = (int64_t)INT32_MAX & ~(int64_t)255;
  if (r < (int64_t)INT32_MIN) r = (int64_t)INT32_MIN;
  return (int32_t)r;
}
#endif
