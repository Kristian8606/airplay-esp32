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
  uint32_t ssrc;
  size_t packet_len;
  bool have_transport_prev;
  uint32_t transport_prev_seq;
  uint32_t transport_prev_rtp;
} ap2_buffered_packet_ref_t;

typedef struct {
  uint32_t ready_total;
  uint32_t ready_stale;
  uint32_t ready_overlap;
  uint32_t ready_forward_window;
  uint32_t ready_future;
  uint32_t decoding_total;
  uint32_t invalid_total;
  uint32_t writing_total;

  uint32_t exact_expected_rtp_ready;
  uint32_t exact_expected_rtp_decoding;
  uint32_t exact_expected_rtp_invalid;
  uint32_t exact_expected_seq_ready;
  uint32_t exact_expected_seq_decoding;
  uint32_t exact_expected_seq_invalid;

  bool expected_seq_invalid_by_rule;
  bool expected_rule_is_range;
  uint32_t expected_rule_from_seq;
  uint32_t expected_rule_until_seq;

  bool nearest_before_valid;
  uint32_t nearest_before_seq;
  uint32_t nearest_before_rtp;
  int32_t nearest_before_delta;

  bool nearest_after_valid;
  uint32_t nearest_after_seq;
  uint32_t nearest_after_rtp;
  int32_t nearest_after_delta;

  bool nearest_expected_valid;
  uint32_t nearest_expected_seq;
  uint32_t nearest_expected_rtp;
  int32_t nearest_expected_delta;

  bool transport_last_valid;
  uint32_t transport_last_seq;
  uint32_t transport_last_rtp;
  uint32_t invalidation_rules;
  uint32_t invalidation_rule_capacity;
  uint32_t free_packet_slots;
  uint32_t free_pages;
} ap2_buffered_transport_media_diag_t;

typedef struct {
  uint64_t socket_bytes;
  uint64_t packet_bytes_released;
  size_t store_payload_bytes;
  size_t store_allocated_bytes;
  size_t store_high_water;
  uint64_t packets_received_total;
  uint32_t packets_ready;
  uint32_t packets_decoding;
  uint32_t packets_invalid;
  uint64_t stale_ready_reaped;
  uint32_t invalidation_rules;
  uint32_t free_packet_slots;
  uint32_t free_pages;
  uint32_t invalidation_rule_capacity;
  uint64_t invalidation_rule_grows;
  uint64_t invalidation_rule_alloc_failures;
  uint64_t invalidation_rules_retired;
} ap2_buffered_transport_stats_t;

esp_err_t ap2_buffered_transport_create(ap2_buffered_transport_t **out,
                                        const ap2_buffered_transport_config_t *cfg);
void ap2_buffered_transport_destroy(ap2_buffered_transport_t *t);
esp_err_t ap2_buffered_transport_start(ap2_buffered_transport_t *t,
                                       uint16_t requested_port,
                                       uint16_t *bound_port);
void ap2_buffered_transport_stop(ap2_buffered_transport_t *t);

/* New codec/session boundary. READY/INVALID packets are immediately reusable.
 * WRITING/DECODING retain ownership only until the in-flight
 * operation exits. PCM is never touched here. Persistent invalidation rules
 * are cleared because they belong to the old transport epoch. */
void ap2_buffered_transport_clear(ap2_buffered_transport_t *t);

/* TCP packets are published directly from WRITING to READY/INVALID after the
 * payload header is parsed. Transport arrival order is retained only as
 * diagnostic predecessor metadata inside each packet descriptor; it is not an
 * ownership queue and cannot gate decode. */
bool ap2_buffered_transport_mark_invalid(ap2_buffered_transport_t *t,
                                         const ap2_buffered_packet_ref_t *ref);

/* Select the best READY packet for the media timeline.
 *
 * - wanted_rtp is the sample position required by the active PTP playhead.
 * - expected_rtp is the decoder's next contiguous media position when valid.
 * - frame_samples defines stale/overlap bounds.
 * - max_lead_samples keeps decode bounded around the active playhead.
 *
 * Exact expected RTP uses the READY hash index (O(1) average). Only when that
 * media position is absent do we perform a bounded full-store fallback search
 * for the nearest forward/overlap candidate. At equal RTP the newest arrival
 * wins so replacement data supersedes an older duplicate. Transport arrival
 * order never determines decode order. */
bool ap2_buffered_transport_acquire_media_next(
    ap2_buffered_transport_t *t, uint32_t wanted_rtp, uint32_t expected_rtp,
    bool expected_valid, uint32_t frame_samples, int32_t max_lead_samples,
    uint32_t media_generation, ap2_buffered_packet_ref_t *out);

/* DECODING -> READY. Used by the reorder guard when a forward media jump is
 * visible but there is still enough decoded PCM time to wait for a missing
 * replacement packet. */
bool ap2_buffered_transport_defer_decode(ap2_buffered_transport_t *t,
                                         const ap2_buffered_packet_ref_t *ref);

/* Gather a DECODING packet into the codec scratch buffer. TCP wrote directly
 * into page storage; this is the one contiguous copy required by the current
 * crypto/AAC decoder API. */
ssize_t ap2_buffered_transport_copy_packet(ap2_buffered_transport_t *t,
                                           const ap2_buffered_packet_ref_t *ref,
                                           void *dst, size_t dst_capacity);

/* DECODING -> FREE after successful use/drop. Payload bytes are not zeroed. */
void ap2_buffered_transport_release(ap2_buffered_transport_t *t,
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

/* Publish the timing generation without taking the transport mutex. Safe to
 * call while the receiver state critical section is held. */
void ap2_buffered_transport_set_media_generation(
    ap2_buffered_transport_t *t, uint32_t generation);

/* Forget the writer-side stale-GC playhead hint at a new timeline anchor. */
void ap2_buffered_transport_reset_media_floor(ap2_buffered_transport_t *t);

bool ap2_buffered_transport_ref_is_invalid(
    ap2_buffered_transport_t *t, const ap2_buffered_packet_ref_t *ref);

void ap2_buffered_transport_get_stats(ap2_buffered_transport_t *t,
                                      ap2_buffered_transport_stats_t *out);

/* Diagnostic snapshot of the compressed media neighbourhood. It is read-only
 * and intended for rare cursor/miss diagnostics, not the steady-state path. */
void ap2_buffered_transport_get_media_diag(
    ap2_buffered_transport_t *t, uint32_t wanted_rtp, uint32_t expected_rtp,
    uint32_t expected_seq, bool expected_valid, uint32_t frame_samples,
    int32_t max_lead_samples, ap2_buffered_transport_media_diag_t *out);

size_t ap2_buffered_transport_capacity(ap2_buffered_transport_t *t);
