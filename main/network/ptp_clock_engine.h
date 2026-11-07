#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A mastership is usable this long after its line became ready. */
#define PTP_CLOCK_ENGINE_READY_MS 400U

/* Convert between remote PTP time and ESP-local monotonic time. */
bool ptp_clock_engine_remote_to_local(uint64_t remote_ns, int64_t offset_ns,
                                      uint64_t *local_ns);
bool ptp_clock_engine_local_to_remote(uint64_t local_ns, int64_t offset_ns,
                                      uint64_t *remote_ns);

/* Buffered handover qualification. A transient GM must never move the media
 * anchor; only a qualified, different master may take it over. */
bool ptp_clock_engine_handover_ready(bool have_local_anchor, bool clock_locked,
                                     uint64_t anchor_clock_id,
                                     uint64_t current_master_clock_id,
                                     uint32_t mastership_age_ms,
                                     uint32_t last_anchor_age_ms);

#ifdef __cplusplus
}
#endif
