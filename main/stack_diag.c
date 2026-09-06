#include "stack_diag.h"

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "stack_diag";

/* 0xffffffff means not sampled yet. The table is tiny (14 x 4 bytes) and the
 * names/sizes below live in flash. */
static volatile uint32_t s_min_free[STACK_DIAG_COUNT] = {
    UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
    UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
    UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
};

static inline int32_t sample_or_minus_one(stack_diag_id_t id) {
  const uint32_t v = __atomic_load_n(&s_min_free[id], __ATOMIC_RELAXED);
  return v == UINT32_MAX ? -1 : (int32_t)v;
}

void stack_diag_sample(stack_diag_id_t id) {
  if ((unsigned)id >= (unsigned)STACK_DIAG_COUNT) return;

  const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
  uint32_t old = __atomic_load_n(&s_min_free[id], __ATOMIC_RELAXED);
  while (free_bytes < old &&
         !__atomic_compare_exchange_n(&s_min_free[id], &old, free_bytes, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

void stack_diag_log(void) {
  ESP_LOGI(TAG,
           "STACK minFree/size B audio: tcp=%" PRId32 "/4096 proc=%" PRId32
           "/6144 play=%" PRId32 "/4096 stage=%" PRId32 "/4096 stats=%" PRId32
           "/8192",
           sample_or_minus_one(STACK_DIAG_AP2_TCP_READER),
           sample_or_minus_one(STACK_DIAG_AP2_BUF_PROC),
           sample_or_minus_one(STACK_DIAG_AP2_PLAYOUT),
           sample_or_minus_one(STACK_DIAG_ALAC_STAGE),
           sample_or_minus_one(STACK_DIAG_AP2_STATS));

  ESP_LOGI(TAG,
           "STACK minFree/size B realtime: data=%" PRId32 "/4096 ctrl=%" PRId32
           "/4096 work=%" PRId32 "/6144 resend=%" PRId32 "/4096",
           sample_or_minus_one(STACK_DIAG_ALAC_DATA),
           sample_or_minus_one(STACK_DIAG_ALAC_CTRL),
           sample_or_minus_one(STACK_DIAG_ALAC_WORK),
           sample_or_minus_one(STACK_DIAG_ALAC_RESEND));

  ESP_LOGI(TAG,
           "STACK minFree/size B control: ptp=%" PRId32 "/4096 srv=%" PRId32
           "/4096 client=%" PRId32 "/8192 event=%" PRId32 "/3072 logws=%" PRId32
           "/4096",
           sample_or_minus_one(STACK_DIAG_PTP_CLOCK),
           sample_or_minus_one(STACK_DIAG_RTSP_SERVER),
           sample_or_minus_one(STACK_DIAG_RTSP_CLIENT),
           sample_or_minus_one(STACK_DIAG_EVENT_PORT),
           sample_or_minus_one(STACK_DIAG_LOG_WS));
}
