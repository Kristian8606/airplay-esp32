#include "apap_frame_queue.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* One fixed-size payload slot per APAP AAC access unit. 64 x 4096 B is only
 * 256 KiB, yet it decouples roughly 1.49 s of 1024-frame/44.1-kHz audio. The
 * queue stores semantic frames, never an anonymous TCP byte stream. */
typedef struct {
  apap_frame_meta_t meta;
  uint8_t aac[APAP_FRAME_QUEUE_MAX_AAC];
} apap_frame_slot_t;

struct apap_frame_queue {
  apap_frame_slot_t *slots;
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  uint32_t bytes;
  uint32_t high_water_frames;
  uint64_t pushed;
  uint64_t popped;
  uint64_t dropped_on_clear;
  SemaphoreHandle_t mutex;
  TaskHandle_t producer;
  TaskHandle_t consumer;
};

esp_err_t apap_frame_queue_create(apap_frame_queue_t **out) {
  if (!out || *out) return ESP_ERR_INVALID_ARG;
  apap_frame_queue_t *q = calloc(1, sizeof(*q));
  if (!q) return ESP_ERR_NO_MEM;
  q->mutex = xSemaphoreCreateMutex();
  if (!q->mutex) {
    free(q);
    return ESP_ERR_NO_MEM;
  }
  q->slots = heap_caps_calloc(APAP_FRAME_QUEUE_CAPACITY,
                              sizeof(apap_frame_slot_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!q->slots) q->slots = calloc(APAP_FRAME_QUEUE_CAPACITY,
                                    sizeof(apap_frame_slot_t));
  if (!q->slots) {
    vSemaphoreDelete(q->mutex);
    free(q);
    return ESP_ERR_NO_MEM;
  }
  *out = q;
  return ESP_OK;
}

void apap_frame_queue_destroy(apap_frame_queue_t *q) {
  if (!q) return;
  free(q->slots);
  vSemaphoreDelete(q->mutex);
  free(q);
}

bool apap_frame_queue_push(apap_frame_queue_t *q,
                           const apap_frame_meta_t *meta,
                           const uint8_t *aac, size_t aac_len) {
  if (!q || !meta || aac_len > APAP_FRAME_QUEUE_MAX_AAC ||
      (aac_len > 0 && !aac)) return false;

  TaskHandle_t consumer = NULL;
  bool ok = false;
  xSemaphoreTake(q->mutex, portMAX_DELAY);
  if (q->count < APAP_FRAME_QUEUE_CAPACITY) {
    apap_frame_slot_t *slot = &q->slots[q->head];
    slot->meta = *meta;
    slot->meta.aac_len = (uint16_t)aac_len;
    if (aac_len > 0) memcpy(slot->aac, aac, aac_len);
    q->head = (q->head + 1U) % APAP_FRAME_QUEUE_CAPACITY;
    ++q->count;
    q->bytes += (uint32_t)aac_len;
    if (q->count > q->high_water_frames) q->high_water_frames = q->count;
    ++q->pushed;
    consumer = q->consumer;
    ok = true;
  }
  xSemaphoreGive(q->mutex);
  if (ok && consumer) xTaskNotifyGive(consumer);
  return ok;
}

bool apap_frame_queue_pop(apap_frame_queue_t *q,
                          apap_frame_meta_t *meta,
                          uint8_t *aac, size_t aac_capacity) {
  if (!q || !meta || !aac) return false;

  TaskHandle_t producer = NULL;
  bool ok = false;
  xSemaphoreTake(q->mutex, portMAX_DELAY);
  if (q->count > 0U) {
    apap_frame_slot_t *slot = &q->slots[q->tail];
    if ((size_t)slot->meta.aac_len <= aac_capacity) {
      *meta = slot->meta;
      if (slot->meta.aac_len > 0)
        memcpy(aac, slot->aac, slot->meta.aac_len);
      q->tail = (q->tail + 1U) % APAP_FRAME_QUEUE_CAPACITY;
      --q->count;
      q->bytes -= slot->meta.aac_len;
      ++q->popped;
      producer = q->producer;
      ok = true;
    }
  }
  xSemaphoreGive(q->mutex);
  if (ok && producer) xTaskNotifyGive(producer);
  return ok;
}

void apap_frame_queue_clear(apap_frame_queue_t *q) {
  if (!q) return;
  TaskHandle_t producer = NULL;
  TaskHandle_t consumer = NULL;
  xSemaphoreTake(q->mutex, portMAX_DELAY);
  q->dropped_on_clear += q->count;
  q->head = 0;
  q->tail = 0;
  q->count = 0;
  q->bytes = 0;
  producer = q->producer;
  consumer = q->consumer;
  xSemaphoreGive(q->mutex);
  if (producer) xTaskNotifyGive(producer);
  if (consumer) xTaskNotifyGive(consumer);
}

void apap_frame_queue_get_stats(apap_frame_queue_t *q,
                                apap_frame_queue_stats_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!q) return;
  xSemaphoreTake(q->mutex, portMAX_DELAY);
  out->frames = q->count;
  out->bytes = q->bytes;
  out->high_water_frames = q->high_water_frames;
  out->pushed = q->pushed;
  out->popped = q->popped;
  out->dropped_on_clear = q->dropped_on_clear;
  xSemaphoreGive(q->mutex);
}

void apap_frame_queue_set_producer(apap_frame_queue_t *q, void *task_handle) {
  if (!q) return;
  xSemaphoreTake(q->mutex, portMAX_DELAY);
  q->producer = (TaskHandle_t)task_handle;
  xSemaphoreGive(q->mutex);
}

void apap_frame_queue_set_consumer(apap_frame_queue_t *q, void *task_handle) {
  if (!q) return;
  xSemaphoreTake(q->mutex, portMAX_DELAY);
  q->consumer = (TaskHandle_t)task_handle;
  xSemaphoreGive(q->mutex);
}

void apap_frame_queue_wake_all(apap_frame_queue_t *q) {
  if (!q) return;
  TaskHandle_t producer = NULL;
  TaskHandle_t consumer = NULL;
  xSemaphoreTake(q->mutex, portMAX_DELAY);
  producer = q->producer;
  consumer = q->consumer;
  xSemaphoreGive(q->mutex);
  if (producer) xTaskNotifyGive(producer);
  if (consumer) xTaskNotifyGive(consumer);
}
