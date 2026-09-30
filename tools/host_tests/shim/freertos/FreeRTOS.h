#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1
int xPortGetCoreID(void);
void vTaskDelay(TickType_t t);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack,
                                   void *arg, int prio, TaskHandle_t *out, int core);
void vTaskDelete(TaskHandle_t t);
/* Test hooks: make the next N internal-stack task creations fail (as when
 * internal RAM is exhausted) and count creations with a PSRAM stack. */
extern int shim_task_create_fail_next;
extern int shim_task_create_with_caps_count;
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name,
                                           uint32_t stack, void *arg, int prio,
                                           TaskHandle_t *out, int core,
                                           unsigned caps);
void vTaskDeleteWithCaps(TaskHandle_t t);
TickType_t xTaskGetTickCount(void);
static inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 0; }

