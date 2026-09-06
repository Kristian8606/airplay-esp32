#include "settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "settings";
static const char *NS = "airplay";
static float s_volume_db = 0.0f;

#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_WIFI_LIST "wifi_list"
#define WIFI_STORE_MAGIC 0x57494649U
#define WIFI_STORE_VERSION 1U

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint16_t latest_index;
  uint16_t next_replace;
  settings_wifi_network_t network[SETTINGS_MAX_WIFI_NETWORKS];
} wifi_store_t;

static void wifi_store_init(wifi_store_t *store) {
  memset(store, 0, sizeof(*store));
  store->magic = WIFI_STORE_MAGIC;
  store->version = WIFI_STORE_VERSION;
}

static bool wifi_store_valid(const wifi_store_t *store) {
  if (!store || store->magic != WIFI_STORE_MAGIC ||
      store->version != WIFI_STORE_VERSION ||
      store->count > SETTINGS_MAX_WIFI_NETWORKS) {
    return false;
  }
  if (store->count == 0) return true;
  return store->latest_index < store->count;
}

static esp_err_t wifi_store_load(nvs_handle_t h, wifi_store_t *store) {
  if (!store) return ESP_ERR_INVALID_ARG;
  size_t size = sizeof(*store);
  esp_err_t e = nvs_get_blob(h, NVS_KEY_WIFI_LIST, store, &size);
  if (e != ESP_OK) return e;
  if (size != sizeof(*store) || !wifi_store_valid(store)) {
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

static esp_err_t wifi_store_save(nvs_handle_t h, const wifi_store_t *store) {
  if (!store || !wifi_store_valid(store)) return ESP_ERR_INVALID_ARG;
  return nvs_set_blob(h, NVS_KEY_WIFI_LIST, store, sizeof(*store));
}

static esp_err_t nvs_get_str_fixed(nvs_handle_t h, const char *key, char *out,
                                   size_t len) {
  if (!out || len == 0) return ESP_ERR_INVALID_ARG;
  size_t need = len;
  esp_err_t e = nvs_get_str(h, key, out, &need);
  if (e != ESP_OK) out[0] = '\0';
  return e;
}

static bool load_legacy_network(nvs_handle_t h,
                                settings_wifi_network_t *network) {
  if (!network) return false;
  memset(network, 0, sizeof(*network));
  if (nvs_get_str_fixed(h, NVS_KEY_WIFI_SSID, network->ssid,
                        sizeof(network->ssid)) != ESP_OK ||
      network->ssid[0] == '\0') {
    return false;
  }
  if (nvs_get_str_fixed(h, NVS_KEY_WIFI_PASS, network->password,
                        sizeof(network->password)) != ESP_OK) {
    network->password[0] = '\0';
  }
  return true;
}

static esp_err_t load_store_or_legacy(nvs_handle_t h, wifi_store_t *store) {
  esp_err_t e = wifi_store_load(h, store);
  if (e == ESP_OK) return ESP_OK;

  wifi_store_init(store);
  settings_wifi_network_t legacy;
  if (load_legacy_network(h, &legacy)) {
    store->network[0] = legacy;
    store->count = 1;
    store->latest_index = 0;
    store->next_replace = 0;
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}

esp_err_t settings_init(void) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;

  wifi_store_t store;
  e = wifi_store_load(h, &store);
  if (e != ESP_OK) {
    wifi_store_init(&store);
    settings_wifi_network_t legacy;
    if (load_legacy_network(h, &legacy)) {
      store.network[0] = legacy;
      store.count = 1;
      store.latest_index = 0;
      e = wifi_store_save(h, &store);
      if (e == ESP_OK) e = nvs_commit(h);
      if (e == ESP_OK) {
        ESP_LOGI(TAG, "Migrated legacy WiFi credentials into saved network list");
      }
    } else {
      e = wifi_store_save(h, &store);
      if (e == ESP_OK) e = nvs_commit(h);
    }
  }

  nvs_close(h);
  return e;
}

static esp_err_t get_str(const char *key, char *out, size_t len) {
  if (!out || !len) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
  if (e != ESP_OK) return e;
  size_t need = len;
  e = nvs_get_str(h, key, out, &need);
  nvs_close(h);
  return e;
}

static esp_err_t set_str(const char *key, const char *value) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_str(h, key, value ? value : "");
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}

size_t settings_get_wifi_network_count(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
  wifi_store_t store;
  esp_err_t e = load_store_or_legacy(h, &store);
  nvs_close(h);
  return e == ESP_OK ? store.count : 0;
}

esp_err_t settings_get_wifi_network(size_t index,
                                    settings_wifi_network_t *network) {
  if (!network) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
  if (e != ESP_OK) return e;

  wifi_store_t store;
  e = load_store_or_legacy(h, &store);
  if (e == ESP_OK) {
    if (index >= store.count) {
      e = ESP_ERR_NOT_FOUND;
    } else {
      *network = store.network[index];
    }
  }
  nvs_close(h);
  return e;
}

static esp_err_t get_latest_wifi_network(settings_wifi_network_t *network) {
  if (!network) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
  if (e != ESP_OK) return e;

  wifi_store_t store;
  e = load_store_or_legacy(h, &store);
  if (e == ESP_OK) {
    if (store.count == 0 || store.latest_index >= store.count) {
      e = ESP_ERR_NOT_FOUND;
    } else {
      *network = store.network[store.latest_index];
    }
  }
  nvs_close(h);
  return e;
}

esp_err_t settings_get_wifi_ssid(char *ssid, size_t len) {
  if (!ssid || len == 0) return ESP_ERR_INVALID_ARG;
  settings_wifi_network_t network;
  esp_err_t e = get_latest_wifi_network(&network);
  if (e != ESP_OK) return e;
  strlcpy(ssid, network.ssid, len);
  return ESP_OK;
}

esp_err_t settings_get_wifi_password(char *password, size_t len) {
  if (!password || len == 0) return ESP_ERR_INVALID_ARG;
  settings_wifi_network_t network;
  esp_err_t e = get_latest_wifi_network(&network);
  if (e != ESP_OK) return e;
  strlcpy(password, network.password, len);
  return ESP_OK;
}

esp_err_t settings_set_wifi_credentials(const char *ssid,
                                        const char *password) {
  if (!ssid || ssid[0] == '\0' || strlen(ssid) >= SETTINGS_WIFI_SSID_LEN ||
      (password && strlen(password) >= SETTINGS_WIFI_PASSWORD_LEN)) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;

  wifi_store_t store;
  if (load_store_or_legacy(h, &store) != ESP_OK) wifi_store_init(&store);

  size_t index = SETTINGS_MAX_WIFI_NETWORKS;
  for (size_t i = 0; i < store.count; ++i) {
    if (strcmp(store.network[i].ssid, ssid) == 0) {
      index = i;
      break;
    }
  }

  if (index == SETTINGS_MAX_WIFI_NETWORKS) {
    if (store.count < SETTINGS_MAX_WIFI_NETWORKS) {
      index = store.count++;
    } else {
      index = store.next_replace % SETTINGS_MAX_WIFI_NETWORKS;
      store.next_replace =
          (uint16_t)((store.next_replace + 1U) % SETTINGS_MAX_WIFI_NETWORKS);
      ESP_LOGW(TAG, "WiFi list full; replacing saved network slot %u",
               (unsigned)index);
    }
  }

  memset(&store.network[index], 0, sizeof(store.network[index]));
  strlcpy(store.network[index].ssid, ssid,
          sizeof(store.network[index].ssid));
  strlcpy(store.network[index].password, password ? password : "",
          sizeof(store.network[index].password));
  store.latest_index = (uint16_t)index;

  e = wifi_store_save(h, &store);
  if (e == ESP_OK) {
    // Keep the old keys in sync so older firmware can still boot using the
    // most recently added network after a downgrade.
    e = nvs_set_str(h, NVS_KEY_WIFI_SSID, store.network[index].ssid);
  }
  if (e == ESP_OK) {
    e = nvs_set_str(h, NVS_KEY_WIFI_PASS, store.network[index].password);
  }
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);

  if (e == ESP_OK) {
    ESP_LOGI(TAG, "Saved WiFi network '%s' (%u/%u slots used)", ssid,
             (unsigned)store.count, (unsigned)SETTINGS_MAX_WIFI_NETWORKS);
  }
  return e;
}

bool settings_has_wifi_credentials(void) {
  return settings_get_wifi_network_count() > 0;
}

esp_err_t settings_get_device_name(char *name, size_t len) {
  if (!name || !len) return ESP_ERR_INVALID_ARG;
  esp_err_t e = get_str("device_name", name, len);
  if (e != ESP_OK || name[0] == '\0') {
    strlcpy(name, SETTINGS_DEFAULT_DEVICE_NAME, len);
    return ESP_OK;
  }
  return ESP_OK;
}
esp_err_t settings_set_device_name(const char *name) { return set_str("device_name", name); }
esp_err_t settings_get_volume(float *volume_db) {
  if (!volume_db) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
  if (e != ESP_OK) { *volume_db = s_volume_db; return e; }
  int32_t mv = 0;
  e = nvs_get_i32(h, "volume_mdb", &mv);
  nvs_close(h);
  if (e == ESP_OK) s_volume_db = (float)mv / 1000.0f;
  *volume_db = s_volume_db;
  return e;
}
esp_err_t settings_set_volume(float volume_db) { s_volume_db = volume_db; return ESP_OK; }
esp_err_t settings_persist_volume(void) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_i32(h, "volume_mdb", (int32_t)(s_volume_db * 1000.0f));
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}
