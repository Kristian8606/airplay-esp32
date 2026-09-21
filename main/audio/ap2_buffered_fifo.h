#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AP2_BUFFERED_FIFO_MAX_DEFERRED 16U
#define AP2_BUFFERED_FIFO_MAX_ACTIVATIONS AP2_BUFFERED_FIFO_MAX_DEFERRED

typedef struct ap2_buffered_fifo ap2_buffered_fifo_t;

typedef struct {
  size_t buffer_bytes;
  int task_core;
  int task_priority;
  uint32_t task_stack;
} ap2_buffered_fifo_config_t;

typedef struct {
  uint32_t seq;
  uint32_t rtp;
  size_t len;
  uint32_t stream_epoch;
} ap2_buffered_packet_t;

typedef struct {
  uint32_t from_rtp;
  uint32_t until_rtp;
} ap2_buffered_flush_activation_t;

typedef struct {
  bool drop;
  bool discontinuity;
  bool immediate_completed;
  bool immediate_overshoot;
  uint32_t immediate_target_seq;
  uint8_t activation_count;
  ap2_buffered_flush_activation_t activations[AP2_BUFFERED_FIFO_MAX_ACTIVATIONS];
} ap2_buffered_packet_decision_t;

typedef struct {
  size_t capacity_bytes;
  size_t used_bytes;
  bool immediate_flush_active;
  uint32_t immediate_target_seq;
  uint32_t deferred_requests;
} ap2_buffered_fifo_usage_t;

esp_err_t ap2_buffered_fifo_create_with_storage(
    ap2_buffered_fifo_t **out, const ap2_buffered_fifo_config_t *cfg,
    void *storage, size_t storage_bytes);
esp_err_t ap2_buffered_fifo_destroy(ap2_buffered_fifo_t *fifo);

esp_err_t ap2_buffered_fifo_start(ap2_buffered_fifo_t *fifo,
                                  uint16_t requested_port,
                                  uint16_t *bound_port);
void ap2_buffered_fifo_stop(ap2_buffered_fifo_t *fifo);
bool ap2_buffered_fifo_is_idle(ap2_buffered_fifo_t *fifo);
void ap2_buffered_fifo_clear(ap2_buffered_fifo_t *fifo);
/* Abort only the current buffered TCP client after a fatal framing/session
 * error. The listener stays up so AirPlay can reconnect. This clears queued
 * bytes, advances the stream epoch and wakes all waiters. */
void ap2_buffered_fifo_abort_client(ap2_buffered_fifo_t *fifo);

size_t ap2_buffered_fifo_capacity(const ap2_buffered_fifo_t *fifo);
void ap2_buffered_fifo_get_usage(ap2_buffered_fifo_t *fifo,
                                 ap2_buffered_fifo_usage_t *out);

void ap2_buffered_fifo_notify(ap2_buffered_fifo_t *fifo);
void ap2_buffered_fifo_wait(ap2_buffered_fifo_t *fifo, uint32_t timeout_ms);

/* Sequential Shairport-style packet reader. It consumes exactly one framed
 * block from the raw TCP byte FIFO: 2-byte big-endian length followed by the
 * packet body. No SEQ/RTP lookup exists in this layer. */
esp_err_t ap2_buffered_fifo_read_packet(ap2_buffered_fifo_t *fifo,
                                        uint8_t *packet_storage,
                                        size_t packet_capacity,
                                        ap2_buffered_packet_t *packet);

/* Apply the current FLUSHBUFFERED control state to the packet currently held
 * by the sequential consumer. This may be called repeatedly while that packet
 * is being held for its RTP presentation time; newly-arrived control commands
 * are therefore observed without searching or rebinding the FIFO. */
void ap2_buffered_fifo_classify_packet(
    ap2_buffered_fifo_t *fifo, const ap2_buffered_packet_t *packet,
    ap2_buffered_packet_decision_t *decision);

esp_err_t ap2_buffered_fifo_add_deferred_flush(
    ap2_buffered_fifo_t *fifo, uint32_t from_seq, uint32_t from_rtp,
    uint32_t until_seq, uint32_t until_rtp);
void ap2_buffered_fifo_set_immediate_flush(ap2_buffered_fifo_t *fifo,
                                           uint32_t until_seq,
                                           uint32_t until_rtp,
                                           bool has_endpoint);
bool ap2_buffered_fifo_immediate_flush_active(ap2_buffered_fifo_t *fifo);

