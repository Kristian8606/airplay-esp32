#include "audio_eq.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "audio_eq";
static const char *EQ_NS = "audio_eq";
static const char *EQ_KEY = "config";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
  /* Standard biquad naming: b = numerator, a = denominator (a0 = 1). */
  float b0;
  float b1;
  float b2;
  float a1;
  float a2;
} biquad_coeff_t;

typedef struct {
  /* Transposed Direct Form II state. */
  float z1;
  float z2;
} biquad_state_t;

typedef struct {
  audio_eq_config_t config;
  biquad_coeff_t coeff_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  biquad_coeff_t coeff_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  biquad_state_t state_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  biquad_state_t state_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  int sample_rate;
  float preamp_gain;
  bool ready;
  uint64_t clip_count;
} audio_eq_runtime_t;

static audio_eq_runtime_t s_eq;

static bool eq_is_finite(float value) { return isfinite((double)value); }

void audio_eq_default_config(audio_eq_config_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->version = AUDIO_EQ_CONFIG_VERSION;
  out->enabled = 0;
  out->channel_mode = AUDIO_EQ_CHANNEL_STEREO;
  out->preamp_db = 0.0f;
}

bool audio_eq_validate_config(const audio_eq_config_t *config) {
  if (!config || config->version != AUDIO_EQ_CONFIG_VERSION ||
      config->enabled > 1 || config->channel_mode >= AUDIO_EQ_CHANNEL_COUNT ||
      !eq_is_finite(config->preamp_db) || config->preamp_db < -24.0f ||
      config->preamp_db > 0.0f ||
      config->left.filter_count > AUDIO_EQ_MAX_FILTERS_PER_CHANNEL ||
      config->right.filter_count > AUDIO_EQ_MAX_FILTERS_PER_CHANNEL) {
    return false;
  }

  const audio_eq_output_config_t *outputs[2] = {&config->left, &config->right};
  for (int ch = 0; ch < 2; ++ch) {
    const audio_eq_output_config_t *output = outputs[ch];
    for (uint8_t i = 0; i < output->filter_count; ++i) {
      const audio_eq_filter_config_t *f = &output->filters[i];
      if (f->enabled > 1 || f->type >= AUDIO_EQ_FILTER_COUNT ||
          !eq_is_finite(f->frequency_hz) || f->frequency_hz < 10.0f ||
          f->frequency_hz > 20000.0f || !eq_is_finite(f->gain_db) ||
          f->gain_db < -24.0f || f->gain_db > 24.0f || !eq_is_finite(f->q) ||
          f->q < 0.05f || f->q > 50.0f) {
        return false;
      }
    }
  }
  return true;
}

esp_err_t audio_eq_load_config(audio_eq_config_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  audio_eq_default_config(out);

  nvs_handle_t h;
  esp_err_t err = nvs_open(EQ_NS, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  audio_eq_config_t saved;
  size_t len = sizeof(saved);
  err = nvs_get_blob(h, EQ_KEY, &saved, &len);
  nvs_close(h);

  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  if (len != sizeof(saved) || !audio_eq_validate_config(&saved)) {
    ESP_LOGW(TAG, "Ignoring invalid/old EQ config (size=%u)",
             (unsigned)len);
    return ESP_OK;
  }

  *out = saved;
  return ESP_OK;
}

esp_err_t audio_eq_save_config(const audio_eq_config_t *config) {
  if (!audio_eq_validate_config(config)) return ESP_ERR_INVALID_ARG;

  nvs_handle_t h;
  esp_err_t err = nvs_open(EQ_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(h, EQ_KEY, config, sizeof(*config));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

static bool calc_biquad(audio_eq_filter_type_t type, float frequency_hz,
                        float peak_gain_db, float q, int sample_rate,
                        biquad_coeff_t *out) {
  if (!out || sample_rate <= 0 || frequency_hz <= 0.0f ||
      frequency_hz >= ((float)sample_rate * 0.5f) || q <= 0.0f) {
    return false;
  }

  /* These are the same bilinear-transform equations used by the supplied
   * Biquad.h. Its Fc was pre-normalised as Hz / SAMPLE_RATE; do it here
   * explicitly so 44.1/48 kHz streams are both handled correctly. */
  const double K = tan(M_PI * (double)frequency_hz / (double)sample_rate);
  const double V = pow(10.0, fabs((double)peak_gain_db) / 20.0);
  const double Q = (double)q;
  double norm = 1.0;
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

  switch (type) {
    case AUDIO_EQ_FILTER_LP:
      norm = 1.0 / (1.0 + K / Q + K * K);
      b0 = K * K * norm;
      b1 = 2.0 * b0;
      b2 = b0;
      a1 = 2.0 * (K * K - 1.0) * norm;
      a2 = (1.0 - K / Q + K * K) * norm;
      break;

    case AUDIO_EQ_FILTER_HP:
      norm = 1.0 / (1.0 + K / Q + K * K);
      b0 = norm;
      b1 = -2.0 * b0;
      b2 = b0;
      a1 = 2.0 * (K * K - 1.0) * norm;
      a2 = (1.0 - K / Q + K * K) * norm;
      break;

    case AUDIO_EQ_FILTER_BP:
      norm = 1.0 / (1.0 + K / Q + K * K);
      b0 = (K / Q) * norm;
      b1 = 0.0;
      b2 = -b0;
      a1 = 2.0 * (K * K - 1.0) * norm;
      a2 = (1.0 - K / Q + K * K) * norm;
      break;

    case AUDIO_EQ_FILTER_NOTCH:
      norm = 1.0 / (1.0 + K / Q + K * K);
      b0 = (1.0 + K * K) * norm;
      b1 = 2.0 * (K * K - 1.0) * norm;
      b2 = b0;
      a1 = b1;
      a2 = (1.0 - K / Q + K * K) * norm;
      break;

    case AUDIO_EQ_FILTER_PK:
      if (peak_gain_db >= 0.0f) {
        norm = 1.0 / (1.0 + K / Q + K * K);
        b0 = (1.0 + V * K / Q + K * K) * norm;
        b1 = 2.0 * (K * K - 1.0) * norm;
        b2 = (1.0 - V * K / Q + K * K) * norm;
        a1 = b1;
        a2 = (1.0 - K / Q + K * K) * norm;
      } else {
        norm = 1.0 / (1.0 + V * K / Q + K * K);
        b0 = (1.0 + K / Q + K * K) * norm;
        b1 = 2.0 * (K * K - 1.0) * norm;
        b2 = (1.0 - K / Q + K * K) * norm;
        a1 = b1;
        a2 = (1.0 - V * K / Q + K * K) * norm;
      }
      break;

    case AUDIO_EQ_FILTER_LS:
      /* Match the supplied Biquad.h: fixed shelf slope (Q is not used). */
      if (peak_gain_db >= 0.0f) {
        norm = 1.0 / (1.0 + sqrt(2.0) * K + K * K);
        b0 = (1.0 + sqrt(2.0 * V) * K + V * K * K) * norm;
        b1 = 2.0 * (V * K * K - 1.0) * norm;
        b2 = (1.0 - sqrt(2.0 * V) * K + V * K * K) * norm;
        a1 = 2.0 * (K * K - 1.0) * norm;
        a2 = (1.0 - sqrt(2.0) * K + K * K) * norm;
      } else {
        norm = 1.0 / (1.0 + sqrt(2.0 * V) * K + V * K * K);
        b0 = (1.0 + sqrt(2.0) * K + K * K) * norm;
        b1 = 2.0 * (K * K - 1.0) * norm;
        b2 = (1.0 - sqrt(2.0) * K + K * K) * norm;
        a1 = 2.0 * (V * K * K - 1.0) * norm;
        a2 = (1.0 - sqrt(2.0 * V) * K + V * K * K) * norm;
      }
      break;

    case AUDIO_EQ_FILTER_HS:
      /* Match the supplied Biquad.h: fixed shelf slope (Q is not used). */
      if (peak_gain_db >= 0.0f) {
        norm = 1.0 / (1.0 + sqrt(2.0) * K + K * K);
        b0 = (V + sqrt(2.0 * V) * K + K * K) * norm;
        b1 = 2.0 * (K * K - V) * norm;
        b2 = (V - sqrt(2.0 * V) * K + K * K) * norm;
        a1 = 2.0 * (K * K - 1.0) * norm;
        a2 = (1.0 - sqrt(2.0) * K + K * K) * norm;
      } else {
        norm = 1.0 / (V + sqrt(2.0 * V) * K + K * K);
        b0 = (1.0 + sqrt(2.0) * K + K * K) * norm;
        b1 = 2.0 * (K * K - 1.0) * norm;
        b2 = (1.0 - sqrt(2.0) * K + K * K) * norm;
        a1 = 2.0 * (K * K - V) * norm;
        a2 = (V - sqrt(2.0 * V) * K + K * K) * norm;
      }
      break;

    default:
      return false;
  }

  if (!isfinite(b0) || !isfinite(b1) || !isfinite(b2) || !isfinite(a1) ||
      !isfinite(a2)) {
    return false;
  }
  out->b0 = (float)b0;
  out->b1 = (float)b1;
  out->b2 = (float)b2;
  out->a1 = (float)a1;
  out->a2 = (float)a2;
  return true;
}

void audio_eq_reset_state(void) {
  memset(s_eq.state_l, 0, sizeof(s_eq.state_l));
  memset(s_eq.state_r, 0, sizeof(s_eq.state_r));
}

static bool prepare_output(const audio_eq_output_config_t *cfg,
                           biquad_coeff_t *coeff, int sample_rate,
                           const char *name) {
  for (uint8_t i = 0; i < cfg->filter_count; ++i) {
    const audio_eq_filter_config_t *f = &cfg->filters[i];
    if (!f->enabled) {
      memset(&coeff[i], 0, sizeof(coeff[i]));
      coeff[i].b0 = 1.0f;
      continue;
    }
    if (f->frequency_hz >= ((float)sample_rate * 0.5f) ||
        !calc_biquad((audio_eq_filter_type_t)f->type, f->frequency_hz,
                     f->gain_db, f->q, sample_rate, &coeff[i])) {
      ESP_LOGE(TAG, "Invalid %s filter %u for %d Hz stream", name,
               (unsigned)(i + 1), sample_rate);
      return false;
    }
  }
  return true;
}

static bool prepare_for_rate(int sample_rate) {
  if (sample_rate <= 0) return false;
  if (s_eq.ready && s_eq.sample_rate == sample_rate) return true;

  if (!prepare_output(&s_eq.config.left, s_eq.coeff_l, sample_rate, "left") ||
      !prepare_output(&s_eq.config.right, s_eq.coeff_r, sample_rate, "right")) {
    s_eq.ready = false;
    return false;
  }

  s_eq.preamp_gain = powf(10.0f, s_eq.config.preamp_db / 20.0f);
  s_eq.sample_rate = sample_rate;
  s_eq.ready = true;
  audio_eq_reset_state();
  ESP_LOGI(TAG,
           "Ready: enabled=%u mode=%s left=%u right=%u preamp=%.2fdB sr=%d",
           (unsigned)s_eq.config.enabled,
           audio_eq_channel_mode_name(
               (audio_eq_channel_mode_t)s_eq.config.channel_mode),
           (unsigned)s_eq.config.left.filter_count,
           (unsigned)s_eq.config.right.filter_count, s_eq.config.preamp_db,
           sample_rate);
  return true;
}

esp_err_t audio_eq_init(void) {
  memset(&s_eq, 0, sizeof(s_eq));
  esp_err_t err = audio_eq_load_config(&s_eq.config);
  if (err != ESP_OK) {
    audio_eq_default_config(&s_eq.config);
    ESP_LOGW(TAG, "EQ config load failed: %s; using defaults",
             esp_err_to_name(err));
    return ESP_OK;
  }
  ESP_LOGI(TAG,
           "Config loaded: enabled=%u mode=%s left=%u right=%u preamp=%.2fdB",
           (unsigned)s_eq.config.enabled,
           audio_eq_channel_mode_name(
               (audio_eq_channel_mode_t)s_eq.config.channel_mode),
           (unsigned)s_eq.config.left.filter_count,
           (unsigned)s_eq.config.right.filter_count, s_eq.config.preamp_db);
  return ESP_OK;
}

static inline float biquad_process(float x, const biquad_coeff_t *c,
                                   biquad_state_t *state) {
  const float y = c->b0 * x + state->z1;
  state->z1 = c->b1 * x - c->a1 * y + state->z2;
  state->z2 = c->b2 * x - c->a2 * y;
  return y;
}

static inline float process_chain(float x, const audio_eq_output_config_t *cfg,
                                  const biquad_coeff_t *coeff,
                                  biquad_state_t *states) {
  if (!s_eq.config.enabled) return x;
  x *= s_eq.preamp_gain;
  for (uint8_t i = 0; i < cfg->filter_count; ++i) {
    if (cfg->filters[i].enabled) {
      x = biquad_process(x, &coeff[i], &states[i]);
    }
  }
  return x;
}

static inline int16_t saturate_s16(float sample) {
  if (sample > 32767.0f) {
    s_eq.clip_count++;
    return 32767;
  }
  if (sample < -32768.0f) {
    s_eq.clip_count++;
    return -32768;
  }
  return (int16_t)lrintf(sample);
}

void audio_eq_process(int16_t *pcm, size_t frames, int channels,
                      int sample_rate) {
  if (!pcm || frames == 0 || channels != 2 || !prepare_for_rate(sample_rate)) {
    return;
  }

  const audio_eq_channel_mode_t mode =
      (audio_eq_channel_mode_t)s_eq.config.channel_mode;

  if (!s_eq.config.enabled && mode == AUDIO_EQ_CHANNEL_STEREO) return;

  for (size_t i = 0; i < frames; ++i) {
    const float in_l = (float)pcm[i * 2];
    const float in_r = (float)pcm[i * 2 + 1];
    float source_l;
    float source_r;

    switch (mode) {
      case AUDIO_EQ_CHANNEL_MONO: {
        const float mono = 0.5f * (in_l + in_r);
        source_l = mono;
        source_r = mono;
        break;
      }
      case AUDIO_EQ_CHANNEL_LEFT:
        source_l = in_l;
        source_r = in_l;
        break;
      case AUDIO_EQ_CHANNEL_RIGHT:
        source_l = in_r;
        source_r = in_r;
        break;
      case AUDIO_EQ_CHANNEL_STEREO:
      default:
        source_l = in_l;
        source_r = in_r;
        break;
    }

    const float out_l =
        process_chain(source_l, &s_eq.config.left, s_eq.coeff_l, s_eq.state_l);
    const float out_r = process_chain(source_r, &s_eq.config.right,
                                      s_eq.coeff_r, s_eq.state_r);
    pcm[i * 2] = saturate_s16(out_l);
    pcm[i * 2 + 1] = saturate_s16(out_r);
  }
}

const char *audio_eq_filter_type_name(audio_eq_filter_type_t type) {
  switch (type) {
    case AUDIO_EQ_FILTER_PK: return "PK";
    case AUDIO_EQ_FILTER_LP: return "LP";
    case AUDIO_EQ_FILTER_HP: return "HP";
    case AUDIO_EQ_FILTER_BP: return "BP";
    case AUDIO_EQ_FILTER_NOTCH: return "NOTCH";
    case AUDIO_EQ_FILTER_LS: return "LS";
    case AUDIO_EQ_FILTER_HS: return "HS";
    default: return "PK";
  }
}

bool audio_eq_filter_type_from_name(const char *name,
                                    audio_eq_filter_type_t *out) {
  if (!name || !out) return false;
  for (int i = 0; i < AUDIO_EQ_FILTER_COUNT; ++i) {
    if (strcasecmp(name, audio_eq_filter_type_name((audio_eq_filter_type_t)i)) == 0) {
      *out = (audio_eq_filter_type_t)i;
      return true;
    }
  }
  return false;
}

const char *audio_eq_channel_mode_name(audio_eq_channel_mode_t mode) {
  switch (mode) {
    case AUDIO_EQ_CHANNEL_STEREO: return "stereo";
    case AUDIO_EQ_CHANNEL_MONO: return "mono";
    case AUDIO_EQ_CHANNEL_LEFT: return "left";
    case AUDIO_EQ_CHANNEL_RIGHT: return "right";
    default: return "stereo";
  }
}

bool audio_eq_channel_mode_from_name(const char *name,
                                     audio_eq_channel_mode_t *out) {
  if (!name || !out) return false;
  for (int i = 0; i < AUDIO_EQ_CHANNEL_COUNT; ++i) {
    if (strcasecmp(name, audio_eq_channel_mode_name((audio_eq_channel_mode_t)i)) == 0) {
      *out = (audio_eq_channel_mode_t)i;
      return true;
    }
  }
  return false;
}
