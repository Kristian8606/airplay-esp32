#include "airplay_identity.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base64.h"
#include "esp_mac.h"
#include "hap.h"
#include "sodium.h"
#include "wifi.h"

#if CONFIG_AIRPLAY_HP_INFO
#include "bplist_writer.h"
#endif

/* 16 stable pseudo-random bytes for this device and purpose. */
static void mac_hash16(const char *domain, uint8_t out[16]) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  uint8_t hash[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state st;
  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, (const uint8_t *)domain, strlen(domain));
  crypto_hash_sha512_update(&st, mac, sizeof(mac));
  crypto_hash_sha512_final(&st, hash);
  memcpy(out, hash, 16);
}

void airplay_get_pairing_id(char *out, size_t len) {
  if (!out || len == 0) {
    return;
  }
  /* Same MAC -> same UUID on every boot; different devices -> different
   * UUIDs. Hash so the UUID does not expose the MAC directly. */
  uint8_t u[16];
  mac_hash16("airplay-esp32 pairing id", u);
  u[6] = (uint8_t)((u[6] & 0x0F) | 0x40); /* version 4 layout */
  u[8] = (uint8_t)((u[8] & 0x3F) | 0x80); /* RFC 4122 variant */

  snprintf(out, len,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
           u[11], u[12], u[13], u[14], u[15]);
}

void airplay_get_psi(char *out, size_t len) {
  if (!out || len == 0) {
    return;
  }
  /* A HomePod's psi starts with its device id (first byte | 1) and continues
   * as a version-4 UUID; build ours the same way. */
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  uint8_t u[16];
  mac_hash16("airplay-esp32 psi", u);
  memcpy(u, mac, 6);
  u[0] |= 0x01;
  u[6] = (uint8_t)((u[6] & 0x0F) | 0x40);
  u[8] = (uint8_t)((u[8] & 0x3F) | 0x80);
  snprintf(out, len,
           "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
           "%02X%02X%02X%02X%02X%02X",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
           u[11], u[12], u[13], u[14], u[15]);
}

void airplay_get_fex(char *out, size_t len) {
  if (!out || len == 0) {
    return;
  }
  uint8_t bytes[16];
  size_t n = 0;
  const uint64_t f = AIRPLAY_FEATURES_64;
  for (int i = 0; i < 8; i++) bytes[n++] = (uint8_t)(f >> (8 * i));
#if CONFIG_AIRPLAY_HP_FEATURES
  /* HomePod feature bits 64-103, copied as advertised. */
  static const uint8_t hp_ext[] = {0x10, 0xa1, 0xae, 0xd8, 0x0d};
  memcpy(bytes + n, hp_ext, sizeof(hp_ext));
  n += sizeof(hp_ext);
#endif
  char b64[32];
  const int b64_len = base64_encode(bytes, n, b64, sizeof(b64));
  size_t used = b64_len > 0 ? (size_t)b64_len : 0;
  while (used > 0 && b64[used - 1] == '=') used--; /* no padding, as Apple */
  if (used >= len) used = len - 1;
  memcpy(out, b64, used);
  out[used] = '\0';
}

static void add(airplay_txt_t *t, const char *key, const char *value) {
  if (t->count < sizeof(t->items) / sizeof(t->items[0])) {
    t->items[t->count].key = key;
    t->items[t->count].value = value;
    t->count++;
  }
}

void airplay_txt_build(airplay_txt_t *t) {
  memset(t, 0, sizeof(*t));
  wifi_get_mac_str(t->deviceid, sizeof(t->deviceid));
  snprintf(t->features, sizeof(t->features), "0x%" PRIX32 ",0x%" PRIX32,
           (uint32_t)AIRPLAY_FEATURES_LO, (uint32_t)AIRPLAY_FEATURES_HI);
  snprintf(t->flags, sizeof(t->flags), "0x%" PRIX32, (uint32_t)AIRPLAY_STATUS_FLAGS);
  snprintf(t->vv, sizeof(t->vv), "%d", AIRPLAY_PROTOCOL_VERSION);
  const uint8_t *pk = hap_get_public_key();
  for (int i = 0; i < 32; i++) {
    snprintf(t->pk + (size_t)i * 2, 3, "%02x", pk[i]);
  }
  airplay_get_pairing_id(t->pi, sizeof(t->pi));

#if CONFIG_AIRPLAY_HP_TXT_EXTRAS
  uint8_t bt[6] = {0};
  esp_read_mac(bt, ESP_MAC_BT);
  snprintf(t->btaddr, sizeof(t->btaddr), "%02X:%02X:%02X:%02X:%02X:%02X",
           bt[0], bt[1], bt[2], bt[3], bt[4], bt[5]);
  airplay_get_psi(t->psi, sizeof(t->psi));
  airplay_get_fex(t->fex, sizeof(t->fex));

  /* Same keys and order as a HomePod. Not grouped: gid is our own pi and we
   * are neither group leader nor in a group with one (as Shairport Sync). */
  add(t, "acl", "0");
  add(t, "btaddr", t->btaddr);
  add(t, "deviceid", t->deviceid);
  add(t, "c", "2");
  add(t, "fex", t->fex);
  add(t, "features", t->features);
  add(t, "flags", t->flags);
  add(t, "gid", t->pi);
  add(t, "igl", "0");
  add(t, "gcgl", "0");
  add(t, "model", AIRPLAY_MODEL);
  add(t, "protovers", AIRPLAY_PROTOVERS);
  add(t, "pi", t->pi);
  add(t, "psi", t->psi);
  add(t, "pk", t->pk);
  add(t, "srcvers", AIRPLAY_SOURCE_VERSION);
  add(t, "osvers", AIRPLAY_OS_VERSION);
  add(t, "vv", t->vv);
#else
  add(t, "deviceid", t->deviceid);
  add(t, "features", t->features);
  add(t, "flags", t->flags);
  add(t, "model", AIRPLAY_MODEL);
  add(t, "pk", t->pk);
  add(t, "pi", t->pi);
  add(t, "srcvers", AIRPLAY_SOURCE_VERSION);
  add(t, "vv", t->vv);
  add(t, "acl", "0");
#endif
}

size_t airplay_txt_encode(const airplay_txt_t *t, uint8_t *out, size_t cap) {
  size_t pos = 0;
  for (size_t i = 0; i < t->count; i++) {
    const size_t kl = strlen(t->items[i].key);
    const size_t vl = strlen(t->items[i].value);
    const size_t n = kl + 1 + vl;
    if (n > 255 || pos + 1 + n > cap) return 0;
    out[pos++] = (uint8_t)n;
    memcpy(out + pos, t->items[i].key, kl);
    pos += kl;
    out[pos++] = '=';
    memcpy(out + pos, t->items[i].value, vl);
    pos += vl;
  }
  return pos;
}

#if CONFIG_AIRPLAY_HP_INFO
/* Formats this receiver can really decode (bit numbers as in the
 * supportedFormats masks): 18 = ALAC 44100/16/2 (realtime),
 * 22 = AAC-LC 44100/2 (buffered). A HomePod lists many more, but announcing
 * a format we cannot decode would make the sender pick it. */
#define FMT_ALAC_44100_16_2 18
#define FMT_AAC_LC_44100_2  22

size_t airplay_build_info_homepod(uint8_t *out, size_t cap,
                                  const char *device_name,
                                  float initial_volume_db,
                                  const char *sender_address) {
  airplay_txt_t *txt = malloc(sizeof(*txt));
  uint8_t *txt_bytes = malloc(512);
  bpw_t *w = malloc(sizeof(*w));
  size_t len = 0;
  if (!txt || !txt_bytes || !w) goto done;

  airplay_txt_build(txt);
  const size_t txt_len = airplay_txt_encode(txt, txt_bytes, 512);
  char fex[32];
  airplay_get_fex(fex, sizeof(fex));
  char psi[AIRPLAY_PAIRING_ID_LEN];
  airplay_get_psi(psi, sizeof(psi));

  bpw_init(w);
  bpw_dict_begin(w);
  bpw_kv_bool(w, "canRecordScreenStream", false);
  bpw_kv_string(w, "deviceEnclosureColor", "2");
  bpw_kv_string(w, "deviceID", txt->deviceid);
  bpw_kv_uint(w, "features", AIRPLAY_FEATURES_64);
  bpw_kv_string(w, "featuresEx", fex);
  bpw_kv_bool(w, "forwardFrameUserData", false);
  bpw_kv_bool(w, "hasUDPMirroringSupport", true);
  bpw_kv_real(w, "initialVolume", (double)initial_volume_db);
  bpw_kv_bool(w, "keepAliveSendStatsAsBody", true);
  bpw_kv_string(w, "macAddress", txt->deviceid);
  bpw_kv_string(w, "model", AIRPLAY_MODEL);
  bpw_kv_string(w, "name", device_name ? device_name : "");
  bpw_kv_string(w, "osBuildVersion", AIRPLAY_OS_BUILD);
  bpw_kv_string(w, "pi", txt->pi);
  bpw_kv_data(w, "pk", hap_get_public_key(), 32);

  bpw_key(w, "playbackCapabilities");
  bpw_dict_begin(w);
  bpw_kv_bool(w, "supportsAirPlayVideoWithSharePlay", true);
  bpw_kv_bool(w, "supportsAVMetrics", true);
  bpw_kv_bool(w, "supportsCoordinatedMultiViewPlayback", true);
  bpw_kv_bool(w, "supportsFPSSecureStop", false);
  bpw_kv_bool(w, "supportsIntegratedTimeline", false);
  bpw_kv_bool(w, "supportsInterstitials", false);
  bpw_kv_bool(w, "supportsOfflineHLS", false);
  bpw_kv_bool(w, "supportsStopAtEndOfQueue", false);
  bpw_kv_bool(w, "supportsUIForAudioOnlyContent", false);
  bpw_kv_bool(w, "supportsV2ArtworkMetadata", false);
  bpw_end(w);

  bpw_kv_string(w, "protocolVersion", AIRPLAY_PROTOVERS);
  bpw_kv_string(w, "psi", psi);
  bpw_kv_string(w, "receiverHDRCapability", "4k60");
  bpw_kv_bool(w, "screenDemoMode", false);
  if (sender_address && sender_address[0]) {
    bpw_kv_string(w, "senderAddress", sender_address);
  }
  bpw_kv_string(w, "sourceVersion", AIRPLAY_SOURCE_VERSION);
  bpw_kv_uint(w, "statusFlags", AIRPLAY_STATUS_FLAGS);

  bpw_key(w, "supportedAudioFormatsExtended");
  bpw_dict_begin(w);
  bpw_key(w, "bufferStream");
  bpw_array_begin(w);
  bpw_int(w, FMT_AAC_LC_44100_2);
  bpw_end(w);
  bpw_end(w);

  bpw_key(w, "supportedFormats");
  bpw_dict_begin(w);
  bpw_kv_uint(w, "audioStream", 1ULL << FMT_ALAC_44100_16_2);
  bpw_kv_uint(w, "bufferStream", 1ULL << FMT_AAC_LC_44100_2);
  bpw_end(w);

  if (txt_len > 0) bpw_kv_data(w, "txtAirPlay", txt_bytes, txt_len);
  bpw_kv_int(w, "volumeControlType", 3);
  bpw_kv_int(w, "vv", AIRPLAY_PROTOCOL_VERSION);
  bpw_end(w);

  len = bpw_finish(w, out, cap);

done:
  free(w);
  free(txt_bytes);
  free(txt);
  return len;
}
#endif /* CONFIG_AIRPLAY_HP_INFO */
