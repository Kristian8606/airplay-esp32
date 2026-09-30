#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "sodium.h"
#include <stdio.h>
#include <string.h>

#include "hap.h"
#include "mdns_airplay.h"
#include "airplay_version.h"
#include "rtsp_handlers.h"
#include "wifi.h"
#include "settings.h"

static const char *TAG = "mdns_airplay";

// Feature flags are defined in rtsp_handlers.h (shared with /info handler)

// Protocol version
#define AIRPLAY_PROTOCOL_VERSION "2"

// Flags: 0x4 = audio receiver
#define AIRPLAY_FLAGS "0x4"

// Metadata types advertised in the "md" txt record:
//   0 = text (track title/artist/album), 1 = artwork (cover art images),
//   2 = progress.
// When artwork is disabled, drop "1" so senders do not transmit cover art
// (which can stall the audio pipeline and cause drop-outs on realtime
// streams) while still sending text and progress metadata.
#ifdef CONFIG_ENABLE_AIRPLAY_ARTWORK
#define AIRPLAY_METADATA_TYPES "0,1,2"
#else
#define AIRPLAY_METADATA_TYPES "0,2"
#endif

/* Values of the _airplay._tcp TXT record. One source for the mDNS
 * advertisement and for "txtAirPlay" in the event-channel updateInfo. */
typedef struct {
  char device_id[18];
  char features[32];
  char fex[32]; // base64 of the 128-bit feature set, see airplay_txt_values()
  char pk[65];  // 32 bytes = 64 hex chars + null
} airplay_txt_values_t;

static void airplay_txt_values(airplay_txt_values_t *v) {
  wifi_get_mac_str(v->device_id, sizeof(v->device_id));
  const uint8_t *pk = hap_get_public_key();
  for (int i = 0; i < 32; i++) {
    snprintf(v->pk + (size_t)i * 2, 3, "%02x", pk[i]);
  }
  snprintf(v->features, sizeof(v->features), "0x%X,0x%X", AIRPLAY_FEATURES_LO,
           AIRPLAY_FEATURES_HI);

  /* "fex": all feature bits (0-127) as little-endian bytes, trailing zero
   * bytes dropped, base64 without padding - the form HomePods advertise. */
  uint8_t bytes[16];
  const uint64_t words[2] = {AIRPLAY_FEATURES, AIRPLAY_FEATURES_EX};
  size_t len = 0;
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = (uint8_t)(words[i / 8] >> (8 * (i % 8)));
    if (bytes[i]) len = i + 1;
  }
  sodium_bin2base64(v->fex, sizeof(v->fex), bytes, len,
                    sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
}

/* Published only when an extended feature bit (64+) is selected. */
#if AIRPLAY_FEATURES_EX != 0
#define AIRPLAY_TXT_FEX(v) {"fex", (v).fex},
#else
#define AIRPLAY_TXT_FEX(v)
#endif

#define AIRPLAY_TXT_ITEMS(v)                                             \
  {                                                                      \
    {"deviceid", (v).device_id}, {"features", (v).features},             \
        AIRPLAY_TXT_FEX(v)                                               \
        {"flags", AIRPLAY_FLAGS}, {"model", AIRPLAY_MODEL},              \
        {"pk", (v).pk},                                                  \
        {"pi", "00000000-0000-0000-0000-000000000000"},                  \
        {"srcvers", AIRPLAY_SOURCE_VERSION},                             \
        {"vv", AIRPLAY_PROTOCOL_VERSION}, {"acl", "0"},                  \
  }

size_t mdns_airplay_txt_record_data(uint8_t *out, size_t capacity) {
  if (!out) return 0;
  airplay_txt_values_t v;
  airplay_txt_values(&v);
  const mdns_txt_item_t items[] = AIRPLAY_TXT_ITEMS(v);
  size_t pos = 0;
  for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
    const size_t klen = strlen(items[i].key);
    const size_t vlen = strlen(items[i].value);
    const size_t len = klen + 1 + vlen;
    if (len > 255 || pos + 1 + len > capacity) return 0;
    out[pos++] = (uint8_t)len;
    memcpy(out + pos, items[i].key, klen);
    pos += klen;
    out[pos++] = '=';
    memcpy(out + pos, items[i].value, vlen);
    pos += vlen;
  }
  return pos;
}

void mdns_airplay_init(void) {
  char features_str[32];
  char service_name[80];
  char pk_str[65]; // 32 bytes = 64 hex chars + null
  char device_name[65];

  // Get device name from settings
  settings_get_device_name(device_name, sizeof(device_name));

  // Get real Ed25519 public key from HAP module
  const uint8_t *pk = hap_get_public_key();
  for (int i = 0; i < 32; i++) {
    snprintf(pk_str + (size_t)i * 2, 3, "%02x", pk[i]);
  }

  // Format features as "lo,hi" hex string
  snprintf(features_str, sizeof(features_str), "0x%X,0x%X", AIRPLAY_FEATURES_LO,
           AIRPLAY_FEATURES_HI);

  // Create service name for RAOP: <mac>@<name>
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(service_name, sizeof(service_name), "%02X%02X%02X%02X%02X%02X@%s",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], device_name);

  // Initialize mDNS
  ESP_ERROR_CHECK(mdns_init());

  // Set hostname
  ESP_ERROR_CHECK(mdns_hostname_set(device_name));

  // ========================================
  // _airplay._tcp service (port 7000)
  // ========================================
  airplay_txt_values_t txt_values;
  airplay_txt_values(&txt_values);
  mdns_txt_item_t airplay_txt[] = AIRPLAY_TXT_ITEMS(txt_values);

  esp_err_t err =
      mdns_service_add(device_name, "_airplay", "_tcp", 7000, airplay_txt,
                       sizeof(airplay_txt) / sizeof(airplay_txt[0]));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _airplay._tcp service: %s",
             esp_err_to_name(err));
  }

  // ========================================
  // _raop._tcp service (port 7000)
  // RAOP = Remote Audio Output Protocol
  // Service name format: <MAC>@<DeviceName>
  // ========================================
  // Dual-mode: include et=1 (RSA) so RAOP-only clients (TuneBlade, AirMusic,
  // shairtunes2, etc.) accept the advertisement, while keeping et=3,5 for
  // AirPlay 2 FairPlay/MFi-SAP. ek=1 advertises that an RSA-encrypted key
  // can be supplied via SDP rsaaeskey: at ANNOUNCE time.
  mdns_txt_item_t raop_txt[] = {
      {"am", AIRPLAY_MODEL},
      {"cn", "0,1,2,3"},              // Audio codecs: PCM, ALAC, AAC, AAC-ELD
      {"da", "true"},                 // Digest auth
      {"ek", "1"},                    // Encryption key available (RSA)
      {"et", "0,1,3,5"},              // Encryption types
      {"ft", features_str},           // Features (same as airplay)
      {"md", AIRPLAY_METADATA_TYPES}, // Metadata types
      {"pk", pk_str},                 // Public key
      {"sf", AIRPLAY_FLAGS},          // Status flags
      {"tp", "UDP"},                  // Transport protocol
      {"vn", "65537"},                // Version number
      {"vs", AIRPLAY_SOURCE_VERSION},
      {"vv", AIRPLAY_PROTOCOL_VERSION},
  };

  esp_err_t err_raop =
      mdns_service_add(service_name, "_raop", "_tcp", 7000, raop_txt,
                       sizeof(raop_txt) / sizeof(raop_txt[0]));
  if (err_raop != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _raop._tcp service: %s",
             esp_err_to_name(err_raop));
  }
}
