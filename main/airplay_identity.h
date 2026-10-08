#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

/* How this receiver presents itself to AirPlay 2 senders. Used by mDNS
 * (_airplay/_raop TXT records), GET /info and the RTSP "Server" header, so
 * all of them always agree.
 *
 * Lab build: the "HomePod profile (lab)" menuconfig switches copy what a real
 * HomePod (HomePod OS 27.2) advertises, one group of values per switch, so a
 * part that breaks something can be turned off without code changes. */

/* AudioAccessory6,1 = HomePod (2nd generation). */
#define AIRPLAY_MODEL          "AudioAccessory6,1"

#if CONFIG_AIRPLAY_HP_VERSION
#define AIRPLAY_SOURCE_VERSION "1005.12.1"
#else
#define AIRPLAY_SOURCE_VERSION "377.40.00"
#endif
#define AIRPLAY_OS_VERSION     "27.2"
#define AIRPLAY_OS_BUILD       "24K5103f"
#define AIRPLAY_SERVER_HEADER  "AirTunes/" AIRPLAY_SOURCE_VERSION
#define AIRPLAY_PROTOVERS      "1.1"

/* "vv": a HomePod reports 1, Shairport-style receivers 2. */
#if CONFIG_AIRPLAY_HP_VV1
#define AIRPLAY_PROTOCOL_VERSION 1
#else
#define AIRPLAY_PROTOCOL_VERSION 2
#endif

/* statusFlags / "flags" / "sf". 0x4 = audio receiver; the HomePod value adds
 * bits 10, 15, 16 and 19 (as seen on an idle HomePod). */
#if CONFIG_AIRPLAY_HP_STATUS_FLAGS
#define AIRPLAY_STATUS_FLAGS   0x98404
#else
#define AIRPLAY_STATUS_FLAGS   0x4
#endif

/* Feature bits advertised in mDNS "features"/"ft" and GET /info. */
#if CONFIG_AIRPLAY_HP_FEATURES
#define AIRPLAY_FEATURES_LO 0x4A7FCA00
#if CONFIG_AIRPLAY_HP_FEATURES_STREAM && CONFIG_AIRPLAY_HP_BIT58_HANGDOG
#define AIRPLAY_FEATURES_HI 0x3C354BD0 /* HomePod value, bits 58-61 set */
#elif CONFIG_AIRPLAY_HP_FEATURES_STREAM
#define AIRPLAY_FEATURES_HI 0x38354BD0 /* HomePod value without bit 58 */
#else
#define AIRPLAY_FEATURES_HI 0x00354BD0 /* HomePod value without bits 58-61 */
#endif
#else
/* Bit 38: SupportsCoreUtilsPairingAndEncryption
 * Bit 46: SupportsHKPairingAndAccessControl
 * Bit 48: SupportsTransientPairing */
#define AIRPLAY_FEATURES_HI 0x1C340
#define AIRPLAY_FEATURES_LO 0x405C4A00
#endif
#define AIRPLAY_FEATURES_64 \
  (((uint64_t)AIRPLAY_FEATURES_HI << 32) | (uint64_t)AIRPLAY_FEATURES_LO)

/* Pairing identity UUID ("pi" in mDNS and GET /info), lowercase
 * 8-4-4-4-12 form. Derived from the Wi-Fi MAC, so it is stable across
 * reboots and unique per device. */
#define AIRPLAY_PAIRING_ID_LEN 37 /* 36 chars + NUL */
void airplay_get_pairing_id(char *out, size_t len);

/* "psi": uppercase UUID, also derived from the MAC. */
void airplay_get_psi(char *out, size_t len);

/* "fex": the feature bits as little-endian bytes (plus the HomePod's bits
 * above 63 when the HomePod feature set is on), base64 without padding. */
void airplay_get_fex(char *out, size_t len);

/* _airplay._tcp TXT record: built in one place, used by mDNS and GET /info. */
typedef struct {
  const char *key;
  const char *value;
} airplay_txt_item_t;

typedef struct {
  char deviceid[18];
  char btaddr[18];
  char features[32];
  char flags[16];
  char fex[32];
  char pk[65];
  char pi[AIRPLAY_PAIRING_ID_LEN];
  char psi[AIRPLAY_PAIRING_ID_LEN];
  char vv[4];
  airplay_txt_item_t items[24];
  size_t count;
} airplay_txt_t;

void airplay_txt_build(airplay_txt_t *t);

/* Length-prefixed "key=value" strings (DNS TXT wire form). Returns the number
 * of bytes written, 0 if it does not fit. */
size_t airplay_txt_encode(const airplay_txt_t *t, uint8_t *out, size_t cap);

#if CONFIG_AIRPLAY_HP_INFO
/* GET /info in the form a HomePod answers it (binary plist). Returns the
 * number of bytes written, 0 on error. */
size_t airplay_build_info_homepod(uint8_t *out, size_t cap,
                                  const char *device_name,
                                  float initial_volume_db,
                                  const char *sender_address);
#endif
