#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Simple PTP (IEEE 1588) slave for AirPlay time synchronization.
 * Listens for SYNC/FOLLOW_UP messages and tracks offset to PTP master.
 */

/**
 * Initialize and start PTP clock synchronization.
 * Creates a task that listens for PTP multicast messages.
 * Returns ESP_ERR_INVALID_STATE while an earlier task is running or stopping.
 */
esp_err_t ptp_clock_init(void);


/**
 * Clear PTP clock synchronization state.
 * Resets offset and lock status without stopping the clock.
 * Called during TEARDOWN to allow re-sync on new session.
 */
void ptp_clock_clear(void);

typedef struct {
  bool realtime_mode;
  bool valid;
  bool locked;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t epoch;
  int64_t raw_offset_ns;
  int64_t filtered_offset_ns;
  int64_t raw_filter_delta_ns;
  /* Age of the current mastership; buffered audio uses this to qualify a
   * new GM while the media timeline remains in ESP-local time. */
  uint32_t mastership_age_ms;
  uint32_t sample_count;
  uint32_t sample_age_ms; /* UINT32_MAX when no accepted timing sample exists. */
  /* Diagnostics: why a buffered anchor may never become usable. */
  bool source_mixed;          /* source identity changed within selected peer */
  uint64_t expected_clock_id; /* D7/timeline hint, 0 = none */
} ptp_clock_snapshot_t;

/* Coherent status/diagnostic snapshot for either buffered or realtime mode. */
void ptp_clock_get_snapshot(ptp_clock_snapshot_t *snapshot);

/**
 * Notify the PTP clock that playback is resuming after a pause.
 *
 * If the pause lasted longer than PTP_LONG_PAUSE_THRESHOLD_MS, this
 * resets the asymmetric smoothing filter so the next received PTP sample is
 * accepted unconditionally — mirroring the nqptp "B" (begin) signal behaviour
 * described in nqptp-shm-structures.h.
 *
 * Without this, after a pause long enough for the local crystal to drift,
 * the 1/256-negative-jitter damping would take several minutes to re-converge,
 * causing audible multi-room sync loss on resume.
 *
 * @param pause_duration_ms  Wall-clock length of the pause in milliseconds.
 */
void ptp_clock_notify_resume(uint32_t pause_duration_ms);

/**
 * Stream SETUP equivalent of Shairport/NQPTP T <RTSP client IP>, then B.
 * Starts one fresh timing record and selects that IP for both AAC and ALAC.
 * SETPEERS/SETPEERSX and media anchors never reset this record.
 */
void ptp_clock_start_timing(bool realtime, uint32_t timing_peer_ip);

/**
 * Record the clockIdentity carried by the latest realtime D7 anchor. This is
 * an observation/hint only; it never creates a virtual PTP domain or rewrites
 * the realtime audio timeline.
 */
void ptp_clock_note_realtime_d7(uint64_t clock_id);


typedef struct {
  bool realtime_mode;
  bool master_ready;
  uint64_t master_clock_id;
  uint64_t source_clock_id;
  int64_t master_offset_ns;
  uint32_t mastership_age_ms;
  uint32_t sample_count;
  uint32_t sample_age_ms; /* UINT32_MAX when no Follow_Up is available. */
  /* Monotonic within one realtime PTP session. Incremented whenever Announce
   * changes grandmasterIdentity after the first master has been observed.
   * Audio uses this only to distinguish mastership epochs; it never feeds
   * back into PTP selection/filtering. */
  uint32_t gm_change_count;
} ptp_realtime_snapshot_t;

/* Convert using one coherent clock snapshot. Both clock identity and readiness
 * belong to the same observation as the offset (including D7 admission). */
static inline bool ptp_clock_realtime_snapshot_to_local(
    const ptp_realtime_snapshot_t *snapshot, uint64_t clock_id,
    uint64_t remote_ns, uint64_t *local_ns) {
  if (!snapshot || !local_ns || !snapshot->realtime_mode ||
      !snapshot->master_ready || clock_id == 0 ||
      clock_id != snapshot->master_clock_id || remote_ns > INT64_MAX)
    return false;
  const int64_t offset = snapshot->master_offset_ns;
  if (offset >= 0) {
    if (remote_ns < (uint64_t)offset) return false;
    *local_ns = remote_ns - (uint64_t)offset;
  } else {
    const uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1U;
    if (magnitude > INT64_MAX || remote_ns > (uint64_t)INT64_MAX - magnitude)
      return false;
    *local_ns = remote_ns + magnitude;
  }
  return true;
}

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *snapshot);

/* Media-anchor hint for diagnostics only. Source selection belongs to
 * ptp_clock_start_timing(); changing this hint never resets the estimator. */
void ptp_clock_set_master_clock_id(uint64_t clock_id);
