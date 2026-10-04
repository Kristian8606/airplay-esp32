#pragma once

#include "sdkconfig.h"
#include "esp_err.h"

/* Startup-only; configured active-low GPIO buttons connected to GND.
 * Disabled builds contain no worker or button state. */
#ifdef CONFIG_AIRPLAY_HARDWARE_BUTTONS
esp_err_t playback_buttons_init(void);
#else
static inline esp_err_t playback_buttons_init(void) { return ESP_OK; }
#endif
