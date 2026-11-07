#pragma once

/* The sender's clock as the audio engine sees it. The time comes from the
 * two-way clock (apple_clock.c, started in main.c, selected by RTSP SETUP);
 * this file only turns its line into the snapshots the audio reads. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
  bool realtime_mode;
  bool valid;
  bool locked;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t epoch;
  int64_t raw_offset_ns;
  /* The usable remote-local offset: the clock line at the moment of the
   * snapshot. */
  int64_t filtered_offset_ns;
  int64_t raw_filter_delta_ns;
  /* Age of the current mastership; buffered audio uses this to qualify a
   * new GM while the media timeline remains in ESP-local time. */
  uint32_t mastership_age_ms;
  uint32_t sample_count;
  uint32_t sample_age_ms; /* UINT32_MAX when the line is not ready. */
  /* Diagnostics: why a buffered anchor may never become usable. */
  bool source_mixed;          /* always false: one master per line */
  uint64_t expected_clock_id; /* D7/timeline hint, 0 = none */
} ptp_clock_snapshot_t;

/* Coherent status/diagnostic snapshot for either buffered or realtime mode. */
void ptp_clock_get_snapshot(ptp_clock_snapshot_t *snapshot);

/* The audio timing watchdog gave up on an anchor: forget the hints. The
 * clock itself keeps running. */
void ptp_clock_clear(void);

/* Record the clockIdentity carried by the latest realtime D7 anchor. This is
 * an observation/hint only; it never changes the clock. */
void ptp_clock_note_realtime_d7(uint64_t clock_id);

typedef struct {
  bool realtime_mode;
  bool master_ready;
  uint64_t master_clock_id;
  uint64_t source_clock_id;
  int64_t master_offset_ns;
  uint32_t mastership_age_ms;
  uint32_t sample_count;
  uint32_t sample_age_ms; /* UINT32_MAX when the line is not ready. */
  /* Monotonic within one realtime session. Incremented whenever the
   * grandmaster changes after the first one has been seen. Audio uses this
   * only to distinguish mastership epochs. */
  uint32_t gm_change_count;
} ptp_realtime_snapshot_t;

/* Convert using one coherent clock snapshot. Both clock identity and readiness
 * belong to the same observation as the offset. */
bool ptp_clock_realtime_snapshot_to_local(
    const ptp_realtime_snapshot_t *snapshot, uint64_t clock_id,
    uint64_t remote_ns, uint64_t *local_ns);

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *snapshot);

/* Media-anchor hint for diagnostics only. */
void ptp_clock_set_master_clock_id(uint64_t clock_id);
