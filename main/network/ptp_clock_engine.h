#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool source_changed;
  bool gm_changed;
  bool first_domain;
  uint64_t old_source_clock_id;
  uint64_t old_grandmaster_clock_id;
  uint64_t new_source_clock_id;
  uint64_t new_grandmaster_clock_id;
  bool old_estimator_valid;
  int64_t old_raw_offset_ns;
  int64_t old_filtered_offset_ns;
  uint32_t old_accepted_samples;
  uint32_t epoch;
} ptp_clock_engine_domain_event_t;

typedef struct {
  bool accepted;
  bool outlier;
  int64_t raw_offset_ns;
  int64_t filtered_offset_ns;
  int64_t filtered_step_ns;
  int64_t raw_filter_delta_ns;
  uint32_t accepted_samples;
  uint32_t epoch;
} ptp_clock_engine_sample_t;

typedef struct {
  bool valid;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t epoch;

  int64_t raw_offset_ns;
  int64_t filtered_offset_ns;
  int64_t previous_offset_ns;
  int64_t previous_sample_time_ns;
  int64_t mastership_start_time_ns;
  int64_t last_accepted_time_ns;
  uint32_t accepted_samples;

  int64_t outlier_threshold_ns;
} ptp_clock_engine_t;

void ptp_clock_engine_init(ptp_clock_engine_t *engine,
                           int64_t outlier_threshold_ns);

/* Set the currently observed PTP source / grandmaster domain. A first domain
 * starts epoch 1; a later source or GM change increments the epoch and resets
 * only estimator history. Returns true when the domain record changed. */
bool ptp_clock_engine_set_domain(ptp_clock_engine_t *engine,
                                 uint64_t source_clock_id,
                                 uint64_t grandmaster_clock_id,
                                 ptp_clock_engine_domain_event_t *event_out);

/* Reset smoothing/history while preserving the current source/GM identity.
 * Bumps the estimator epoch so readers can distinguish pre/post-reset state. */
void ptp_clock_engine_reset_filter(ptp_clock_engine_t *engine,
                                   bool bump_epoch);

/* NQPTP-derived passive one-way estimator:
 *  - first sample accepted directly;
 *  - positive jitter accepted fully during first second, then at 1/16;
 *  - negative jitter ignored during first second, then clamped to -2.5 ms and
 *    applied at 1/256;
 *  - optional gross-outlier guard rejects a sample without updating freshness
 *    or accepted-sample count. */
ptp_clock_engine_sample_t ptp_clock_engine_add_sample(
    ptp_clock_engine_t *engine, int64_t raw_offset_ns,
    int64_t reception_time_ns);

/* Convert between remote PTP time and ESP-local monotonic time. */
bool ptp_clock_engine_remote_to_local(uint64_t remote_ns, int64_t offset_ns,
                                      uint64_t *local_ns);
bool ptp_clock_engine_local_to_remote(uint64_t local_ns, int64_t offset_ns,
                                      uint64_t *remote_ns);

/* Shairport-style buffered handover qualification. A transient GM must never
 * move the media anchor; only a qualified, different master may take it over. */
bool ptp_clock_engine_handover_ready(bool have_local_anchor, bool clock_locked,
                                     uint64_t anchor_clock_id,
                                     uint64_t current_master_clock_id,
                                     uint32_t mastership_age_ms,
                                     uint32_t last_anchor_age_ms);

#ifdef __cplusplus
}
#endif
