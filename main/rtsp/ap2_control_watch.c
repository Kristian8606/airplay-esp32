#include "ap2_control_watch.h"

#include "sdkconfig.h"

#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ap2_ctrl";

#define WATCH_STACK_SIZE 4096U
#define WATCH_PRIORITY 3
#define WATCH_SELECT_MS 200
#define WATCH_PACKET_MAX 1536U
#define WATCH_DETAIL_PACKETS 24U
#define WATCH_SUMMARY_US (10LL * 1000 * 1000)

/* The watcher holds s_mutex while it uses s_sock (at most one select period),
 * so unwatch() returns only when the socket is no longer touched. */
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static int s_sock = -1;
static unsigned s_port;
static uint32_t s_packets;
static uint32_t s_bytes;
static uint32_t s_summary_packets;
static int64_t s_last_summary_us;
static uint16_t s_type_count[256];

static void log_summary(const char *why) {
  char types[160];
  size_t pos = 0;
  types[0] = '\0';
  for (unsigned t = 0; t < 256 && pos + 16 < sizeof(types); ++t) {
    if (s_type_count[t]) {
      pos += (size_t)snprintf(types + pos, sizeof(types) - pos, "%s0x%02x:%u",
                              pos ? " " : "", t, (unsigned)s_type_count[t]);
    }
  }
  ESP_LOGI(TAG, "TRACE ctrl port %u %s: %u packets, %u bytes, byte1 types [%s]",
           s_port, why, (unsigned)s_packets, (unsigned)s_bytes, types);
}

static void log_packet(const uint8_t *p, size_t n, const struct sockaddr_in *from) {
  char hex[3 * 32 + 1];
  size_t shown = n < 32 ? n : 32;
  for (size_t i = 0; i < shown; ++i) {
    snprintf(hex + 3 * i, 4, "%02x ", p[i]);
  }
  hex[3 * shown] = '\0';
  const unsigned version = n ? (unsigned)(p[0] >> 6) : 0U;
  const unsigned b1 = n > 1 ? p[1] : 0U;
  char addr[16];
  inet_ntoa_r(from->sin_addr, addr, sizeof(addr));
  ESP_LOGI(TAG,
           "TRACE ctrl #%u from %s:%u len=%u v=%u byte1=0x%02x (rtcp pt=%u / "
           "rtp pt=%u) %s",
           (unsigned)s_packets, addr, (unsigned)ntohs(from->sin_port),
           (unsigned)n, version, b1, b1, b1 & 0x7FU, hex);
}

static void watch_task(void *arg) {
  (void)arg;
  uint8_t *buf = heap_caps_malloc(WATCH_PACKET_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  for (;;) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const int sock = s_sock;
    if (sock < 0 || !buf) {
      xSemaphoreGive(s_mutex);
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = WATCH_SELECT_MS * 1000};
    const int ret = select(sock + 1, &fds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(sock, &fds)) {
      struct sockaddr_in from = {0};
      socklen_t from_len = sizeof(from);
      const ssize_t n = recvfrom(sock, buf, WATCH_PACKET_MAX, MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &from_len);
      if (n > 0) {
        s_packets++;
        s_summary_packets++;
        s_bytes += (uint32_t)n;
        if (n > 1 && s_type_count[buf[1]] < UINT16_MAX) s_type_count[buf[1]]++;
        if (s_packets <= WATCH_DETAIL_PACKETS) log_packet(buf, (size_t)n, &from);
      }
    }
    const int64_t now = esp_timer_get_time();
    if (s_summary_packets && now - s_last_summary_us >= WATCH_SUMMARY_US) {
      log_summary("so far");
      s_summary_packets = 0;
      s_last_summary_us = now;
    }
    xSemaphoreGive(s_mutex);
    if (ret < 0 && errno != EINTR) vTaskDelay(pdMS_TO_TICKS(WATCH_SELECT_MS));
  }
}

void ap2_control_watch(int sock, unsigned port) {
  if (sock < 0) return;
  if (!s_mutex) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return;
  }
  if (!s_task &&
      xTaskCreatePinnedToCoreWithCaps(watch_task, "ap2_ctrl_watch",
                                      WATCH_STACK_SIZE, NULL, WATCH_PRIORITY,
                                      &s_task, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    s_task = NULL;
    ESP_LOGW(TAG, "control port trace task not started");
    return;
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_sock = sock;
  s_port = port;
  s_packets = 0;
  s_bytes = 0;
  s_summary_packets = 0;
  s_last_summary_us = esp_timer_get_time();
  memset(s_type_count, 0, sizeof(s_type_count));
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "TRACE ctrl port %u: watching", port);
  xTaskNotifyGive(s_task);
}

void ap2_control_unwatch(int sock) {
  if (!s_mutex || sock < 0) return;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_sock == sock) {
    log_summary("closed");
    s_sock = -1;
  }
  xSemaphoreGive(s_mutex);
}

#else

void ap2_control_watch(int sock, unsigned port) {
  (void)sock;
  (void)port;
}

void ap2_control_unwatch(int sock) { (void)sock; }

#endif
