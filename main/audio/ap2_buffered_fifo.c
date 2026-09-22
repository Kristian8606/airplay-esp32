#include "ap2_buffered_fifo.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio_diag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network/socket_utils.h"

#define FIFO_RECV_CHUNK 4096U
#define FIFO_MIN_WIRE_LEN 14U
#define FIFO_SEQ_MASK 0x007fffffU
/* Fast discard works under fifo_mutex; release it this often so the TCP
 * reader (other core) is never starved while megabytes are skipped. */
#define FIFO_FAST_SKIP_BATCH 256U
/* v4.1.19: a deferred FLUSH whose fromSeq the consumer has ALREADY passed is
 * still honoured if it passed it by at most this many packets (~1.5 s of AAC,
 * i.e. what can still sit decoded-but-unplayed in the ~1.1 s PCM lead). The
 * old exact-match rule ignored such a late request completely: the old tail
 * kept playing and the sender's replacement audio (AutoMix crossfade, whose
 * RTP restarts at fromTS) then arrived "behind" the playhead. */
#define FIFO_LATE_DEFER_MAX_PKTS 64

static const char *TAG = "aac_fifo";

typedef struct {
  bool in_use;
  bool active;
  uint32_t from_seq;
  uint32_t from_rtp;
  uint32_t until_seq;
  uint32_t until_rtp;
} deferred_flush_t;

struct ap2_buffered_fifo {
  uint8_t *buffer;
  size_t capacity;
  size_t read_pos;
  size_t write_pos;
  size_t occupancy;
  /* Monotonic byte counters of the current TCP connection (fifo_mutex). */
  uint64_t total_written;
  uint64_t total_read;
  /* Packet-aligned full-flush marker (fifo_mutex): every packet whose length
   * prefix starts before this stream offset is stale and is skipped whole.
   * 0 = no full flush pending. */
  uint64_t discard_before;
  uint32_t fast_skipped;  /* packets skipped for the current flush (fifo_mutex) */
  /* Set when fast skip dropped packets; the next classified packet reports a
   * discontinuity so decoder/EQ history is rebuilt (fifo_mutex). */
  bool skip_discontinuity;

  SemaphoreHandle_t fifo_mutex;
  SemaphoreHandle_t not_empty;
  SemaphoreHandle_t not_full;
  SemaphoreHandle_t control_mutex;
  SemaphoreHandle_t control_wake;

  bool immediate_active;
  uint32_t immediate_until_seq;
  uint32_t immediate_until_rtp;
  deferred_flush_t deferred[AP2_BUFFERED_FIFO_MAX_DEFERRED];

  bool connected;

  int listen_sock;
  int client_sock;
  uint16_t port;
  volatile bool running;
  TaskHandle_t reader_task;
  volatile uint32_t stream_epoch;

  int task_core;
  int task_priority;
  uint32_t task_stack;
};

static inline uint32_t be32_local(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int32_t seq23_delta(uint32_t a, uint32_t b) {
  uint32_t d = (a - b) & FIFO_SEQ_MASK;
  if (d & 0x00400000U) d |= 0xff800000U;
  return (int32_t)d;
}

static uint32_t next_epoch(ap2_buffered_fifo_t *fifo) {
  uint32_t v = __atomic_add_fetch(&fifo->stream_epoch, 1U, __ATOMIC_ACQ_REL);
  if (v == 0U) {
    __atomic_store_n(&fifo->stream_epoch, 1U, __ATOMIC_RELEASE);
    v = 1U;
  }
  return v;
}

static void signal_all(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  if (fifo->not_empty) xSemaphoreGive(fifo->not_empty);
  if (fifo->not_full) xSemaphoreGive(fifo->not_full);
  if (fifo->control_wake) xSemaphoreGive(fifo->control_wake);
}

void ap2_buffered_fifo_notify(ap2_buffered_fifo_t *fifo) { signal_all(fifo); }

void ap2_buffered_fifo_wait(ap2_buffered_fifo_t *fifo, uint32_t timeout_ms) {
  if (!fifo || !fifo->control_wake) return;
  TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (ticks == 0) ticks = 1;
  (void)xSemaphoreTake(fifo->control_wake, ticks);
}

static void fifo_discard_all(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  fifo->discard_before = 0;
  /* Change epoch while the FIFO lock is held. An in-flight recv() captured
   * the old epoch before blocking; when it returns it must not commit those
   * pre-flush bytes into the newly-cleared FIFO. */
  (void)next_epoch(fifo);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);
}

static void control_clear_locked(ap2_buffered_fifo_t *fifo) {
  fifo->immediate_active = false;
  fifo->immediate_until_seq = 0;
  fifo->immediate_until_rtp = 0;
  memset(fifo->deferred, 0, sizeof(fifo->deferred));
}

static inline uint8_t fifo_peek_locked(const ap2_buffered_fifo_t *fifo,
                                       size_t off) {
  size_t p = fifo->read_pos + off;
  if (p >= fifo->capacity) p -= fifo->capacity;
  return fifo->buffer[p];
}

static inline void fifo_advance_locked(ap2_buffered_fifo_t *fifo, size_t n) {
  fifo->read_pos += n;
  if (fifo->read_pos >= fifo->capacity) fifo->read_pos -= fifo->capacity;
  fifo->occupancy -= n;
  fifo->total_read += n;
}

/* FAST FLUSH. Skips whole, completely received packets without copying,
 * decrypting or decoding them, and without going back to the consumer loop
 * for each one. Only length prefixes (and for a seq-bounded flush the 4 seq
 * bytes) are inspected, so a 6 MiB backlog is dropped in a few milliseconds.
 *
 * A packet is skipped when
 *   - it starts before discard_before (full FLUSH without endpoint), or
 *   - an immediate FLUSH to `until_seq` is active, no deferred request exists,
 *     and the packet's seq is still before until_seq.
 * The endpoint packet itself is NEVER skipped here: it goes through
 * classify_packet(), which completes the flush and marks the discontinuity.
 * A partly received packet stops the fast path; the normal path reads it and
 * classify_packet() drops it, so framing is never broken. */
static uint32_t fifo_fast_skip(ap2_buffered_fifo_t *fifo, uint32_t epoch) {
  bool seq_skip = false;
  uint32_t until_seq = 0;
  xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
  if (fifo->immediate_active) {
    bool deferred_pending = false;
    for (uint32_t i = 0; i < AP2_BUFFERED_FIFO_MAX_DEFERRED; ++i)
      if (fifo->deferred[i].in_use) deferred_pending = true;
    /* Deferred ranges need per-packet activation bookkeeping: leave those
     * to the exact slow path. */
    seq_skip = !deferred_pending;
    until_seq = fifo->immediate_until_seq;
  }
  xSemaphoreGive(fifo->control_mutex);

  uint32_t skipped = 0;
  uint32_t skipped_full = 0;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  for (;;) {
    if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) != epoch) break;
    const bool full_pending =
        fifo->discard_before != 0U && fifo->total_read < fifo->discard_before;
    if (!full_pending && !seq_skip) break;
    if (fifo->occupancy < 2U) break;

    const size_t wire_len = ((size_t)fifo_peek_locked(fifo, 0) << 8) |
                            (size_t)fifo_peek_locked(fifo, 1);
    /* Invalid length: let the normal path detect it and restore framing. */
    if (wire_len < FIFO_MIN_WIRE_LEN) break;
    /* Not fully received yet: the slow path handles the straddling packet. */
    if (fifo->occupancy < wire_len) break;

    bool drop = full_pending;
    if (!drop) {
      const uint32_t seq = (((uint32_t)fifo_peek_locked(fifo, 2) << 24) |
                            ((uint32_t)fifo_peek_locked(fifo, 3) << 16) |
                            ((uint32_t)fifo_peek_locked(fifo, 4) << 8) |
                            (uint32_t)fifo_peek_locked(fifo, 5)) &
                           FIFO_SEQ_MASK;
      drop = seq23_delta(seq, until_seq) < 0;
    }
    if (!drop) break;

    fifo_advance_locked(fifo, wire_len);
    ++skipped;
    if (full_pending) ++skipped_full;
    if ((skipped % FIFO_FAST_SKIP_BATCH) == 0U) {
      xSemaphoreGive(fifo->fifo_mutex);
      xSemaphoreGive(fifo->not_full);
      xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    }
  }
  uint32_t report = 0;
  if (skipped) {
    fifo->fast_skipped += skipped_full;
    fifo->skip_discontinuity = true;
  }
  if (fifo->discard_before != 0U && fifo->total_read >= fifo->discard_before) {
    fifo->discard_before = 0;  /* full flush fully drained */
    report = fifo->fast_skipped;
    fifo->fast_skipped = 0;
  }
  xSemaphoreGive(fifo->fifo_mutex);
  if (skipped) xSemaphoreGive(fifo->not_full);
  if (report)
    ESP_LOGI(TAG, "full FLUSH drained: %" PRIu32 " stale packets skipped", report);
  return skipped;
}

/* Full FLUSH on a LIVE stream: mark everything received so far as stale.
 * Raw bytes are not thrown away blindly (that would cut a packet in half);
 * fifo_fast_skip() drops them on packet boundaries instead. */
static void fifo_request_packet_discard(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  if (fifo->connected && fifo->total_written > fifo->total_read &&
      fifo->total_written > fifo->discard_before) {
    fifo->discard_before = fifo->total_written;
  }
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);
}

static bool fifo_read_exact(ap2_buffered_fifo_t *fifo, uint8_t *dst,
                            size_t len, uint32_t expected_epoch) {
  size_t copied = 0;
  while (copied < len && fifo->running) {
    if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) != expected_epoch)
      return false;

    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    if (fifo->occupancy == 0U) {
      const bool connected = fifo->connected;
      xSemaphoreGive(fifo->fifo_mutex);
      if (!connected) return false;
      (void)xSemaphoreTake(fifo->not_empty, pdMS_TO_TICKS(20));
      continue;
    }

    size_t n = len - copied;
    if (n > fifo->occupancy) n = fifo->occupancy;
    const size_t to_end = fifo->capacity - fifo->read_pos;
    if (n > to_end) n = to_end;
    memcpy(dst + copied, fifo->buffer + fifo->read_pos, n);
    fifo->read_pos += n;
    if (fifo->read_pos == fifo->capacity) fifo->read_pos = 0;
    fifo->occupancy -= n;
    fifo->total_read += n;
    xSemaphoreGive(fifo->fifo_mutex);
    copied += n;
    xSemaphoreGive(fifo->not_full);
  }
  return copied == len;
}

static bool wait_for_connected_epoch(ap2_buffered_fifo_t *fifo,
                                     uint32_t *epoch) {
  while (fifo->running) {
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    const bool connected = fifo->connected;
    const size_t occupancy = fifo->occupancy;
    xSemaphoreGive(fifo->fifo_mutex);
    if (connected || occupancy != 0U) {
      *epoch = __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
      return true;
    }
    (void)xSemaphoreTake(fifo->not_empty, pdMS_TO_TICKS(20));
  }
  return false;
}

static void tcp_reader_task(void *arg) {
  ap2_buffered_fifo_t *fifo = (ap2_buffered_fifo_t *)arg;
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_TCP_READER,
                                    xPortGetCoreID(), fifo->task_priority, 0U);

  while (fifo->running) {
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    int c = accept(fifo->listen_sock, (struct sockaddr *)&addr, &alen);
    if (c < 0) {
      if (fifo->running && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR)
        ESP_LOGW(TAG, "accept errno=%d", errno);
      if (fifo->running) vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    fifo->read_pos = 0;
    fifo->write_pos = 0;
    fifo->occupancy = 0;
    fifo->total_written = 0;
    fifo->total_read = 0;
    fifo->discard_before = 0;
    fifo->fast_skipped = 0;
    fifo->skip_discontinuity = false;
    fifo->connected = true;
    fifo->client_sock = c;
    xSemaphoreGive(fifo->fifo_mutex);
    (void)next_epoch(fifo);

    xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
    control_clear_locked(fifo);
    xSemaphoreGive(fifo->control_mutex);

    ESP_LOGI(TAG, "buffered TCP connected fifo=%uKiB",
             (unsigned)(fifo->capacity / 1024U));
    signal_all(fifo);

    while (fifo->running) {
      size_t write_pos = 0;
      size_t request = 0;
      uint32_t write_epoch = 0;

      xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
      if (fifo->occupancy == fifo->capacity) {
        xSemaphoreGive(fifo->fifo_mutex);
        (void)xSemaphoreTake(fifo->not_full, pdMS_TO_TICKS(100));
        continue;
      }

      const size_t free_bytes = fifo->capacity - fifo->occupancy;
      request = free_bytes < FIFO_RECV_CHUNK ? free_bytes : FIFO_RECV_CHUNK;
      const size_t to_end = fifo->capacity - fifo->write_pos;
      if (request > to_end) request = to_end;
      write_pos = fifo->write_pos;
      write_epoch = __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
      xSemaphoreGive(fifo->fifo_mutex);

      const ssize_t n = recv(c, fifo->buffer + write_pos, request, 0);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
          continue;
        break;
      }

      xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
      if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) == write_epoch &&
          fifo->connected) {
        fifo->write_pos += (size_t)n;
        if (fifo->write_pos == fifo->capacity) fifo->write_pos = 0;
        fifo->occupancy += (size_t)n;
        fifo->total_written += (uint64_t)n;
      }
      xSemaphoreGive(fifo->fifo_mutex);
      xSemaphoreGive(fifo->not_empty);
    }

    /* Detach the shared descriptor before close(). Stop/abort may run on a
     * different core; never leave a closed descriptor in client_sock where it
     * could be reused by lwIP and then shutdown() by a stale control path. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    if (fifo->client_sock == c) fifo->client_sock = -1;
    fifo->connected = false;
    fifo->read_pos = fifo->write_pos;
    fifo->occupancy = 0;
    fifo->total_read = fifo->total_written;
    fifo->discard_before = 0;
    xSemaphoreGive(fifo->fifo_mutex);

    (void)shutdown(c, SHUT_RDWR);
    close(c);
    (void)next_epoch(fifo);
    signal_all(fifo);
    ESP_LOGI(TAG, "buffered TCP disconnected");
  }

  __atomic_store_n(&fifo->reader_task, NULL, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

esp_err_t ap2_buffered_fifo_create_with_storage(
    ap2_buffered_fifo_t **out, const ap2_buffered_fifo_config_t *cfg,
    void *storage, size_t storage_bytes) {
  if (!out || !cfg || !storage || cfg->buffer_bytes < 16384U ||
      storage_bytes < cfg->buffer_bytes)
    return ESP_ERR_INVALID_ARG;

  ap2_buffered_fifo_t *fifo = calloc(1, sizeof(*fifo));
  if (!fifo) return ESP_ERR_NO_MEM;
  fifo->buffer = (uint8_t *)storage;
  fifo->capacity = cfg->buffer_bytes;
  fifo->task_core = cfg->task_core;
  fifo->task_priority = cfg->task_priority;
  fifo->task_stack = cfg->task_stack;
  fifo->listen_sock = -1;
  fifo->client_sock = -1;
  fifo->stream_epoch = 1U;
  fifo->fifo_mutex = xSemaphoreCreateMutex();
  fifo->not_empty = xSemaphoreCreateBinary();
  fifo->not_full = xSemaphoreCreateBinary();
  fifo->control_mutex = xSemaphoreCreateMutex();
  fifo->control_wake = xSemaphoreCreateBinary();
  if (!fifo->fifo_mutex || !fifo->not_empty || !fifo->not_full ||
      !fifo->control_mutex || !fifo->control_wake) {
    (void)ap2_buffered_fifo_destroy(fifo);
    return ESP_ERR_NO_MEM;
  }
  *out = fifo;
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_destroy(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return ESP_OK;
  /* create_with_storage() may call destroy after only some synchronization
   * objects were allocated. Do not enter stop() through a NULL FIFO mutex. */
  if (fifo->fifo_mutex) {
    ap2_buffered_fifo_stop(fifo);
  } else {
    fifo->running = false;
  }
  if (__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE)) {
    /* The backing store belongs to audio_receiver. Returning an error is
     * essential: the caller must not free that store while recv() can still
     * write into it. */
    ESP_LOGE(TAG, "destroy refused: TCP reader still active");
    return ESP_ERR_TIMEOUT;
  }
  if (fifo->fifo_mutex) vSemaphoreDelete(fifo->fifo_mutex);
  if (fifo->not_empty) vSemaphoreDelete(fifo->not_empty);
  if (fifo->not_full) vSemaphoreDelete(fifo->not_full);
  if (fifo->control_mutex) vSemaphoreDelete(fifo->control_mutex);
  if (fifo->control_wake) vSemaphoreDelete(fifo->control_wake);
  free(fifo);
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_start(ap2_buffered_fifo_t *fifo,
                                  uint16_t requested_port,
                                  uint16_t *bound_port) {
  if (!fifo) return ESP_ERR_INVALID_ARG;
  if (fifo->running) {
    if (bound_port) *bound_port = fifo->port;
    return ESP_OK;
  }
  if (__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE))
    return ESP_ERR_INVALID_STATE;

  uint16_t bound = requested_port;
  fifo->listen_sock = socket_utils_bind_tcp_listener(requested_port, 1, true, &bound);
  if (fifo->listen_sock < 0) return ESP_FAIL;
  fifo->port = bound;
  fifo->running = true;
  if (xTaskCreatePinnedToCore(tcp_reader_task, "aac_fifo_rx", fifo->task_stack,
                              fifo, fifo->task_priority, &fifo->reader_task,
                              fifo->task_core) != pdPASS) {
    fifo->running = false;
    close(fifo->listen_sock);
    fifo->listen_sock = -1;
    return ESP_FAIL;
  }
  if (bound_port) *bound_port = bound;
  return ESP_OK;
}

void ap2_buffered_fifo_stop(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  fifo->running = false;
  int client = -1;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  client = fifo->client_sock;
  fifo->client_sock = -1;
  fifo->connected = false;
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  fifo->discard_before = 0;
  /* shutdown() while still holding the lock: the reader can only close() this
   * fd after taking the same lock, so the fd cannot be closed and reused by
   * lwIP in between. */
  if (client >= 0) (void)shutdown(client, SHUT_RDWR);
  xSemaphoreGive(fifo->fifo_mutex);
  (void)next_epoch(fifo);
  signal_all(fifo);

  if (fifo->listen_sock >= 0) {
    shutdown(fifo->listen_sock, SHUT_RDWR);
    close(fifo->listen_sock);
    fifo->listen_sock = -1;
  }
  for (int i = 0; __atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) && i < 100;
       ++i)
    vTaskDelay(pdMS_TO_TICKS(10));
  fifo->port = 0;
}

bool ap2_buffered_fifo_is_idle(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return true;
  return !fifo->running &&
         __atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) == NULL;
}

void ap2_buffered_fifo_clear(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  const bool connected = fifo->connected;
  xSemaphoreGive(fifo->fifo_mutex);

  /* On a live stream a raw byte purge would cut a packet in half; mark the
   * backlog stale and let fifo_fast_skip() drop it on packet boundaries. */
  if (!connected) fifo_discard_all(fifo);
  xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
  control_clear_locked(fifo);
  xSemaphoreGive(fifo->control_mutex);
  if (connected) fifo_request_packet_discard(fifo);
  signal_all(fifo);
}

void ap2_buffered_fifo_abort_client(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;

  int client = -1;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  client = fifo->client_sock;
  fifo->client_sock = -1;
  fifo->connected = false;
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  fifo->discard_before = 0;
  (void)next_epoch(fifo);
  /* The reader task owns close(). shutdown() wakes a blocking recv(); it is
   * issued under the lock because the reader closes the fd only after taking
   * this lock, so it can never hit a closed/reused descriptor. */
  if (client >= 0) (void)shutdown(client, SHUT_RDWR);
  xSemaphoreGive(fifo->fifo_mutex);

  xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
  control_clear_locked(fifo);
  xSemaphoreGive(fifo->control_mutex);

  signal_all(fifo);
}

size_t ap2_buffered_fifo_capacity(const ap2_buffered_fifo_t *fifo) {
  return fifo ? fifo->capacity : 0U;
}

void ap2_buffered_fifo_get_usage(ap2_buffered_fifo_t *fifo,
                                 ap2_buffered_fifo_usage_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!fifo) return;

  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  out->capacity_bytes = fifo->capacity;
  out->used_bytes = fifo->occupancy;
  xSemaphoreGive(fifo->fifo_mutex);

  xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
  out->immediate_flush_active = fifo->immediate_active;
  out->immediate_target_seq = fifo->immediate_until_seq;
  for (uint32_t i = 0; i < AP2_BUFFERED_FIFO_MAX_DEFERRED; ++i)
    if (fifo->deferred[i].in_use) out->deferred_requests++;
  xSemaphoreGive(fifo->control_mutex);
}

esp_err_t ap2_buffered_fifo_read_packet(ap2_buffered_fifo_t *fifo,
                                        uint8_t *packet_storage,
                                        size_t packet_capacity,
                                        ap2_buffered_packet_t *packet) {
  if (!fifo || !packet_storage || !packet || packet_capacity < 12U)
    return ESP_ERR_INVALID_ARG;

  for (;;) {
    if (!fifo->running) return ESP_ERR_INVALID_STATE;
    uint32_t epoch = 0;
    if (!wait_for_connected_epoch(fifo, &epoch)) return ESP_ERR_INVALID_STATE;

    /* Drop stale backlog in bulk before touching the next packet. */
    (void)fifo_fast_skip(fifo, epoch);

    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    const uint64_t packet_offset = fifo->total_read;
    xSemaphoreGive(fifo->fifo_mutex);

    uint8_t length_bytes[2];
    if (!fifo_read_exact(fifo, length_bytes, sizeof(length_bytes), epoch))
      continue;
    const uint16_t wire_len = ((uint16_t)length_bytes[0] << 8) | length_bytes[1];
    if (wire_len < FIFO_MIN_WIRE_LEN || (size_t)wire_len > packet_capacity + 2U) {
      ESP_LOGW(TAG,
               "invalid buffered block length=%u; aborting client to restore framing",
               (unsigned)wire_len);
      ap2_buffered_fifo_abort_client(fifo);
      return ESP_ERR_INVALID_SIZE;
    }

    const size_t body_len = (size_t)wire_len - 2U;
    if (!fifo_read_exact(fifo, packet_storage, body_len, epoch))
      continue;
    if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) != epoch)
      continue;

    packet->seq = be32_local(packet_storage) & FIFO_SEQ_MASK;
    packet->rtp = be32_local(packet_storage + 4);
    packet->len = body_len;
    packet->stream_epoch = epoch;
    packet->stream_offset = packet_offset;

    return ESP_OK;
  }
}

void ap2_buffered_fifo_classify_packet(
    ap2_buffered_fifo_t *fifo, const ap2_buffered_packet_t *packet,
    ap2_buffered_packet_decision_t *decision) {
  if (!decision) return;
  memset(decision, 0, sizeof(*decision));
  if (!fifo || !packet) {
    decision->drop = true;
    decision->discontinuity = true;
    return;
  }

  if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) !=
      packet->stream_epoch) {
    decision->drop = true;
    decision->discontinuity = true;
    return;
  }

  /* A packet that started before a full-FLUSH point is stale even if it was
   * only partly received when the flush arrived (the fast path skips only
   * complete packets). */
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  const bool before_full_flush = fifo->discard_before != 0U &&
                                 packet->stream_offset < fifo->discard_before;
  const bool skipped_before = fifo->skip_discontinuity;
  fifo->skip_discontinuity = false;
  xSemaphoreGive(fifo->fifo_mutex);
  if (before_full_flush) {
    decision->drop = true;
    decision->discontinuity = true;
    return;
  }
  if (skipped_before) decision->discontinuity = true;

  xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);

  if (fifo->immediate_active) {
    const int32_t delta = seq23_delta(packet->seq, fifo->immediate_until_seq);
    decision->immediate_target_seq = fifo->immediate_until_seq;
    decision->discontinuity = true;
    if (delta >= 0) {
      decision->immediate_completed = true;
      decision->immediate_overshoot = delta > 0;
      fifo->immediate_active = false;
      /* Shairport Sync cancels deferred requests when an immediate flush
       * actually reaches (or overshoots) its endpoint. */
      memset(fifo->deferred, 0, sizeof(fifo->deferred));
    } else {
      decision->drop = true;
    }
  }

  /* Shairport evaluates deferred requests against the sequential packet
   * stream. fromSeq activates a cut; untilSeq is exclusive; overshoot also
   * terminates it. No descriptor search or retroactive cursor move exists. */
  for (uint32_t i = 0; i < AP2_BUFFERED_FIFO_MAX_DEFERRED; ++i) {
    deferred_flush_t *r = &fifo->deferred[i];
    if (!r->in_use) continue;

    /* Activate at fromSeq, or late if fromSeq was passed only recently
     * (see FIFO_LATE_DEFER_MAX_PKTS). Never activate a rule twice. */
    const int32_t past_from = seq23_delta(packet->seq, r->from_seq);
    if (!r->active && r->until_seq != packet->seq &&
        seq23_delta(packet->seq, r->until_seq) < 0 && past_from >= 0 &&
        past_from <= FIFO_LATE_DEFER_MAX_PKTS) {
      r->active = true;
      decision->discontinuity = true;
      if (decision->activation_count < AP2_BUFFERED_FIFO_MAX_ACTIVATIONS) {
        ap2_buffered_flush_activation_t *a =
            &decision->activations[decision->activation_count++];
        a->from_rtp = r->from_rtp;
        a->until_rtp = r->until_rtp;
      }
    }

    if (r->until_seq == packet->seq) {
      r->active = false;
      r->in_use = false;
    } else if (seq23_delta(packet->seq, r->until_seq) > 0) {
      r->active = false;
      r->in_use = false;
    } else if (r->active) {
      decision->drop = true;
      decision->discontinuity = true;
    }
  }

  xSemaphoreGive(fifo->control_mutex);
}

esp_err_t ap2_buffered_fifo_add_deferred_flush(
    ap2_buffered_fifo_t *fifo, uint32_t from_seq, uint32_t from_rtp,
    uint32_t until_seq, uint32_t until_rtp) {
  if (!fifo) return ESP_ERR_INVALID_ARG;
  from_seq &= FIFO_SEQ_MASK;
  until_seq &= FIFO_SEQ_MASK;

  xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
  int free_slot = -1;
  for (uint32_t i = 0; i < AP2_BUFFERED_FIFO_MAX_DEFERRED; ++i) {
    deferred_flush_t *r = &fifo->deferred[i];
    if (r->in_use && r->from_seq == from_seq && r->until_seq == until_seq) {
      r->from_rtp = from_rtp;
      r->until_rtp = until_rtp;
      xSemaphoreGive(fifo->control_mutex);
      signal_all(fifo);
      return ESP_OK;
    }
    if (!r->in_use && free_slot < 0) free_slot = (int)i;
  }
  if (free_slot < 0) {
    xSemaphoreGive(fifo->control_mutex);
    return ESP_ERR_NO_MEM;
  }

  fifo->deferred[free_slot] = (deferred_flush_t){
      .in_use = true,
      .active = false,
      .from_seq = from_seq,
      .from_rtp = from_rtp,
      .until_seq = until_seq,
      .until_rtp = until_rtp,
  };
  xSemaphoreGive(fifo->control_mutex);
  signal_all(fifo);
  return ESP_OK;
}

void ap2_buffered_fifo_set_immediate_flush(ap2_buffered_fifo_t *fifo,
                                           uint32_t until_seq,
                                           uint32_t until_rtp,
                                           bool has_endpoint) {
  if (!fifo) return;
  if (!has_endpoint) {
    /* A live buffered connection is a framed byte stream. Never move read_pos
     * to write_pos here: recv() can be in the middle of [length][body], so a
     * byte-level purge destroys framing. A full FLUSH invalidates presentation
     * state/PCM in audio_receiver; compressed blocks remain sequential and are
     * consumed as complete frames after the next valid anchor, where stale RTP
     * blocks are discarded naturally. TEARDOWN/stop is the only operation that
     * may clear the raw FIFO because it first terminates the TCP stream.
     *
     * v4.1.13: relying on the next anchor alone was slow (every stale packet
     * went through the consumer one by one) and could STALL: stale packets
     * whose RTP lies ahead of a new anchor (seek backwards) were held as
     * "too early" and blocked the sequential stream. Mark the backlog stale
     * instead; fifo_fast_skip() drops it on packet boundaries immediately,
     * even while paused. */
    xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
    control_clear_locked(fifo);
    xSemaphoreGive(fifo->control_mutex);
    fifo_request_packet_discard(fifo);
    return;
  }

  xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
  fifo->immediate_active = true;
  fifo->immediate_until_seq = until_seq & FIFO_SEQ_MASK;
  fifo->immediate_until_rtp = until_rtp;
  xSemaphoreGive(fifo->control_mutex);
  signal_all(fifo);
}

bool ap2_buffered_fifo_immediate_flush_active(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return false;
  xSemaphoreTake(fifo->control_mutex, portMAX_DELAY);
  bool active = fifo->immediate_active;
  xSemaphoreGive(fifo->control_mutex);
  if (!active) {
    /* A pending full FLUSH also needs the consumer to drain while paused. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    active = fifo->discard_before != 0U &&
             fifo->total_read < fifo->discard_before;
    xSemaphoreGive(fifo->fifo_mutex);
  }
  return active;
}
