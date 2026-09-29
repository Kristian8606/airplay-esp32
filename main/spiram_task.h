/**
 * Task-creation wrappers used by the network tasks.
 *
 * Task stacks stay in internal RAM: SPI flash operations disable the cache,
 * making SPIRAM inaccessible, and a stack there trips the
 * esp_task_stack_is_sane_cache_disabled() assert. The mem argument is kept
 * for call-site compatibility; it is always cleared and task_free_spiram()
 * is a no-op.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
  void *stack;
  void *tcb;
} spiram_task_mem_t;

static inline BaseType_t
task_create_pinned_spiram(TaskFunction_t fn, const char *name, uint32_t depth,
                          void *param, UBaseType_t prio, TaskHandle_t *handle,
                          BaseType_t core, spiram_task_mem_t *mem) {
  if (mem) {
    mem->stack = NULL;
    mem->tcb = NULL;
  }
  return xTaskCreatePinnedToCore(fn, name, depth, param, prio, handle, core);
}

static inline void task_free_spiram(spiram_task_mem_t *mem) {
  (void)mem;
}
