#include "alac_decoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/impl/esp_alac_dec.h"
#include "esp_audio_dec.h"
#include "esp_log.h"

#define ALAC_COOKIE_LEN 24U

struct alac_decoder {
  alac_decoder_config_t config;
  void *handle;
  uint8_t cookie[ALAC_COOKIE_LEN];
};

static const char *TAG = "alac_dec_v1";

static void put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* Build the standard 24-byte ALACSpecificConfig ("magic cookie").
 * AirPlay realtime tells us the fields that matter here: frame size, bit
 * depth, channel count and sample rate. The ALAC reference defaults are
 * pb=40, mb=10, kb=14, maxRun=255. maxFrameBytes/avgBitRate may be zero when
 * unknown. Multi-byte fields in the cookie are big-endian. */
static void make_cookie(alac_decoder_t *d) {
  memset(d->cookie, 0, sizeof(d->cookie));
  put_be32(d->cookie + 0, (uint32_t)d->config.frame_size);
  d->cookie[4] = 0; /* compatibleVersion */
  d->cookie[5] = (uint8_t)d->config.bits_per_sample;
  d->cookie[6] = 40; /* pb */
  d->cookie[7] = 10; /* mb */
  d->cookie[8] = 14; /* kb */
  d->cookie[9] = (uint8_t)d->config.channels;
  put_be16(d->cookie + 10, 255);
  put_be32(d->cookie + 12, 0); /* maxFrameBytes unknown */
  put_be32(d->cookie + 16, 0); /* avgBitRate unknown */
  put_be32(d->cookie + 20, (uint32_t)d->config.sample_rate);
}

static bool open_decoder(alac_decoder_t *d) {
  if (d->handle) {
    esp_alac_dec_close(d->handle);
    d->handle = NULL;
  }

  make_cookie(d);
  esp_alac_dec_cfg_t cfg = ESP_ALAC_DEC_CONFIG_DEFAULT();
  cfg.codec_spec_info = d->cookie;
  cfg.spec_info_len = sizeof(d->cookie);
  esp_audio_err_t err = esp_alac_dec_open(&cfg, sizeof(cfg), &d->handle);
  if (err != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "esp_alac_dec_open failed: %d", err);
    d->handle = NULL;
    return false;
  }
  return true;
}

alac_decoder_t *alac_decoder_create(const alac_decoder_config_t *config) {
  if (!config || config->sample_rate <= 0 || config->channels <= 0 ||
      config->bits_per_sample <= 0 || config->frame_size <= 0) {
    return NULL;
  }

  alac_decoder_t *d = calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->config = *config;
  if (!open_decoder(d)) {
    alac_decoder_destroy(d);
    return NULL;
  }
  return d;
}

void alac_decoder_destroy(alac_decoder_t *d) {
  if (!d) {
    return;
  }
  if (d->handle) {
    esp_alac_dec_close(d->handle);
  }
  free(d);
}

int alac_decoder_decode(alac_decoder_t *d, const uint8_t *input,
                        size_t input_len, int16_t *output,
                        size_t output_capacity_frames,
                        alac_decode_info_t *info) {
  if (!d || !d->handle || !input || input_len == 0 || !output ||
      output_capacity_frames == 0) {
    return -1;
  }

  const int channels = d->config.channels > 0 ? d->config.channels : 2;
  esp_audio_dec_in_raw_t raw = {
      .buffer = (uint8_t *)input,
      .len = (uint32_t)input_len,
      .consumed = 0,
      .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
  };
  esp_audio_dec_out_frame_t frame = {
      .buffer = (uint8_t *)output,
      .len = (uint32_t)(output_capacity_frames * (size_t)channels * sizeof(int16_t)),
      .decoded_size = 0,
  };
  esp_audio_dec_info_t dec_info = {0};

  esp_audio_err_t err = esp_alac_dec_decode(d->handle, &raw, &frame, &dec_info);
  if (err != ESP_AUDIO_ERR_OK) {
    ESP_LOGW(TAG, "decode error=%d; resetting ALAC decoder", err);
    esp_alac_dec_reset(d->handle);
    return -1;
  }

  int out_channels = dec_info.channel > 0 ? dec_info.channel : channels;
  if (out_channels <= 0) {
    out_channels = 2;
  }
  size_t frames = frame.decoded_size / ((size_t)out_channels * sizeof(int16_t));
  if (frames > output_capacity_frames) {
    frames = output_capacity_frames;
  }
  if (info) {
    info->channels = out_channels;
    info->sample_rate = dec_info.sample_rate > 0 ? dec_info.sample_rate
                                                 : d->config.sample_rate;
    info->bits_per_sample = dec_info.bits_per_sample > 0
                                ? dec_info.bits_per_sample
                                : d->config.bits_per_sample;
  }
  return (int)frames;
}
