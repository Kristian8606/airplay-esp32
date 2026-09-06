#include "audio_receiver.h"
#include "audio_eq.h"
#include "dns_server.h"
#include "hap.h"
#include "log_stream.h"
#include "led.h"
#include "mdns_airplay.h"
#include "nvs_flash.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "settings.h"
#include "spiffs_storage.h"
#include "web_server.h"
#include "wifi.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";
#define AP_IP_ADDR 0x0104A8C0

static void log_memory_state(const char *where) {
  ESP_LOGI(TAG,
           "MEM %s internalFree=%u KiB internalLargest=%u KiB internalMin=%u KiB "
           "psramFree=%u KiB psramLargest=%u KiB psramMin=%u KiB",
           where,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024U));
}
#define FW_NAME "airplay-esp32_V22_ALAC_R23P_LOG_CLEANUP"

static void print_firmware_banner(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  ESP_LOGI(TAG, "============================================================");
  ESP_LOGI(TAG, "FIRMWARE: %s", FW_NAME);
  ESP_LOGI(TAG, "BUILD: %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "IDF: %s", app ? app->idf_ver : "unknown");
  ESP_LOGI(TAG, "CORE PLAN: Core0=WiFi/network/PTP + AAC TCP/decode/EQ + ALAC UDP/decrypt/decode");
  ESP_LOGI(TAG, "CORE PLAN: Core1=high-priority PTP/RTP playout + ALAC ordered staging/EQ");
  ESP_LOGI(TAG, "============================================================");
}

void app_main(void) {
  print_firmware_banner();
  log_memory_state("boot");

  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    e = nvs_flash_init();
  }
  ESP_ERROR_CHECK(e);
  ESP_ERROR_CHECK(settings_init());
  ESP_ERROR_CHECK(audio_eq_init());
  ESP_ERROR_CHECK(spiffs_storage_init());
  ESP_ERROR_CHECK(log_stream_init());

  wifi_init_apsta(NULL, NULL);
  ESP_ERROR_CHECK(web_server_start(80));

  if (!settings_has_wifi_credentials() || !wifi_wait_connected(30000)) {
    ESP_LOGW(TAG,
             "STA not connected; captive WiFi setup remains available at 192.168.4.1");
    dns_server_start(AP_IP_ADDR);
  } else {
    ESP_LOGI(TAG, "WiFi connected");
  }
  log_memory_state("post-wifi");

  ESP_ERROR_CHECK(ptp_clock_init());
  ESP_ERROR_CHECK(hap_init());
  ESP_ERROR_CHECK(audio_receiver_init());
  log_memory_state("post-audio-init");
  led_init();
  mdns_airplay_init();
  ESP_ERROR_CHECK(rtsp_server_start());

  char ip[32] = {0};
  if (wifi_get_ip_str(ip, sizeof(ip)) == ESP_OK) {
    ESP_LOGI(TAG, "%s ready; Web UI / WiFi scan / logs / OTA: http://%s/",
             FW_NAME, ip);
  } else {
    ESP_LOGI(TAG, "%s ready; setup UI: http://192.168.4.1/", FW_NAME);
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
