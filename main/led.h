#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* RGB/VU diagnostic LED. When CONFIG_ENABLE_RGB_AUDIO_LED=n all public
 * functions compile to tiny no-op stubs in led.c and no RMT/WS2812 work is
 * performed from the audio path. */
void led_init(void);
void led_audio_feed(const int16_t *pcm, size_t stereo_frames);
void led_set_error(bool error);
esp_err_t led_set_brightness(uint8_t brightness);
uint8_t led_get_brightness(void);
