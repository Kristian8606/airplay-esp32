#pragma once

#include <string.h>

#include "esp_log.h"

static inline void audio_log_stereo_playback(
    const char *tag, const char *codec, int sample_rate, int pcm_bits,
    const char *transport) {
  ESP_LOGW(tag, "================================================");
  ESP_LOGW(tag, "NOW PLAYING | %s | %d Hz | stereo (2ch) | %d-bit PCM | %s | "
                "surround selection inactive",
           strcmp(codec, "AAC") == 0 ? "AAC-LC" : codec,
           sample_rate, pcm_bits, transport);
  ESP_LOGW(tag, "================================================");
}

static inline void audio_log_surround_playback(
    const char *tag, int sample_rate, int channels,
    const char *requested_speaker, int decoded_channel) {
  const char *layout = channels == 8 ? "7.1" : channels == 6 ? "5.1" : "multichannel";
  ESP_LOGW(tag, "================================================");
  if (decoded_channel >= 0) {
    ESP_LOGW(tag, "NOW PLAYING | AAC-LC | %d Hz | %s (%dch) | 16-bit PCM | "
                  "%s | decoded #%d -> I2S L=R",
             sample_rate, layout, channels, requested_speaker, decoded_channel);
  } else {
    ESP_LOGW(tag, "NOW PLAYING | AAC-LC | %d Hz | %s (%dch) | 16-bit PCM | "
                  "Mono mix (no LFE) -> I2S L=R | requested=%s",
             sample_rate, layout, channels, requested_speaker);
  }
  ESP_LOGW(tag, "================================================");
}
