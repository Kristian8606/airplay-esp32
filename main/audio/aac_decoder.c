#include "aac_decoder.h"
#include "audio_out_sample.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "airplay_log.h"
#include "aacdecoder_lib.h"

#define ADTS_HEADER_LEN AAC_DECODER_INPUT_HEADROOM
#define AAC_INPUT_MAX   8192U
/* S32 scratch for one AAC-LC access unit. FDK is opened with its full
 * channel limit (the parameters the surround build runs with); the buffer
 * matches it so an unexpected layout can never overrun it. Only stereo and
 * mono are played. 2048 frames leaves room in case the decoder hands out
 * more than 1024. */
#define FDK_MAX_CHANNELS 8
#define FDK_MAX_FRAMES   2048U

struct aac_decoder {
  aac_decoder_config_t config;
  HANDLE_AACDECODER fdk;
  LONG *pcm; /* FDK signed32 before 16-bit quantization; never INT_PCM. */
  uint32_t decode_errors;
};

static const char *TAG = "aac_dec_v1";

static int adts_frequency_index(int sample_rate) {
  static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                              24000, 22050, 16000, 12000, 11025, 8000, 7350};
  for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); ++i) {
    if (rates[i] == sample_rate) {
      return i;
    }
  }
  return 4; // 44.1 kHz
}

static bool has_adts(const uint8_t *data, size_t len) {
  return len >= 2U && data[0] == 0xffU && (data[1] & 0xf0U) == 0xf0U;
}

static void make_adts(uint8_t *h, size_t payload_len, int sample_rate,
                      int channels) {
  const int profile = 2; // AAC LC
  const int freq_idx = adts_frequency_index(sample_rate);
  const int chan_cfg = channels > 0 ? channels : 2;
  const int packet_len = (int)(payload_len + ADTS_HEADER_LEN);

  h[0] = 0xff;
  h[1] = 0xf1;
  h[2] = (uint8_t)(((profile - 1) << 6) | (freq_idx << 2) | (chan_cfg >> 2));
  h[3] = (uint8_t)(((chan_cfg & 3) << 6) | (packet_len >> 11));
  h[4] = (uint8_t)((packet_len & 0x7ff) >> 3);
  h[5] = (uint8_t)(((packet_len & 7) << 5) | 0x1f);
  h[6] = 0xfc;
}

static bool open_fdk_decoder(aac_decoder_t *d) {
  d->fdk = aacDecoder_Open(TT_MP4_ADTS, 1);
  if (!d->fdk) {
    ESP_LOGE(TAG, "FDK aacDecoder_Open failed");
    return false;
  }
  /* No FDK downmix, WAV channel order, and no limiter: DecodeFrame32
   * requires it disabled. Preserve FDK's existing DRC defaults and scaling;
   * no extra gain or source-depth assumption. */
  const AAC_DECODER_ERROR e1 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_MAX_OUTPUT_CHANNELS, FDK_MAX_CHANNELS);
  const AAC_DECODER_ERROR e2 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1);
  const AAC_DECODER_ERROR e3 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_LIMITER_ENABLE, 0);
  /* RTP already timestamps each AU. FDK's default interpolation adds one
   * unaccounted frame (1024 samples); noise substitution has no extra delay. */
  const AAC_DECODER_ERROR e4 =
      aacDecoder_SetParam(d->fdk, AAC_CONCEAL_METHOD, 1);
  if (e1 != AAC_DEC_OK || e2 != AAC_DEC_OK || e3 != AAC_DEC_OK || e4 != AAC_DEC_OK) {
    ESP_LOGE(TAG, "FDK S32 setup failed (params 0x%x/0x%x/0x%x/0x%x)",
             e1, e2, e3, e4);
    return false;
  }
  return true;
}

aac_decoder_t *aac_decoder_create(const aac_decoder_config_t *config) {
  if (!config || config->sample_rate <= 0 || config->channels <= 0 ||
      config->channels > 2) {
    return NULL;
  }

  aac_decoder_t *d = calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->config = *config;
  const size_t bytes = FDK_MAX_FRAMES * FDK_MAX_CHANNELS * sizeof(*d->pcm);
  d->pcm = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!d->pcm) d->pcm = malloc(bytes);
  if (!d->pcm || !open_fdk_decoder(d)) {
    ESP_LOGE(TAG, "Cannot create FDK S32 AAC decoder");
    aac_decoder_destroy(d);
    return NULL;
  }
  return d;
}

void aac_decoder_destroy(aac_decoder_t *d) {
  if (!d) {
    return;
  }
  if (d->fdk) {
    aacDecoder_Close(d->fdk);
  }
  free(d->pcm);
  free(d);
}

int aac_decoder_decode(aac_decoder_t *d, uint8_t *input,
                       size_t input_len, float *output,
                       size_t output_capacity_frames,
                       aac_decode_info_t *info) {
  if (!d || !d->fdk || !input || input_len == 0 || !output ||
      output_capacity_frames == 0) {
    return -1;
  }

  const uint8_t *decode_data = input;
  size_t decode_len = input_len;
  if (!has_adts(input, input_len)) {
    if (input_len > AAC_INPUT_MAX || input_len > 8191U - ADTS_HEADER_LEN) {
      return -1;
    }
    make_adts(input - ADTS_HEADER_LEN, input_len, d->config.sample_rate,
              d->config.channels);
    decode_data = input - ADTS_HEADER_LEN;
    decode_len = input_len + ADTS_HEADER_LEN;
  }

  UCHAR *in = (UCHAR *)decode_data;
  UINT in_size = (UINT)decode_len;
  UINT valid = (UINT)decode_len;
  AAC_DECODER_ERROR err = aacDecoder_Fill(d->fdk, &in, &in_size, &valid);
  if (err == AAC_DEC_OK) {
    err = aacDecoder_DecodeFrame32(d->fdk, d->pcm,
                                   (INT)(FDK_MAX_FRAMES * FDK_MAX_CHANNELS), 0);
  }
  if (err != AAC_DEC_OK) {
    /* Shairport keeps the AAC decoding chain alive across a bad access unit;
     * FDK conceals the missing frame from its own history. */
    if ((++d->decode_errors & 63U) == 1U || d->decode_errors <= 8U)
      AP_LOGW(TAG, "FDK decode error 0x%x (in %u B, %u left; %lu total)",
               err, (unsigned)decode_len, (unsigned)valid,
               (unsigned long)d->decode_errors);
    return -1;
  }

  const CStreamInfo *si = aacDecoder_GetStreamInfo(d->fdk);
  if (!si || si->numChannels <= 0 || si->numChannels > 2 ||
      si->frameSize <= 0 || (size_t)si->frameSize > FDK_MAX_FRAMES) {
    return -1;
  }

  size_t frames = (size_t)si->frameSize;
  if (frames > output_capacity_frames) frames = output_capacity_frames;

  if (si->numChannels == 2) {
    for (size_t i = 0; i < frames * 2U; ++i)
      output[i] = audio_pcm32_to_float(d->pcm[i]);
  } else {
    for (size_t f = 0; f < frames; ++f) {
      const float v = audio_pcm32_to_float(d->pcm[f]);
      output[2 * f] = v;
      output[2 * f + 1] = v;
    }
  }
  if (info) info->channels = 2;
  return (int)frames;
}
