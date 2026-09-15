#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

/*
 * Buffered AirPlay TCP transport.
 *
 * Payload bytes live in a page-backed addressable store, not in a byte FIFO.
 * The reader parses the 2-byte AirPlay block length, reserves final storage
 * for the packet and recv()s the payload directly into that storage.
 *
 * Packet ownership is explicit. Control can invalidate already received data
 * without erasing it; decode can inspect packets in media order; GC merely
 * returns pages to the writer. The enum is intentionally extensible because
 * additional timeline states may be introduced without changing payload
 * storage or reintroducing FIFO ownership.
 */
typedef enum {
  AP2_BUFFERED_PACKET_FREE = 0,
  AP2_BUFFERED_PACKET_WRITING,
  AP2_BUFFERED_PACKET_READY,
  AP2_BUFFERED_PACKET_DECODING,
  AP2_BUFFERED_PACKET_INVALID,
} ap2_buffered_packet_state_t;

typedef struct ap2_buffered_transport ap2_buffered_transport_t;

typedef struct {
  size_t store_bytes;
  int task_core;
  int task_priority;
  uint32_t task_stack;
} ap2_buffered_transport_config_t;

typedef struct {
  uint16_t slot;
  uint64_t arrival_id;
  uint32_t seq;
  uint32_t rtp;
  size_t packet_len;
} ap2_buffered_packet_ref_t;

esp_err_t ap2_buffered_transport_create(ap2_buffered_transport_t **out,
                                        const ap2_buffered_transport_config_t *cfg);

/* Create the normal page-backed transport while using caller-owned storage
 * for the compressed payload pages. Descriptor/hash/free-list metadata keeps
 * its existing allocation/ownership. The supplied storage must remain valid
 * for the entire lifetime of the transport and must not be used by another
 * codec until ap2_buffered_transport_is_idle() is true.
 *
 * This is used by the audio engine to let buffered AAC and realtime ALAC
 * reuse the same large PSRAM backing store without changing either transport
 * algorithm. */
esp_err_t ap2_buffered_transport_create_with_payload_storage(
    ap2_buffered_transport_t **out,
    const ap2_buffered_transport_config_t *cfg,
    void *payload_storage, size_t payload_storage_bytes);
/* Call only with the consumer stopped. If shutdown times out or DECODING
 * ownership remains, destruction is deferred; retain the pointer and retry. */
void ap2_buffered_transport_destroy(ap2_buffered_transport_t *t);
esp_err_t ap2_buffered_transport_start(ap2_buffered_transport_t *t,
                                       uint16_t requested_port,
                                       uint16_t *bound_port);
void ap2_buffered_transport_stop(ap2_buffered_transport_t *t);

/* True only when no TCP reader and no decoder still own payload storage.
 * A codec sharing the payload backing memory may acquire it only in this
 * state. */
bool ap2_buffered_transport_is_idle(ap2_buffered_transport_t *t);

/* New codec/session boundary. READY/INVALID packets are immediately reusable.
 * WRITING/DECODING retain ownership only until the in-flight
 * operation exits. PCM is never touched here. Persistent invalidation rules
 * are cleared because they belong to the old transport epoch. */
void ap2_buffered_transport_clear(ap2_buffered_transport_t *t);

/* TCP packets are published directly from WRITING to READY/INVALID after the
 * payload header is parsed. Transport arrival order does not gate
 * ownership or decode. */

/* Select the best READY packet for the media timeline.
 *
 * - wanted_rtp is the sample position required by the active PTP playhead.
 * - expected_rtp/expected_seq identify the decoder's next contiguous media
 *   position when expected_valid is true.
 * - allow_recovery_scan is false during normal playback while the missing
 *   continuation is still outside the reorder guard. In that hot path an
 *   absent exact RTP returns immediately after the O(1) hash lookup.
 * - a FLUSH notification for expected_seq survives rule retirement, overrides
 *   that wait and
 *   permits recovery immediately, because that exact continuation can no
 *   longer become valid.
 * - media_generation is the receiver anchor revision, not its playout epoch.
 * - frame_samples defines stale/overlap bounds and max_lead_samples keeps
 *   decode bounded around the active playhead.
 *
 * Full descriptor scans are therefore recovery-only: startup/seek/new media
 * neighbourhood, FLUSH-invalid continuity, or the reorder deadline. At equal
 * RTP the newest arrival wins so replacement data supersedes an older
 * duplicate. Transport arrival order never determines decode order. */
bool ap2_buffered_transport_acquire_media_next(
    ap2_buffered_transport_t *t, uint32_t wanted_rtp, uint32_t expected_rtp,
    uint32_t expected_seq, bool expected_valid, bool allow_recovery_scan,
    uint32_t frame_samples, int32_t max_lead_samples,
    uint32_t media_generation, ap2_buffered_packet_ref_t *out);

/* Gather a DECODING packet into the codec scratch buffer. TCP wrote directly
 * into page storage; this is the one contiguous copy required by the current
 * crypto/AAC decoder API. */
ssize_t ap2_buffered_transport_copy_packet(ap2_buffered_transport_t *t,
                                           const ap2_buffered_packet_ref_t *ref,
                                           void *dst, size_t dst_capacity);

/* DECODING -> FREE after successful use/drop. Payload bytes are not zeroed. */
void ap2_buffered_transport_release(ap2_buffered_transport_t *t,
                                    const ap2_buffered_packet_ref_t *ref);

/* Abandon decoder chronology after a cursor move, retaining valid compressed
 * media for later selection. Invalidated/old-session refs are freed instead. */
void ap2_buffered_transport_return_packet(ap2_buffered_transport_t *t,
                                          const ap2_buffered_packet_ref_t *ref);

/* INVALID -> FREE. Writer also reaps lazily when storage is needed, so GC does
 * not gate playout. */
uint32_t ap2_buffered_transport_reap_invalid(ap2_buffered_transport_t *t,
                                             uint32_t max_packets);

/* Declarative timeline invalidation.
 *
 * add_invalid_seq_range installs [from_seq, until_seq) and retroactively marks
 * matching packets already in the addressable store. Future matching packets
 * are invalidated at publish time before they can become decode candidates.
 * Because TCP preserves block arrival order, the range retires automatically
 * when until_seq (the exclusive endpoint) is published or overshot.
 *
 * Immediate FLUSH uses the same declarative model: invalidate_before_seq()
 * installs a persistent "seq < until" rule, while invalidate_all() rejects all
 * buffered packets until the next anchor commits. These rules apply both
 * retroactively and to future arrivals. clear_invalidation_rules() retires the
 * control rules at the new anchor; packets already marked INVALID stay invalid. */
/* Returns ESP_OK only when the future rule is installed; failure is atomic. */
esp_err_t ap2_buffered_transport_add_invalid_seq_range(
    ap2_buffered_transport_t *t, uint32_t from_seq, uint32_t until_seq);
uint32_t ap2_buffered_transport_invalidate_before_seq(
    ap2_buffered_transport_t *t, uint32_t until_seq);
uint32_t ap2_buffered_transport_invalidate_all(ap2_buffered_transport_t *t);
void ap2_buffered_transport_clear_invalidation_rules(
    ap2_buffered_transport_t *t);

/* Anchor revision, distinct from the playout generation. Increment on EVERY
 * buffered anchor update. begin/end serialize anchor commit with pressure GC.
 * Acquire before entering receiver state_mux; end after leaving it. Never wait
 * for a transport mutex from inside a critical section. A committed timeline
 * passes retire_invalidation_rules=true so rule retirement and the new
 * revision become visible together, before TCP publication can resume. */
void ap2_buffered_transport_begin_media_update(ap2_buffered_transport_t *t);
void ap2_buffered_transport_end_media_update(ap2_buffered_transport_t *t,
                                             uint32_t revision,
                                             bool retire_invalidation_rules);
/* Standalone update (takes the transport mutex). */
void ap2_buffered_transport_set_media_generation(
    ap2_buffered_transport_t *t, uint32_t generation);

/* Forget the writer-side stale-GC playhead hint at a new timeline anchor. */
void ap2_buffered_transport_reset_media_floor(ap2_buffered_transport_t *t);

bool ap2_buffered_transport_ref_is_invalid(
    ap2_buffered_transport_t *t, const ap2_buffered_packet_ref_t *ref);



size_t ap2_buffered_transport_capacity(ap2_buffered_transport_t *t);

/* Low-frequency diagnostics snapshot. This is intentionally pulled by the
 * low-priority audio status task instead of being maintained in the packet
 * hot path. */
typedef struct {
  size_t capacity_bytes;
  size_t used_bytes;
  uint32_t ready_packets;
  uint32_t decoding_packets;
  uint32_t invalid_packets;
  uint32_t free_packets;
} ap2_buffered_transport_usage_t;

void ap2_buffered_transport_get_usage(ap2_buffered_transport_t *t,
                                      ap2_buffered_transport_usage_t *out);

/* One AAC consumer; persistent binary wake hint, safe across codec switches. */
void ap2_buffered_transport_notify_media(ap2_buffered_transport_t *transport);
void ap2_buffered_transport_wait_media(ap2_buffered_transport_t *transport,
                                       uint32_t timeout_ms);
