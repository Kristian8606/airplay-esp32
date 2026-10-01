#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Initialize mDNS and advertise AirPlay 2 services
 *
 * This publishes:
 * - _airplay._tcp service (AirPlay 2)
 * - _raop._tcp service (Remote Audio Output Protocol)
 *
 * With all required TXT records for iOS to recognize the device
 */
void mdns_airplay_init(void);

/**
 * Serialise the _airplay._tcp TXT record as DNS TXT data (each entry a length
 * byte followed by "key=value"). Used as "txtAirPlay" in updateInfo.
 * @return bytes written, 0 on error (NULL buffer or capacity too small)
 */
size_t mdns_airplay_txt_record_data(uint8_t *out, size_t capacity);

/** Return the current AirPlay extended feature set (fex) as HomePod-style
 * unpadded base64. Returns bytes written excluding NUL, or 0 on error. */
size_t mdns_airplay_features_ex_string(char *out, size_t capacity);
