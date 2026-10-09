#include "hap_pairings.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "tlv8.h"

static const char *TAG = "hap_pairings";

#define NVS_NAMESPACE "airplay"
#define NVS_KEY_PAIRS "hk_pairs"
#define TLV_TYPE_PERMISSIONS 0x0B
#define TLV_TYPE_SEPARATOR   0xFF

typedef struct {
  uint8_t used;
  uint8_t perm;
  uint8_t id_len;
  char id[HAP_PAIRING_ID_MAX];
  uint8_t ltpk[32];
} pairing_t;

static pairing_t *s_pairs; /* HAP_PAIRINGS_MAX entries, PSRAM */
static SemaphoreHandle_t s_lock;
static hap_pairings_changed_cb s_cb;

static esp_err_t save_locked(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(nvs, NVS_KEY_PAIRS, s_pairs, sizeof(pairing_t) * HAP_PAIRINGS_MAX);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err != ESP_OK) ESP_LOGE(TAG, "Saving pairings failed: %s", esp_err_to_name(err));
  return err;
}

static bool has_admin_locked(void) {
  for (int i = 0; i < HAP_PAIRINGS_MAX; i++)
    if (s_pairs[i].used && (s_pairs[i].perm & HAP_PERM_ADMIN)) return true;
  return false;
}

static int find_locked(const uint8_t *id, size_t id_len) {
  for (int i = 0; i < HAP_PAIRINGS_MAX; i++)
    if (s_pairs[i].used && s_pairs[i].id_len == id_len &&
        memcmp(s_pairs[i].id, id, id_len) == 0)
      return i;
  return -1;
}

static void notify(bool before, bool after) {
  if (before != after && s_cb) s_cb(after);
}

esp_err_t hap_pairings_init(void) {
  if (s_pairs) return ESP_OK;
  s_pairs = heap_caps_calloc(HAP_PAIRINGS_MAX, sizeof(pairing_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_pairs) s_pairs = calloc(HAP_PAIRINGS_MAX, sizeof(pairing_t));
  s_lock = xSemaphoreCreateMutex();
  if (!s_pairs || !s_lock) return ESP_ERR_NO_MEM;

  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
    size_t len = sizeof(pairing_t) * HAP_PAIRINGS_MAX;
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY_PAIRS, s_pairs, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len != sizeof(pairing_t) * HAP_PAIRINGS_MAX)
      memset(s_pairs, 0, sizeof(pairing_t) * HAP_PAIRINGS_MAX);
  }
  for (int i = 0; i < HAP_PAIRINGS_MAX; i++)
    if (s_pairs[i].id_len > HAP_PAIRING_ID_MAX) s_pairs[i].used = 0;
  hap_pairings_log("loaded", true);
  return ESP_OK;
}

size_t hap_pairings_count(void) {
  if (!s_pairs) return 0;
  size_t n = 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < HAP_PAIRINGS_MAX; i++) n += s_pairs[i].used ? 1U : 0U;
  xSemaphoreGive(s_lock);
  return n;
}

bool hap_pairings_has_admin(void) {
  if (!s_pairs) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool a = has_admin_locked();
  xSemaphoreGive(s_lock);
  return a;
}

bool hap_pairings_find(const uint8_t *id, size_t id_len, uint8_t *ltpk, uint8_t *perm) {
  if (!s_pairs || !id || id_len == 0 || id_len > HAP_PAIRING_ID_MAX) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int i = find_locked(id, id_len);
  if (i >= 0) {
    if (ltpk) memcpy(ltpk, s_pairs[i].ltpk, 32);
    if (perm) *perm = s_pairs[i].perm;
  }
  xSemaphoreGive(s_lock);
  return i >= 0;
}

esp_err_t hap_pairings_add(const uint8_t *id, size_t id_len, const uint8_t ltpk[32],
                           uint8_t perm) {
  if (!s_pairs || !id || id_len == 0 || id_len > HAP_PAIRING_ID_MAX || !ltpk)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool before = has_admin_locked();
  esp_err_t err = ESP_OK;
  int i = find_locked(id, id_len);
  const bool existed = i >= 0;
  bool unchanged = false;
  if (i >= 0) {
    if (memcmp(s_pairs[i].ltpk, ltpk, 32) != 0) {
      err = ESP_ERR_INVALID_STATE; /* HAP: same id, other key -> error */
    } else if (s_pairs[i].perm == perm) {
      /* The Home hub re-adds the same controllers in every management
       * session: nothing to store (spares the flash). */
      unchanged = true;
    } else {
      s_pairs[i].perm = perm; /* permission update */
    }
  } else {
    for (i = 0; i < HAP_PAIRINGS_MAX && s_pairs[i].used; i++) {
    }
    if (i == HAP_PAIRINGS_MAX) {
      err = ESP_ERR_NO_MEM;
    } else {
      memset(&s_pairs[i], 0, sizeof(s_pairs[i]));
      s_pairs[i].used = 1;
      s_pairs[i].perm = perm;
      s_pairs[i].id_len = (uint8_t)id_len;
      memcpy(s_pairs[i].id, id, id_len);
      memcpy(s_pairs[i].ltpk, ltpk, 32);
    }
  }
  if (err == ESP_OK && !unchanged) err = save_locked();
  const bool after = has_admin_locked();
  xSemaphoreGive(s_lock);
  if (err == ESP_OK && unchanged) {
    ESP_LOGD(TAG, "Controller %.*s already paired (%s)", (int)id_len, (const char *)id,
             (perm & HAP_PERM_ADMIN) ? "admin" : "user");
  } else if (err == ESP_OK) {
    ESP_LOGI(TAG, "Controller %.*s %s (%s)", (int)id_len, (const char *)id,
             existed ? "permission changed" : "stored",
             (perm & HAP_PERM_ADMIN) ? "admin" : "user");
  }
  notify(before, after);
  return err;
}

esp_err_t hap_pairings_remove(const uint8_t *id, size_t id_len) {
  if (!s_pairs || !id || id_len == 0 || id_len > HAP_PAIRING_ID_MAX)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool before = has_admin_locked();
  int i = find_locked(id, id_len);
  esp_err_t err = ESP_ERR_NOT_FOUND;
  if (i >= 0) {
    memset(&s_pairs[i], 0, sizeof(s_pairs[i]));
    if (!has_admin_locked()) {
      /* HAP: removing the last admin removes all pairings. */
      memset(s_pairs, 0, sizeof(pairing_t) * HAP_PAIRINGS_MAX);
    }
    err = save_locked();
  }
  const bool after = has_admin_locked();
  xSemaphoreGive(s_lock);
  if (i >= 0)
    ESP_LOGI(TAG, "Controller %.*s removed%s", (int)id_len, (const char *)id,
             after ? "" : " (no admin left: all pairings cleared)");
  notify(before, after);
  return err;
}

size_t hap_pairings_list_tlv(uint8_t *out, size_t cap) {
  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, out, cap);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, 2);
  if (!s_pairs) return tlv8_encoder_size(&enc);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool first = true;
  for (int i = 0; i < HAP_PAIRINGS_MAX; i++) {
    if (!s_pairs[i].used) continue;
    if (!first) tlv8_encode(&enc, TLV_TYPE_SEPARATOR, NULL, 0);
    first = false;
    tlv8_encode(&enc, TLV_TYPE_IDENTIFIER, (const uint8_t *)s_pairs[i].id, s_pairs[i].id_len);
    tlv8_encode(&enc, TLV_TYPE_PUBLIC_KEY, s_pairs[i].ltpk, 32);
    tlv8_encode_byte(&enc, TLV_TYPE_PERMISSIONS, s_pairs[i].perm);
  }
  xSemaphoreGive(s_lock);
  return tlv8_encoder_size(&enc);
}

void hap_pairings_log(const char *why, bool detail) {
  if (!s_pairs) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int n = 0;
  for (int i = 0; i < HAP_PAIRINGS_MAX; i++) n += s_pairs[i].used ? 1 : 0;
  ESP_LOGI(TAG, "HomeKit pairings (%s): %d controller%s", why ? why : "-", n,
           n == 1 ? "" : "s");
  for (int i = 0; i < HAP_PAIRINGS_MAX; i++) {
    if (!s_pairs[i].used) continue;
    if (detail) {
      ESP_LOGI(TAG, "    %.*s  %s  ltpk %02x%02x%02x%02x...", (int)s_pairs[i].id_len,
               s_pairs[i].id, (s_pairs[i].perm & HAP_PERM_ADMIN) ? "admin" : "user ",
               s_pairs[i].ltpk[0], s_pairs[i].ltpk[1], s_pairs[i].ltpk[2], s_pairs[i].ltpk[3]);
    } else {
      ESP_LOGD(TAG, "    %.*s  %s  ltpk %02x%02x%02x%02x...", (int)s_pairs[i].id_len,
               s_pairs[i].id, (s_pairs[i].perm & HAP_PERM_ADMIN) ? "admin" : "user ",
               s_pairs[i].ltpk[0], s_pairs[i].ltpk[1], s_pairs[i].ltpk[2], s_pairs[i].ltpk[3]);
    }
  }
  xSemaphoreGive(s_lock);
}

void hap_pairings_set_changed_cb(hap_pairings_changed_cb cb) { s_cb = cb; }
