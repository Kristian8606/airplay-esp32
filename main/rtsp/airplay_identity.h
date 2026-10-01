#pragma once

#include <stdint.h>

/* Per-device AirPlay identity values that a HomePod publishes in its
 * _airplay TXT record and /info, derived once from this device's MAC so
 * they stay the same across reboots:
 *   pi     - lowercase UUID (HomePod: "39d6b8a5-08cd-...")
 *   psi    - uppercase UUID whose first 12 hex digits are the device ID
 *            (HomePod deviceid FE:CB:1C:F9:C5:43 -> psi FECB1CF9-C543-...)
 *   gid    - this device's own group UUID (a standalone Apple TV publishes
 *            its own UUID with gcgl=1, igl=1)
 *   btaddr - Bluetooth address
 * Call airplay_identity_init() once at boot, before mDNS starts. */
void airplay_identity_init(void);

const char *airplay_identity_pi(void);
const char *airplay_identity_psi(void);
const char *airplay_identity_gid(void);
const char *airplay_identity_btaddr(void);

/* CONFIG_AIRPLAY_STATUS_FLAGS, as a number and as "0x..." for TXT. */
uint32_t airplay_identity_flags(void);
const char *airplay_identity_flags_str(void);
