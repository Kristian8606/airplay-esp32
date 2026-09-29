#pragma once

#include <stddef.h>
#include <stdint.h>

/* WS2812 status/VU LED. When CONFIG_ENABLE_RGB_AUDIO_LED=n all public
 * functions compile to tiny no-op stubs in led.c and no RMT/WS2812 work is
 * performed from the audio path. */
void led_init(void);
void led_audio_feed(const int16_t *pcm, size_t stereo_frames);
