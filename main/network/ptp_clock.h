#pragma once

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
 */
esp_err_t ptp_clock_init(void);

/**
 * Stop PTP clock and free resources.
 */
void ptp_clock_stop(void);

/**
 * Clear PTP clock synchronization state.
 * Resets offset and lock status without stopping the clock.
 * Called during TEARDOWN to allow re-sync on new session.
 */
void ptp_clock_clear(void);

/**
 * Check if PTP is locked to a master clock.
 * @return true if synchronized with acceptable accuracy
 */
bool ptp_clock_is_locked(void);

/**
 * Get current PTP time in nanoseconds.
 * Returns local time adjusted by PTP offset.
 * @return PTP time in nanoseconds since epoch
 */
uint64_t ptp_clock_get_time_ns(void);

/**
 * Get current offset from local clock to PTP time in nanoseconds.
 * PTP_time = local_time + offset
 */
int64_t ptp_clock_get_offset_ns(void);

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
 * Get synchronization statistics.
 */
typedef struct {
  uint32_t sync_count;        // Number of SYNC messages received
  uint32_t followup_count;    // Number of FOLLOW_UP messages received
  int64_t last_offset_ns;     // Last RAW measured offset (pre-smoothing)
  int64_t filtered_offset_ns; // Exported filtered offset (what timing uses)
  int64_t raw_filter_delta_ns; // RAW - filtered estimator delta
  uint32_t lock_time_ms;      // Time since lock achieved (0 if not locked)
  uint32_t outlier_count;     // Samples rejected as outliers since start
} ptp_stats_t;

void ptp_clock_get_stats(ptp_stats_t *stats);


/**
 * Enable the AirPlay 2 realtime NQPTP-style estimator path.
 *
 * In realtime mode the PTP source is selected by the RTSP client's IPv4
 * address and Announce supplies the actual grandmasterIdentity. Realtime PTP
 * only estimates remote-GM -> ESP-local conversion; audio continuity is owned
 * by the ALAC RTP<->local anchor, not by a synthetic/virtual PTP timeline.
 *
 * Buffered AAC leaves this mode disabled and keeps the existing PTP behaviour.
 */
void ptp_clock_set_realtime_mode(bool enabled, uint32_t timing_peer_ip);

/**
 * Record the clockIdentity carried by the latest realtime D7 anchor. This is
 * an observation/hint only; it never creates a virtual PTP domain or rewrites
 * the realtime audio timeline.
 */
void ptp_clock_note_realtime_d7(uint64_t clock_id);

/**
 * Convert a remote timestamp from the current READY realtime grandmaster into
 * ESP monotonic time. Returns false while a new GM is still acquiring or when
 * the D7 clock_id does not match the current grandmaster.
 */
bool ptp_clock_realtime_time_to_local(uint64_t clock_id,
                                      uint64_t remote_ptp_ns,
                                      uint64_t *local_ns);

typedef struct {
  bool realtime_mode;
  bool master_ready;
  uint64_t master_clock_id;
  uint64_t source_clock_id;
  int64_t master_offset_ns;
  uint32_t mastership_age_ms;
  uint32_t sample_count;
  /* Monotonic within one realtime PTP session. Incremented whenever Announce
   * changes grandmasterIdentity after the first master has been observed.
   * Audio uses this only to distinguish mastership epochs; it never feeds
   * back into PTP selection/filtering. */
  uint32_t gm_change_count;
} ptp_realtime_snapshot_t;

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *snapshot);

/**
 * Restrict the PTP clock to a single master identified by its 8-byte
 * clockIdentity (the value carried in the AirPlay 2 0xD7 anchor packet at
 * offset +20, and in the PTP common-header sourcePortIdentity field at
 * bytes 20-27).
 *
 * Pass 0 to clear the filter (accept any master — the default at startup).
 *
 * In buffered/legacy mode this retains the original behaviour: changing the
 * expected clock resets samples and filters PTP by sourcePortIdentity.
 * In AirPlay 2 realtime mode it is only an anchor-clock hint; PTP source
 * selection comes from the RTSP client IP and Announce determines the actual
 * grandmaster, matching the Shairport/NQPTP model.
 */
void ptp_clock_set_master_clock_id(uint64_t clock_id);

/**
 * Read the current expected master clock_id (0 if none / filter cleared).
 */
uint64_t ptp_clock_get_master_clock_id(void);
