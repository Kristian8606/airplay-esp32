#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "wifi.h"
#include "settings.h"

static const char *TAG = "wifi";

// Event group to signal WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Re-enable AP after this many consecutive failures
#define AP_REENABLE_THRESHOLD 5
// lwIP DHCP hostnames are limited to 31 characters plus the trailing NUL.
#define DHCP_HOSTNAME_MAX_LEN 31

static int s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_wifi_initialized = false;
static bool s_sta_connected = false;
static bool s_have_selected_network = false;
static esp_timer_handle_t s_retry_timer = NULL;

// Saved AP config from init, used to re-enable AP without duplication
static wifi_config_t s_ap_config;

static bool wifi_select_best_saved_network(void);
static void scan_and_connect_task(void *arg);
static esp_err_t wifi_collect_scan_results(bool show_hidden,
                                           wifi_ap_record_t **ap_list,
                                           uint16_t *ap_count);

static void sanitize_hostname(const char *name, char *out, size_t out_len) {
  size_t j = 0;
  for (size_t i = 0; name[i] && j < out_len - 1; i++) {
    char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
      out[j++] = c;
    } else if (j > 0 && out[j - 1] != '-') {
      out[j++] = '-';
    }
  }
  while (j > 0 && out[j - 1] == '-') {
    j--;
  }
  if (j == 0) {
    strlcpy(out, "esp32-airplay", out_len);
    return;
  }
  out[j] = '\0';
}

void wifi_set_hostname(const char *device_name) {
  if (!s_sta_netif || !device_name) {
    return;
  }
  char hostname[DHCP_HOSTNAME_MAX_LEN + 1];
  sanitize_hostname(device_name, hostname, sizeof(hostname));
  esp_err_t err = esp_netif_set_hostname(s_sta_netif, hostname);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set hostname '%s': %s", hostname,
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Hostname set to: %s", hostname);
  }
}

static void retry_timer_callback(void *arg) {
  if (!s_sta_connected && s_have_selected_network) {
    ESP_LOGI(TAG, "Retry timer fired, reconnecting same AP (attempt %d)...",
             s_retry_num + 1);
    esp_wifi_connect();
  }
}

static void schedule_retry(void) {
  if (!s_have_selected_network) return;
  // Exponential backoff: 5s, 10s, 20s, 30s (max)
  int delay_s = 5;
  if (s_retry_num > AP_REENABLE_THRESHOLD) {
    int backoff_count = s_retry_num - AP_REENABLE_THRESHOLD;
    delay_s = 5 * (1 << (backoff_count > 3 ? 3 : backoff_count));
    if (delay_s > 30) {
      delay_s = 30;
    }
  }
  ESP_LOGI(TAG, "Scheduling same-AP retry in %d seconds", delay_s);
  esp_timer_stop(s_retry_timer);
  esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
}

static void enable_ap_mode(void) {
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_APSTA) {
    ESP_LOGI(TAG, "Re-enabling AP mode for configuration access");
    if (!s_ap_netif) {
      s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &s_ap_config);
  }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    // Scan once at boot. Choose the strongest AP among all saved SSIDs, then
    // lock to that BSSID for the lifetime of this boot (no roaming).
    xTaskCreatePinnedToCore(scan_and_connect_task, "wifi_scan", 4096, NULL, 3,
                            NULL, 0);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_sta_connected = false;
    wifi_event_sta_disconnected_t *disconnected =
        (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGI(TAG, "Disconnected from AP, reason: %d", disconnected->reason);

    if (!s_have_selected_network) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      enable_ap_mode();
      return;
    }

    s_retry_num++;
    if (s_retry_num < AP_REENABLE_THRESHOLD) {
      // Reconnect only to the BSSID selected at boot. A new strongest-AP scan
      // happens only after reboot, matching the upstream behavior we want.
      ESP_LOGI(TAG, "Retrying same AP (%d/%d)...", s_retry_num,
               AP_REENABLE_THRESHOLD);
      esp_wifi_connect();
    } else {
      if (s_retry_num == AP_REENABLE_THRESHOLD) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGW(TAG,
                 "WiFi connection failed after %d attempts; keeping same "
                 "BSSID and enabling setup AP",
                 AP_REENABLE_THRESHOLD);
        enable_ap_mode();
      }
      schedule_retry();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    s_sta_connected = true;
    xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    // Disable AP mode when STA connects
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
      ESP_LOGI(TAG, "STA connected, disabling AP mode");
      esp_wifi_set_mode(WIFI_MODE_STA);
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
    ESP_LOGI(TAG, "AP started");
  }
}

static esp_err_t wifi_collect_scan_results(bool show_hidden,
                                           wifi_ap_record_t **ap_list,
                                           uint16_t *ap_count) {
  if (!ap_list || !ap_count) return ESP_ERR_INVALID_ARG;
  *ap_list = NULL;
  *ap_count = 0;

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = show_hidden,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time = {.active = {.min = 0, .max = 0}},
  };

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) return err;

  uint16_t number = 0;
  err = esp_wifi_scan_get_ap_num(&number);
  if (err != ESP_OK) return err;
  if (number == 0) return ESP_OK;

  wifi_ap_record_t *aps = malloc(sizeof(*aps) * number);
  if (!aps) return ESP_ERR_NO_MEM;

  err = esp_wifi_scan_get_ap_records(&number, aps);
  if (err != ESP_OK) {
    free(aps);
    return err;
  }

  *ap_list = aps;
  *ap_count = number;
  return ESP_OK;
}

// One-shot task: at boot, scan all channels once and choose the strongest AP
// whose SSID exists in our saved-network list. Once selected, keep that BSSID.
static void scan_and_connect_task(void *arg) {
  const size_t saved_count = settings_get_wifi_network_count();
  if (saved_count == 0) {
    ESP_LOGW(TAG, "No saved WiFi networks; setup AP remains available");
    s_have_selected_network = false;
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    enable_ap_mode();
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "Boot WiFi scan: %u saved network(s)", (unsigned)saved_count);
  if (!wifi_select_best_saved_network()) {
    ESP_LOGW(TAG,
             "None of the saved WiFi networks are available; setup AP remains "
             "available");
    s_have_selected_network = false;
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    enable_ap_mode();
    vTaskDelete(NULL);
    return;
  }

  xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    enable_ap_mode();
  }
  vTaskDelete(NULL);
}

static bool wifi_select_best_saved_network(void) {
  settings_wifi_network_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  size_t saved_count = settings_get_wifi_network_count();
  if (saved_count == 0) return false;
  if (saved_count > SETTINGS_MAX_WIFI_NETWORKS) {
    saved_count = SETTINGS_MAX_WIFI_NETWORKS;
  }

  size_t loaded = 0;
  for (size_t i = 0; i < saved_count; ++i) {
    if (settings_get_wifi_network(i, &saved[loaded]) == ESP_OK &&
        saved[loaded].ssid[0] != '\0') {
      loaded++;
    }
  }
  if (loaded == 0) return false;

  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;
  esp_err_t err = wifi_collect_scan_results(true, &ap_list, &ap_count);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Boot WiFi scan failed: %s", esp_err_to_name(err));
    return false;
  }

  int best_ap = -1;
  int best_saved = -1;
  for (uint16_t i = 0; i < ap_count; ++i) {
    for (size_t j = 0; j < loaded; ++j) {
      if (strcmp((char *)ap_list[i].ssid, saved[j].ssid) != 0) continue;
      if (best_ap < 0 || ap_list[i].rssi > ap_list[best_ap].rssi) {
        best_ap = (int)i;
        best_saved = (int)j;
      }
    }
  }

  if (best_ap < 0 || best_saved < 0) {
    free(ap_list);
    return false;
  }

  wifi_config_t sta_cfg = {0};
  strlcpy((char *)sta_cfg.sta.ssid, saved[best_saved].ssid,
          sizeof(sta_cfg.sta.ssid));
  strlcpy((char *)sta_cfg.sta.password, saved[best_saved].password,
          sizeof(sta_cfg.sta.password));
  memcpy(sta_cfg.sta.bssid, ap_list[best_ap].bssid, sizeof(sta_cfg.sta.bssid));
  sta_cfg.sta.bssid_set = true;
  sta_cfg.sta.channel = 0;
  sta_cfg.sta.threshold.authmode = ap_list[best_ap].authmode;
  sta_cfg.sta.pmf_cfg.capable = true;
  sta_cfg.sta.pmf_cfg.required = false;

  err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure selected WiFi: %s", esp_err_to_name(err));
    free(ap_list);
    return false;
  }

  ESP_LOGI(TAG,
           "Selected saved WiFi '%s': " MACSTR " (rssi=%d, ch=%d); BSSID "
           "locked until reboot",
           saved[best_saved].ssid, MAC2STR(ap_list[best_ap].bssid),
           ap_list[best_ap].rssi, ap_list[best_ap].primary);

  s_have_selected_network = true;
  free(ap_list);
  return true;
}

static void wifi_init_base(void) {
  if (s_wifi_initialized) {
    return;
  }

  s_wifi_event_group = xEventGroupCreate();

  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }

  // Create event loop if it doesn't exist
  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  // Realtime audio: keep modem power-save disabled.
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  // Create one-shot retry timer (no background task needed)
  const esp_timer_create_args_t timer_args = {
      .callback = retry_timer_callback,
      .name = "wifi_retry",
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

  s_wifi_initialized = true;
}

void wifi_init_apsta(const char *ap_ssid, const char *ap_password) {
  wifi_init_base();

  if (!s_sta_netif) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    char dev_name[65];
    settings_get_device_name(dev_name, sizeof(dev_name));
    wifi_set_hostname(dev_name);
  }
  if (!s_ap_netif) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
  }

  // Configure AP and save for later re-enable. STA credentials are selected
  // only after the boot scan has compared all saved networks.
  const char *default_ssid = ap_ssid ? ap_ssid : CONFIG_DEFAULT_AP_SSID;
  const char *default_password =
      ap_password ? ap_password : CONFIG_DEFAULT_AP_PASSWORD;

  memset(&s_ap_config, 0, sizeof(s_ap_config));
  strncpy((char *)s_ap_config.ap.ssid, default_ssid,
          sizeof(s_ap_config.ap.ssid) - 1);
  s_ap_config.ap.ssid_len = strlen(default_ssid);
  s_ap_config.ap.channel = 1;
  s_ap_config.ap.max_connection = 4;

  if (strlen(default_password) == 0) {
    s_ap_config.ap.authmode = WIFI_AUTH_OPEN;
  } else {
    strncpy((char *)s_ap_config.ap.password, default_password,
            sizeof(s_ap_config.ap.password) - 1);
    s_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }

  s_retry_num = 0;
  s_have_selected_network = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "AP+STA mode started: AP SSID=%s, saved networks=%u",
           default_ssid, (unsigned)settings_get_wifi_network_count());
}

bool wifi_wait_connected(uint32_t timeout_ms) {
  if (!s_wifi_event_group) {
    return false;
  }

  TickType_t timeout_ticks =
      timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, timeout_ticks);

  if (bits & WIFI_CONNECTED_BIT) {
    return true;
  }
  if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "No saved WiFi connection available");
  }
  return false;
}

void wifi_get_mac_str(char *mac_str, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_str, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

bool wifi_is_connected(void) {
  return s_sta_connected;
}

esp_err_t wifi_get_ip_str(char *ip_str, size_t len) {
  if (!s_sta_netif || !ip_str || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
  if (err == ESP_OK) {
    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
  }
  return err;
}

esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count) {
  if (!ap_list || !ap_count) {
    return ESP_ERR_INVALID_ARG;
  }

  // Web/UI scan must not disconnect an established AirPlay WiFi link and must
  // not clear the BSSID lock selected at boot. ESP-IDF supports scanning while
  // associated; the radio may briefly leave the channel, but the STA remains
  // connected and reconnect/roaming policy is unchanged.
  esp_err_t err = wifi_collect_scan_results(true, ap_list, ap_count);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
  }
  return err;
}

void wifi_stop(void) {
  if (s_wifi_initialized) {
    esp_timer_stop(s_retry_timer);
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_initialized = false;
    s_sta_connected = false;
      s_have_selected_network = false;
    s_retry_num = 0;
    if (s_wifi_event_group) {
      xEventGroupClearBits(s_wifi_event_group,
                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
  }
}
