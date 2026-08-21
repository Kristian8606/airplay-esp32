#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

typedef struct ap2_buffered_transport ap2_buffered_transport_t;

typedef struct {
  size_t fifo_bytes;
  int task_core;
  int task_priority;
  uint32_t task_stack;
} ap2_buffered_transport_config_t;

typedef struct {
  uint64_t socket_bytes;
  uint64_t fifo_bytes_read;
  uint64_t fifo_full_waits;
  uint64_t fifo_empty_waits;
  size_t fifo_occupancy;
  size_t fifo_high_water;
} ap2_buffered_transport_stats_t;

esp_err_t ap2_buffered_transport_create(ap2_buffered_transport_t **out,
                                        const ap2_buffered_transport_config_t *cfg);
void ap2_buffered_transport_destroy(ap2_buffered_transport_t *t);
esp_err_t ap2_buffered_transport_start(ap2_buffered_transport_t *t,
                                       uint16_t requested_port,
                                       uint16_t *bound_port);
void ap2_buffered_transport_stop(ap2_buffered_transport_t *t);
void ap2_buffered_transport_clear(ap2_buffered_transport_t *t);
ssize_t ap2_buffered_transport_read_exact(ap2_buffered_transport_t *t,
                                          void *dst, size_t bytes);
void ap2_buffered_transport_get_stats(ap2_buffered_transport_t *t,
                                      ap2_buffered_transport_stats_t *out);
size_t ap2_buffered_transport_capacity(ap2_buffered_transport_t *t);
