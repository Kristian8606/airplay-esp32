#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "audio_receiver.h"

#define AAC_FRAMES_PER_PACKET  352
#define AUDIO_MAX_CHANNELS     2
#define AUDIO_BYTES_PER_SAMPLE 2
#define MAX_SAMPLES_PER_FRAME  4096

typedef struct __attribute__((packed)) {
  uint32_t rtp_timestamp;
  uint16_t samples_per_channel;
  uint8_t channels;
  uint8_t reserved;
  uint32_t generation;
  uint32_t reserved2;
} audio_frame_header_t;

_Static_assert(sizeof(audio_frame_header_t) % 8 == 0,
               "audio frame header must remain 8-byte aligned");

#define MAX_RING_BUFFER_FRAMES 1000
#define BYTES_PER_FRAME                                          \
  ((size_t)sizeof(audio_frame_header_t) +                        \
   ((size_t)AAC_FRAMES_PER_PACKET * (size_t)AUDIO_MAX_CHANNELS * \
    (size_t)AUDIO_BYTES_PER_SAMPLE))
#define AUDIO_BUFFER_SIZE (MAX_RING_BUFFER_FRAMES * BYTES_PER_FRAME)

_Static_assert(BYTES_PER_FRAME % 8 == 0,
               "audio ring slot must remain 8-byte aligned");

typedef struct {
  uint8_t *pool;                // Pre-allocated frame data in PSRAM
  uint16_t *sorted;             // Slot indices sorted by RTP timestamp
  uint16_t *free_stack;         // Stack of free slot indices
  int count;                    // Frames currently in buffer
  int free_top;                 // Top of free stack (next free slot)
  int capacity;                 // Max frames
  size_t slot_size;             // BYTES_PER_FRAME
  portMUX_TYPE lock;            // Spinlock for count/index manipulation
  SemaphoreHandle_t data_ready; // Counting semaphore (blocks consumer)
  uint8_t *frame_buffer;        // Temp assembly buffer
  int16_t *decode_buffer;       // Decode buffer pointer
  size_t decode_capacity_samples;
} audio_buffer_t;

esp_err_t audio_buffer_init(audio_buffer_t *buffer);
void audio_buffer_deinit(audio_buffer_t *buffer);
void audio_buffer_flush(audio_buffer_t *buffer);
int audio_buffer_get_frame_count(audio_buffer_t *buffer);
bool audio_buffer_is_nearly_full(audio_buffer_t *buffer);
/* Newest (highest-RTP) frame currently queued.  Diagnostics only: used to
 * report how far behind the head of the buffer playout is running. */
bool audio_buffer_peek_newest_rtp(audio_buffer_t *buffer, uint32_t *rtp_out);
/* Find the first frame of a contiguous same-generation run containing at
 * least required_samples. The metadata scan is bounded and performed outside
 * the critical section. */
bool audio_buffer_find_contiguous_start(audio_buffer_t *buffer,
                                        uint32_t generation, uint32_t min_rtp,
                                        uint32_t required_samples,
                                        uint32_t *rtp_out);
/* Fallback: first queued frame at/after min_rtp in the requested generation. */
bool audio_buffer_find_first_generation_frame(audio_buffer_t *buffer,
                                              uint32_t generation,
                                              uint32_t min_rtp,
                                              uint32_t *rtp_out);
bool audio_buffer_take(audio_buffer_t *buffer, void **item, size_t *item_size,
                       TickType_t ticks);
void audio_buffer_return(audio_buffer_t *buffer, void *item);
int16_t *audio_buffer_get_decode_buffer(audio_buffer_t *buffer,
                                        size_t *capacity_samples);
bool audio_buffer_queue_decoded(audio_buffer_t *buffer, audio_stats_t *stats,
                                uint32_t timestamp, uint32_t generation,
                                const int16_t *pcm_data, size_t samples,
                                int channels);
/**
 * Peek at the RTP timestamp of the oldest (lowest-timestamp) frame in the
 * buffer without removing it.  Returns false if the buffer is empty.
 */
bool audio_buffer_oldest_timestamp(audio_buffer_t *buffer, uint32_t *timestamp);
