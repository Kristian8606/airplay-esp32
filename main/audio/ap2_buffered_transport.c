#include "ap2_buffered_transport.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "audio_diag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network/socket_utils.h"
#include "sdkconfig.h"

#define AP2_STORE_PAGE_BYTES       256U
#define AP2_STORE_MAX_PACKET_SLOTS 8192U
#define AP2_STORE_INVALID_INDEX    UINT16_MAX
#define AP2_STORE_PACKET_MAX       8192U
#define AP2_STORE_WIRE_MIN_LEN     14U
#define AP2_STORE_INITIAL_INVALID_RANGES 32U
#define AP2_STORE_READY_HASH_BUCKETS 8192U

static const char *TAG = "ap2_tcp_store";

typedef struct {
  ap2_buffered_packet_state_t state;
  uint32_t store_epoch;
  uint16_t first_page;
  uint16_t page_count;
  size_t packet_len;
  uint64_t arrival_id;
  uint32_t seq;
  uint32_t rtp;
  bool invalidated;
  bool ready_indexed;
  uint16_t ready_hash_next;
} packet_desc_t;

typedef struct {
  uint32_t from_seq;
  uint32_t until_seq;
} invalid_seq_range_t;

/* Writer-owned snapshot of a WRITING packet. Once reserve_packet_locked()
 * returns, these page-chain coordinates remain stable until this same reader
 * publishes or aborts the packet. FLUSH/session clear never frees WRITING
 * ownership underneath recv(). */
typedef struct {
  uint16_t slot;
  uint16_t first_page;
  uint16_t page_count;
  size_t packet_len;
} packet_write_ref_t;

struct ap2_buffered_transport {
  uint8_t *pages;
  bool owns_pages;
  uint16_t *page_next;
  uint16_t page_count;
  uint16_t free_page_head;
  uint16_t free_pages;

  packet_desc_t *packets;
  uint16_t packet_count;
  uint16_t *free_packet_stack;
  uint16_t free_packet_top;

  /* READY packets are addressable by RTP. The hash is only an index over the
   * descriptor pool; payload ownership remains in packet_desc_t. */
  uint16_t *ready_buckets;
  uint16_t ready_bucket_count;

  /* TCP order is observational only. The reader records predecessor metadata
   * directly into each packet; no metadata FIFO gates ownership or decode. */

  size_t capacity_bytes;
  uint64_t next_arrival_id;
  uint32_t store_epoch;
  uint32_t count_ready;
  uint32_t count_decoding;
  uint32_t count_invalid;

  /* Monotonic revision of the READY candidate set. Inserts/removals bump it
   * under the transport mutex. A failed recovery scan caches the revision it
   * inspected so the higher-priority AAC processor does not rescan the same
   * descriptors until TCP/control actually changes the candidate set. */
  uint32_t ready_revision;

  /* Cache only RECOVERY MISSES. Exact RTP lookup always runs first. The cache
   * key includes the decoder cursor and media revision; wanted_rtp may advance
   * without a READY-set change, so a miss also records the earliest playhead
   * at which a packet that was previously beyond max_lead can become eligible. */
  bool recovery_miss_valid;
  uint32_t recovery_miss_media_generation;
  uint32_t recovery_miss_ready_revision;
  bool recovery_miss_expected_valid;
  uint32_t recovery_miss_expected_rtp;
  uint32_t recovery_miss_expected_seq;
  uint32_t recovery_miss_frame_samples;
  int32_t recovery_miss_max_lead_samples;
  uint32_t recovery_miss_wanted_rtp;
  bool recovery_miss_future_valid;
  uint32_t recovery_miss_future_wanted_rtp;

  /* Last live media floor observed by the AAC processor. This is only used
   * by writer-side GC when the page store is under pressure; it never gates
   * decode. The generation tags prevent a decoder snapshot from the previous
   * anchor from reinstalling a stale pressure-GC floor after a seek. */
  uint32_t media_generation;
  bool cursor_seq_valid;
  uint32_t cursor_seq; /* current next decode sequence, including in-flight AU */
  bool cursor_flush_pending; /* survives rule retirement until cursor advances */
  bool media_floor_valid;
  uint32_t media_floor_generation;
  uint32_t media_floor_rtp;
  uint32_t media_floor_frame_samples;

  invalid_seq_range_t *invalid_ranges;
  uint32_t invalid_range_count;
  uint32_t invalid_range_capacity;
  bool invalid_before_active;
  uint32_t invalid_before_seq;
  bool invalidate_all_active;

  SemaphoreHandle_t mutex;
  SemaphoreHandle_t space_ready;

  int listen_sock;
  int client_sock;
  uint16_t port;
  volatile bool running;
  TaskHandle_t reader_task;

  int task_core;
  int task_priority;
  uint32_t task_stack;

};

static inline uint32_t be32_local(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int32_t seq23_delta_local(uint32_t a, uint32_t b) {
  uint32_t d = (a - b) & 0x007fffffU;
  if (d & 0x00400000U) d |= 0xff800000U;
  return (int32_t)d;
}

static bool invalid_ranges_ensure_capacity_locked(
    ap2_buffered_transport_t *t, uint32_t needed) {
  if (needed <= t->invalid_range_capacity) return true;

  uint32_t new_capacity = t->invalid_range_capacity
                              ? t->invalid_range_capacity
                              : AP2_STORE_INITIAL_INVALID_RANGES;
  while (new_capacity < needed) {
    if (new_capacity > UINT32_MAX / 2U) {
      return false;
    }
    new_capacity *= 2U;
  }
  if ((size_t)new_capacity > SIZE_MAX / sizeof(*t->invalid_ranges)) {
    return false;
  }

  invalid_seq_range_t *grown =
      realloc(t->invalid_ranges,
              (size_t)new_capacity * sizeof(*t->invalid_ranges));
  if (!grown) {
    return false;
  }
  memset(grown + t->invalid_range_capacity, 0,
         (size_t)(new_capacity - t->invalid_range_capacity) * sizeof(*grown));
  t->invalid_ranges = grown;
  t->invalid_range_capacity = new_capacity;
  return true;
}

/* Deferred FLUSHBUFFERED ranges are only needed until TCP reaches their
 * exclusive untilSeq endpoint. TCP byte order is authoritative for arrival,
 * so after publishing untilSeq (or an overshoot) no future packet from this
 * transport position can belong to that discarded interval. This restores the
 * lifecycle used by the pre-addressable transport instead of retaining every
 * Automix range until an unrelated anchor reset. */
static uint32_t retire_completed_invalid_ranges_locked(
    ap2_buffered_transport_t *t, uint32_t current_seq) {
  current_seq &= 0x007fffffU;
  uint32_t retired = 0;
  for (uint32_t i = 0; i < t->invalid_range_count;) {
    invalid_seq_range_t *r = &t->invalid_ranges[i];
    if (seq23_delta_local(current_seq, r->until_seq) >= 0) {
      const uint32_t last = t->invalid_range_count - 1U;
      if (i != last) t->invalid_ranges[i] = t->invalid_ranges[last];
      memset(&t->invalid_ranges[last], 0, sizeof(t->invalid_ranges[last]));
      t->invalid_range_count--;
      retired++;
      continue;
    }
    ++i;
  }
  return retired;
}

static bool packet_is_invalid_by_rule_locked(
    const ap2_buffered_transport_t *t, uint32_t seq) {
  seq &= 0x007fffffU;
  if (t->invalidate_all_active) return true;
  if (t->invalid_before_active &&
      seq23_delta_local(seq, t->invalid_before_seq) < 0) {
    return true;
  }
  for (uint32_t i = 0; i < t->invalid_range_count; ++i) {
    const invalid_seq_range_t *r = &t->invalid_ranges[i];
    const int32_t from_delta = seq23_delta_local(seq, r->from_seq);
    const int32_t until_delta = seq23_delta_local(seq, r->until_seq);
    if (from_delta >= 0 && until_delta < 0) return true;
  }
  return false;
}

static inline uint16_t ready_hash_bucket(const ap2_buffered_transport_t *t,
                                         uint32_t rtp) {
  uint32_t x = rtp;
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  return (uint16_t)(x & (uint32_t)(t->ready_bucket_count - 1U));
}

static inline void ready_revision_bump_locked(ap2_buffered_transport_t *t) {
  t->ready_revision++;
  if (t->ready_revision == 0) t->ready_revision = 1;
}

static void ready_index_insert_locked(ap2_buffered_transport_t *t,
                                      uint16_t slot) {
  packet_desc_t *d = &t->packets[slot];
  if (d->ready_indexed || d->state != AP2_BUFFERED_PACKET_READY ||
      d->invalidated) {
    return;
  }
  uint16_t b = ready_hash_bucket(t, d->rtp);
  d->ready_hash_next = t->ready_buckets[b];
  t->ready_buckets[b] = slot;
  d->ready_indexed = true;
  ready_revision_bump_locked(t);
}

static void ready_index_remove_locked(ap2_buffered_transport_t *t,
                                      uint16_t slot) {
  packet_desc_t *d = &t->packets[slot];
  if (!d->ready_indexed) return;
  uint16_t b = ready_hash_bucket(t, d->rtp);
  uint16_t cur = t->ready_buckets[b];
  uint16_t prev = AP2_STORE_INVALID_INDEX;
  while (cur != AP2_STORE_INVALID_INDEX) {
    if (cur == slot) {
      if (prev == AP2_STORE_INVALID_INDEX) {
        t->ready_buckets[b] = t->packets[cur].ready_hash_next;
      } else {
        t->packets[prev].ready_hash_next = t->packets[cur].ready_hash_next;
      }
      break;
    }
    prev = cur;
    cur = t->packets[cur].ready_hash_next;
  }
  d->ready_indexed = false;
  d->ready_hash_next = AP2_STORE_INVALID_INDEX;
  ready_revision_bump_locked(t);
}

static bool recovery_miss_still_current_locked(
    const ap2_buffered_transport_t *t, uint32_t wanted_rtp,
    uint32_t expected_rtp, uint32_t expected_seq, bool expected_valid,
    uint32_t frame_samples, int32_t max_lead_samples,
    uint32_t media_generation) {
  if (!t->recovery_miss_valid ||
      t->recovery_miss_media_generation != media_generation ||
      t->recovery_miss_ready_revision != t->ready_revision ||
      t->recovery_miss_expected_valid != expected_valid ||
      t->recovery_miss_frame_samples != frame_samples ||
      t->recovery_miss_max_lead_samples != max_lead_samples) {
    return false;
  }
  if (expected_valid &&
      (t->recovery_miss_expected_rtp != expected_rtp ||
       t->recovery_miss_expected_seq != expected_seq)) {
    return false;
  }

  /* A backwards playhead move without a media revision should never be hidden
   * by the cache. Normal forward motion can only create a new candidate when a
   * previously too-far-ahead READY packet reaches the max-lead boundary. */
  if ((int32_t)(wanted_rtp - t->recovery_miss_wanted_rtp) < 0) return false;
  if (t->recovery_miss_future_valid &&
      (int32_t)(wanted_rtp - t->recovery_miss_future_wanted_rtp) >= 0) {
    return false;
  }
  return true;
}

static void recovery_miss_record_locked(
    ap2_buffered_transport_t *t, uint32_t wanted_rtp, uint32_t expected_rtp,
    uint32_t expected_seq, bool expected_valid, uint32_t frame_samples,
    int32_t max_lead_samples, uint32_t media_generation,
    bool future_valid, uint32_t future_wanted_rtp) {
  t->recovery_miss_valid = true;
  t->recovery_miss_media_generation = media_generation;
  t->recovery_miss_ready_revision = t->ready_revision;
  t->recovery_miss_expected_valid = expected_valid;
  t->recovery_miss_expected_rtp = expected_rtp;
  t->recovery_miss_expected_seq = expected_seq;
  t->recovery_miss_frame_samples = frame_samples;
  t->recovery_miss_max_lead_samples = max_lead_samples;
  t->recovery_miss_wanted_rtp = wanted_rtp;
  t->recovery_miss_future_valid = future_valid;
  t->recovery_miss_future_wanted_rtp = future_wanted_rtp;
}

static uint16_t ready_index_find_exact_locked(
    ap2_buffered_transport_t *t, uint32_t rtp, uint32_t wanted_rtp,
    uint32_t frame_samples, int32_t max_lead_samples,
    bool *ready_exact_too_early) {
  if (ready_exact_too_early) *ready_exact_too_early = false;

  uint16_t b = ready_hash_bucket(t, rtp);
  uint16_t cur = t->ready_buckets[b];
  uint16_t best = AP2_STORE_INVALID_INDEX;
  uint64_t newest = 0;
  while (cur != AP2_STORE_INVALID_INDEX) {
    packet_desc_t *d = &t->packets[cur];
    uint16_t next = d->ready_hash_next;
    if (d->state == AP2_BUFFERED_PACKET_READY && d->ready_indexed &&
        !d->invalidated && d->store_epoch == t->store_epoch && d->rtp == rtp) {
      const int32_t playhead_delta = (int32_t)(d->rtp - wanted_rtp);

      /* The exact decoder continuation exists, but decode is intentionally
       * capped around the playhead. This is the normal steady-state pacing
       * case (the log showed the next exact AAC block at ~1104-1115 ms while
       * the decode window is 1100 ms). Do not mistake that for a missing
       * packet and fall back to a full descriptor search on every 5 ms tick.
       * All duplicates with this RTP have the same playhead distance, so one
       * READY match is sufficient to prove that we only need to wait. */
      if (playhead_delta > max_lead_samples) {
        if (ready_exact_too_early) *ready_exact_too_early = true;
      } else if ((int64_t)playhead_delta + (int64_t)frame_samples > 0 &&
                 (best == AP2_STORE_INVALID_INDEX ||
                  d->arrival_id > newest)) {
        best = cur;
        newest = d->arrival_id;
      }
    }
    cur = next;
  }
  return best;
}

static void signal_space(ap2_buffered_transport_t *t) {
  if (t->space_ready) xSemaphoreGive(t->space_ready);
}


static void pages_release_locked(ap2_buffered_transport_t *t,
                                 uint16_t first_page, uint16_t page_count) {
  uint16_t page = first_page;
  for (uint16_t i = 0; i < page_count && page != AP2_STORE_INVALID_INDEX; ++i) {
    uint16_t next = t->page_next[page];
    t->page_next[page] = t->free_page_head;
    t->free_page_head = page;
    t->free_pages++;
    page = next;
  }
}

static void packet_make_free_locked(ap2_buffered_transport_t *t, uint16_t slot) {
  packet_desc_t *d = &t->packets[slot];
  if (d->state == AP2_BUFFERED_PACKET_FREE) return;

  if (d->ready_indexed) ready_index_remove_locked(t, slot);

  switch (d->state) {
  case AP2_BUFFERED_PACKET_READY:
    if (t->count_ready) t->count_ready--;
    break;
  case AP2_BUFFERED_PACKET_DECODING:
    if (t->count_decoding) t->count_decoding--;
    break;
  case AP2_BUFFERED_PACKET_INVALID:
    if (t->count_invalid) t->count_invalid--;
    break;
  default:
    break;
  }

  if (d->page_count != 0 && d->first_page != AP2_STORE_INVALID_INDEX) {
    pages_release_locked(t, d->first_page, d->page_count);
  }

  memset(d, 0, sizeof(*d));
  d->state = AP2_BUFFERED_PACKET_FREE;
  d->first_page = AP2_STORE_INVALID_INDEX;
  t->free_packet_stack[t->free_packet_top++] = slot;
}

static uint32_t reap_invalid_locked(ap2_buffered_transport_t *t,
                                    uint32_t max_packets) {
  if (t->count_invalid == 0) return 0;
  uint32_t reaped = 0;
  for (uint16_t i = 0; i < t->packet_count; ++i) {
    if (max_packets != 0 && reaped >= max_packets) break;
    if (t->packets[i].state == AP2_BUFFERED_PACKET_INVALID) {
      packet_make_free_locked(t, i);
      reaped++;
    }
  }
  return reaped;
}

/* READY packets wholly behind the live playhead can never be selected again:
 * acquire_media_next() uses the same stale predicate before considering a
 * candidate. Reap them only when the TCP writer is out of descriptors/pages,
 * so this O(N) pass is pressure-driven rather than part of the realtime decode
 * hot path. */
static uint32_t reap_stale_ready_locked(ap2_buffered_transport_t *t,
                                        uint32_t wanted_rtp,
                                        uint32_t frame_samples,
                                        uint32_t max_packets) {
  if (frame_samples == 0 || t->count_ready == 0) return 0;
  uint32_t reaped = 0;
  for (uint16_t i = 0; i < t->packet_count; ++i) {
    if (max_packets != 0 && reaped >= max_packets) break;
    packet_desc_t *d = &t->packets[i];
    if (d->state != AP2_BUFFERED_PACKET_READY || !d->ready_indexed ||
        d->invalidated || d->store_epoch != t->store_epoch) {
      continue;
    }
    const int32_t delta = (int32_t)(d->rtp - wanted_rtp);
    if ((int64_t)delta + (int64_t)frame_samples <= 0) {
      packet_make_free_locked(t, i);
      reaped++;
    }
  }
  return reaped;
}

static bool reserve_packet_locked(ap2_buffered_transport_t *t, size_t packet_len,
                                  packet_write_ref_t *out) {
  if (!out) return false;
  const uint16_t needed_pages =
      (uint16_t)((packet_len + AP2_STORE_PAGE_BYTES - 1U) / AP2_STORE_PAGE_BYTES);
  if (needed_pages == 0 || needed_pages > t->page_count) return false;

  if (t->free_packet_top == 0 || t->free_pages < needed_pages) {
    (void)reap_invalid_locked(t, 0);
    const uint32_t media_generation =
        __atomic_load_n(&t->media_generation, __ATOMIC_ACQUIRE);
    if ((t->free_packet_top == 0 || t->free_pages < needed_pages) &&
        t->media_floor_valid &&
        t->media_floor_generation == media_generation) {
      (void)reap_stale_ready_locked(t, t->media_floor_rtp,
                                    t->media_floor_frame_samples, 0);
    }
  }
  if (t->free_packet_top == 0 || t->free_pages < needed_pages) return false;

  uint16_t slot = t->free_packet_stack[--t->free_packet_top];
  packet_desc_t *d = &t->packets[slot];
  memset(d, 0, sizeof(*d));
  d->state = AP2_BUFFERED_PACKET_WRITING;
  d->store_epoch = t->store_epoch;
  d->first_page = AP2_STORE_INVALID_INDEX;
  d->page_count = needed_pages;
  d->packet_len = packet_len;
  d->ready_hash_next = AP2_STORE_INVALID_INDEX;

  uint16_t first = AP2_STORE_INVALID_INDEX;
  uint16_t prev = AP2_STORE_INVALID_INDEX;
  for (uint16_t i = 0; i < needed_pages; ++i) {
    uint16_t page = t->free_page_head;
    if (page == AP2_STORE_INVALID_INDEX) {
      if (first != AP2_STORE_INVALID_INDEX) pages_release_locked(t, first, i);
      memset(d, 0, sizeof(*d));
      d->state = AP2_BUFFERED_PACKET_FREE;
      d->first_page = AP2_STORE_INVALID_INDEX;
      t->free_packet_stack[t->free_packet_top++] = slot;
      return false;
    }
    t->free_page_head = t->page_next[page];
    t->free_pages--;
    t->page_next[page] = AP2_STORE_INVALID_INDEX;
    if (prev != AP2_STORE_INVALID_INDEX) t->page_next[prev] = page;
    else first = page;
    prev = page;
  }
  d->first_page = first;
  if (out) {
    out->slot = slot;
    out->first_page = first;
    out->page_count = needed_pages;
    out->packet_len = packet_len;
  }
  return true;
}

static void abort_write(ap2_buffered_transport_t *t, uint16_t slot) {
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  if (slot < t->packet_count &&
      t->packets[slot].state == AP2_BUFFERED_PACKET_WRITING) {
    packet_make_free_locked(t, slot);
  }
  xSemaphoreGive(t->mutex);
  signal_space(t);
}

static ssize_t socket_read_exact(ap2_buffered_transport_t *t, int sock,
                                 void *dst_, size_t len) {
  uint8_t *dst = (uint8_t *)dst_;
  size_t off = 0;
  while (off < len && t->running) {
    ssize_t n = recv(sock, dst + off, len - off, 0);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }

    return n == 0 ? 0 : -1;
  }
  return off == len ? (ssize_t)off : 0;
}

static bool recv_packet_into_pages(ap2_buffered_transport_t *t, int sock,
                                   const packet_write_ref_t *wr) {
  if (!t || !wr || wr->slot >= t->packet_count ||
      wr->first_page == AP2_STORE_INVALID_INDEX || wr->page_count == 0) {
    return false;
  }

  /* reserve_packet_locked() transferred exclusive WRITING ownership to this
   * TCP reader. The page chain cannot be reclaimed by FLUSH/store clear until
   * publish/abort, so recv() does not need to re-enter transport->mutex just
   * to reread immutable reservation metadata. */
  uint16_t page = wr->first_page;
  size_t remaining = wr->packet_len;
  for (uint16_t i = 0; i < wr->page_count && remaining > 0; ++i) {
    if (page == AP2_STORE_INVALID_INDEX) return false;
    size_t n = remaining > AP2_STORE_PAGE_BYTES ? AP2_STORE_PAGE_BYTES : remaining;
    uint8_t *dst = t->pages + (size_t)page * AP2_STORE_PAGE_BYTES;
    if (socket_read_exact(t, sock, dst, n) != (ssize_t)n) return false;
    remaining -= n;
    page = t->page_next[page];
  }
  return remaining == 0;
}

static void publish_written_packet(ap2_buffered_transport_t *t, uint16_t slot) {
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  if (slot >= t->packet_count) {
    xSemaphoreGive(t->mutex);
    return;
  }
  packet_desc_t *d = &t->packets[slot];
  if (d->state != AP2_BUFFERED_PACKET_WRITING) {
    xSemaphoreGive(t->mutex);
    return;
  }

  /* A hard session clear happened while recv() was filling this packet. */
  if (d->store_epoch != t->store_epoch) {
    packet_make_free_locked(t, slot);
    xSemaphoreGive(t->mutex);
    signal_space(t);
    return;
  }

  const uint8_t *p = t->pages + (size_t)d->first_page * AP2_STORE_PAGE_BYTES;
  d->seq = be32_local(p) & 0x007fffffU;
  d->rtp = be32_local(p + 4);
  d->arrival_id = ++t->next_arrival_id;

  /* untilSeq is exclusive: retire completed deferred rules before judging
   * this packet so untilSeq itself remains valid replacement media. */
  (void)retire_completed_invalid_ranges_locked(t, d->seq);

  /* Control rules are declarative. TCP ingestion never waits for a FLUSH
   * sequence rendezvous: the packet is catalogued immediately as READY or
   * INVALID based on the rules active at publish time. */
  if (packet_is_invalid_by_rule_locked(t, d->seq)) {
    d->invalidated = true;
    d->state = AP2_BUFFERED_PACKET_INVALID;
    t->count_invalid++;
  } else {
    d->state = AP2_BUFFERED_PACKET_READY;
    t->count_ready++;
    ready_index_insert_locked(t, slot);
  }
  xSemaphoreGive(t->mutex);

}

static void store_clear_epoch(ap2_buffered_transport_t *t) {
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  t->store_epoch++;
  if (t->store_epoch == 0) t->store_epoch = 1;
  t->cursor_seq_valid = false;
  t->cursor_flush_pending = false;
  t->media_floor_valid = false;
  t->media_floor_generation =
      __atomic_load_n(&t->media_generation, __ATOMIC_ACQUIRE);
  t->media_floor_rtp = 0;
  t->media_floor_frame_samples = 0;
  if (t->invalid_ranges && t->invalid_range_capacity) {
    memset(t->invalid_ranges, 0,
           (size_t)t->invalid_range_capacity * sizeof(*t->invalid_ranges));
  }
  t->invalid_range_count = 0;
  t->invalid_before_active = false;
  t->invalidate_all_active = false;

  for (uint16_t i = 0; i < t->packet_count; ++i) {
    ap2_buffered_packet_state_t state = t->packets[i].state;
    if (state == AP2_BUFFERED_PACKET_READY ||
        state == AP2_BUFFERED_PACKET_INVALID) {
      packet_make_free_locked(t, i);
    }
    /* WRITING/DECODING retain ownership until their current operation exits;
     * their old epoch guarantees they are reclaimed instead of republished. */
  }
  for (uint16_t i = 0; i < t->ready_bucket_count; ++i) {
    t->ready_buckets[i] = AP2_STORE_INVALID_INDEX;
  }
  xSemaphoreGive(t->mutex);
  signal_space(t);
}

static void tcp_reader_task(void *arg) {
  ap2_buffered_transport_t *t = (ap2_buffered_transport_t *)arg;

  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_TCP_READER,
                                    xPortGetCoreID(), t->task_priority,
                                    t->packet_count);

  while (t->running) {
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    int c = accept(t->listen_sock, (struct sockaddr *)&addr, &alen);
    if (c < 0) {
      if (t->running && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "accept errno=%d", errno);
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    store_clear_epoch(t);
    AUDIO_DIAG_TRANSPORT_AAC_SESSION_RESET();
    t->client_sock = c;

    int rcvbuf = CONFIG_LWIP_TCP_WND_DEFAULT;
    if (setsockopt(c, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
      ESP_LOGW(TAG, "SO_RCVBUF failed errno=%d", errno);
    }

    struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
    if (setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      ESP_LOGW(TAG, "SO_RCVTIMEO failed errno=%d", errno);
    }

    ESP_LOGI(TAG, "buffered TCP connected rcvbuf=%d direct_store=1", rcvbuf);

    while (t->running) {
      uint8_t lb[2];
      ssize_t hn = socket_read_exact(t, c, lb, sizeof(lb));
      if (hn != (ssize_t)sizeof(lb)) break;

      uint16_t wire_len = ((uint16_t)lb[0] << 8) | lb[1];
      if (wire_len < AP2_STORE_WIRE_MIN_LEN ||
          wire_len > AP2_STORE_PACKET_MAX + 2U) {
        ESP_LOGW(TAG, "invalid buffered block length=%u", (unsigned)wire_len);
        /* Framing is no longer trustworthy after an invalid length. Reconnect
         * rather than scanning arbitrary compressed bytes for a new boundary. */
        break;
      }
      size_t packet_len = (size_t)wire_len - 2U;

      packet_write_ref_t wr = {
          .slot = AP2_STORE_INVALID_INDEX,
          .first_page = AP2_STORE_INVALID_INDEX,
      };
      while (t->running) {
        xSemaphoreTake(t->mutex, portMAX_DELAY);
        bool ok = reserve_packet_locked(t, packet_len, &wr);
        xSemaphoreGive(t->mutex);
        if (ok) {
          AUDIO_DIAG_TRANSPORT_AAC_STORE_WAIT_END();
          break;
        }
        /* No writable descriptor/pages: this is the intentional AirPlay TCP
         * backpressure point. We stop recv() until decode/GC returns storage. */
        AUDIO_DIAG_TRANSPORT_AAC_STORE_WAIT_BEGIN();
        xSemaphoreTake(t->space_ready, pdMS_TO_TICKS(100));
      }
      if (!t->running || wr.slot == AP2_STORE_INVALID_INDEX) break;

      if (!recv_packet_into_pages(t, c, &wr)) {
        abort_write(t, wr.slot);
        break;
      }
      publish_written_packet(t, wr.slot);
      AUDIO_DIAG_TRANSPORT_AAC_RX_BLOCK((uint32_t)packet_len);
    }

    shutdown(c, SHUT_RDWR);
    close(c);
    t->client_sock = -1;
    ESP_LOGI(TAG, "buffered TCP disconnected");
  }

  __atomic_store_n(&t->reader_task, NULL, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

static esp_err_t ap2_buffered_transport_create_internal(
    ap2_buffered_transport_t **out,
    const ap2_buffered_transport_config_t *cfg,
    void *payload_storage, size_t payload_storage_bytes) {
  if (!out || !cfg || cfg->store_bytes < 16384U) return ESP_ERR_INVALID_ARG;
  if (payload_storage && payload_storage_bytes < cfg->store_bytes) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t page_count_sz = cfg->store_bytes / AP2_STORE_PAGE_BYTES;
  if (page_count_sz == 0 || page_count_sz >= AP2_STORE_INVALID_INDEX) {
    return ESP_ERR_INVALID_ARG;
  }
  uint16_t page_count = (uint16_t)page_count_sz;
  uint16_t packet_count = page_count;
  if (packet_count > AP2_STORE_MAX_PACKET_SLOTS) {
    packet_count = AP2_STORE_MAX_PACKET_SLOTS;
  }

  ap2_buffered_transport_t *t = calloc(1, sizeof(*t));
  if (!t) return ESP_ERR_NO_MEM;
  t->listen_sock = -1;
  t->client_sock = -1;

  if (payload_storage) {
    t->pages = (uint8_t *)payload_storage;
    t->owns_pages = false;
  } else {
    t->pages = heap_caps_malloc((size_t)page_count * AP2_STORE_PAGE_BYTES,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t->pages) {
      t->pages = malloc((size_t)page_count * AP2_STORE_PAGE_BYTES);
    }
    t->owns_pages = true;
  }
  t->packets = heap_caps_calloc(packet_count, sizeof(packet_desc_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!t->packets) t->packets = calloc(packet_count, sizeof(packet_desc_t));
  t->page_next = heap_caps_malloc((size_t)page_count * sizeof(uint16_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!t->page_next) {
    t->page_next = malloc((size_t)page_count * sizeof(uint16_t));
  }
  t->free_packet_stack =
      heap_caps_malloc((size_t)packet_count * sizeof(uint16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!t->free_packet_stack) {
    t->free_packet_stack = malloc((size_t)packet_count * sizeof(uint16_t));
  }
  t->ready_buckets =
      heap_caps_malloc(AP2_STORE_READY_HASH_BUCKETS * sizeof(uint16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!t->ready_buckets) {
    t->ready_buckets =
        malloc(AP2_STORE_READY_HASH_BUCKETS * sizeof(uint16_t));
  }
  t->invalid_ranges = calloc(AP2_STORE_INITIAL_INVALID_RANGES,
                             sizeof(*t->invalid_ranges));
  t->invalid_range_capacity = t->invalid_ranges
                                  ? AP2_STORE_INITIAL_INVALID_RANGES
                                  : 0U;
  t->mutex = xSemaphoreCreateMutex();
  t->space_ready = xSemaphoreCreateBinary();

  if (!t->pages || !t->packets || !t->page_next || !t->free_packet_stack ||
      !t->ready_buckets || !t->invalid_ranges || !t->mutex ||
      !t->space_ready) {
    ap2_buffered_transport_destroy(t);
    return ESP_ERR_NO_MEM;
  }

  t->page_count = page_count;
  t->packet_count = packet_count;
  t->ready_bucket_count = AP2_STORE_READY_HASH_BUCKETS;
  t->capacity_bytes = (size_t)page_count * AP2_STORE_PAGE_BYTES;
  t->store_epoch = 1;
  t->listen_sock = -1;
  t->client_sock = -1;
  t->task_core = cfg->task_core;
  t->task_priority = cfg->task_priority;
  t->task_stack = cfg->task_stack;

  for (uint16_t i = 0; i < page_count; ++i) {
    t->page_next[i] = (i + 1U < page_count) ? (uint16_t)(i + 1U)
                                             : AP2_STORE_INVALID_INDEX;
  }
  t->free_page_head = 0;
  t->free_pages = page_count;

  for (uint16_t i = 0; i < packet_count; ++i) {
    t->packets[i].state = AP2_BUFFERED_PACKET_FREE;
    t->packets[i].first_page = AP2_STORE_INVALID_INDEX;
    t->packets[i].ready_hash_next = AP2_STORE_INVALID_INDEX;
    t->free_packet_stack[i] = i;
  }
  for (uint16_t i = 0; i < t->ready_bucket_count; ++i) {
    t->ready_buckets[i] = AP2_STORE_INVALID_INDEX;
  }
  t->free_packet_top = packet_count;

  *out = t;
  return ESP_OK;
}

esp_err_t ap2_buffered_transport_create(ap2_buffered_transport_t **out,
                                        const ap2_buffered_transport_config_t *cfg) {
  return ap2_buffered_transport_create_internal(out, cfg, NULL, 0U);
}

esp_err_t ap2_buffered_transport_create_with_payload_storage(
    ap2_buffered_transport_t **out,
    const ap2_buffered_transport_config_t *cfg,
    void *payload_storage, size_t payload_storage_bytes) {
  if (!payload_storage) return ESP_ERR_INVALID_ARG;
  return ap2_buffered_transport_create_internal(out, cfg, payload_storage,
                                                 payload_storage_bytes);
}

void ap2_buffered_transport_destroy(ap2_buffered_transport_t *t) {
  if (!t) return;
  ap2_buffered_transport_stop(t);
  if (__atomic_load_n(&t->reader_task, __ATOMIC_ACQUIRE)) {
    ESP_LOGE(TAG, "destroy deferred: TCP reader still owns transport");
    return;
  }
  if (t->count_decoding) {
    ESP_LOGE(TAG, "destroy deferred: decoder still owns transport");
    return;
  }
  if (t->mutex) vSemaphoreDelete(t->mutex);
  if (t->space_ready) vSemaphoreDelete(t->space_ready);
  free(t->invalid_ranges);
  free(t->ready_buckets);
  free(t->free_packet_stack);
  free(t->page_next);
  free(t->packets);
  if (t->owns_pages) free(t->pages);
  free(t);
}

esp_err_t ap2_buffered_transport_start(ap2_buffered_transport_t *t,
                                       uint16_t requested_port,
                                       uint16_t *bound_port) {
  if (!t) return ESP_ERR_INVALID_ARG;
  if (t->running) {
    if (bound_port) *bound_port = t->port;
    return ESP_OK;
  }

  if (__atomic_load_n(&t->reader_task, __ATOMIC_ACQUIRE))
    return ESP_ERR_INVALID_STATE;
  uint16_t bound = requested_port;
  t->listen_sock = socket_utils_bind_tcp_listener(requested_port, 1, true, &bound);
  if (t->listen_sock < 0) return ESP_FAIL;
  t->port = bound;
  t->running = true;

  if (xTaskCreatePinnedToCore(tcp_reader_task, "ap2_tcp_store", t->task_stack,
                              t, t->task_priority, &t->reader_task,
                              t->task_core) != pdPASS) {
    t->running = false;
    close(t->listen_sock);
    t->listen_sock = -1;
    return ESP_FAIL;
  }
  if (bound_port) *bound_port = bound;
  return ESP_OK;
}

void ap2_buffered_transport_clear(ap2_buffered_transport_t *t) {
  if (!t) return;
  store_clear_epoch(t);
}

void ap2_buffered_transport_stop(ap2_buffered_transport_t *t) {
  if (!t) return;
  t->running = false;
  if (t->client_sock >= 0) shutdown(t->client_sock, SHUT_RDWR);
  if (t->listen_sock >= 0) {
    shutdown(t->listen_sock, SHUT_RDWR);
    close(t->listen_sock);
    t->listen_sock = -1;
  }
  signal_space(t);
  for (int i = 0; __atomic_load_n(&t->reader_task, __ATOMIC_ACQUIRE) && i < 100; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  t->port = 0;
}

bool ap2_buffered_transport_is_idle(ap2_buffered_transport_t *t) {
  if (!t) return true;
  if (t->running ||
      __atomic_load_n(&t->reader_task, __ATOMIC_ACQUIRE) != NULL) {
    return false;
  }
  if (!t->mutex) return t->count_decoding == 0U;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  const bool idle = t->count_decoding == 0U;
  xSemaphoreGive(t->mutex);
  return idle;
}

static bool fill_ref_locked(ap2_buffered_transport_t *t, uint16_t slot,
                            ap2_buffered_packet_ref_t *out) {
  if (!out || slot >= t->packet_count) return false;
  const packet_desc_t *d = &t->packets[slot];
  out->slot = slot;
  out->arrival_id = d->arrival_id;
  out->seq = d->seq;
  out->rtp = d->rtp;
  out->packet_len = d->packet_len;
  return true;
}

static bool ref_matches_locked(ap2_buffered_transport_t *t,
                               const ap2_buffered_packet_ref_t *ref) {
  return ref && ref->slot < t->packet_count &&
         t->packets[ref->slot].arrival_id == ref->arrival_id;
}

bool ap2_buffered_transport_acquire_media_next(
    ap2_buffered_transport_t *t, uint32_t wanted_rtp, uint32_t expected_rtp,
    uint32_t expected_seq, bool expected_valid, bool allow_recovery_scan,
    uint32_t frame_samples, int32_t max_lead_samples,
    uint32_t media_generation, ap2_buffered_packet_ref_t *out) {
  if (!t || !out || frame_samples == 0) return false;
  if (__atomic_load_n(&t->media_generation, __ATOMIC_ACQUIRE) !=
      media_generation) {
    return false;
  }

  uint16_t best_slot = AP2_STORE_INVALID_INDEX;
  int best_class = INT_MAX;
  int32_t best_distance = INT32_MAX;
  uint64_t best_arrival = 0;

  xSemaphoreTake(t->mutex, portMAX_DELAY);

  /* The anchor may have changed while this task was waiting for the transport
   * lock. Never let a stale decoder snapshot select media or update GC state. */
  if (__atomic_load_n(&t->media_generation, __ATOMIC_ACQUIRE) !=
      media_generation) {
    xSemaphoreGive(t->mutex);
    return false;
  }

  t->media_floor_valid = true;
  t->media_floor_generation = media_generation;
  t->media_floor_rtp = wanted_rtp;
  t->media_floor_frame_samples = frame_samples;

  expected_seq &= 0x007fffffU;
  if (!expected_valid || !t->cursor_seq_valid || t->cursor_seq != expected_seq) {
    t->cursor_flush_pending = false;
  }
  t->cursor_seq_valid = expected_valid;
  t->cursor_seq = expected_seq;

  /* Normal playback is exact-addressed only. The overwhelmingly common case
   * is an O(1) READY-hash hit for the next decoder RTP. A missing exact packet
   * above the reorder guard is not recovery: release the transport mutex and
   * let the receiver sleep/retry while TCP is free to publish it.
   *
   * FLUSH is the exception. If control has explicitly invalidated expected_seq,
   * waiting for that exact continuation can never succeed, so recovery is
   * allowed immediately even before the time guard. */
  bool exact_ready_too_early = false;
  if (expected_valid) {
    best_slot = ready_index_find_exact_locked(
        t, expected_rtp, wanted_rtp, frame_samples, max_lead_samples,
        &exact_ready_too_early);
    if (best_slot != AP2_STORE_INVALID_INDEX) {
      AUDIO_DIAG_TRANSPORT_CURSOR_EXACT_HIT();
    } else {
      AUDIO_DIAG_TRANSPORT_CURSOR_EXACT_MISS(exact_ready_too_early ? 1U : 0U);
    }
    if (best_slot == AP2_STORE_INVALID_INDEX && exact_ready_too_early) {
      xSemaphoreGive(t->mutex);
      return false;
    }
    if (best_slot == AP2_STORE_INVALID_INDEX && !allow_recovery_scan) {
      if (!t->cursor_flush_pending &&
          !packet_is_invalid_by_rule_locked(t, expected_seq)) {
        AUDIO_DIAG_TRANSPORT_CURSOR_GUARD_WAIT();
        xSemaphoreGive(t->mutex);
        return false;
      }
    }
  }

  /* Recovery-only path. Pay for the full descriptor scan only when there is
   * no contiguous decoder cursor (startup/seek/new neighbourhood), FLUSH has
   * made that cursor impossible, or the reorder deadline has been reached.
   *
   * A failed scan is memoized against the READY-set revision and cursor query.
   * Repeating the same O(N) scan before TCP publishes/removes anything cannot
   * produce a different answer, so yield back to the receiver loop instead. */
  if (best_slot == AP2_STORE_INVALID_INDEX && t->count_ready != 0) {
    if (recovery_miss_still_current_locked(
            t, wanted_rtp, expected_rtp, expected_seq, expected_valid,
            frame_samples, max_lead_samples, media_generation)) {
      xSemaphoreGive(t->mutex);
      return false;
    }

    bool future_valid = false;
    uint32_t future_wanted_rtp = 0;
    int32_t future_frames = INT32_MAX;

    AUDIO_DIAG_TRANSPORT_CURSOR_RECOVERY_SCAN(
        (expected_valid && !allow_recovery_scan) ? 1U : 0U);
    for (uint16_t i = 0; i < t->packet_count; ++i) {
      packet_desc_t *d = &t->packets[i];
      if (d->state != AP2_BUFFERED_PACKET_READY || !d->ready_indexed ||
          d->invalidated || d->store_epoch != t->store_epoch) {
        continue;
      }

      const int32_t playhead_delta = (int32_t)(d->rtp - wanted_rtp);
      if (playhead_delta > max_lead_samples) {
        const int64_t until_eligible =
            (int64_t)playhead_delta - (int64_t)max_lead_samples;
        if (until_eligible > 0 && until_eligible <= INT32_MAX &&
            (int32_t)until_eligible < future_frames) {
          future_frames = (int32_t)until_eligible;
          future_wanted_rtp = wanted_rtp + (uint32_t)future_frames;
          future_valid = true;
        }
        continue;
      }
      if ((int64_t)playhead_delta + (int64_t)frame_samples <= 0) continue;

      int klass;
      int32_t distance;
      if (expected_valid) {
        const int32_t d_expected = (int32_t)(d->rtp - expected_rtp);
        if (d_expected == 0) {
          klass = 0;
          distance = 0;
        } else if (d_expected > 0) {
          klass = 1;
          distance = d_expected;
        } else if ((int64_t)d_expected + (int64_t)frame_samples > 0) {
          klass = 2;
          distance = -d_expected;
        } else {
          continue;
        }
      } else {
        if (playhead_delta <= 0 &&
            (int64_t)playhead_delta + (int64_t)frame_samples > 0) {
          klass = 0;
          distance = -playhead_delta;
        } else if (playhead_delta > 0) {
          klass = 1;
          distance = playhead_delta;
        } else {
          continue;
        }
      }

      if (best_slot == AP2_STORE_INVALID_INDEX || klass < best_class ||
          (klass == best_class && distance < best_distance) ||
          (klass == best_class && distance == best_distance &&
           d->arrival_id > best_arrival)) {
        best_slot = i;
        best_class = klass;
        best_distance = distance;
        best_arrival = d->arrival_id;
      }
    }
    AUDIO_DIAG_TRANSPORT_CURSOR_RECOVERY_RESULT(
        best_slot != AP2_STORE_INVALID_INDEX ? 1U : 0U,
        best_slot != AP2_STORE_INVALID_INDEX ? t->packets[best_slot].rtp : 0U,
        best_slot != AP2_STORE_INVALID_INDEX ? best_distance : 0,
        best_slot != AP2_STORE_INVALID_INDEX ? (uint32_t)best_class : 0U);
    if (best_slot == AP2_STORE_INVALID_INDEX) {
      recovery_miss_record_locked(
          t, wanted_rtp, expected_rtp, expected_seq, expected_valid,
          frame_samples, max_lead_samples, media_generation, future_valid,
          future_wanted_rtp);
    } else {
      t->recovery_miss_valid = false;
    }
  } else if (best_slot == AP2_STORE_INVALID_INDEX) {
    AUDIO_DIAG_TRANSPORT_CURSOR_NO_READY();
  }

  /* A genuine acquire miss can require a full descriptor scan. Control may
   * commit another anchor during that search, so re-check before consuming the
   * chosen READY packet. */
  if (__atomic_load_n(&t->media_generation, __ATOMIC_ACQUIRE) !=
      media_generation) {
    xSemaphoreGive(t->mutex);
    return false;
  }

  if (best_slot != AP2_STORE_INVALID_INDEX) {
    packet_desc_t *d = &t->packets[best_slot];
    ready_index_remove_locked(t, best_slot);
    if (t->count_ready) t->count_ready--;
    t->count_decoding++;
    d->state = AP2_BUFFERED_PACKET_DECODING;
    fill_ref_locked(t, best_slot, out);
    /* FLUSH can arrive while this AU is being decoded. Track its continuation
     * now, before TCP can retire the rule at untilSeq. */
    t->cursor_seq = (d->seq + 1U) & 0x007fffffU;
    t->cursor_seq_valid = true;
    t->cursor_flush_pending = packet_is_invalid_by_rule_locked(t, t->cursor_seq);
  }
  xSemaphoreGive(t->mutex);
  return best_slot != AP2_STORE_INVALID_INDEX;
}

ssize_t ap2_buffered_transport_copy_packet(ap2_buffered_transport_t *t,
                                           const ap2_buffered_packet_ref_t *ref,
                                           void *dst_, size_t dst_capacity) {
  if (!t || !ref || !dst_) return -1;
  uint8_t *dst = (uint8_t *)dst_;

  uint16_t first_page;
  uint16_t page_count;
  size_t packet_len;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  if (!ref_matches_locked(t, ref) ||
      t->packets[ref->slot].state != AP2_BUFFERED_PACKET_DECODING) {
    xSemaphoreGive(t->mutex);
    return -1;
  }
  const packet_desc_t *d = &t->packets[ref->slot];
  /* This is the early FLUSH/epoch validation point. It shares the metadata
   * lock already needed for the page snapshot, avoiding a second transport
   * lock on every normal AAC packet. A later FLUSH is still caught by the
   * publish_mutex commit barrier before decoded PCM becomes visible. */
  if (d->invalidated || d->store_epoch != t->store_epoch) {
    xSemaphoreGive(t->mutex);
    return -1;
  }
  packet_len = d->packet_len;
  first_page = d->first_page;
  page_count = d->page_count;
  xSemaphoreGive(t->mutex);

  if (packet_len > dst_capacity) return -1;

  size_t remaining = packet_len;
  size_t off = 0;
  uint16_t page = first_page;
  for (uint16_t i = 0; i < page_count && remaining > 0; ++i) {
    if (page == AP2_STORE_INVALID_INDEX) return -1;
    size_t n = remaining > AP2_STORE_PAGE_BYTES ? AP2_STORE_PAGE_BYTES : remaining;
    memcpy(dst + off, t->pages + (size_t)page * AP2_STORE_PAGE_BYTES, n);
    off += n;
    remaining -= n;
    page = t->page_next[page];
  }
  return remaining == 0 ? (ssize_t)packet_len : -1;
}

void ap2_buffered_transport_release(ap2_buffered_transport_t *t,
                                    const ap2_buffered_packet_ref_t *ref) {
  if (!t || !ref) return;
  bool released = false;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  if (ref_matches_locked(t, ref) &&
      t->packets[ref->slot].state == AP2_BUFFERED_PACKET_DECODING) {
    packet_make_free_locked(t, ref->slot);
    released = true;
  }
  xSemaphoreGive(t->mutex);
  if (released) signal_space(t);
}

uint32_t ap2_buffered_transport_reap_invalid(ap2_buffered_transport_t *t,
                                             uint32_t max_packets) {
  if (!t) return 0;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  uint32_t n = reap_invalid_locked(t, max_packets);
  xSemaphoreGive(t->mutex);
  if (n) signal_space(t);
  return n;
}

esp_err_t ap2_buffered_transport_add_invalid_seq_range(
    ap2_buffered_transport_t *t, uint32_t from_seq, uint32_t until_seq) {
  if (!t) return ESP_ERR_INVALID_ARG;
  from_seq &= 0x007fffffU;
  until_seq &= 0x007fffffU;
  uint32_t marked = 0;
  bool rule_installed = false;

  xSemaphoreTake(t->mutex, portMAX_DELAY);

  for (uint32_t i = 0; i < t->invalid_range_count; ++i) {
    const invalid_seq_range_t *r = &t->invalid_ranges[i];
    if (r->from_seq == from_seq && r->until_seq == until_seq) {
      rule_installed = true;
      break;
    }
  }

  if (!rule_installed &&
      invalid_ranges_ensure_capacity_locked(t, t->invalid_range_count + 1U)) {
    invalid_seq_range_t *r = &t->invalid_ranges[t->invalid_range_count++];
    r->from_seq = from_seq;
    r->until_seq = until_seq;
    rule_installed = true;
  }

  /* Failure must leave both current packets and future rules unchanged. */
  if (!rule_installed) {
    xSemaphoreGive(t->mutex);
    ESP_LOGE(TAG, "FLUSH rule allocation failed");
    return ESP_ERR_NO_MEM;
  }
  if (t->cursor_seq_valid &&
      seq23_delta_local(t->cursor_seq, from_seq) >= 0 &&
      seq23_delta_local(t->cursor_seq, until_seq) < 0) {
    t->cursor_flush_pending = true;
  }
  for (uint16_t i = 0; i < t->packet_count; ++i) {
    packet_desc_t *d = &t->packets[i];
    if (d->store_epoch != t->store_epoch || d->arrival_id == 0) continue;
    if (d->state != AP2_BUFFERED_PACKET_READY &&
        d->state != AP2_BUFFERED_PACKET_DECODING) {
      continue;
    }
    const int32_t from_delta = seq23_delta_local(d->seq, from_seq);
    const int32_t until_delta = seq23_delta_local(d->seq, until_seq);
    if (from_delta >= 0 && until_delta < 0) {
      if (!d->invalidated) marked++;
      d->invalidated = true;
      if (d->state == AP2_BUFFERED_PACKET_READY) {
        ready_index_remove_locked(t, i);
        if (t->count_ready) t->count_ready--;
        t->count_invalid++;
        d->state = AP2_BUFFERED_PACKET_INVALID;
      }
    }
  }
  xSemaphoreGive(t->mutex);
  if (marked) signal_space(t);
  return ESP_OK;
}

void ap2_buffered_transport_clear_invalidation_rules(
    ap2_buffered_transport_t *t) {
  if (!t) return;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  if (t->invalid_ranges && t->invalid_range_capacity) {
    memset(t->invalid_ranges, 0,
           (size_t)t->invalid_range_capacity * sizeof(*t->invalid_ranges));
  }
  t->invalid_range_count = 0;
  t->invalid_before_active = false;
  t->invalidate_all_active = false;
  xSemaphoreGive(t->mutex);
}

uint32_t ap2_buffered_transport_invalidate_before_seq(
    ap2_buffered_transport_t *t, uint32_t until_seq) {
  if (!t) return 0;
  until_seq &= 0x007fffffU;
  uint32_t marked = 0;

  xSemaphoreTake(t->mutex, portMAX_DELAY);
  t->invalid_before_active = true;
  t->invalid_before_seq = until_seq;
  if (t->cursor_seq_valid && seq23_delta_local(t->cursor_seq, until_seq) < 0)
    t->cursor_flush_pending = true;
  for (uint16_t i = 0; i < t->packet_count; ++i) {
    packet_desc_t *d = &t->packets[i];
    if (d->store_epoch != t->store_epoch || d->arrival_id == 0) continue;
    if (d->state != AP2_BUFFERED_PACKET_READY &&
        d->state != AP2_BUFFERED_PACKET_DECODING) {
      continue;
    }
    if (seq23_delta_local(d->seq, until_seq) < 0) {
      if (!d->invalidated) marked++;
      d->invalidated = true;
      if (d->state == AP2_BUFFERED_PACKET_READY) {
        ready_index_remove_locked(t, i);
        if (t->count_ready) t->count_ready--;
        t->count_invalid++;
        d->state = AP2_BUFFERED_PACKET_INVALID;
      }
    }
  }
  xSemaphoreGive(t->mutex);
  if (marked) signal_space(t);
  return marked;
}

uint32_t ap2_buffered_transport_invalidate_all(ap2_buffered_transport_t *t) {
  if (!t) return 0;
  uint32_t marked = 0;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  t->invalidate_all_active = true;
  if (t->cursor_seq_valid) t->cursor_flush_pending = true;
  for (uint16_t i = 0; i < t->packet_count; ++i) {
    packet_desc_t *d = &t->packets[i];
    if (d->store_epoch != t->store_epoch || d->arrival_id == 0) continue;
    if (d->state != AP2_BUFFERED_PACKET_READY &&
        d->state != AP2_BUFFERED_PACKET_DECODING) {
      continue;
    }
    if (!d->invalidated) marked++;
    d->invalidated = true;
    if (d->state == AP2_BUFFERED_PACKET_READY) {
      ready_index_remove_locked(t, i);
      if (t->count_ready) t->count_ready--;
      t->count_invalid++;
      d->state = AP2_BUFFERED_PACKET_INVALID;
    }
  }
  xSemaphoreGive(t->mutex);
  if (marked) signal_space(t);
  return marked;
}

void ap2_buffered_transport_begin_media_update(ap2_buffered_transport_t *t) {
  if (t) xSemaphoreTake(t->mutex, portMAX_DELAY);
}

void ap2_buffered_transport_end_media_update(ap2_buffered_transport_t *t,
                                             uint32_t revision) {
  if (!t) return;
  t->media_floor_valid = false;
  __atomic_store_n(&t->media_generation, revision, __ATOMIC_RELEASE);
  xSemaphoreGive(t->mutex);
}

void ap2_buffered_transport_set_media_generation(
    ap2_buffered_transport_t *t, uint32_t revision) {
  ap2_buffered_transport_begin_media_update(t);
  ap2_buffered_transport_end_media_update(t, revision);
}

void ap2_buffered_transport_reset_media_floor(ap2_buffered_transport_t *t) {
  if (!t) return;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  t->media_floor_valid = false;
  t->media_floor_generation =
      __atomic_load_n(&t->media_generation, __ATOMIC_ACQUIRE);
  t->media_floor_rtp = 0;
  t->media_floor_frame_samples = 0;
  xSemaphoreGive(t->mutex);
}

bool ap2_buffered_transport_ref_is_invalid(
    ap2_buffered_transport_t *t, const ap2_buffered_packet_ref_t *ref) {
  if (!t || !ref) return true;
  bool invalid = true;
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  if (ref_matches_locked(t, ref)) {
    const packet_desc_t *d = &t->packets[ref->slot];
    invalid = d->invalidated || d->store_epoch != t->store_epoch;
  }
  xSemaphoreGive(t->mutex);
  return invalid;
}

size_t ap2_buffered_transport_capacity(ap2_buffered_transport_t *t) {
  return t ? t->capacity_bytes : 0U;
}

void ap2_buffered_transport_get_usage(ap2_buffered_transport_t *t,
                                      ap2_buffered_transport_usage_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!t) return;

  /* One constant-time snapshot every two seconds. Do not add counters to the
   * TCP/decode path just for logging. */
  xSemaphoreTake(t->mutex, portMAX_DELAY);
  out->capacity_bytes = t->capacity_bytes;
  out->used_bytes =
      (size_t)(t->page_count - t->free_pages) * AP2_STORE_PAGE_BYTES;
  out->ready_packets = t->count_ready;
  out->decoding_packets = t->count_decoding;
  out->invalid_packets = t->count_invalid;
  out->free_packets = t->free_packet_top;
  xSemaphoreGive(t->mutex);
}
