#!/usr/bin/env python3
"""Extract the buffered FLUSH classifier (control state + classify) from
main/audio/audio_receiver.c into flush_under_test.c for host tests."""
import pathlib, sys
root = pathlib.Path(__file__).resolve().parents[2]
src = (root / "main/audio/audio_receiver.c").read_text()
a = src.index("/* Shairport Sync keeps FLUSHBUFFERED state alongside the connection")
b = src.index("static inline void realtime_stage_kick(void)")
out = ('#include <stdint.h>\n#include <stdbool.h>\n#include <string.h>\n'
       '#include <inttypes.h>\n#include "freertos/FreeRTOS.h"\n'
       '#include "freertos/semphr.h"\n#include "esp_err.h"\n#include "esp_log.h"\n'
       'static const char *TAG = "flush";\n#define AP2_MAX_DEFERRED_FLUSH 10U\n'
       + src[a:b] + '''
/* test API */
void t_init(void) { s_buffered_control.mutex = xSemaphoreCreateMutex(); buffered_control_reset(); }
void t_immediate(uint32_t seq, uint32_t ts, bool valid) {
  s_buffered_control.immediate_active = true;
  __atomic_store_n(&s_buffered_control.immediate_by_ts, !valid, __ATOMIC_RELEASE);
  s_buffered_control.immediate_until_seq = seq & 0x007fffffU;
  s_buffered_control.immediate_until_rtp = ts;
}
esp_err_t t_deferred(uint32_t fs, uint32_t ft, uint32_t us, uint32_t ut) {
  return buffered_control_add_deferred(fs, ft, us, ut);
}
bool t_armed_ts(void) { return buffered_control_ts_flush_armed(); }
bool t_active(void) { buffered_control_snapshot_t c; buffered_control_snapshot(&c); return c.immediate_active; }
int t_classify(uint32_t seq, uint32_t rtp, int hint_valid, int32_t lead, int *end_kind) {
  buffered_packet_t p = {.seq = seq, .rtp = rtp};
  buffered_timeline_hint_t h = {.valid = hint_valid != 0, .lead = lead};
  buffered_packet_decision_t d;
  buffered_control_classify(&p, &h, &d);
  if (end_kind) *end_kind = d.immediate_completed ? (int)d.immediate_end : -1;
  return d.drop ? 1 : 0;
}
''')
pathlib.Path(sys.argv[1]).write_text(out)
