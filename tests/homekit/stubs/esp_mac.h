#pragma once
#include <stdint.h>
#include <string.h>
#define ESP_MAC_WIFI_STA 0
static inline int esp_read_mac(uint8_t *p,int t){(void)t;const uint8_t m[]={1,2,3,4,5,6};memcpy(p,m,6);return 0;}
