#include "aac_decoder.h"
#include "audio_out_sample.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "airplay_log.h"
#include "aacdecoder_lib.h"
#include "sdkconfig.h"

#define AAC_LIMITER_ATTACK_MS 15U
#if defined(CONFIG_AIRPLAY_AAC_FDK_LIMITER) && CONFIG_AIRPLAY_AAC_FDK_LIMITER
#define AAC_LIMITER_ENABLED 1
#else
#define AAC_LIMITER_ENABLED 0
#endif

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
  uint32_t limiter_delay_frames;
  uint32_t next_input_rtp;
  bool have_next_rtp;
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
  /* Keep DRC/channel mapping unchanged; the S32 limiter is an explicit
   * test option with its own lookahead compensation. */
  const AAC_DECODER_ERROR e1 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_MAX_OUTPUT_CHANNELS, FDK_MAX_CHANNELS);
  const AAC_DECODER_ERROR e2 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1);
  const AAC_DECODER_ERROR e3 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_LIMITER_ENABLE, AAC_LIMITER_ENABLED);
  /* RTP already timestamps each AU. FDK's default interpolation adds one
   * unaccounted frame (1024 samples); noise substitution has no extra delay. */
  const AAC_DECODER_ERROR e4 =
      aacDecoder_SetParam(d->fdk, AAC_CONCEAL_METHOD, 1);
  /* Reserve the limiter at open, instead of silently falling back to an
   * unlimited frame on allocation failure. Do not allocate it when off. */
  const AAC_DECODER_ERROR e5 = AAC_LIMITER_ENABLED
      ? aacDecoder_SetParam(d->fdk, AAC_PCM_LIMITER_ATTACK_TIME, AAC_LIMITER_ATTACK_MS)
      : AAC_DEC_OK;
  if (e1 != AAC_DEC_OK || e2 != AAC_DEC_OK || e3 != AAC_DEC_OK ||
      e4 != AAC_DEC_OK || e5 != AAC_DEC_OK) {
    ESP_LOGE(TAG, "FDK S32 setup failed (params 0x%x/0x%x/0x%x/0x%x/0x%x)",
             e1, e2, e3, e4, e5);
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
  d->limiter_delay_frames = AAC_LIMITER_ENABLED
      ? (uint32_t)config->sample_rate * AAC_LIMITER_ATTACK_MS / 1000U : 0;
  const size_t bytes = FDK_MAX_FRAMES * FDK_MAX_CHANNELS * sizeof(*d->pcm);
  d->pcm = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!d->pcm) d->pcm = malloc(bytes);
  if (!d->pcm || !open_fdk_decoder(d)) {
    ESP_LOGE(TAG, "Cannot create FDK S32 AAC decoder");
    aac_decoder_destroy(d);
    return NULL;
  }
  if (AAC_LIMITER_ENABLED)
    ESP_LOGI(TAG, "FDK PCM limiter ON: S32, lookahead %lu frames, RTP compensated",
             (unsigned long)d->limiter_delay_frames);
  return d;
}

uint32_t aac_decoder_limiter_delay_frames(const aac_decoder_t *d) {
  return d ? d->limiter_delay_frames : 0;
}

uint32_t aac_decoder_tail_wait_ms(const aac_decoder_t *d, uint32_t requested,
                                   uint32_t maximum) {
  return d && d->have_next_rtp && d->limiter_delay_frames && requested > maximum
             ? maximum : requested;
}

bool aac_decoder_tail_due(const aac_decoder_t *d, uint32_t wanted,
                            uint32_t lead) {
  if (!d || !d->have_next_rtp || !d->limiter_delay_frames || lead > INT32_MAX)
    return false;
  const uint32_t start = d->next_input_rtp - d->limiter_delay_frames;
  return (int32_t)(start - wanted) <= (int32_t)lead;
}

int aac_decoder_drain(aac_decoder_t *d, float *out, size_t cap,
                       aac_decode_info_t *info) {
  if (info) memset(info, 0, sizeof(*info));
  if (!d || !out || !info) return -1;
  if (!d->have_next_rtp) return 0;
  const uint32_t delay = d->limiter_delay_frames;
  if (delay > cap) return -1;
  UINT frames = 0;
  if (aacDecoder_DrainLimiter32(d->fdk, d->pcm,
                                 FDK_MAX_FRAMES * FDK_MAX_CHANNELS,
                                 &frames) != AAC_DEC_OK || frames != delay)
    return -1;
  const CStreamInfo *si = aacDecoder_GetStreamInfo(d->fdk);
  if (frames && (!si || si->numChannels < 1 || si->numChannels > 2)) return -1;
  for (UINT f = 0; f < frames; ++f) {
    out[2*f] = audio_pcm32_to_float(d->pcm[f * si->numChannels]);
    out[2*f+1] = audio_pcm32_to_float(d->pcm[f * si->numChannels +
                                                (si->numChannels == 2 ? 1 : 0)]);
  }
  info->channels = 2;
  info->limiter_delay_frames = delay;
  info->output_rtp = d->next_input_rtp - delay;
  d->have_next_rtp = false;
  return (int)frames;
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
                       uint32_t input_rtp, bool mute_output,
                       aac_decode_info_t *info) {
  if (info) memset(info, 0, sizeof(*info));
  if (!d || !d->fdk || !input || input_len == 0 || !output ||
      output_capacity_frames == 0) {
    return -1;
  }

  /* Pause with a contiguous RTP sequence preserves the pending music.
   * A new/muted sequence cannot carry old PCM or gain into the new one. */
  if (mute_output || !d->have_next_rtp || input_rtp != d->next_input_rtp) {
    if (aacDecoder_ResetLimiter(d->fdk) != AAC_DEC_OK) return -1;
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
    (void)aacDecoder_ResetLimiter(d->fdk);
    d->have_next_rtp = false;
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
  d->limiter_delay_frames = aacDecoder_GetLimiterDelay(d->fdk);
  d->next_input_rtp = input_rtp + (uint32_t)frames;
  d->have_next_rtp = true;
  if (mute_output) {
    memset(output, 0, frames * 2U * sizeof(*output));
    /* Also discard this muted AU's tail, which otherwise leaks out during
     * the next unmuted AU even though output[] was zeroed. */
    if (aacDecoder_ResetLimiter(d->fdk) != AAC_DEC_OK) return -1;
  }
  if (info) {
    info->channels = 2;
    info->limiter_delay_frames = d->limiter_delay_frames;
    info->output_rtp = input_rtp - d->limiter_delay_frames;
  }
  return (int)frames;
}
