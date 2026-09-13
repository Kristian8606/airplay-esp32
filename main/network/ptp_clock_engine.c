#include "ptp_clock_engine.h"

#include <string.h>
#include <limits.h>

#define PTP_ENGINE_STARTUP_NS        1000000000LL
#define PTP_ENGINE_POS_STEADY_DIV    16LL
#define PTP_ENGINE_NEG_DIV           256LL
#define PTP_ENGINE_NEG_CLAMP_NS      (-2500000LL)

static uint32_t next_epoch(uint32_t epoch) {
  epoch++;
  return epoch ? epoch : 1U;
}

static void clear_filter_state(ptp_clock_engine_t *engine) {
  engine->valid = false;
  engine->raw_offset_ns = 0;
  engine->filtered_offset_ns = 0;
  engine->previous_offset_ns = 0;
  engine->previous_sample_time_ns = 0;
  engine->mastership_start_time_ns = 0;
  engine->last_accepted_time_ns = 0;
  engine->accepted_samples = 0;
}

void ptp_clock_engine_init(ptp_clock_engine_t *engine,
                           int64_t outlier_threshold_ns) {
  if (!engine) return;
  memset(engine, 0, sizeof(*engine));
  engine->outlier_threshold_ns = outlier_threshold_ns;
}

bool ptp_clock_engine_set_domain(ptp_clock_engine_t *engine,
                                 uint64_t source_clock_id,
                                 uint64_t grandmaster_clock_id,
                                 ptp_clock_engine_domain_event_t *event_out) {
  if (!engine) return false;
  if (event_out) memset(event_out, 0, sizeof(*event_out));

  const bool first_domain = engine->source_clock_id == 0 &&
                            engine->grandmaster_clock_id == 0;
  const bool source_changed = engine->source_clock_id != 0 &&
                              source_clock_id != 0 &&
                              engine->source_clock_id != source_clock_id;
  const bool gm_changed = engine->grandmaster_clock_id != 0 &&
                          grandmaster_clock_id != 0 &&
                          engine->grandmaster_clock_id != grandmaster_clock_id;

  bool changed = first_domain || source_changed || gm_changed;
  if (!changed) {
    if (engine->source_clock_id == 0 && source_clock_id != 0) changed = true;
    if (engine->grandmaster_clock_id == 0 && grandmaster_clock_id != 0) changed = true;
  }
  if (!changed) return false;

  const uint64_t old_source = engine->source_clock_id;
  const uint64_t old_gm = engine->grandmaster_clock_id;
  const bool old_valid = engine->valid;
  const int64_t old_raw_offset_ns = engine->raw_offset_ns;
  const int64_t old_filtered_offset_ns = engine->filtered_offset_ns;
  const uint32_t old_accepted_samples = engine->accepted_samples;

  if (first_domain || source_changed || gm_changed) {
    engine->epoch = next_epoch(engine->epoch);
    clear_filter_state(engine);
  }
  if (source_clock_id != 0) engine->source_clock_id = source_clock_id;
  if (first_domain || source_changed) {
    /* A new source invalidates the old source's grandmaster identity. Until an
     * Announce for the new source arrives, GM may legitimately be unknown. */
    engine->grandmaster_clock_id = grandmaster_clock_id;
  } else if (grandmaster_clock_id != 0) {
    engine->grandmaster_clock_id = grandmaster_clock_id;
  }

  if (event_out) {
    event_out->source_changed = source_changed;
    event_out->gm_changed = gm_changed;
    event_out->first_domain = first_domain;
    event_out->old_source_clock_id = old_source;
    event_out->old_grandmaster_clock_id = old_gm;
    event_out->new_source_clock_id = engine->source_clock_id;
    event_out->new_grandmaster_clock_id = engine->grandmaster_clock_id;
    event_out->old_estimator_valid = old_valid;
    event_out->old_raw_offset_ns = old_raw_offset_ns;
    event_out->old_filtered_offset_ns = old_filtered_offset_ns;
    event_out->old_accepted_samples = old_accepted_samples;
    event_out->epoch = engine->epoch;
  }
  return true;
}

void ptp_clock_engine_reset_filter(ptp_clock_engine_t *engine,
                                   bool bump_epoch) {
  if (!engine) return;
  if (bump_epoch) engine->epoch = next_epoch(engine->epoch);
  clear_filter_state(engine);
}

ptp_clock_engine_sample_t ptp_clock_engine_add_sample(
    ptp_clock_engine_t *engine, int64_t raw_offset_ns,
    int64_t reception_time_ns) {
  ptp_clock_engine_sample_t result = {0};
  if (!engine || reception_time_ns <= 0) return result;

  result.raw_offset_ns = raw_offset_ns;
  result.epoch = engine->epoch;

  if (engine->valid && engine->outlier_threshold_ns > 0) {
    int64_t diff = raw_offset_ns - engine->filtered_offset_ns;
    if (diff < 0) diff = -diff;
    if (diff > engine->outlier_threshold_ns) {
      result.outlier = true;
      result.filtered_offset_ns = engine->filtered_offset_ns;
      result.raw_filter_delta_ns = raw_offset_ns - engine->filtered_offset_ns;
      result.accepted_samples = engine->accepted_samples;
      return result;
    }
  }

  const int64_t old_filtered = engine->filtered_offset_ns;
  int64_t smoothed = raw_offset_ns;

  if (!engine->valid || engine->previous_sample_time_ns == 0) {
    engine->mastership_start_time_ns = reception_time_ns;
  } else {
    const int64_t jitter = raw_offset_ns - engine->previous_offset_ns;
    int64_t mastership_ns = reception_time_ns - engine->mastership_start_time_ns;
    if (mastership_ns < 0) mastership_ns = 0;

    if (jitter < 0) {
      smoothed = engine->previous_offset_ns;
      if (mastership_ns > PTP_ENGINE_STARTUP_NS) {
        int64_t clamped = jitter;
        if (clamped < PTP_ENGINE_NEG_CLAMP_NS)
          clamped = PTP_ENGINE_NEG_CLAMP_NS;
        smoothed += clamped / PTP_ENGINE_NEG_DIV;
      }
    } else if (mastership_ns < PTP_ENGINE_STARTUP_NS) {
      smoothed = engine->previous_offset_ns + jitter;
    } else {
      smoothed = engine->previous_offset_ns + jitter / PTP_ENGINE_POS_STEADY_DIV;
    }
  }

  engine->valid = true;
  engine->raw_offset_ns = raw_offset_ns;
  engine->filtered_offset_ns = smoothed;
  engine->previous_offset_ns = smoothed;
  engine->previous_sample_time_ns = reception_time_ns;
  engine->last_accepted_time_ns = reception_time_ns;
  engine->accepted_samples++;

  result.accepted = true;
  result.raw_offset_ns = raw_offset_ns;
  result.filtered_offset_ns = smoothed;
  result.filtered_step_ns = engine->accepted_samples > 1
                                ? smoothed - old_filtered
                                : 0;
  result.raw_filter_delta_ns = raw_offset_ns - smoothed;
  result.accepted_samples = engine->accepted_samples;
  result.epoch = engine->epoch;
  return result;
}



static bool add_signed_u64(uint64_t base, int64_t delta, uint64_t *out) {
  if (!out) return false;
  if (delta >= 0) {
    const uint64_t add = (uint64_t)delta;
    if (base > UINT64_MAX - add) return false;
    *out = base + add;
  } else {
    const uint64_t sub = (uint64_t)(-(delta + 1)) + 1U;
    if (base < sub) return false;
    *out = base - sub;
  }
  return true;
}

bool ptp_clock_engine_remote_to_local(uint64_t remote_ns, int64_t offset_ns,
                                      uint64_t *local_ns) {
  if (offset_ns == INT64_MIN) {
    if (!local_ns || remote_ns > UINT64_MAX - ((uint64_t)INT64_MAX + 1ULL))
      return false;
    *local_ns = remote_ns + ((uint64_t)INT64_MAX + 1ULL);
    return true;
  }
  return add_signed_u64(remote_ns, -offset_ns, local_ns);
}

bool ptp_clock_engine_local_to_remote(uint64_t local_ns, int64_t offset_ns,
                                      uint64_t *remote_ns) {
  return add_signed_u64(local_ns, offset_ns, remote_ns);
}

bool ptp_clock_engine_handover_ready(bool have_local_anchor, bool clock_locked,
                                     uint64_t anchor_clock_id,
                                     uint64_t current_master_clock_id,
                                     uint32_t mastership_age_ms,
                                     uint32_t last_anchor_age_ms) {
  return have_local_anchor && clock_locked && anchor_clock_id != 0 &&
         current_master_clock_id != 0 &&
         anchor_clock_id != current_master_clock_id &&
         mastership_age_ms >= 400U && last_anchor_age_ms >= 5000U;
}
