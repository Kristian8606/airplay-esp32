#pragma once
typedef struct{const char *version;} esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void){static const esp_app_desc_t d={"v4.5.7"};return &d;}
