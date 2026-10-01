#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APAP_FRAME_QUEUE_CAPACITY 64U
#define APAP_FRAME_QUEUE_MAX_AAC  4096U
#define APAP_FRAME_QUEUE_BYTES \
  (APAP_FRAME_QUEUE_CAPACITY * APAP_FRAME_QUEUE_MAX_AAC)

typedef struct apap_frame_queue apap_frame_queue_t;

typedef struct {
  uint32_t seq;          /* APAP 24-bit sequence, stored in uint32_t */
  uint64_t media_sample; /* normalized 44.1-kHz sample-domain address */
  int64_t media_time;    /* original APAP mediaTimeValue, diagnostics */
  uint32_t timescale;    /* original APAP mediaTimeScale */
  uint16_t aac_len;
} apap_frame_meta_t;

typedef struct {
  uint32_t frames;
  uint32_t bytes;
  uint32_t high_water_frames;
  uint64_t pushed;
  uint64_t popped;
  uint64_t dropped_on_clear;
} apap_frame_queue_stats_t;

esp_err_t apap_frame_queue_create(apap_frame_queue_t **out);
void apap_frame_queue_destroy(apap_frame_queue_t *q);

/* SPSC fast path. push/pop are additionally protected against an asynchronous
 * control-thread clear, because fshb can arrive on MediaDataControl while the
 * APAP TCP and decoder tasks are active. */
bool apap_frame_queue_push(apap_frame_queue_t *q,
                           const apap_frame_meta_t *meta,
                           const uint8_t *aac, size_t aac_len);
bool apap_frame_queue_pop(apap_frame_queue_t *q,
                          apap_frame_meta_t *meta,
                          uint8_t *aac, size_t aac_capacity);

void apap_frame_queue_clear(apap_frame_queue_t *q);
void apap_frame_queue_get_stats(apap_frame_queue_t *q,
                                apap_frame_queue_stats_t *out);

/* Optional task-notification hints used only to avoid polling while the queue
 * is empty/full. Correctness never depends on the notification. */
void apap_frame_queue_set_producer(apap_frame_queue_t *q, void *task_handle);
void apap_frame_queue_set_consumer(apap_frame_queue_t *q, void *task_handle);
void apap_frame_queue_wake_all(apap_frame_queue_t *q);
