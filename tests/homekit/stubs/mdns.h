#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct {const char *key;const char *value;} mdns_txt_item_t;
esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char*);
esp_err_t mdns_service_add(const char*,const char*,const char*,uint16_t,const mdns_txt_item_t*,size_t);
esp_err_t mdns_service_txt_item_set(const char*,const char*,const char*,const char*);
