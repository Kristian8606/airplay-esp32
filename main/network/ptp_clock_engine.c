#include "ptp_clock_engine.h"

#include <string.h>
#include <limits.h>

/* Positive jitter is a shorter network delay, so it is always trusted. During
 * the first seconds of a mastership it is taken in full: the session start is
 * when Wi-Fi is busiest (buffered audio burst), and the estimate must reach
 * the minimum-delay envelope before steady-state 1/16 tracking begins. */
#define PTP_ENGINE_STARTUP_NS        2000000000LL
#define PTP_ENGINE_POS_STEADY_DIV    16LL
#define PTP_ENGINE_NEG_DIV           256LL
#define PTP_ENGINE_NEG_CLAMP_NS      (-2500000LL)
#define PTP_ENGINE_READY_NS ((int64_t)PTP_CLOCK_ENGINE_READY_MS * 1000000LL)
/* No accepted sample for this long (Sync stopped during a pause, Wi-Fi
 * outage): the crystal drift since the last sample is unknown, start over. */
#define PTP_ENGINE_GAP_NS            3000000000LL
/* A backward step of the sender's clock (seen at iPhone session start) is
 * tracked at only ~80 us/s by the 1/256 path. A shorter delay can only raise
 * the offset, so a sustained run of samples all far BELOW the filter cannot be
 * network jitter: 16 Syncs (~2 s) 3 ms or more below reacquire the filter. */
#define PTP_ENGINE_STEP_NS           3000000LL
#define PTP_ENGINE_STEP_COUNT        16U
/* Once audio may use the estimate, its published value moves at most this
 * fast. Crystal drift is < 100 us/s, so drift passes through unchanged while a
 * filter step is spread over seconds instead of hitting the I2S servo. */
#define PTP_ENGINE_SLEW_NS_PER_S     250000LL
/* Once the drift is known, the published offset follows the predicted drift
 * plus only dt/tau of the remaining filter difference: the filter's 1/16
 * steps would otherwise reach the I2S servo as slope noise (seen on the board
 * as +/-40 ppm retunes every window). */
#define PTP_ENGINE_OUTPUT_TAU_NS     15000000000LL
/* Drift feed-forward window and bound (two crystals at +/-100 ppm). */
#define PTP_ENGINE_DRIFT_WINDOW_NS   30000000000LL
#define PTP_ENGINE_DRIFT_MAX_NS_S    200000LL
/* Real drift moves the filter ~10 us per Sync; a larger single move is a
 * filter step (a better sample, a clock step) and spoils that window. */
#define PTP_ENGINE_DRIFT_STEP_NS     500000LL
#define PTP_ENGINE_DRIFT_MAX_CHANGE  50000LL /* per window, ns/s */

static uint32_t next_epoch(uint32_t epoch) {
  epoch++;
  return epoch ? epoch : 1U;
}

static void clear_filter_state(ptp_clock_engine_t *engine) {
  engine->valid = false;
  engine->raw_offset_ns = 0;
  engine->filtered_offset_ns = 0;
  engine->output_offset_ns = 0;
  engine->mastership_start_time_ns = 0;
  engine->last_accepted_time_ns = 0;
  engine->accepted_samples = 0;
  engine->neg_run = 0;
  engine->neg_run_max_ns = 0;
  engine->drift_ns_per_s = 0;
  engine->drift_valid = false;
  engine->drift_window_start_ns = 0;
  engine->drift_window_start_offset_ns = 0;
}

void ptp_clock_engine_init(ptp_clock_engine_t *engine) {
  if (!engine) return;
  memset(engine, 0, sizeof(*engine));
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
  const int64_t old_filtered_offset_ns = engine->output_offset_ns;
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

static void fill_result(const ptp_clock_engine_t *engine, int64_t old_output,
                        ptp_clock_engine_sample_t *r) {
  r->accepted = true;
  r->raw_offset_ns = engine->raw_offset_ns;
  r->filtered_offset_ns = engine->filtered_offset_ns;
  r->output_offset_ns = engine->output_offset_ns;
  r->filtered_step_ns =
      engine->accepted_samples > 1 ? engine->output_offset_ns - old_output : 0;
  r->raw_filter_delta_ns = engine->raw_offset_ns - engine->filtered_offset_ns;
  r->accepted_samples = engine->accepted_samples;
  r->epoch = engine->epoch;
}

ptp_clock_engine_sample_t ptp_clock_engine_add_sample(
    ptp_clock_engine_t *engine, int64_t raw_offset_ns,
    int64_t reception_time_ns) {
  ptp_clock_engine_sample_t result = {0};
  if (!engine || reception_time_ns <= 0) return result;

  if (engine->valid &&
      (reception_time_ns - engine->last_accepted_time_ns > PTP_ENGINE_GAP_NS ||
       reception_time_ns < engine->last_accepted_time_ns)) {
    /* Same domain: the crystals did not change, keep the learned drift. */
    const int64_t drift = engine->drift_ns_per_s;
    const bool drift_valid = engine->drift_valid;
    engine->epoch = next_epoch(engine->epoch);
    clear_filter_state(engine);
    engine->drift_ns_per_s = drift;
    engine->drift_valid = drift_valid;
    result.restarted = true;
  }

  if (!engine->valid) {
    engine->valid = true;
    engine->raw_offset_ns = raw_offset_ns;
    engine->filtered_offset_ns = raw_offset_ns;
    engine->output_offset_ns = raw_offset_ns;
    engine->mastership_start_time_ns = reception_time_ns;
    engine->last_accepted_time_ns = reception_time_ns;
    engine->accepted_samples = 1;
    fill_result(engine, raw_offset_ns, &result);
    return result;
  }

  const int64_t mastership_ns =
      reception_time_ns - engine->mastership_start_time_ns;
  /* Predict the envelope at this sample from the learned drift. */
  const int64_t dt_ns = reception_time_ns - engine->last_accepted_time_ns;
  engine->filtered_offset_ns +=
      engine->drift_ns_per_s * dt_ns / 1000000000LL;
  const int64_t old_filtered = engine->filtered_offset_ns;
  const int64_t old_output = engine->output_offset_ns;
  const int64_t jitter = raw_offset_ns - old_filtered;
  int64_t smoothed;

  if (jitter < -PTP_ENGINE_STEP_NS) {
    if (engine->neg_run == 0 || raw_offset_ns > engine->neg_run_max_ns)
      engine->neg_run_max_ns = raw_offset_ns;
    engine->neg_run++;
  } else {
    engine->neg_run = 0;
  }

  if (engine->neg_run >= PTP_ENGINE_STEP_COUNT) {
    smoothed = engine->neg_run_max_ns;
    engine->neg_run = 0;
    engine->epoch = next_epoch(engine->epoch);
    result.reacquired = true;
    result.reacquire_jump_ns = smoothed - old_filtered;
  } else if (jitter < 0) {
    smoothed = old_filtered;
    if (mastership_ns > PTP_ENGINE_STARTUP_NS) {
      const int64_t clamped =
          jitter < PTP_ENGINE_NEG_CLAMP_NS ? PTP_ENGINE_NEG_CLAMP_NS : jitter;
      smoothed += clamped / PTP_ENGINE_NEG_DIV;
    }
  } else if (mastership_ns < PTP_ENGINE_STARTUP_NS) {
    smoothed = raw_offset_ns;
  } else {
    smoothed = old_filtered + jitter / PTP_ENGINE_POS_STEADY_DIV;
  }

  /* Learn the drift from the filter's slope once startup has settled. A
   * reacquire is a step, not drift: start a new window. */
  const int64_t filter_move = smoothed - old_filtered;
  if (result.reacquired || mastership_ns < PTP_ENGINE_STARTUP_NS ||
      filter_move > PTP_ENGINE_DRIFT_STEP_NS ||
      filter_move < -PTP_ENGINE_DRIFT_STEP_NS) {
    engine->drift_window_start_ns = 0;
  } else if (engine->drift_window_start_ns == 0) {
    engine->drift_window_start_ns = reception_time_ns;
    engine->drift_window_start_offset_ns = smoothed;
  } else if (reception_time_ns - engine->drift_window_start_ns >=
             PTP_ENGINE_DRIFT_WINDOW_NS) {
    const int64_t span_ns = reception_time_ns - engine->drift_window_start_ns;
    int64_t measured = (smoothed - engine->drift_window_start_offset_ns) *
                       1000LL / (span_ns / 1000000LL);
    if (measured > PTP_ENGINE_DRIFT_MAX_NS_S) measured = PTP_ENGINE_DRIFT_MAX_NS_S;
    if (measured < -PTP_ENGINE_DRIFT_MAX_NS_S) measured = -PTP_ENGINE_DRIFT_MAX_NS_S;
    if (!engine->drift_valid) {
      /* First measurement: take it whole, so the smoothed output below never
       * runs on an unknown drift. Later windows only refine it. */
      engine->drift_ns_per_s = measured;
      engine->drift_valid = true;
    } else {
      int64_t change = (measured - engine->drift_ns_per_s) / 2;
      if (change > PTP_ENGINE_DRIFT_MAX_CHANGE) change = PTP_ENGINE_DRIFT_MAX_CHANGE;
      if (change < -PTP_ENGINE_DRIFT_MAX_CHANGE) change = -PTP_ENGINE_DRIFT_MAX_CHANGE;
      engine->drift_ns_per_s += change;
    }
    engine->drift_window_start_ns = reception_time_ns;
    engine->drift_window_start_offset_ns = smoothed;
  }

  if (result.reacquired || mastership_ns < PTP_ENGINE_READY_NS) {
    engine->output_offset_ns = smoothed;
  } else {
    /* Follow the learned drift exactly, and only a fraction dt/tau of the
     * remaining difference: the filter's 1/16 steps no longer reach audio as
     * slope noise, while drift passes without lag. */
    const int64_t predicted =
        old_output + engine->drift_ns_per_s * dt_ns / 1000000000LL;
    const int64_t max_step = PTP_ENGINE_SLEW_NS_PER_S * dt_ns / 1000000000LL;
    /* Until the drift is known, follow the filter (slew-limited only). */
    int64_t step = engine->drift_valid
        ? (smoothed - predicted) * dt_ns / PTP_ENGINE_OUTPUT_TAU_NS
        : smoothed - predicted;
    if (step > max_step) step = max_step;
    if (step < -max_step) step = -max_step;
    engine->output_offset_ns = predicted + step;
  }

  engine->raw_offset_ns = raw_offset_ns;
  engine->filtered_offset_ns = smoothed;
  engine->last_accepted_time_ns = reception_time_ns;
  engine->accepted_samples++;
  fill_result(engine, old_output, &result);
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
