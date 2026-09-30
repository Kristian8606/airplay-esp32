#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct ap2_buffered_fifo ap2_buffered_fifo_t;

typedef struct {
  size_t buffer_bytes;
  int task_core;
  int task_priority;
  uint32_t task_stack;
} ap2_buffered_fifo_config_t;

typedef struct {
  size_t capacity_bytes;
  size_t used_bytes;
  uint64_t bytes_received;
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

/* Hard session-boundary reset. Call only while the buffered transport is
 * stopped/idle; live FLUSHBUFFERED never rewinds or purges this byte FIFO. */
void ap2_buffered_fifo_clear(ap2_buffered_fifo_t *fifo);

/* Abort only the current buffered TCP client after fatal framing corruption.
 * The listening socket remains available for the RTSP session. */
void ap2_buffered_fifo_abort_client(ap2_buffered_fifo_t *fifo);

size_t ap2_buffered_fifo_capacity(const ap2_buffered_fifo_t *fifo);
void ap2_buffered_fifo_get_usage(ap2_buffered_fifo_t *fifo,
                                 ap2_buffered_fifo_usage_t *out);

/* TCP reader diagnostics (read-only; used by the audio_status line).
 * `state` says where the reader task is right now. When it is RECV and no
 * bytes arrive, lwIP has nothing queued for us: the sender is not sending. */
typedef enum {
  AP2_FIFO_RX_ACCEPT = 0, /* no client: waiting in accept() */
  AP2_FIFO_RX_RECV,       /* blocked in recv(): waiting for sender bytes */
  AP2_FIFO_RX_WAIT_SPACE, /* FIFO full: waiting for the decoder */
  AP2_FIFO_RX_PACE,       /* Shairport-style 10 ms pacing sleep */
} ap2_fifo_rx_state_t;

typedef struct {
  bool connected;
  ap2_fifo_rx_state_t state;
  uint32_t state_ms;      /* time in the current state */
  uint32_t idle_ms;       /* time since the last received byte */
  int socket_unread;      /* bytes queued in the socket (FIONREAD), -1 unknown */
  uint32_t recv_timeouts; /* recv() returning EAGAIN on this connection */
} ap2_buffered_fifo_rx_diag_t;

void ap2_buffered_fifo_get_rx_diag(ap2_buffered_fifo_t *fifo,
                                   ap2_buffered_fifo_rx_diag_t *out);
const char *ap2_buffered_fifo_rx_state_name(ap2_fifo_rx_state_t state);

/* Wake the single packet consumer after a control-plane change. */
void ap2_buffered_fifo_notify(ap2_buffered_fifo_t *fifo);
void ap2_buffered_fifo_wait(ap2_buffered_fifo_t *fifo, uint32_t timeout_ms);

/* Sequential block access for the single buffered processor
 * (Shairport buffered_read.c / read_sized_block boundary).
 *
 * The wire format is [2-byte big-endian length][block bytes]. This layer only
 * frames bytes: it never parses RTP, sequence numbers, SSRC or FLUSH state.
 *
 * read_block_head() consumes the length prefix and the first `head_len` bytes
 * of the next block (the RTP header) and returns the full block length.
 * The caller then MUST finish that same block with read_block_rest(), either
 * copying the remaining block_len - head_len bytes (dst != NULL) or consuming
 * them without a copy (dst == NULL) after it decided to drop the block.
 * stream_epoch identifies the byte stream that supplied it (a new accepted
 * connection or any discard/abort/stop starts a new epoch); a different epoch
 * at read_block_rest() time fails and the block is lost. */
esp_err_t ap2_buffered_fifo_read_block_head(ap2_buffered_fifo_t *fifo,
                                            uint8_t *head, size_t head_len,
                                            size_t block_capacity,
                                            size_t *block_len,
                                            uint32_t *stream_epoch);
esp_err_t ap2_buffered_fifo_read_block_rest(ap2_buffered_fifo_t *fifo,
                                            uint8_t *dst, size_t len,
                                            uint32_t stream_epoch);
