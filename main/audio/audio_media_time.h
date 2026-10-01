#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

/* Exact rational media-time conversion without __int128 or floating point.
 * APAP keeps the media address 64-bit end-to-end; only the final proven PCM
 * ring maps it modulo 2^32 because that ring is shared with RTP realtime audio. */
static inline bool audio_media_time_to_sample64(int64_t value, uint32_t scale,
                                                uint32_t sample_rate,
                                                uint64_t *out) {
  if (!out || value < 0 || scale == 0U || sample_rate == 0U) return false;

  const uint64_t u = (uint64_t)value;
  const uint64_t whole = u / (uint64_t)scale;
  const uint64_t rem = u % (uint64_t)scale;
  const uint64_t sr = (uint64_t)sample_rate;
  if (whole > UINT64_MAX / sr) return false;
  const uint64_t base = whole * sr;

  /* rem < 2^32 and sample_rate is audio-rate sized, so this product is well
   * inside uint64_t for all supported AirPlay formats. */
  const uint64_t frac = (rem * sr) / (uint64_t)scale;
  if (base > UINT64_MAX - frac) return false;
  *out = base + frac;
  return true;
}
