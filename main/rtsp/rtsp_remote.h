#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 0..5 are MediaRemote IDs; 6/7 are local action selectors for dvlc,
 * never transmitted as modernMediaRemoteCommand numbers. */
#define RTSP_REMOTE_VOLUME_DOWN 6U
#define RTSP_REMOTE_VOLUME_UP 7U

typedef enum {
  RTSP_REMOTE_IDLE, RTSP_REMOTE_QUEUED, RTSP_REMOTE_SENT,
  RTSP_REMOTE_ACKNOWLEDGED, RTSP_REMOTE_REJECTED,
  RTSP_REMOTE_TIMEOUT, RTSP_REMOTE_FAILED, RTSP_REMOTE_DISCONNECTED
} rtsp_remote_result_t;

typedef struct {
  bool connected;
  uint32_t id;
  uint8_t command;
  rtsp_remote_result_t result;
  int response_code;
} rtsp_remote_status_t;

/* Only enqueue here. The existing event task owns all socket/crypto work.
 * ESP_ERR_NOT_FOUND: no encrypted event connection; INVALID_STATE: busy. */
esp_err_t rtsp_remote_enqueue(uint8_t command, uint32_t *id);
void rtsp_remote_get_status(rtsp_remote_status_t *status);
const char *rtsp_remote_result_name(rtsp_remote_result_t result);
