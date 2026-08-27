#include "led.h"
#include "sdkconfig.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "rtsp_events.h"

static const char *TAG = "rgb_vu";

#if CONFIG_ENABLE_RGB_AUDIO_LED
#include "led_strip.h"

#ifndef CONFIG_RGB_AUDIO_LED_GPIO
#define CONFIG_RGB_AUDIO_LED_GPIO 38
#endif
#ifndef CONFIG_RGB_AUDIO_LED_BRIGHTNESS
#define CONFIG_RGB_AUDIO_LED_BRIGHTNESS 96
#endif
#ifndef CONFIG_RGB_AUDIO_LED_UPDATE_HZ
#define CONFIG_RGB_AUDIO_LED_UPDATE_HZ 30
#endif

#define SILENCE_THRESH 200.0f
#define VU_FULL_SCALE 16000.0f
#define UPDATE_INTERVAL_US (1000000LL / CONFIG_RGB_AUDIO_LED_UPDATE_HZ)

typedef enum {
  LED_STATE_STANDBY,
  LED_STATE_PAUSED,
  LED_STATE_PLAYING,
  LED_STATE_ERROR,
} led_state_t;

static led_strip_handle_t s_strip;
static led_state_t s_state = LED_STATE_STANDBY;
static led_state_t s_prev_state = LED_STATE_STANDBY;
static uint8_t s_brightness = CONFIG_RGB_AUDIO_LED_BRIGHTNESS;
static int64_t s_last_update_us;

static uint8_t scale_bright(uint8_t v) {
  return (uint8_t)(((uint16_t)v * s_brightness) / 255U);
}

static void rgb_refresh_color(uint8_t r, uint8_t g, uint8_t b) {
  if (!s_strip) return;
  led_strip_set_pixel(s_strip, 0, r, g, b);
  led_strip_refresh(s_strip);
}

static void rgb_clear(void) {
  if (!s_strip) return;
  led_strip_clear(s_strip);
  led_strip_refresh(s_strip);
}

static void render_state(void) {
  switch (s_state) {
    case LED_STATE_PLAYING:
#if defined(CONFIG_RGB_AUDIO_LED_PLAYING_OFF)
      rgb_clear();
#elif defined(CONFIG_RGB_AUDIO_LED_PLAYING_STEADY)
      rgb_refresh_color(0, scale_bright(0x60), scale_bright(0x10));
#else
      /* VU mode is rendered by led_audio_feed(). */
#endif
      break;

    case LED_STATE_PAUSED:
#if defined(CONFIG_RGB_AUDIO_LED_PAUSED_OFF)
      rgb_clear();
#else
      /* Same visual convention as upstream: paused = dim blue. */
      rgb_refresh_color(0, 0, scale_bright(0x60));
#endif
      break;

    case LED_STATE_STANDBY:
#if defined(CONFIG_RGB_AUDIO_LED_STANDBY_STEADY)
      rgb_refresh_color(0, scale_bright(0x28), 0);
#else
      rgb_clear();
#endif
      break;

    case LED_STATE_ERROR:
      rgb_refresh_color(scale_bright(0xC0), 0, 0);
      break;
  }
}

static void apply_state(led_state_t state) {
  if (state == s_state) return;
  s_prev_state = s_state;
  s_state = state;
  s_last_update_us = 0;
  ESP_LOGI(TAG, "state %d -> %d", (int)s_prev_state, (int)s_state);
  render_state();
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;
  switch (event) {
    case RTSP_EVENT_CLIENT_CONNECTED:
      apply_state(LED_STATE_PAUSED);
      break;
    case RTSP_EVENT_PLAYING:
      apply_state(LED_STATE_PLAYING);
      break;
    case RTSP_EVENT_PAUSED:
      apply_state(LED_STATE_PAUSED);
      break;
    case RTSP_EVENT_DISCONNECTED:
      apply_state(LED_STATE_STANDBY);
      break;
    case RTSP_EVENT_METADATA:
      break;
  }
}

void led_audio_feed(const int16_t *pcm, size_t stereo_frames) {
#if defined(CONFIG_RGB_AUDIO_LED_PLAYING_VU)
  if (!pcm || stereo_frames == 0 || s_state != LED_STATE_PLAYING || !s_strip) {
    return;
  }

  /* Keep the original project's cheap ~30 Hz VU strategy. The call is made
   * from the final playout/I2S path, but almost every 256-frame block returns
   * here immediately because of this rate limit. */
  const int64_t now = esp_timer_get_time();
  if (now - s_last_update_us < UPDATE_INTERVAL_US) return;
  s_last_update_us = now;

  const size_t total_samples = stereo_frames * 2U;
  uint64_t sum_sq = 0;
  for (size_t i = 0; i < total_samples; ++i) {
    const int32_t s = pcm[i];
    sum_sq += (uint64_t)((int64_t)s * (int64_t)s);
  }
  const float rms = sqrtf((float)sum_sq / (float)total_samples);

  /* Approximate bass content using the same low-cost successive-sample
   * difference idea as upstream (left channel is enough for the color cue). */
  uint64_t diff_sum = 0;
  size_t diff_count = 0;
  for (size_t i = 2; i < total_samples; i += 2) {
    const int32_t d = (int32_t)pcm[i] - (int32_t)pcm[i - 2];
    diff_sum += (uint64_t)(d < 0 ? -(int64_t)d : (int64_t)d);
    ++diff_count;
  }
  const float high_energy = diff_count ? (float)diff_sum / (float)diff_count : 0.0f;

  float bass_ratio = 0.0f;
  if (rms > SILENCE_THRESH) {
    bass_ratio = 1.0f - high_energy / (rms * 2.0f + 1.0f);
    if (bass_ratio < 0.0f) bass_ratio = 0.0f;
    if (bass_ratio > 1.0f) bass_ratio = 1.0f;
  }

  float norm = 0.0f;
  if (rms >= SILENCE_THRESH) {
    norm = (rms - SILENCE_THRESH) / (VU_FULL_SCALE - SILENCE_THRESH);
    if (norm > 1.0f) norm = 1.0f;
  }

  if (norm <= 0.0f || s_brightness == 0) {
    rgb_clear();
    return;
  }

  uint8_t val = (uint8_t)(norm * (float)s_brightness);
  if (val == 0) val = 1;

  /* Upstream-style hue: blue when quiet -> green -> red when loud, with a
   * purple/magenta shift for bass-heavy material. */
  uint16_t hue = (uint16_t)(170.0f * (1.0f - norm));
  if (bass_ratio > 0.30f) {
    hue += (uint16_t)(bass_ratio * 60.0f);
    if (hue > 255U) hue = 255U;
  }
  uint8_t sat = 255U;
  if (norm > 0.85f) {
    sat = (uint8_t)(255.0f - ((norm - 0.85f) / 0.15f) * 80.0f);
  }

  led_strip_set_pixel_hsv(s_strip, 0, hue, sat, val);
  led_strip_refresh(s_strip);
#else
  (void)pcm;
  (void)stereo_frames;
#endif
}

void led_init(void) {
  led_strip_config_t strip_cfg = {
      .strip_gpio_num = CONFIG_RGB_AUDIO_LED_GPIO,
      .max_leds = 1,
      .led_model = LED_MODEL_WS2812,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,
      .flags.with_dma = false,
  };

  esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WS2812 init failed GPIO=%d: %s",
             CONFIG_RGB_AUDIO_LED_GPIO, esp_err_to_name(err));
    s_strip = NULL;
    return;
  }

  rgb_clear();
  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    ESP_LOGW(TAG, "RTSP LED event listener registration failed");
  }

  ESP_LOGI(TAG, "WS2812 audio LED ready GPIO=%d brightness=%u update=%dHz",
           CONFIG_RGB_AUDIO_LED_GPIO, (unsigned)s_brightness,
           CONFIG_RGB_AUDIO_LED_UPDATE_HZ);
  render_state();
}

void led_set_error(bool error) {
  if (error) {
    if (s_state != LED_STATE_ERROR) {
      s_prev_state = s_state;
      s_state = LED_STATE_ERROR;
    }
    render_state();
  } else if (s_state == LED_STATE_ERROR) {
    s_state = s_prev_state;
    render_state();
  }
}

esp_err_t led_set_brightness(uint8_t brightness) {
  s_brightness = brightness;
  render_state();
  return ESP_OK;
}

uint8_t led_get_brightness(void) {
  return s_brightness;
}

#else  /* CONFIG_ENABLE_RGB_AUDIO_LED */

void led_init(void) {}
void led_audio_feed(const int16_t *pcm, size_t stereo_frames) {
  (void)pcm;
  (void)stereo_frames;
}
void led_set_error(bool error) { (void)error; }
esp_err_t led_set_brightness(uint8_t brightness) {
  (void)brightness;
  return ESP_ERR_NOT_SUPPORTED;
}
uint8_t led_get_brightness(void) { return 0; }

#endif
