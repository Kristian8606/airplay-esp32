#include "ptp_clock.h"
#include "ptp_clock_engine.h"

#include <string.h>

#include "apple_clock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* A realtime master must hold the line this long before audio uses it. */
#define RT_MIN_SAMPLES 4U

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* Identity of the last ready line: a new source or grandmaster is a new
 * epoch; in realtime mode a new grandmaster also counts as a GM change. */
static struct {
  bool known, realtime;
  uint64_t source, gm;
  uint32_t epoch, gm_changes;
} s_id;
static uint64_t s_hint;

static uint32_t clamp_ms(int64_t ns) {
  if (ns <= 0) return 0;
  const int64_t ms = ns / 1000000LL;
  return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

void ptp_clock_get_snapshot(ptp_clock_snapshot_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  apple_clock_line_t l;
  apple_clock_get(&l);
  const int64_t now = esp_timer_get_time() * 1000LL;

  taskENTER_CRITICAL(&s_mux);
  if (s_id.realtime != l.realtime) {
    memset(&s_id, 0, sizeof(s_id));
    s_id.realtime = l.realtime;
  }
  if (l.ready && (!s_id.known || s_id.source != l.source_clock_id ||
                  s_id.gm != l.grandmaster_clock_id)) {
    if (l.realtime && s_id.known && s_id.gm != l.grandmaster_clock_id)
      s_id.gm_changes++;
    if (++s_id.epoch == 0) s_id.epoch = 1;
    s_id.known = true;
    s_id.source = l.source_clock_id;
    s_id.gm = l.grandmaster_clock_id;
  }
  const uint32_t epoch = s_id.epoch, changes = s_id.gm_changes;
  out->expected_clock_id = s_hint;
  taskEXIT_CRITICAL(&s_mux);

  out->realtime_mode = l.realtime;
  out->source_clock_id = l.source_clock_id;
  out->grandmaster_clock_id = l.grandmaster_clock_id;
  out->sample_count = l.samples;
  out->sample_age_ms = l.ready ? 0U : UINT32_MAX;
  out->mastership_age_ms = l.ready ? clamp_ms(now - l.ready_since_ns) : 0U;
  if (l.ready) {
    out->raw_offset_ns = apple_clock_at(&l, now) - now;
    out->filtered_offset_ns = out->raw_offset_ns;
  }
  out->locked = l.realtime
      ? l.ready && l.grandmaster_clock_id && l.samples >= RT_MIN_SAMPLES &&
        out->mastership_age_ms >= PTP_CLOCK_ENGINE_READY_MS
      : l.ready;
  out->valid = l.realtime ? out->locked : l.ready;
  out->epoch = l.realtime ? (l.grandmaster_clock_id ? changes + 1U : 0U) : epoch;
}

void ptp_clock_clear(void) {
  taskENTER_CRITICAL(&s_mux);
  s_hint = 0;
  taskEXIT_CRITICAL(&s_mux);
}

void ptp_clock_note_realtime_d7(uint64_t clock_id) {
  ptp_clock_set_master_clock_id(clock_id);
}

void ptp_clock_set_master_clock_id(uint64_t clock_id) {
  taskENTER_CRITICAL(&s_mux);
  s_hint = clock_id;
  taskEXIT_CRITICAL(&s_mux);
}

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *out) {
  if (!out) return;
  ptp_clock_snapshot_t snap;
  ptp_clock_get_snapshot(&snap);
  *out = (ptp_realtime_snapshot_t){
      .realtime_mode = snap.realtime_mode,
      .master_ready = snap.realtime_mode && snap.locked,
      .master_clock_id = snap.grandmaster_clock_id,
      .source_clock_id = snap.source_clock_id,
      .master_offset_ns = snap.filtered_offset_ns,
      .mastership_age_ms = snap.mastership_age_ms,
      .sample_count = snap.sample_count,
      .sample_age_ms = snap.sample_age_ms,
      .gm_change_count = snap.epoch ? snap.epoch - 1U : 0U};
}

bool ptp_clock_realtime_snapshot_to_local(const ptp_realtime_snapshot_t *snap,
                                          uint64_t id, uint64_t remote,
                                          uint64_t *local) {
  return snap && snap->realtime_mode && snap->master_ready && id &&
         id == snap->master_clock_id && remote <= INT64_MAX &&
         ptp_clock_engine_remote_to_local(remote, snap->master_offset_ns,
                                          local) &&
         *local <= INT64_MAX;
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
         mastership_age_ms >= PTP_CLOCK_ENGINE_READY_MS && last_anchor_age_ms >= 5000U;
}
