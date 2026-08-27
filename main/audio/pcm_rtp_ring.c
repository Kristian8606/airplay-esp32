#include "pcm_rtp_ring.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define PCM_SLOT_SAMPLES (PCM_RTP_SLOT_FRAMES * PCM_RTP_CHANNELS)
#define PCM_RING_MASK    (PCM_RTP_SLOT_COUNT - 1U)
#define VALID_WORDS      (PCM_RTP_SLOT_FRAMES / 32U)

_Static_assert((PCM_RTP_SLOT_COUNT & (PCM_RTP_SLOT_COUNT - 1U)) == 0,
               "PCM_RTP_SLOT_COUNT must be power of two");
_Static_assert((PCM_RTP_SLOT_FRAMES & (PCM_RTP_SLOT_FRAMES - 1U)) == 0,
               "PCM_RTP_SLOT_FRAMES must be power of two");
_Static_assert((PCM_RTP_SLOT_FRAMES % 32U) == 0,
               "PCM_RTP_SLOT_FRAMES must be multiple of 32");

typedef struct {
  /* Seqlock: odd while writer changes PCM/tag/validity, even when stable. */
  volatile uint32_t seq;
  uint32_t page_rtp;            /* absolute RTP aligned down to 1024 */
  uint32_t generation;
  uint32_t valid[VALID_WORDS];  /* one bit per stereo PCM frame */
} pcm_slot_tag_t;

struct pcm_rtp_ring {
  int16_t *pcm;
  pcm_slot_tag_t *tags;
  uint32_t generation;
  uint32_t tagged_slots;
  uint64_t slot_writes;
  uint64_t future_collisions;
  uint64_t unanchored_replacements;
};

static const char *TAG = "pcm_rtp_ring";

static inline uint32_t page_base(uint32_t rtp) {
  return rtp & ~(PCM_RTP_SLOT_FRAMES - 1U);
}

static inline uint32_t slot_for_page(uint32_t page_rtp) {
  return (page_rtp >> 10) & PCM_RING_MASK;
}

static inline int32_t rtp_delta(uint32_t a, uint32_t b) {
  return (int32_t)(a - b);
}

static void validity_clear(uint32_t valid[VALID_WORDS]) {
  memset(valid, 0, VALID_WORDS * sizeof(valid[0]));
}

static void validity_set_range(uint32_t valid[VALID_WORDS], uint32_t off,
                               uint32_t count) {
  while (count) {
    uint32_t word = off >> 5;
    uint32_t bit = off & 31U;
    uint32_t n = 32U - bit;
    if (n > count) {
      n = count;
    }
    uint32_t mask =
        (n == 32U) ? 0xFFFFFFFFU : (((1U << n) - 1U) << bit);
    valid[word] |= mask;
    off += n;
    count -= n;
  }
}


static void validity_clear_range(uint32_t valid[VALID_WORDS], uint32_t off,
                                 uint32_t count) {
  while (count) {
    uint32_t word = off >> 5;
    uint32_t bit = off & 31U;
    uint32_t n = 32U - bit;
    if (n > count) {
      n = count;
    }
    uint32_t mask =
        (n == 32U) ? 0xFFFFFFFFU : (((1U << n) - 1U) << bit);
    valid[word] &= ~mask;
    off += n;
    count -= n;
  }
}

static bool validity_has_range(const uint32_t valid[VALID_WORDS], uint32_t off,
                               uint32_t count) {
  while (count) {
    uint32_t word = off >> 5;
    uint32_t bit = off & 31U;
    uint32_t n = 32U - bit;
    if (n > count) {
      n = count;
    }
    uint32_t mask =
        (n == 32U) ? 0xFFFFFFFFU : (((1U << n) - 1U) << bit);
    if ((valid[word] & mask) != mask) {
      return false;
    }
    off += n;
    count -= n;
  }
  return true;
}

esp_err_t pcm_rtp_ring_create(pcm_rtp_ring_t **out) {
  if (!out) {
    return ESP_ERR_INVALID_ARG;
  }
  *out = NULL;

  pcm_rtp_ring_t *r = calloc(1, sizeof(*r));
  if (!r) {
    return ESP_ERR_NO_MEM;
  }

  const size_t pcm_bytes = (size_t)PCM_RTP_RING_FRAMES * PCM_RTP_CHANNELS *
                           sizeof(int16_t);
  r->pcm = heap_caps_malloc(pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  r->tags = heap_caps_calloc(PCM_RTP_SLOT_COUNT, sizeof(pcm_slot_tag_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!r->tags) {
    r->tags = calloc(PCM_RTP_SLOT_COUNT, sizeof(pcm_slot_tag_t));
  }
  if (!r->pcm || !r->tags) {
    pcm_rtp_ring_destroy(r);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG,
           "direct RTP PCM ring: %u slots x %u frames = %u frames, %u bytes, %u ms @44.1k",
           (unsigned)PCM_RTP_SLOT_COUNT, (unsigned)PCM_RTP_SLOT_FRAMES,
           (unsigned)PCM_RTP_RING_FRAMES, (unsigned)pcm_bytes,
           (unsigned)(((uint64_t)PCM_RTP_RING_FRAMES * 1000ULL) / 44100ULL));
  *out = r;
  return ESP_OK;
}

void pcm_rtp_ring_destroy(pcm_rtp_ring_t *r) {
  if (!r) {
    return;
  }
  free(r->pcm);
  free(r->tags);
  free(r);
}

void pcm_rtp_ring_set_generation(pcm_rtp_ring_t *r, uint32_t generation) {
  if (!r) {
    return;
  }
  __atomic_store_n(&r->generation, generation, __ATOMIC_RELEASE);
  r->tagged_slots = 0;
}

static bool old_page_is_future(const pcm_slot_tag_t *tag, uint32_t wanted_rtp) {
  uint32_t end_rtp = tag->page_rtp + PCM_RTP_SLOT_FRAMES;
  return rtp_delta(end_rtp, wanted_rtp) > 0;
}

/* Acquire a slot writer token with CAS. Readers only trust even sequence
 * values; invalidate_range() uses the same odd/even ownership protocol.
 * Keep retries bounded: if a lower-priority task on the same core was
 * preempted while holding the slot, an unbounded spin here could deadlock it. */
static bool slot_writer_acquire(pcm_slot_tag_t *tag, uint32_t *seq_even) {
  for (int retry = 0; retry < 32; ++retry) {
    uint32_t seq = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq & 1U) {
      continue;
    }
    uint32_t expected = seq;
    if (__atomic_compare_exchange_n(&tag->seq, &expected, seq + 1U, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      *seq_even = seq;
      return true;
    }
  }
  return false;
}

typedef struct {
  pcm_slot_tag_t *tag;
  uint32_t slot;
  uint32_t base;
  uint32_t offset;
  uint32_t chunk;
  size_t src_frame;
  uint32_t seq_even;
  bool locked;
} pcm_write_chunk_t;

bool pcm_rtp_ring_write(pcm_rtp_ring_t *r, uint32_t first_rtp,
                        const int16_t *pcm, size_t frames, int channels,
                        uint32_t generation, uint32_t wanted_rtp,
                        bool wanted_valid) {
  if (!r || !pcm || channels != (int)PCM_RTP_CHANNELS || frames == 0 ||
      generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    return false;
  }

  /* Current producers are AAC (1024 frames) and ALAC (352 frames), therefore
   * one decoded block can touch at most two 1024-frame RTP pages. Keeping that
   * invariant explicit lets us acquire every destination page before copying,
   * so a concurrent FLUSH invalidate can never leave a partially-published AU. */
  if (frames > PCM_RTP_SLOT_FRAMES) {
    return false;
  }

  pcm_write_chunk_t chunks[2] = {0};
  unsigned chunk_count = 0;
  uint32_t cur_rtp = first_rtp;
  size_t remain = frames;
  size_t src_frame = 0;
  while (remain) {
    if (chunk_count >= 2U) {
      return false;
    }
    uint32_t base = page_base(cur_rtp);
    uint32_t offset = cur_rtp - base;
    uint32_t chunk = PCM_RTP_SLOT_FRAMES - offset;
    if ((size_t)chunk > remain) {
      chunk = (uint32_t)remain;
    }
    uint32_t slot = slot_for_page(base);
    chunks[chunk_count++] = (pcm_write_chunk_t){
        .tag = &r->tags[slot],
        .slot = slot,
        .base = base,
        .offset = offset,
        .chunk = chunk,
        .src_frame = src_frame,
    };
    src_frame += chunk;
    cur_rtp += chunk;
    remain -= chunk;
  }

  /* Acquire all destination pages first. If invalidate_range() currently owns
   * one of them, publish nothing from this decoded block. */
  for (unsigned i = 0; i < chunk_count; ++i) {
    if (!slot_writer_acquire(chunks[i].tag, &chunks[i].seq_even)) {
      for (unsigned j = 0; j < i; ++j) {
        if (chunks[j].locked) {
          __atomic_store_n(&chunks[j].tag->seq, chunks[j].seq_even + 2U,
                           __ATOMIC_RELEASE);
        }
      }
      return false;
    }
    chunks[i].locked = true;
  }

  /* A session generation can change while we are acquiring pages. Never let a
   * producer from the old timeline publish after the O(1) generation reset. */
  if (generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    for (unsigned j = 0; j < chunk_count; ++j) {
      __atomic_store_n(&chunks[j].tag->seq, chunks[j].seq_even + 2U,
                       __ATOMIC_RELEASE);
    }
    return false;
  }

  /* Collision decisions must be made while we own the tags; a preflight done
   * before acquiring the seqlock can itself race with FLUSH/invalidation. */
  for (unsigned i = 0; i < chunk_count; ++i) {
    pcm_slot_tag_t *tag = chunks[i].tag;
    if (tag->generation == generation && tag->page_rtp != chunks[i].base) {
      if (wanted_valid && old_page_is_future(tag, wanted_rtp)) {
        r->future_collisions++;
        for (unsigned j = 0; j < chunk_count; ++j) {
          __atomic_store_n(&chunks[j].tag->seq, chunks[j].seq_even + 2U,
                           __ATOMIC_RELEASE);
        }
        return false;
      }
      if (!wanted_valid) {
        r->unanchored_replacements++;
      }
    }
  }

  for (unsigned i = 0; i < chunk_count; ++i) {
    pcm_write_chunk_t *w = &chunks[i];
    pcm_slot_tag_t *tag = w->tag;
    const bool replacing =
        tag->generation != generation || tag->page_rtp != w->base;

    if (replacing) {
      tag->page_rtp = w->base;
      tag->generation = generation;
      validity_clear(tag->valid);
      if (r->tagged_slots < PCM_RTP_SLOT_COUNT) {
        r->tagged_slots++;
      }
    }

    memcpy(r->pcm + ((size_t)w->slot * PCM_SLOT_SAMPLES) +
               ((size_t)w->offset * PCM_RTP_CHANNELS),
           pcm + (w->src_frame * PCM_RTP_CHANNELS),
           (size_t)w->chunk * PCM_RTP_CHANNELS * sizeof(int16_t));
    validity_set_range(tag->valid, w->offset, w->chunk);
    r->slot_writes++;
  }

  for (unsigned i = 0; i < chunk_count; ++i) {
    __atomic_store_n(&chunks[i].tag->seq, chunks[i].seq_even + 2U,
                     __ATOMIC_RELEASE);
  }
  return true;
}

static bool read_page_range(const pcm_rtp_ring_t *r, uint32_t rtp,
                            uint32_t generation, uint32_t frames,
                            int16_t *out) {
  uint32_t base = page_base(rtp);
  uint32_t offset = rtp - base;
  if (offset + frames > PCM_RTP_SLOT_FRAMES) {
    return false;
  }

  uint32_t slot = slot_for_page(base);
  const pcm_slot_tag_t *tag = &r->tags[slot];
  for (int retry = 0; retry < 2; ++retry) {
    uint32_t seq1 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 & 1U) {
      continue;
    }
    if (tag->generation != generation || tag->page_rtp != base ||
        !validity_has_range(tag->valid, offset, frames)) {
      return false;
    }

    memcpy(out,
           r->pcm + ((size_t)slot * PCM_SLOT_SAMPLES) +
               ((size_t)offset * PCM_RTP_CHANNELS),
           (size_t)frames * PCM_RTP_CHANNELS * sizeof(int16_t));

    uint32_t seq2 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 == seq2 && !(seq2 & 1U)) {
      return true;
    }
  }
  return false;
}

static bool has_page_range(const pcm_rtp_ring_t *r, uint32_t rtp,
                           uint32_t generation, uint32_t frames) {
  uint32_t base = page_base(rtp);
  uint32_t offset = rtp - base;
  if (offset + frames > PCM_RTP_SLOT_FRAMES) {
    return false;
  }

  uint32_t slot = slot_for_page(base);
  const pcm_slot_tag_t *tag = &r->tags[slot];
  for (int retry = 0; retry < 2; ++retry) {
    uint32_t seq1 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 & 1U) {
      continue;
    }
    if (tag->generation != generation || tag->page_rtp != base ||
        !validity_has_range(tag->valid, offset, frames)) {
      return false;
    }
    uint32_t seq2 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 == seq2 && !(seq2 & 1U)) {
      return true;
    }
  }
  return false;
}

bool pcm_rtp_ring_has_range(const pcm_rtp_ring_t *r, uint32_t first_rtp,
                            uint32_t frames, uint32_t generation) {
  if (!r || frames == 0 || generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    return false;
  }
  uint32_t cur = first_rtp;
  uint32_t remain = frames;
  while (remain) {
    uint32_t offset = cur & (PCM_RTP_SLOT_FRAMES - 1U);
    uint32_t chunk = PCM_RTP_SLOT_FRAMES - offset;
    if (chunk > remain) {
      chunk = remain;
    }
    if (!has_page_range(r, cur, generation, chunk)) {
      return false;
    }
    cur += chunk;
    remain -= chunk;
  }
  return true;
}

bool pcm_rtp_ring_read(const pcm_rtp_ring_t *r, uint32_t first_rtp,
                       uint32_t frames, uint32_t generation, int16_t *out) {
  if (!r || !out || frames == 0 || generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    return false;
  }

  uint32_t cur = first_rtp;
  uint32_t remain = frames;
  size_t out_frame = 0;
  while (remain) {
    uint32_t offset = cur & (PCM_RTP_SLOT_FRAMES - 1U);
    uint32_t chunk = PCM_RTP_SLOT_FRAMES - offset;
    if (chunk > remain) {
      chunk = remain;
    }
    if (!read_page_range(r, cur, generation, chunk,
                         out + out_frame * PCM_RTP_CHANNELS)) {
      return false;
    }
    cur += chunk;
    remain -= chunk;
    out_frame += chunk;
  }
  return true;
}

bool pcm_rtp_ring_read_256(const pcm_rtp_ring_t *r, uint32_t first_rtp,
                           uint32_t generation, int16_t *out) {
  return pcm_rtp_ring_read(r, first_rtp, 256U, generation, out);
}


void pcm_rtp_ring_invalidate_range(pcm_rtp_ring_t *r, uint32_t from_rtp,
                                   uint32_t until_rtp, uint32_t generation) {
  if (!r || (int32_t)(until_rtp - from_rtp) <= 0) {
    return;
  }

  uint32_t cur = from_rtp;
  uint32_t remaining = until_rtp - from_rtp;
  while (remaining) {
    uint32_t base = page_base(cur);
    uint32_t off = cur - base;
    uint32_t chunk = PCM_RTP_SLOT_FRAMES - off;
    if (chunk > remaining) {
      chunk = remaining;
    }

    pcm_slot_tag_t *tag = &r->tags[slot_for_page(base)];
    uint32_t seq0 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (!(seq0 & 1U) && tag->generation == generation && tag->page_rtp == base) {
      uint32_t expected = seq0;
      if (__atomic_compare_exchange_n(&tag->seq, &expected, seq0 + 1U,
                                      false, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE)) {
        validity_clear_range(tag->valid, off, chunk);
        __atomic_store_n(&tag->seq, seq0 + 2U, __ATOMIC_RELEASE);
      }
    }

    cur += chunk;
    remaining -= chunk;
  }
}

void pcm_rtp_ring_get_stats(const pcm_rtp_ring_t *r,
                            pcm_rtp_ring_stats_t *out) {
  if (!r || !out) {
    return;
  }
  out->generation = __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE);
  out->tagged_slots = r->tagged_slots;
  out->slot_writes = r->slot_writes;
  out->future_collisions = r->future_collisions;
  out->unanchored_replacements = r->unanchored_replacements;
}
