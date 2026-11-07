#include "sound_engine.h"

#include "airplay_log.h"
#include "esp_log.h"
#include "network/ptp_clock.h"

static const char *TAG = "sound_engine";

void sound_engine_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                  uint32_t rtp_time) {
  ptp_realtime_snapshot_t ps = {0};
  ptp_clock_get_realtime_snapshot(&ps);
  if (!ps.realtime_mode) {
    audio_receiver_set_anchor_time(clock_id, network_time_ns, rtp_time);
    return;
  }

  /* Realtime follows the same rule as D7: convert the sender time once into
   * ESP time. While a new master is still settling, the running local map
   * stays and this anchor is left out. */
  ptp_clock_set_master_clock_id(clock_id);
  uint64_t local_ns = 0;
  if (!ptp_clock_realtime_snapshot_to_local(&ps, clock_id, network_time_ns,
                                            &local_ns)) {
    AP_LOGI(TAG,
            "realtime anchor deferred: clock=%016llx gm=%016llx ready=%d "
            "age=%lums",
            (unsigned long long)clock_id,
            (unsigned long long)ps.master_clock_id, ps.master_ready ? 1 : 0,
            (unsigned long)ps.mastership_age_ms);
    return;
  }
  AP_LOGI(TAG, "realtime anchor: ptp=%llu local=%llu gm=%016llx age=%lums",
          (unsigned long long)network_time_ns, (unsigned long long)local_ns,
          (unsigned long long)ps.master_clock_id,
          (unsigned long)ps.mastership_age_ms);
  audio_realtime_anchor_result_t result = {0};
  if (!audio_receiver_set_realtime_anchor_local(
          clock_id, ps.gm_change_count, ps.mastership_age_ms, network_time_ns,
          local_ns, rtp_time, &result)) {
    AP_LOGI(TAG, "realtime anchor: media rebase deferred (epoch %lu)",
            (unsigned long)ps.gm_change_count);
  }
}
