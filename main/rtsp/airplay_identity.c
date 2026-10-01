#include "airplay_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "sdkconfig.h"
#include "sodium.h"

static bool s_ready;
static char s_pi[37];
static char s_psi[37];
static char s_gid[37];
static char s_btaddr[18];
static char s_flags[12];

/* 16 bytes -> RFC 4122 version-4 style text, as Apple formats these IDs. */
static void format_uuid(const uint8_t b[16], bool upper, char out[37]) {
  const char *fmt = upper ? "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
                            "%02X%02X%02X%02X%02X%02X"
                          : "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                            "%02x%02x%02x%02x%02x%02x";
  snprintf(out, 37, fmt, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8],
           b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void derive(const char *label, const uint8_t mac[6], uint8_t out[16]) {
  uint8_t h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_state st;
  crypto_hash_sha256_init(&st);
  crypto_hash_sha256_update(&st, (const uint8_t *)label, strlen(label));
  crypto_hash_sha256_update(&st, mac, 6);
  crypto_hash_sha256_final(&st, h);
  memcpy(out, h, 16);
  out[6] = (uint8_t)((out[6] & 0x0F) | 0x40); /* version 4 */
  out[8] = (uint8_t)((out[8] & 0x3F) | 0x80); /* RFC 4122 variant */
}

void airplay_identity_init(void) {
  if (s_ready) return;
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  uint8_t b[16];
  derive("airplay-pi", mac, b);
  format_uuid(b, false, s_pi);
  derive("airplay-psi", mac, b);
  memcpy(b, mac, 6); /* first 12 hex digits = device ID, as on HomePods */
  format_uuid(b, true, s_psi);
  derive("airplay-gid", mac, b);
  format_uuid(b, true, s_gid);

  uint8_t bt[6] = {0};
  esp_read_mac(bt, ESP_MAC_BT);
  snprintf(s_btaddr, sizeof(s_btaddr), "%02X:%02X:%02X:%02X:%02X:%02X", bt[0],
           bt[1], bt[2], bt[3], bt[4], bt[5]);
  snprintf(s_flags, sizeof(s_flags), "0x%x",
           (unsigned)CONFIG_AIRPLAY_STATUS_FLAGS);
  s_ready = true;
}

const char *airplay_identity_pi(void) { airplay_identity_init(); return s_pi; }
const char *airplay_identity_psi(void) { airplay_identity_init(); return s_psi; }
const char *airplay_identity_gid(void) { airplay_identity_init(); return s_gid; }
const char *airplay_identity_btaddr(void) {
  airplay_identity_init();
  return s_btaddr;
}
uint32_t airplay_identity_flags(void) { return (uint32_t)CONFIG_AIRPLAY_STATUS_FLAGS; }
const char *airplay_identity_flags_str(void) {
  airplay_identity_init();
  return s_flags;
}
