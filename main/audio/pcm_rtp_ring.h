#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Direct RTP-addressed PCM store.
 *
 * 256 slots x 1024 stereo frames = 262144 frames = 5.94 s @ 44.1 kHz.
 * A slot is keyed by absolute RTP page base (rtp & ~1023). RTP itself is the
 * address: no sorting, no scan, no linked list, no binary search.
 *
 * An arbitrary 1024-frame AAC AU touches at most two slots. Partial slots are
 * tracked with a 1024-bit validity bitmap so missing PCM is never mistaken for
 * valid audio.
 */

#define PCM_RTP_SLOT_FRAMES 1024U
#define PCM_RTP_SLOT_COUNT  256U
#define PCM_RTP_RING_FRAMES (PCM_RTP_SLOT_FRAMES * PCM_RTP_SLOT_COUNT)
#define PCM_RTP_CHANNELS    2U

typedef struct pcm_rtp_ring pcm_rtp_ring_t;

typedef struct {
  uint32_t generation;
  uint32_t tagged_slots;
  uint64_t slot_writes;
  uint64_t future_collisions;
  uint64_t unanchored_replacements;
} pcm_rtp_ring_stats_t;

esp_err_t pcm_rtp_ring_create(pcm_rtp_ring_t **out);
void pcm_rtp_ring_destroy(pcm_rtp_ring_t *ring);

/* O(1) invalidation: old PCM becomes unreachable by generation tag. */
void pcm_rtp_ring_set_generation(pcm_rtp_ring_t *ring, uint32_t generation);

/*
 * Write decoded stereo PCM at its exact RTP address.
 *
 * If wanted_valid is true, a write is rejected rather than overwriting a
 * different slot page that still contains future PCM from the same generation.
 * This keeps TCP independent from ring capacity: callers may drop that far-
 * future frame and continue receiving instead of blocking the socket.
 */
bool pcm_rtp_ring_write(pcm_rtp_ring_t *ring, uint32_t first_rtp,
                        const int16_t *pcm, size_t frames, int channels,
                        uint32_t generation, uint32_t wanted_rtp,
                        bool wanted_valid);

/* Exact RTP lookup for an arbitrary contiguous stereo range. */
bool pcm_rtp_ring_read(const pcm_rtp_ring_t *ring, uint32_t first_rtp,
                       uint32_t frames, uint32_t generation,
                       int16_t *out_stereo);

/* Playout fast path: exact RTP lookup, at most two memcpy operations. */
bool pcm_rtp_ring_read_256(const pcm_rtp_ring_t *ring, uint32_t first_rtp,
                           uint32_t generation, int16_t *out_stereo_256);

/* O(number of touched 1024-frame pages) readiness check used only while
 * priming a new timeline. No PCM is copied. */
bool pcm_rtp_ring_has_range(const pcm_rtp_ring_t *ring, uint32_t first_rtp,
                            uint32_t frames, uint32_t generation);


/* Rare control-path operation used by FLUSHBUFFERED. Clears PCM validity in
 * [from_rtp, until_rtp) for the current generation without changing timeline. */
void pcm_rtp_ring_invalidate_range(pcm_rtp_ring_t *ring, uint32_t from_rtp,
                                   uint32_t until_rtp, uint32_t generation);

void pcm_rtp_ring_get_stats(const pcm_rtp_ring_t *ring,
                            pcm_rtp_ring_stats_t *out);
