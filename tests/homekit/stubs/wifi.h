#pragma once
#include <stddef.h>
#define WIFI_HOSTNAME_MAX_LEN 63
void wifi_sanitize_hostname(const char*,char*,size_t);
void wifi_get_mac_str(char*,size_t);
