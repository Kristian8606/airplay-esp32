#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_EQ_CONFIG_VERSION 2U
#define AUDIO_EQ_MAX_FILTERS_PER_CHANNEL 24U

typedef enum {
  AUDIO_EQ_FILTER_PK = 0,
  AUDIO_EQ_FILTER_LP,
  AUDIO_EQ_FILTER_HP,
  AUDIO_EQ_FILTER_BP,
  AUDIO_EQ_FILTER_NOTCH,
  AUDIO_EQ_FILTER_LS,
  AUDIO_EQ_FILTER_HS,
  AUDIO_EQ_FILTER_COUNT
} audio_eq_filter_type_t;

typedef enum {
  AUDIO_EQ_CHANNEL_STEREO = 0,
  AUDIO_EQ_CHANNEL_MONO,
  AUDIO_EQ_CHANNEL_LEFT,
  AUDIO_EQ_CHANNEL_RIGHT,
  AUDIO_EQ_CHANNEL_COUNT
} audio_eq_channel_mode_t;

typedef struct {
  uint8_t enabled;
  uint8_t type;
  uint16_t reserved;
  float frequency_hz;
  float gain_db;
  float q;
} audio_eq_filter_config_t;

typedef struct {
  uint8_t filter_count;
  uint8_t reserved[3];
  audio_eq_filter_config_t filters[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
} audio_eq_output_config_t;

typedef struct {
  uint32_t version;
  uint8_t enabled;
  uint8_t channel_mode;
  uint16_t reserved;
  float preamp_db;
  audio_eq_output_config_t left;
  audio_eq_output_config_t right;
} audio_eq_config_t;

/* Load the saved configuration and initialise the runtime DSP state. */
esp_err_t audio_eq_init(void);

/* Read/write persistent configuration. Save does not change active runtime
 * state; the web UI restarts the ESP after a successful save. */
esp_err_t audio_eq_load_config(audio_eq_config_t *out);
esp_err_t audio_eq_save_config(const audio_eq_config_t *config);
void audio_eq_default_config(audio_eq_config_t *out);
bool audio_eq_validate_config(const audio_eq_config_t *config);

/* Process interleaved signed 16-bit stereo PCM in place.
 * Source selection:
 *   stereo: L source -> left EQ, R source -> right EQ
 *   mono:   (L+R)/2 -> both independent EQ chains
 *   left:   L source -> both independent EQ chains
 *   right:  R source -> both independent EQ chains
 * Coefficients are calculated once per sample-rate change, never per sample. */
void audio_eq_process(int16_t *pcm, size_t frames, int channels,
                      int sample_rate);

/* Clear biquad delay state at AirPlay timeline generation boundaries. */
void audio_eq_reset_state(void);

const char *audio_eq_filter_type_name(audio_eq_filter_type_t type);
bool audio_eq_filter_type_from_name(const char *name,
                                    audio_eq_filter_type_t *out);
const char *audio_eq_channel_mode_name(audio_eq_channel_mode_t mode);
bool audio_eq_channel_mode_from_name(const char *name,
                                     audio_eq_channel_mode_t *out);

#ifdef __cplusplus
}
#endif
