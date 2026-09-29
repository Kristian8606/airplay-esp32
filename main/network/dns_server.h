#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Captive portal DNS server
 * Answers every A/IN query with the specified IP address; other query types
 * get an empty NOERROR reply.
 */

/**
 * Start the DNS server for captive portal
 * @param redirect_ip IPv4 address returned for A queries (network byte order)
 * @return ESP_OK on success
 */
esp_err_t dns_server_start(uint32_t redirect_ip);

/**
 * Stop the DNS server
 */
void dns_server_stop(void);
