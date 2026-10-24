#include "aac_decoder.h"
#include "audio_format_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/impl/esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "airplay_log.h"
#include "sdkconfig.h"

#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
#include "aacdecoder_lib.h"
#define AAC_MULTICHANNEL 1
#else
#define AAC_MULTICHANNEL 0
#endif

#define ADTS_HEADER_LEN AAC_DECODER_INPUT_HEADROOM
#define AAC_INPUT_MAX   8192U
/* EXPERIMENT (multichannel): scratch for one 7.1 AAC-LC access unit (2048
 * leaves room in case the decoder hands out more than 1024 frames). */
#define MC_MAX_CHANNELS 8
#define MC_MAX_FRAMES   2048U
/* CPU-time statistics are logged once per this many decoded frames
 * (~10.7 s at 48 kHz / 1024). */
#define MC_TIMING_EVERY 500U

struct aac_decoder {
  aac_decoder_config_t config;
  void *handle; /* esp_aac_dec (PacketVideo): mono/stereo only */
  uint32_t decode_errors;
#if AAC_MULTICHANNEL
  /* channels > 2: Fraunhofer FDK decodes into mc_pcm, which is then reduced
   * to the stereo output the pipeline expects. */
  HANDLE_AACDECODER fdk;
  int16_t *mc_pcm;
  uint32_t mc_decodes;
  bool mc_layout_known;
  int mc_lfe_index; /* -1 if the stream has no LFE */
  int mc_channels;
  /* Decoded channel index of each aac_speaker_t (-1 = not in the stream),
   * and FDK's label of each decoded channel. */
  int8_t mc_speaker_channel[AAC_SPEAKER_COUNT];
  AUDIO_CHANNEL_TYPE mc_type[MC_MAX_CHANNELS];
  UCHAR mc_index[MC_MAX_CHANNELS];
  char mc_label[MC_MAX_CHANNELS][16];
  int mc_applied_speaker; /* speaker the synth mask is set for; -2 = none */
  int mc_play_channel;    /* decoded channel played, -1 = mono mix */
  int64_t mc_time_sum_us;
  int64_t mc_time_max_us;
  uint32_t mc_time_frames;
  /* Per stage (parse / synthesis / output) over the same window, and how
   * many frames took FDK's single-channel output (local fdk-aac patch). */
  int64_t mc_stage_sum_us[3];
  int64_t mc_stage_max_us[3];
  uint32_t mc_stage_frames;
  uint32_t mc_fast_frames;
#endif
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
  return 4; // AirPlay 2 path is 44.1 kHz in this project.
}

static bool has_adts(const uint8_t *data, size_t len) {
  return len >= 2U && data[0] == 0xffU && (data[1] & 0xf0U) == 0xf0U;
}

static void make_adts(uint8_t *h, size_t payload_len, int sample_rate,
                      int channels) {
  const int profile = 2; // AAC LC
  const int freq_idx = adts_frequency_index(sample_rate);
  /* ADTS channel_configuration: 1..6 = that many channels (6 = 5.1),
   * 7 = 8 channels (7.1). */
  const int chan_cfg = channels == 8 ? 7 : (channels > 0 ? channels : 2);
  const int packet_len = (int)(payload_len + ADTS_HEADER_LEN);

  h[0] = 0xff;
  h[1] = 0xf1;
  h[2] = (uint8_t)(((profile - 1) << 6) | (freq_idx << 2) | (chan_cfg >> 2));
  h[3] = (uint8_t)(((chan_cfg & 3) << 6) | (packet_len >> 11));
  h[4] = (uint8_t)((packet_len & 0x7ff) >> 3);
  h[5] = (uint8_t)(((packet_len & 7) << 5) | 0x1f);
  h[6] = 0xfc;
}

static bool open_decoder(aac_decoder_t *d) {
  if (d->handle) {
    esp_aac_dec_close(d->handle);
    d->handle = NULL;
  }

  esp_aac_dec_cfg_t cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
  cfg.sample_rate = d->config.sample_rate;
  cfg.channel = d->config.channels;
  cfg.bits_per_sample = d->config.bits_per_sample > 0 ? d->config.bits_per_sample : 16;
  cfg.no_adts_header = false;
  cfg.aac_plus_enable = false;

  esp_audio_err_t err = esp_aac_dec_open(&cfg, sizeof(cfg), &d->handle);
  if (err != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "esp_aac_dec_open failed: %d", err);
    d->handle = NULL;
    return false;
  }
  return true;
}

#if AAC_MULTICHANNEL

/* Crash breadcrumbs: the last FDK step and frame count survive a panic
 * reboot in RTC memory, so the next boot can say where it died even when
 * only the web log (not the UART panic dump) is visible. */
enum {
  MC_STAGE_IDLE = 0,
  MC_STAGE_ALLOC,
  MC_STAGE_OPEN,
  MC_STAGE_SETPARAM,
  MC_STAGE_FILL,
  MC_STAGE_DECODE,
  MC_STAGE_POST,
};
#define MC_TRACE_MAGIC 0x4D43A0A0U
static RTC_NOINIT_ATTR uint32_t s_mc_trace_magic;
static RTC_NOINIT_ATTR uint32_t s_mc_trace_stage;
static RTC_NOINIT_ATTR uint32_t s_mc_trace_frames;

static inline void mc_trace(uint32_t stage, uint32_t frames) {
  s_mc_trace_stage = stage;
  s_mc_trace_frames = frames;
  s_mc_trace_magic = MC_TRACE_MAGIC;
}

/* Selected speaker (aac_speaker_t), written by the web UI / boot code and
 * read by the decode task once per frame. */
static int s_mc_speaker =
#ifdef CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER
    CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER;
#else
    AAC_SPEAKER_MIX;
#endif

/* What the running decoder plays, for the web UI. */
static portMUX_TYPE s_mc_status_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_mc_status_active;
static int s_mc_status_channel;
static int s_mc_status_channels;
static char s_mc_status_label[16];

static void mc_publish_status(bool active, int channel, int channels,
                              const char *label) {
  taskENTER_CRITICAL(&s_mc_status_mux);
  s_mc_status_active = active;
  s_mc_status_channel = channel;
  s_mc_status_channels = channels;
  strlcpy(s_mc_status_label, label ? label : "", sizeof(s_mc_status_label));
  taskEXIT_CRITICAL(&s_mc_status_mux);
}

/* Clock for FDK's per-stage timestamps (local fdk-aac patch). */
static INT64 mc_clock_us(void) { return (INT64)esp_timer_get_time(); }

static bool open_fdk_decoder(aac_decoder_t *d) {
  mc_trace(MC_STAGE_OPEN, 0);
  d->fdk = aacDecoder_Open(TT_MP4_ADTS, 1);
  if (!d->fdk) {
    ESP_LOGE(TAG, "MC: aacDecoder_Open failed");
    return false;
  }
  /* Keep every encoded channel (no FDK downmix) in WAV order, and no
   * limiter: it adds look-ahead delay and CPU, and one channel of a mix
   * cannot clip anyway. */
  mc_trace(MC_STAGE_SETPARAM, 0);
  const AAC_DECODER_ERROR e1 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_MAX_OUTPUT_CHANNELS, MC_MAX_CHANNELS);
  const AAC_DECODER_ERROR e2 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1);
  const AAC_DECODER_ERROR e3 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_LIMITER_ENABLE, 0);
  /* The first frame synthesizes every channel; once the layout is known the
   * synth mask is narrowed to the selected speaker (mc_apply_speaker). */
  d->mc_applied_speaker = -2;
  d->mc_play_channel = -1;
  d->mc_lfe_index = -1;
  aacDecoder_SetTimeHook(d->fdk, mc_clock_us);
  ESP_LOGW(TAG, "MC: FDK AAC decoder for %d Hz / %d ch (params 0x%x/0x%x/0x%x)",
           d->config.sample_rate, d->config.channels, e1, e2, e3);
  mc_trace(MC_STAGE_IDLE, 0);
  return true;
}

/* EXPERIMENT: one decoded channel on both stereo slots. */
static void mc_pick_channel(const int16_t *in, int channels, int pick,
                            size_t frames, int16_t *out) {
  for (size_t f = 0; f < frames; ++f) {
    const int16_t v = in[f * (size_t)channels + (size_t)pick];
    out[2 * f] = v;
    out[2 * f + 1] = v;
  }
}

/* EXPERIMENT: fold every channel except LFE into mono on both slots. */
static void mc_downmix_to_mono(const int16_t *in, int channels, int lfe,
                               size_t frames, int16_t *out) {
  const int used = lfe >= 0 ? channels - 1 : channels;
  const float gain = 1.0f / (float)used;
  for (size_t f = 0; f < frames; ++f) {
    const int16_t *s = in + f * (size_t)channels;
    float m = 0.0f;
    for (int c = 0; c < channels; ++c) {
      if (c != lfe) m += s[c];
    }
    m *= gain;
    const int16_t v =
        (int16_t)(m > 32767.f ? 32767 : (m < -32768.f ? -32768 : m));
    out[2 * f] = v;
    out[2 * f + 1] = v;
  }
}

static const char *mc_channel_type_name(AUDIO_CHANNEL_TYPE t) {
  switch (t) {
    case ACT_FRONT: return "front";
    case ACT_SIDE: return "side";
    case ACT_BACK: return "back";
    case ACT_LFE: return "lfe";
    case ACT_FRONT_TOP: return "front-top";
    case ACT_SIDE_TOP: return "side-top";
    case ACT_BACK_TOP: return "back-top";
    default: return "none";
  }
}

static int mc_find(const CStreamInfo *si, AUDIO_CHANNEL_TYPE type, int index) {
  for (int c = 0; c < si->numChannels && c < MC_MAX_CHANNELS; ++c) {
    if (si->pChannelType[c] == type && si->pChannelIndices[c] == index) return c;
  }
  return -1;
}

static int mc_count(const CStreamInfo *si, AUDIO_CHANNEL_TYPE type) {
  int n = 0;
  for (int c = 0; c < si->numChannels && c < MC_MAX_CHANNELS; ++c) {
    if (si->pChannelType[c] == type) ++n;
  }
  return n;
}

/* Once per decoder: map every speaker name to a decoded channel.
 *
 * FDK labels channels by MPEG position (front #0..n, side, back, lfe). 5.1
 * (configuration 6) is unambiguous: front C/L/R, back = surround pair. For
 * 7.1, Apple TV sends configuration 7, which FDK (following ISO) reports as
 * five fronts + two backs: front #0 = C, #1/#2 and #3/#4 = two pairs. ISO
 * calls #1/#2 "left/right centre" and #3/#4 L/R; Apple's 7.1 order (C L R
 * Ls Rs Lrs Rrs LFE) makes #1/#2 L/R, #3/#4 the side pair and the backs the
 * rear pair. We follow Apple: in the first test the #1/#2 pair carried the
 * most energy of an upmixed stereo song, as main L/R should. */
static void mc_build_speaker_map(aac_decoder_t *d, const CStreamInfo *si) {
  int8_t *m = d->mc_speaker_channel;
  for (int s = 0; s < AAC_SPEAKER_COUNT; ++s) m[s] = -1;
  if (!si->pChannelType || !si->pChannelIndices) {
    d->mc_layout_known = true;
    return;
  }
  const int fronts = mc_count(si, ACT_FRONT);
  const int sides = mc_count(si, ACT_SIDE);
  const int backs = mc_count(si, ACT_BACK);
  const int lr = (fronts & 1) ? 1 : 0; /* odd count: front #0 is the centre */

  if (fronts & 1) m[AAC_SPEAKER_C] = (int8_t)mc_find(si, ACT_FRONT, 0);
  m[AAC_SPEAKER_L] = (int8_t)mc_find(si, ACT_FRONT, lr);
  m[AAC_SPEAKER_R] = (int8_t)mc_find(si, ACT_FRONT, lr + 1);
  m[AAC_SPEAKER_LFE] = (int8_t)mc_find(si, ACT_LFE, 0);
  bool rear_pair = false;
  if (sides >= 2) {
    m[AAC_SPEAKER_LS] = (int8_t)mc_find(si, ACT_SIDE, 0);
    m[AAC_SPEAKER_RS] = (int8_t)mc_find(si, ACT_SIDE, 1);
    rear_pair = backs >= 2;
  } else if (fronts >= lr + 4) { /* Apple 7.1 in configuration 7 */
    m[AAC_SPEAKER_LS] = (int8_t)mc_find(si, ACT_FRONT, lr + 2);
    m[AAC_SPEAKER_RS] = (int8_t)mc_find(si, ACT_FRONT, lr + 3);
    rear_pair = backs >= 2;
  } else if (backs >= 2) { /* 5.1: the backs are the surround pair */
    m[AAC_SPEAKER_LS] = (int8_t)mc_find(si, ACT_BACK, 0);
    m[AAC_SPEAKER_RS] = (int8_t)mc_find(si, ACT_BACK, 1);
  }
  if (rear_pair) {
    m[AAC_SPEAKER_LRS] = (int8_t)mc_find(si, ACT_BACK, 0);
    m[AAC_SPEAKER_RRS] = (int8_t)mc_find(si, ACT_BACK, 1);
  } else {
    m[AAC_SPEAKER_LRS] = m[AAC_SPEAKER_LS];
    m[AAC_SPEAKER_RRS] = m[AAC_SPEAKER_RS];
  }
  d->mc_lfe_index = m[AAC_SPEAKER_LFE];
  d->mc_channels = si->numChannels;

  for (int c = 0; c < si->numChannels && c < MC_MAX_CHANNELS; ++c) {
    d->mc_type[c] = si->pChannelType[c];
    d->mc_index[c] = si->pChannelIndices[c];
    snprintf(d->mc_label[c], sizeof(d->mc_label[c]), "%s #%u",
             mc_channel_type_name(si->pChannelType[c]),
             si->pChannelIndices[c]);
    const char *speaker = "-";
    for (int s = 0; s < AAC_SPEAKER_COUNT; ++s) {
      /* First match wins, so a 5.1 side pair is named Side, not Rear. */
      if (m[s] == c) {
        speaker = aac_speaker_name(s);
        break;
      }
    }
    ESP_LOGW(TAG, "MC: decoded channel %d = %s -> %s", c, d->mc_label[c],
             speaker);
  }
  ESP_LOGW(TAG, "MC: stream sr=%d ch=%d frame=%d channelConfig=%d aacCh=%d",
           si->sampleRate, si->numChannels, si->frameSize, si->channelConfig,
           si->aacNumChannels);
  d->mc_layout_known = true;
}

/* Point FDK's synthesis (local fdk-aac patch) at the selected speaker, so it
 * runs the filterbank for that channel only: all eight do not fit one frame.
 * The channel is named by its label (type + index), not its number: FDK
 * reorders channels after synthesis, and only the label follows the data.
 * Called before every decode; acts only when the selection changed. A
 * channel switched on mid-stream has stale overlap for one frame. */
static void mc_apply_speaker(aac_decoder_t *d) {
  if (!d->mc_layout_known) return;
  const int speaker = __atomic_load_n(&s_mc_speaker, __ATOMIC_RELAXED);
  if (speaker == d->mc_applied_speaker) return;
  const int channel =
      (speaker >= 0 && speaker < AAC_SPEAKER_COUNT) ? d->mc_speaker_channel[speaker] : -1;
  if (channel >= 0) {
    aacDecoder_SetSynthChannel(d->fdk, d->mc_type[channel], d->mc_index[channel]);
  } else {
    aacDecoder_SetSynthChannel(d->fdk, ACT_NONE, 0);
  }
  d->mc_applied_speaker = speaker;
  d->mc_play_channel = channel;
  if (d->mc_channels > 2)
    audio_log_surround_playback(TAG, d->config.sample_rate, d->mc_channels,
                                aac_speaker_name(speaker), channel);
  else
    audio_log_stereo_playback(TAG, "AAC", d->config.sample_rate, 16, "buffered");
  mc_publish_status(true, channel, d->mc_channels,
                    channel >= 0 ? d->mc_label[channel] : "mix");
}

/* At the start of a stream and then every MC_TIMING_EVERY frames. In the
 * single-channel fast path only the played channel is in mc_pcm (mono). */
static void mc_log_peaks(aac_decoder_t *d, int ch, size_t frames, bool fast) {
  const uint32_t n = d->mc_decodes;
  if (n > 5 && (n % MC_TIMING_EVERY) != 0) return;
  if (fast) {
    int peak = 0;
    for (size_t f = 0; f < frames; ++f) {
      int v = d->mc_pcm[f];
      if (v < 0) v = -v;
      if (v > peak) peak = v;
    }
    ESP_LOGW(TAG, "MC #%lu: %u frames, fast path: channel %d only, peak %d",
             (unsigned long)n, (unsigned)frames, d->mc_play_channel, peak);
    return;
  }
  char peaks[96];
  size_t pos = 0;
  peaks[0] = '\0';
  int played_peak = 0, other_peak = 0, other_channel = -1;
  for (int c = 0; c < ch && c < MC_MAX_CHANNELS; ++c) {
    int peak = 0;
    for (size_t f = 0; f < frames; ++f) {
      int v = d->mc_pcm[f * (size_t)ch + (size_t)c];
      if (v < 0) v = -v;
      if (v > peak) peak = v;
    }
    if (c == d->mc_play_channel) {
      played_peak = peak;
    } else if (peak > other_peak) {
      other_peak = peak;
      other_channel = c;
    }
    if (pos < sizeof(peaks))
      pos += (size_t)snprintf(peaks + pos, sizeof(peaks) - pos, "%s%d",
                              c ? "," : "", peak);
  }
  ESP_LOGW(TAG, "MC #%lu: %u frames x %d ch, peaks[%s]", (unsigned long)n,
           (unsigned)frames, ch, peaks);
  /* Full output with one channel synthesized: the others are stale planes
   * FDK no longer clears, so only flag a silent played channel when every
   * channel is synthesized (mix) or the played one is plainly missing. */
  if (d->mc_play_channel >= 0 && played_peak == 0 && other_peak > 0 &&
      d->mc_applied_speaker == AAC_SPEAKER_MIX)
    ESP_LOGE(TAG, "MC: played channel %d is silent but channel %d has signal",
             d->mc_play_channel, other_channel);
}

static void mc_account_stage(aac_decoder_t *d, int stage, int64_t us) {
  d->mc_stage_sum_us[stage] += us;
  if (us > d->mc_stage_max_us[stage]) d->mc_stage_max_us[stage] = us;
}

/* Every MC_TIMING_EVERY frames: total decode time per frame against the
 * frame budget, split into parse (bitstream + spectra of all channels),
 * synthesis (concealment/DRC/IMDCT of the played channel) and output
 * (channel reorder + PCM conversion). */
static void mc_account_time(aac_decoder_t *d, int64_t us, int frame_size,
                            int sample_rate) {
  d->mc_time_sum_us += us;
  if (us > d->mc_time_max_us) d->mc_time_max_us = us;
  if (++d->mc_time_frames < MC_TIMING_EVERY) return;
  const int64_t budget_us =
      sample_rate > 0 ? (int64_t)frame_size * 1000000LL / sample_rate : 0;
  const int64_t avg_us = d->mc_time_sum_us / (int64_t)d->mc_time_frames;
  ESP_LOGW(TAG, "MC CPU: FDK decode avg %lld us, max %lld us per frame over %lu "
                "frames; frame budget %lld us -> avg %lld%% of one core; "
                "decode task stack never below %u B free",
           (long long)avg_us, (long long)d->mc_time_max_us,
           (unsigned long)d->mc_time_frames, (long long)budget_us,
           budget_us > 0 ? (long long)(avg_us * 100 / budget_us) : -1LL,
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  if (d->mc_stage_frames > 0) {
    const int64_t n = d->mc_stage_frames;
    ESP_LOGW(TAG, "MC CPU stages (avg/max us): parse %lld/%lld, synthesis "
                  "%lld/%lld, output %lld/%lld; fast single-channel output "
                  "%lu/%lu frames",
             (long long)(d->mc_stage_sum_us[0] / n), (long long)d->mc_stage_max_us[0],
             (long long)(d->mc_stage_sum_us[1] / n), (long long)d->mc_stage_max_us[1],
             (long long)(d->mc_stage_sum_us[2] / n), (long long)d->mc_stage_max_us[2],
             (unsigned long)d->mc_fast_frames, (unsigned long)d->mc_time_frames);
  }
  d->mc_time_sum_us = 0;
  d->mc_time_max_us = 0;
  d->mc_time_frames = 0;
  memset(d->mc_stage_sum_us, 0, sizeof(d->mc_stage_sum_us));
  memset(d->mc_stage_max_us, 0, sizeof(d->mc_stage_max_us));
  d->mc_stage_frames = 0;
  d->mc_fast_frames = 0;
}

static int mc_decode(aac_decoder_t *d, const uint8_t *data, size_t len,
                     int16_t *output, size_t output_capacity_frames,
                     aac_decode_info_t *info) {
  UCHAR *in = (UCHAR *)data;
  UINT in_size = (UINT)len;
  UINT valid = (UINT)len;

  mc_apply_speaker(d);
  const int64_t t0 = esp_timer_get_time();
  mc_trace(MC_STAGE_FILL, d->mc_decodes);
  AAC_DECODER_ERROR err = aacDecoder_Fill(d->fdk, &in, &in_size, &valid);
  if (err == AAC_DEC_OK) {
    mc_trace(MC_STAGE_DECODE, d->mc_decodes);
    err = aacDecoder_DecodeFrame(d->fdk, (INT_PCM *)d->mc_pcm,
                                 (INT)(MC_MAX_FRAMES * MC_MAX_CHANNELS), 0);
  }
  const int64_t t1 = esp_timer_get_time();
  mc_trace(MC_STAGE_POST, d->mc_decodes);

  if (err != AAC_DEC_OK) {
    if ((++d->decode_errors & 63U) == 1U || d->decode_errors <= 8U)
      ESP_LOGW(TAG, "MC: FDK decode error 0x%x (in %u B, %u left; %lu total)",
               err, (unsigned)len, (unsigned)valid,
               (unsigned long)d->decode_errors);
    return -1;
  }

  const CStreamInfo *si = aacDecoder_GetStreamInfo(d->fdk);
  if (!si || si->numChannels <= 0 || si->numChannels > MC_MAX_CHANNELS ||
      si->frameSize <= 0) {
    return -1;
  }
  INT fast = 0;
  INT64 synth_start = 0, synth_end = 0;
  aacDecoder_GetSynthInfo(d->fdk, &fast, &synth_start, &synth_end);
  if (!d->mc_layout_known) {
    mc_build_speaker_map(d, si);
    mc_apply_speaker(d); /* narrows synthesis from the next frame on */
  }
  ++d->mc_decodes;
  mc_trace(MC_STAGE_IDLE, d->mc_decodes);
  if (synth_start >= t0 && synth_end >= synth_start && t1 >= synth_end) {
    mc_account_stage(d, 0, synth_start - t0);
    mc_account_stage(d, 1, synth_end - synth_start);
    mc_account_stage(d, 2, t1 - synth_end);
    ++d->mc_stage_frames;
  }
  if (fast) ++d->mc_fast_frames;
  mc_account_time(d, t1 - t0, si->frameSize, si->sampleRate);

  const int ch = si->numChannels;
  size_t frames = (size_t)si->frameSize;
  mc_log_peaks(d, ch, frames, fast != 0);
  if (frames > output_capacity_frames) frames = output_capacity_frames;

  const int pick = d->mc_play_channel;
  if (fast) {
    /* FDK wrote just the played channel, mono. */
    mc_pick_channel(d->mc_pcm, 1, 0, frames, output);
  } else if (ch >= 3 && pick >= 0 && pick < ch) {
    mc_pick_channel(d->mc_pcm, ch, pick, frames, output);
  } else if (ch >= 3) {
    mc_downmix_to_mono(d->mc_pcm, ch, d->mc_lfe_index, frames, output);
  } else if (ch == 2) {
    memcpy(output, d->mc_pcm, frames * 2U * sizeof(int16_t));
  } else {
    mc_pick_channel(d->mc_pcm, 1, 0, frames, output);
  }
  if (info) info->channels = 2;
  return (int)frames;
}

#endif /* AAC_MULTICHANNEL */

void aac_decoder_report_last_run(void) {
#if AAC_MULTICHANNEL
  static const char *const stages[] = {"idle", "scratch alloc",
                                       "aacDecoder_Open", "aacDecoder_SetParam",
                                       "aacDecoder_Fill", "aacDecoder_DecodeFrame",
                                       "after decode"};
  const esp_reset_reason_t why = esp_reset_reason();
  if (s_mc_trace_magic == MC_TRACE_MAGIC && why != ESP_RST_POWERON) {
    const uint32_t st = s_mc_trace_stage;
    ESP_LOGW(TAG, "MC: last reset reason %d; FDK step at reset: %s, after %lu "
                  "decoded frames",
             (int)why,
             st < sizeof(stages) / sizeof(stages[0]) ? stages[st] : "?",
             (unsigned long)s_mc_trace_frames);
  } else {
    ESP_LOGW(TAG, "MC: last reset reason %d; no FDK trace", (int)why);
  }
  mc_trace(MC_STAGE_IDLE, 0);
#endif
}

const char *aac_speaker_name(int speaker) {
  static const char *const names[AAC_SPEAKER_COUNT] = {
      "Front Left (L)",  "Front Right (R)", "Centre (C)",
      "Subwoofer (LFE)", "Side Left (Ls)",  "Side Right (Rs)",
      "Rear Left (Lrs)", "Rear Right (Rrs)"};
  return (speaker >= 0 && speaker < AAC_SPEAKER_COUNT) ? names[speaker]
                                                       : "Mono mix";
}

void aac_decoder_set_speaker(int speaker) {
  if (speaker < AAC_SPEAKER_MIX || speaker >= AAC_SPEAKER_COUNT)
    speaker = AAC_SPEAKER_MIX;
#if AAC_MULTICHANNEL
  __atomic_store_n(&s_mc_speaker, speaker, __ATOMIC_RELAXED);
#else
  (void)speaker;
#endif
}

int aac_decoder_get_speaker(void) {
#if AAC_MULTICHANNEL
  return __atomic_load_n(&s_mc_speaker, __ATOMIC_RELAXED);
#else
  return AAC_SPEAKER_MIX;
#endif
}

bool aac_decoder_get_mc_status(int *output_channel, int *stream_channels,
                               char *label, size_t label_len) {
#if AAC_MULTICHANNEL
  taskENTER_CRITICAL(&s_mc_status_mux);
  const bool active = s_mc_status_active;
  if (output_channel) *output_channel = s_mc_status_channel;
  if (stream_channels) *stream_channels = s_mc_status_channels;
  if (label && label_len) strlcpy(label, s_mc_status_label, label_len);
  taskEXIT_CRITICAL(&s_mc_status_mux);
  return active;
#else
  (void)output_channel;
  (void)stream_channels;
  if (label && label_len) label[0] = '\0';
  return false;
#endif
}

aac_decoder_t *aac_decoder_create(const aac_decoder_config_t *config) {
  if (!config || config->sample_rate <= 0 || config->channels <= 0) {
    return NULL;
  }
#if !AAC_MULTICHANNEL
  if (config->channels > 2) {
    ESP_LOGE(TAG, "%d-channel AAC needs CONFIG_AIRPLAY_MULTICHANNEL",
             config->channels);
    return NULL;
  }
#endif

  aac_decoder_t *d = calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->config = *config;
#if AAC_MULTICHANNEL
  if (config->channels > 2) {
    if (config->channels > MC_MAX_CHANNELS) {
      aac_decoder_destroy(d);
      return NULL;
    }
    const size_t bytes = MC_MAX_FRAMES * MC_MAX_CHANNELS * sizeof(int16_t);
    mc_trace(MC_STAGE_ALLOC, 0);
    d->mc_pcm = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d->mc_pcm) d->mc_pcm = malloc(bytes);
    if (!d->mc_pcm || !open_fdk_decoder(d)) {
      ESP_LOGE(TAG, "MC: cannot create the multichannel decoder");
      aac_decoder_destroy(d);
      return NULL;
    }
    return d;
  }
#endif
  if (!open_decoder(d)) {
    aac_decoder_destroy(d);
    return NULL;
  }
  return d;
}

void aac_decoder_destroy(aac_decoder_t *d) {
  if (!d) {
    return;
  }
  if (d->handle) {
    esp_aac_dec_close(d->handle);
  }
#if AAC_MULTICHANNEL
  if (d->fdk) {
    aacDecoder_Close(d->fdk);
    mc_publish_status(false, -1, 0, NULL);
  }
  free(d->mc_pcm);
#endif
  free(d);
}

int aac_decoder_decode(aac_decoder_t *d, uint8_t *input,
                       size_t input_len, int16_t *output,
                       size_t output_capacity_frames,
                       aac_decode_info_t *info) {
  bool ready = d && d->handle;
#if AAC_MULTICHANNEL
  ready = ready || (d && d->fdk);
#endif
  if (!ready || !input || input_len == 0 || !output ||
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

#if AAC_MULTICHANNEL
  if (d->fdk) {
    return mc_decode(d, decode_data, decode_len, output,
                     output_capacity_frames, info);
  }
#endif

  const int channels = d->config.channels > 0 ? d->config.channels : 2;
  esp_audio_dec_in_raw_t raw = {
      .buffer = (uint8_t *)decode_data,
      .len = (uint32_t)decode_len,
      .consumed = 0,
      .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
  };
  esp_audio_dec_out_frame_t frame = {
      .buffer = (uint8_t *)output,
      .len = (uint32_t)(output_capacity_frames * (size_t)channels * sizeof(int16_t)),
      .decoded_size = 0,
  };
  esp_audio_dec_info_t dec_info = {0};

  esp_audio_err_t err = esp_aac_dec_decode(d->handle, &raw, &frame, &dec_info);
  if (err != ESP_AUDIO_ERR_OK) {
    /* Shairport keeps the AAC decoding chain alive across a bad access unit.
     * Dropping the AU here lets the codec preserve/recover its overlap history
     * on the following block instead of manufacturing a new decoder epoch. */
    if ((++d->decode_errors & 63U) == 1U)
      AP_LOGW(TAG, "decode error=%d; dropping AAC block without decoder reset (%lu total)",
               err, (unsigned long)d->decode_errors);
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
  }
  return (int)frames;
}
