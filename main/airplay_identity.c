#include "airplay_identity.h"

#include <stdio.h>

#include "esp_mac.h"
#include "sodium.h"

void airplay_get_pairing_id(char *out, size_t len) {
  if (!out || len == 0) {
    return;
  }
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  /* Same MAC -> same UUID on every boot; different devices -> different
   * UUIDs. Hash so the UUID does not expose the MAC directly. */
  static const char kDomain[] = "airplay-esp32 pairing id";
  uint8_t hash[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state st;
  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, (const uint8_t *)kDomain, sizeof(kDomain) - 1);
  crypto_hash_sha512_update(&st, mac, sizeof(mac));
  crypto_hash_sha512_final(&st, hash);

  uint8_t *u = hash; /* first 16 bytes form the UUID */
  u[6] = (uint8_t)((u[6] & 0x0F) | 0x40); /* version 4 layout */
  u[8] = (uint8_t)((u[8] & 0x3F) | 0x80); /* RFC 4122 variant */

  snprintf(out, len,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
           u[11], u[12], u[13], u[14], u[15]);
}
