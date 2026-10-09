#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "airplay_identity.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "wifi.h"
#include "settings.h"

static const char *TAG = "mdns_airplay";

#define STR_(x) #x
#define STR(x) STR_(x)

// Model, versions, flags and feature bits come from airplay_identity.h
// (shared with GET /info). Status flags change when the receiver joins or
// leaves a HomeKit home (mdns_airplay_update_flags).
static char s_flags_str[16];

/* AirPlay group the receiver currently plays in (TXT gid / gcgl). A sender
 * puts its groupUUID in the session SETUP; senders and the Home app use it to
 * see which group (and whose now-playing info) this speaker belongs to.
 * Without a session, gid is our own pairing identifier (as shairport-sync). */
static char s_gid[48];
static char s_gcgl[2] = "0";

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

void mdns_airplay_init(void) {
  char mac_str[18];
  char device_id[18];
  char features_str[32];
  char service_name[80];
  char pk_str[65]; // 32 bytes = 64 hex chars + null
  char device_name[65];
  char hostname[WIFI_HOSTNAME_MAX_LEN + 1];
  char pairing_id[AIRPLAY_PAIRING_ID_LEN];

  snprintf(s_flags_str, sizeof(s_flags_str), "0x%" PRIX32, airplay_status_flags());
#ifdef CONFIG_AIRPLAY_HOMEKIT
  char serial[16];
  char fw_version[32];
  airplay_get_serial_number(serial, sizeof(serial));
  airplay_get_firmware_revision(fw_version, sizeof(fw_version));
#endif

  // Get device name from settings. The service instance names may contain
  // spaces/UTF-8; the host name must be a plain DNS label (same as DHCP).
  settings_get_device_name(device_name, sizeof(device_name));
  wifi_sanitize_hostname(device_name, hostname, sizeof(hostname));
  airplay_get_pairing_id(pairing_id, sizeof(pairing_id));
  snprintf(s_gid, sizeof(s_gid), "%s", pairing_id);

  // Get MAC address
  wifi_get_mac_str(mac_str, sizeof(mac_str));
  strncpy(device_id, mac_str, sizeof(device_id));

  // Get real Ed25519 public key from HAP module
  const uint8_t *pk = hap_get_public_key();
  for (int i = 0; i < 32; i++) {
    snprintf(pk_str + (size_t)i * 2, 3, "%02x", pk[i]);
  }

  // Format features as "hi,lo" hex string
  snprintf(features_str, sizeof(features_str), "0x%X,0x%X", AIRPLAY_FEATURES_LO,
           AIRPLAY_FEATURES_HI);

  // Create service name for RAOP: <mac>@<name>
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(service_name, sizeof(service_name), "%02X%02X%02X%02X%02X%02X@%s",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], device_name);

  // Initialize mDNS. A failure here must not reboot the device in a loop:
  // log it and keep the rest of the firmware (web UI, OTA) reachable by IP.
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
    return;
  }

  err = mdns_hostname_set(hostname);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set mDNS hostname '%s': %s", hostname,
             esp_err_to_name(err));
  }

  // ========================================
  // _airplay._tcp service (port 7000)
  // ========================================
  mdns_txt_item_t airplay_txt[] = {
      {"deviceid", device_id},
      {"features", features_str},
      {"flags", s_flags_str},
      {"model", AIRPLAY_MODEL},
      {"pk", pk_str},
      {"pi", pairing_id},
      {"srcvers", AIRPLAY_SOURCE_VERSION},
      {"vv", STR(AIRPLAY_PROTOCOL_VERSION)},
      {"acl", "0"},
      {"gid", s_gid},
      {"igl", "0"},
      {"isGroupLeader", "0"}, /* AirPort Express spelling of igl */
      {"gcgl", s_gcgl},
      {"rsf", "0x0"},
#ifdef CONFIG_AIRPLAY_HOMEKIT
      /* Accessory details the Home app shows, as an AirPort Express has. */
      {"manufacturer", CONFIG_AIRPLAY_HOMEKIT_MANUFACTURER},
      {"serialNumber", serial},
      {"fv", fw_version},
      {"protovers", AIRPLAY_PROTOVERS},
#endif
  };

  err = mdns_service_add(device_name, "_airplay", "_tcp", 7000, airplay_txt,
                         sizeof(airplay_txt) / sizeof(airplay_txt[0]));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _airplay._tcp service: %s",
             esp_err_to_name(err));
  }

  // ========================================
  // _raop._tcp service (port 7000)
  // RAOP = Remote Audio Output Protocol. AirPlay 2 senders still discover
  // audio receivers through it. Service name format: <MAC>@<DeviceName>
  // No RSA (et=1/ek): classic RAOP (AirPlay 1) is not supported, so legacy
  // RAOP-only clients must not be invited to use it.
  // ========================================
  mdns_txt_item_t raop_txt[] = {
      {"am", AIRPLAY_MODEL},
      {"cn", "0,1,2,3"},              // Audio codecs: PCM, ALAC, AAC, AAC-ELD
      {"da", "true"},                 // Digest auth
      {"et", "0,3,5"},                // Encryption types: none, FairPlay, MFi-SAP
      {"ft", features_str},           // Features (same as airplay)
      {"md", AIRPLAY_METADATA_TYPES}, // Metadata types
      {"pk", pk_str},                 // Public key
      {"sf", s_flags_str},            // Status flags
      {"tp", "UDP"},                  // Transport protocol
      {"vn", "65537"},                // Version number
      {"vs", AIRPLAY_SOURCE_VERSION},
      {"vv", STR(AIRPLAY_PROTOCOL_VERSION)},
  };

  esp_err_t err_raop =
      mdns_service_add(service_name, "_raop", "_tcp", 7000, raop_txt,
                       sizeof(raop_txt) / sizeof(raop_txt[0]));
  if (err_raop != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _raop._tcp service: %s",
             esp_err_to_name(err_raop));
  }
}

void mdns_airplay_update_flags(void) {
  char flags[16];
  snprintf(flags, sizeof(flags), "0x%" PRIX32, airplay_status_flags());
  if (strcmp(flags, s_flags_str) == 0) return;
  snprintf(s_flags_str, sizeof(s_flags_str), "%s", flags);
  esp_err_t a = mdns_service_txt_item_set("_airplay", "_tcp", "flags", s_flags_str);
  esp_err_t r = mdns_service_txt_item_set("_raop", "_tcp", "sf", s_flags_str);
  ESP_LOGI(TAG, "Status flags now %s (TXT update: airplay %s, raop %s)", s_flags_str,
           esp_err_to_name(a), esp_err_to_name(r));
}

void mdns_airplay_set_group(const char *gid, bool contains_leader) {
  char want[sizeof(s_gid)];
  if (gid && gid[0]) {
    snprintf(want, sizeof(want), "%s", gid);
  } else {
    airplay_get_pairing_id(want, sizeof(want));
  }
  const char *gcgl = contains_leader ? "1" : "0";
  if (strcmp(want, s_gid) == 0 && strcmp(gcgl, s_gcgl) == 0) return;
  snprintf(s_gid, sizeof(s_gid), "%s", want);
  snprintf(s_gcgl, sizeof(s_gcgl), "%s", gcgl);
  esp_err_t a = mdns_service_txt_item_set("_airplay", "_tcp", "gid", s_gid);
  esp_err_t b = mdns_service_txt_item_set("_airplay", "_tcp", "gcgl", s_gcgl);
  ESP_LOGI(TAG, "AirPlay group now %s%s (TXT update: %s, %s)", s_gid,
           (gid && gid[0]) ? (contains_leader ? ", group has a leader" : "") : " (own, no session)",
           esp_err_to_name(a), esp_err_to_name(b));
}
