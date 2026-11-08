/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/* Reset cannot race the startup tasks that create identity and services. */
void device_reset_mark_ready(void);
esp_err_t device_reset_register(httpd_handle_t server);
#ifdef CONFIG_AIRPLAY_HOMEKIT
/* Called after acknowledging an authenticated removal from Apple Home. */
void device_reset_schedule_homekit(void);
#endif
