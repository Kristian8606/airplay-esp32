#pragma once

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

/* F24 is the announced AirPlay AAC format profile, not lossless source
 * depth. The decoder's signed32 output is independent of that metadata. */
#define AUDIO_AAC_INPUT_BITS "24-bit (F24)"

static inline const char *audio_format_input_label(const char *codec, int pcm_bits,
                                                  char *buffer, size_t capacity) {
  if (strcmp(codec, "AAC") == 0) return AUDIO_AAC_INPUT_BITS;
  snprintf(buffer, capacity, "%d-bit", pcm_bits);
  return buffer;
}

static inline void audio_log_stereo_playback(
    const char *tag, const char *codec, int sample_rate, int pcm_bits,
    const char *transport) {
  (void)transport;
  char bits[16];
  ESP_LOGW(tag, "================================================");
  ESP_LOGW(tag, "NOW PLAYING | %s | %d Hz | %s | stereo L/R",
           strcmp(codec, "AAC") == 0 ? "AAC-LC" : codec,
           sample_rate, audio_format_input_label(codec, pcm_bits, bits, sizeof(bits)));
  ESP_LOGW(tag, "================================================");
}
