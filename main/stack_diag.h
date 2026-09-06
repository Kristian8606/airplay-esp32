#pragma once

#include <stdint.h>

typedef enum {
  STACK_DIAG_AP2_TCP_READER = 0,
  STACK_DIAG_AP2_BUF_PROC,
  STACK_DIAG_AP2_PLAYOUT,
  STACK_DIAG_ALAC_STAGE,
  STACK_DIAG_AP2_STATS,
  STACK_DIAG_ALAC_DATA,
  STACK_DIAG_ALAC_CTRL,
  STACK_DIAG_ALAC_WORK,
  STACK_DIAG_ALAC_RESEND,
  STACK_DIAG_PTP_CLOCK,
  STACK_DIAG_RTSP_SERVER,
  STACK_DIAG_RTSP_CLIENT,
  STACK_DIAG_EVENT_PORT,
  STACK_DIAG_LOG_WS,
  STACK_DIAG_COUNT
} stack_diag_id_t;

/* Sample the calling task's historical minimum free stack. ESP-IDF reports
 * uxTaskGetStackHighWaterMark() in bytes. Values are retained as the minimum
 * seen across task restarts for the whole boot/test run. */
void stack_diag_sample(stack_diag_id_t id);

/* Print the current minima. A value of -1 means that task has not sampled yet. */
void stack_diag_log(void);
