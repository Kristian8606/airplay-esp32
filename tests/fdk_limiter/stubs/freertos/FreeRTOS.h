#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;typedef void *TaskHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
