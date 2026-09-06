#include "audio_receiver.h"

#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aac_decoder.h"
#include "audio_eq.h"
#include "audio_crypto.h"
#include "ap2_buffered_transport.h"
#include "pcm_rtp_ring.h"
#include "audio_playout.h"
#include "realtime_receiver.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "network/ptp_clock.h"
#include "network/socket_utils.h"

#define AP2_PACKET_MAX             8192U
#define AP2_RX_STACK               4096U
#define AP2_PROCESS_STACK          6144U
#define AP2_STATS_STACK            8192U
#define AP2_PLAYOUT_STACK          4096U
#define AP2_RT_STAGE_STACK         4096U
#define AP2_NETWORK_CORE           0
#define AP2_DECODE_CORE            1
#define AP2_BUFFERED_PROCESSOR_CORE 0
#define AP2_RX_PRIORITY            7
#define AP2_DECODE_PRIORITY        6
#define AP2_PLAYOUT_PRIORITY       8
#define AP2_STATS_PRIORITY         2
#define AP2_RT_STAGE_PRIORITY      7
#define AP2_PCM_CAPACITY_FRAMES    4096U
#define AP2_STATS_PERIOD_MS        2000U
#define AP2_FIRST_DECODE_LOGS      3U

#define AP2_PID_CALC_PERIOD_US    1000000LL  /* PID math at 1 Hz */
#define AP2_PID_TUNE_PERIOD_US    5000000LL  /* physical I2S retune <= 0.2 Hz */
#define AP2_PID_DEADBAND_US          1000    /* +/-1 ms is already good */
#define AP2_PID_SOFTBAND_US          2000    /* 1..2 ms => gentler P action */
#define AP2_PID_MAX_PPM               160
#define AP2_PID_MIN_TUNE_PPM            5
#define AP2_PID_MAX_JUMP_PPM            80

/* Continuous-start alignment. A new generation enables I2S only once.
 * Two silent DMA blocks establish the actual running sample-clock phase.  The
 * first tagged EOF gives a precise PTP observation; while the second silent
 * block is still playing, the first real block is queued from the RTP sample
 * that belongs at the next physical DMA boundary.  I2S is never disabled
 * between phase measurement and real audio, avoiding non-repeatable
 * second-enable latency. */
#define AP2_START_SILENCE_FUTURE_BLOCKS    4U
#define AP2_START_ALIGN_TIMEOUT_US      30000LL
#define AP2_START_PRIME_GUARD_BLOCKS        8U

/* PID servo around tagged DMA-EOF phase.
 * P reacts to phase error, I learns the steady crystal/rate bias, D damps
 * motion through zero.  The derivative is low-pass filtered because the PTP
 * timestamp itself has some jitter.  PID is evaluated more often than the
 * physical tune operation so control knowledge can evolve without repeatedly
 * disable/tune/enable cycling I2S. */
#define AP2_PID_KP_PPM_PER_MS          22.0
#define AP2_PID_KI_PPM_PER_MS_S         0.55
#define AP2_PID_KD_PPM_PER_MS_PER_S    55.0
#define AP2_PID_D_ALPHA                  0.20
#define AP2_PID_I_TERM_LIMIT_PPM       110.0
#define AP2_PCM_TARGET_MS           1000U
#define AP2_PLAYOUT_PRIME_MS         500U
#define AP2_REALTIME_PRIME_MS        100U
#define AP2_RT_GM_REBASE_SETTLE_MS  1000U
#define AP2_BUFFERED_FIFO_REQUEST_BYTES AP2_BUFFERED_AUDIO_BUFFER_REQUEST_BYTES
#define AP2_BUFFERED_LEAD_MS       (AP2_PCM_TARGET_MS + 100U)
#define AP2_MAX_DEFERRED_FLUSH     10U
#define AP2_DECODE_IDLE_TICKS       1U

/* ALAC reorder release point. Missing PCM stays absent in the raw RTP ring
 * while retransmission runs independently. Only when the physical PTP
 * deadline is this close do we commit one frame of silence through EQ. */
#define AP2_RT_STAGE_COMMIT_MARGIN_US REALTIME_RECOVERY_FINAL_MARGIN_US

static const char *TAG = "audio_shairport";

/* Updated by RTSP control on Core0, consumed by playout on Core1. */
static volatile int32_t s_volume_target_q15 = 32768;

typedef struct {
  bool in_use;
  bool active;
  uint32_t from_seq;
  uint32_t from_rtp;
  uint32_t until_seq;
  uint32_t until_rtp;
  /* Diagnostic-only counters for one deferred request. They never influence
   * FLUSH decisions; they let a hardware log prove exactly what transport
   * blocks were discarded between the sender's advertised endpoints. */
  uint32_t diag_drop_count;
  uint32_t diag_first_drop_seq;
  uint32_t diag_first_drop_rtp;
  uint32_t diag_last_drop_seq;
  uint32_t diag_last_drop_rtp;
} ap2_flush_request_t;

typedef struct {
  bool anchor_valid;
  bool playing;
  uint64_t anchor_ptp_ns;
  uint64_t anchor_local_ns; /* authoritative presentation anchor for realtime ALAC */
  uint32_t anchor_rtp;
  uint32_t generation;
  audio_format_t format;
  uint32_t format_generation;
  bool timeline_reset_pending;
  bool immediate_flush_requested;
  bool immediate_flush_has_endpoint;
  uint32_t immediate_flush_until_seq;
  uint32_t immediate_flush_until_rtp;
  ap2_flush_request_t deferred_flush[AP2_MAX_DEFERRED_FLUSH];
  audio_stream_type_t stream_type;
  uint32_t playout_latency_samples;
} timing_snapshot_t;

typedef struct {
  uint64_t rx;
  uint64_t decoded;
  uint64_t stale_predecrypt;
  uint64_t stale_predecode;
  uint64_t timeline_drop;
  uint64_t pcm_write_error;
  uint64_t realtime_sink_no_ptp;
  uint64_t realtime_sink_stale;
  uint64_t realtime_sink_ring;
  uint64_t playout_servo_session_resets;
  uint64_t playout_servo_reset_errors;
  uint64_t decrypt_error;
  uint64_t decode_error;
  uint64_t empty_payload;
  uint64_t decode_reacquires;
  uint64_t seq_gap;
  uint64_t rtp_gap;
  uint32_t last_seq;
  uint32_t last_rtp;
  uint32_t last_decoded_end_rtp;
  uint64_t playout_blocks;
  uint64_t playout_underruns;
  uint64_t playout_resyncs;
  uint64_t playout_flushes;
  uint64_t playout_prime_waits;
  uint64_t playout_starts;
  uint32_t playout_state; /* 0=STOPPED, 1=PRIMING, 2=RUNNING */
  uint32_t last_playout_rtp;
  uint32_t tx_fetch_us;
  int32_t desired_cursor_err_frames;
  /* Tagged TX DMA EOF phase. + means physical DMA completion was early
   * versus the AirPlay PTP target, - means late. */
  int32_t output_sync_frames;
  int32_t output_sync_us;      /* EMA used by compact log */
  int32_t output_sync_raw_us;  /* most recent tagged completion */
  uint32_t output_sync_generation;
  bool output_sync_valid;
  uint64_t dma_tagged_completions;
  uint32_t dma_pipeline_blocks;
  /* Startup diagnostics are produced by the realtime playout task but logged
   * by the low-priority stats task. ESP_LOG in the first DMA periods can
   * itself stall the producer long enough to underrun a 2x256 DMA pipeline. */
  int32_t startup_probe_sync_us;
  int32_t startup_align_samples;
  uint32_t startup_real_rtp;
  uint32_t startup_align_generation;
  volatile uint32_t startup_align_log_pending;
  int32_t startup_sync_start_us;
  uint32_t startup_sync_generation;
  int32_t startup_anchor_to_sync_ms;
  volatile uint32_t startup_sync_log_pending;
  int32_t first_decode_from_anchor_ms;
  uint32_t first_decode_generation;
  uint32_t pcm_peak_in;
  uint32_t pcm_peak_out;
  uint64_t pcm_rail_in;
  uint64_t pcm_clip_out;
  int32_t volume_q15;
  int32_t servo_ppm;
  int32_t servo_target_ppm;
  int32_t servo_slope_us_per_s;
  uint32_t servo_mclk_hz;
  uint64_t servo_updates;
  uint64_t servo_errors;
} diag_stats_t;

typedef struct {
  audio_format_t format;
  audio_encrypt_t encrypt;
  audio_stats_t public_stats;
  audio_stream_type_t stream_type;

  uint16_t port;
  volatile bool engine_running;
  volatile bool rx_running;

  ap2_buffered_transport_t *transport;
  TaskHandle_t processor_task;
  TaskHandle_t playout_task;
  TaskHandle_t stats_task;
  volatile bool stats_session_running;
  TaskHandle_t realtime_stage_task;
  uint8_t *packet;
  uint8_t *decrypt_buf;
  int16_t *decode_pcm;
  pcm_rtp_ring_t *pcm_ring;          /* final EQ'd PCM -> PTP/I2S */
  pcm_rtp_ring_t *realtime_stage_ring; /* raw decoded ALAC, RTP-addressed */

  portMUX_TYPE state_mux;
  bool playing;
  bool anchor_valid;
  uint64_t anchor_clock_id;
  uint64_t anchor_ptp_ns;
  uint64_t anchor_local_ns; /* ESP monotonic time corresponding to anchor_rtp */
  uint32_t anchor_rtp;
  /* Realtime media-domain rebase. This is deliberately separate from PTP:
   * it only maps a new GM epoch onto the already-running local media phase. */
  bool rt_media_rebase_valid;
  uint64_t rt_media_rebase_clock_id;
  uint32_t rt_media_rebase_epoch;
  int64_t rt_media_rebase_bias_ns;
  uint32_t generation;
  int64_t anchor_set_local_us;
  uint32_t anchor_set_generation;
  uint32_t format_generation;
  bool timeline_reset_pending;
  bool immediate_flush_requested;
  bool immediate_flush_has_endpoint;
  uint32_t immediate_flush_until_seq;
  uint32_t immediate_flush_until_rtp;
  bool immediate_flush_diag_active;
  int64_t immediate_flush_start_us;
  uint32_t immediate_flush_start_seq;
  uint64_t immediate_flush_start_drop;
  size_t immediate_flush_start_fifo;
  uint64_t immediate_flush_start_socket_bytes;
  uint64_t immediate_flush_start_fifo_read;
  ap2_flush_request_t deferred_flush[AP2_MAX_DEFERRED_FLUSH];
  uint32_t playout_latency_samples;
  uint32_t realtime_eq_generation;
  volatile bool realtime_stage_running;
  volatile bool realtime_stage_idle;
  bool realtime_stage_cursor_valid;
  uint32_t realtime_stage_cursor_rtp;
  uint32_t realtime_stage_generation;
  uint32_t realtime_stage_missing;
  uint32_t realtime_stage_recovered;
  uint32_t realtime_stage_silence;
  uint32_t realtime_stage_late_fill;
  volatile int32_t realtime_stage_ahead_us;
  volatile int32_t realtime_stage_min_ahead_us;
  volatile uint32_t realtime_stage_wait_max_us;
  /* Buffered transport control state belongs to one codec/session only.
   * Diagnostics remain cumulative, but FLUSH decisions must never reuse a
   * sequence number from an older AAC session after ALAC was active. */
  uint32_t buffered_session_seq;
  uint64_t buffered_session_rx;
  bool buffered_session_seq_valid;
  volatile bool i2s_flush_requested;
  volatile bool playout_servo_reset_requested;

  diag_stats_t diag;
} ap2_state_t;

static ap2_state_t s = {
    .stream_type = AUDIO_STREAM_NONE,
    .generation = 1,
    .format_generation = 1,
    .timeline_reset_pending = true,
    .state_mux = portMUX_INITIALIZER_UNLOCKED,
    .realtime_stage_idle = true,
};

static inline void realtime_stage_kick(void) {
  TaskHandle_t task = s.realtime_stage_task;
  if (task) {
    xTaskNotifyGive(task);
  }
}

static TickType_t realtime_stage_wait_ticks(int64_t wait_us) {
  if (wait_us <= 0) return 0;
  const uint64_t tick_us = (uint64_t)portTICK_PERIOD_MS * 1000ULL;
  if (tick_us == 0) return 1;

  /* Floor to the previous RTOS tick so the notification timeout cannot move
   * the existing final staging margin later. A sub-tick remainder simply
   * wakes one tick early and re-evaluates the same deadline. */
  uint64_t ticks64 = (uint64_t)wait_us / tick_us;
  if (ticks64 == 0) ticks64 = 1;
  if (ticks64 >= (uint64_t)portMAX_DELAY) ticks64 = (uint64_t)portMAX_DELAY - 1ULL;
  return (TickType_t)ticks64;
}

static void snapshot_state(timing_snapshot_t *out) {
  taskENTER_CRITICAL(&s.state_mux);
  out->anchor_valid = s.anchor_valid;
  out->playing = s.playing;
  out->anchor_ptp_ns = s.anchor_ptp_ns;
  out->anchor_local_ns = s.anchor_local_ns;
  out->anchor_rtp = s.anchor_rtp;
  out->generation = s.generation;
  out->format = s.format;
  out->format_generation = s.format_generation;
  out->timeline_reset_pending = s.timeline_reset_pending;
  out->stream_type = s.stream_type;
  out->playout_latency_samples = s.playout_latency_samples;
  taskEXIT_CRITICAL(&s.state_mux);
}

static uint32_t next_generation(uint32_t generation) {
  generation++;
  return generation ? generation : 1U;
}

/* Immediate FLUSH/pause ends the current timeline now, but the processor deliberately
 * does not publish a new generation yet. TCP may keep arriving while the
 * anchor is invalid; those packets are provisional and will be made
 * unreachable atomically when the next valid anchor commits a new epoch. */
static void mark_timeline_discontinuity(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.anchor_valid = false;
  s.timeline_reset_pending = true;
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();
  s.diag.last_decoded_end_rtp = 0;
  s.i2s_flush_requested = true;
}

static uint32_t commit_anchor_epoch_locked(void) {
  uint32_t gen = s.generation;
  if (s.timeline_reset_pending) {
    gen = next_generation(gen);
    /* Publish the backing stores first. Any task holding the previous state
     * snapshot can only fail a generation check during this tiny window. */
    if (s.pcm_ring) {
      pcm_rtp_ring_set_generation(s.pcm_ring, gen);
    }
    if (s.realtime_stage_ring) {
      pcm_rtp_ring_set_generation(s.realtime_stage_ring, gen);
    }
    s.realtime_stage_cursor_valid = false;
    s.realtime_stage_generation = gen;
    s.generation = gen;
    s.timeline_reset_pending = false;
    s.diag.last_decoded_end_rtp = 0;
  }
  return gen;
}

static inline int32_t rtp_delta(uint32_t a, uint32_t b) {
  return (int32_t)(a - b);
}

/* Presentation-clock boundary.
 * Buffered AAC intentionally keeps the existing PTP-domain behaviour.
 * Realtime ALAC is different: every validated D7 is converted once from the
 * current GM's PTP domain into ESP monotonic time, and all lower audio timing
 * (staging deadlines, startup phase, DMA completion error and PID input) then
 * stays in that single local clock domain. A PTP estimator adjustment or GM
 * epoch change therefore cannot masquerade as physical loudspeaker motion. */
static bool timing_clock_ready(const timing_snapshot_t *snap) {
  if (!snap || !snap->anchor_valid || snap->timeline_reset_pending) return false;
  if (snap->stream_type == AUDIO_STREAM_REALTIME) return snap->anchor_local_ns != 0;
  return ptp_clock_is_locked();
}

static uint64_t presentation_now_ns(const timing_snapshot_t *snap) {
  if (snap && snap->stream_type == AUDIO_STREAM_REALTIME) {
    return (uint64_t)esp_timer_get_time() * 1000ULL;
  }
  return ptp_clock_get_time_ns();
}

static uint64_t presentation_anchor_ns(const timing_snapshot_t *snap) {
  return snap->stream_type == AUDIO_STREAM_REALTIME ? snap->anchor_local_ns
                                                     : snap->anchor_ptp_ns;
}

static bool wanted_rtp_now(const timing_snapshot_t *snap, uint32_t *out) {
  if (!snap || !out || !snap->playing || !timing_clock_ready(snap)) return false;
  const int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
  const int64_t now_ns = (int64_t)presentation_now_ns(snap);
  const int64_t dt_ns = now_ns - (int64_t)presentation_anchor_ns(snap);
  int64_t ds = (dt_ns * (int64_t)sr) / 1000000000LL;
  ds -= (int64_t)snap->playout_latency_samples;
  *out = snap->anchor_rtp + (uint32_t)ds;
  return true;
}

static bool wanted_rtp_at_presentation_ns(const timing_snapshot_t *snap,
                                           uint64_t time_ns, uint32_t *out) {
  if (!snap || !out || !snap->playing || !timing_clock_ready(snap)) return false;
  const int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
  const int64_t dt_ns = (int64_t)time_ns - (int64_t)presentation_anchor_ns(snap);
  int64_t ds = (dt_ns * (int64_t)sr) / 1000000000LL;
  ds -= (int64_t)snap->playout_latency_samples;
  *out = snap->anchor_rtp + (uint32_t)ds;
  return true;
}

static bool rtp_to_presentation_ns(const timing_snapshot_t *snap, uint32_t rtp,
                                    uint64_t *out_time_ns) {
  if (!snap || !out_time_ns || !timing_clock_ready(snap)) return false;
  const int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
  const int32_t ds = rtp_delta(rtp, snap->anchor_rtp);
  const int64_t dt_ns = ((int64_t)ds * 1000000000LL) / (int64_t)sr;
  const int64_t latency_ns =
      ((int64_t)snap->playout_latency_samples * 1000000000LL) / (int64_t)sr;
  const int64_t t = (int64_t)presentation_anchor_ns(snap) + dt_ns + latency_ns;
  if (t < 0) return false;
  *out_time_ns = (uint64_t)t;
  return true;
}

static int64_t completion_presentation_ns(const timing_snapshot_t *snap,
                                          const audio_playout_completion_t *done) {
  const int64_t local_ns = done->done_local_us * 1000LL;
  if (snap->stream_type == AUDIO_STREAM_REALTIME) return local_ns;
  return local_ns + ptp_clock_get_offset_ns();
}

/* Pace DMA submission in the stream's presentation clock. For AAC this is
 * still PTP. For realtime ALAC it is ESP monotonic time after D7->local
 * conversion. */
static void wait_until_presentation_ns(const timing_snapshot_t *snap,
                                       uint64_t target_ns) {
  while (s.engine_running) {
    const uint64_t now = presentation_now_ns(snap);
    if (now >= target_ns) return;
    const uint64_t remain_us = (target_ns - now) / 1000ULL;
    if (remain_us > 2000ULL) {
      vTaskDelay(1);
    } else if (remain_us > 250ULL) {
      esp_rom_delay_us(100);
    } else if (remain_us > 40ULL) {
      esp_rom_delay_us(20);
    } else {
      esp_rom_delay_us(2);
    }
  }
}

/* Buffered AirPlay 2 follows the Shairport Sync model: transport ordering,
 * timeline validity and decoded-player buffering are separate concerns. */
static inline uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int32_t seq23_delta(uint32_t a, uint32_t b) {
  uint32_t d = (a - b) & 0x007fffffU;
  if (d & 0x00400000U) d |= 0xff800000U;
  return (int32_t)d;
}

static bool deferred_flush_should_drop(uint32_t seq, uint32_t rtp) {
  bool drop = false;
  struct {
    uint32_t index;
    uint32_t event; /* 1=activate, 2=until, 3=overshoot, 4=skipped-range */
    uint32_t from_seq;
    uint32_t from_rtp;
    uint32_t until_seq;
    uint32_t until_rtp;
    uint32_t drops;
    uint32_t first_drop_seq;
    uint32_t first_drop_rtp;
    uint32_t last_drop_seq;
    uint32_t last_drop_rtp;
  } events[AP2_MAX_DEFERRED_FLUSH];
  uint32_t event_count = 0;

  taskENTER_CRITICAL(&s.state_mux);
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    ap2_flush_request_t *r = &s.deferred_flush[i];
    if (!r->in_use) continue;

    const int32_t from_delta = seq23_delta(seq, r->from_seq);
    const int32_t until_delta = seq23_delta(seq, r->until_seq);

    /* A delayed FLUSH is a transport range, not an equality rendezvous.
     * Normally the sender delivers from_seq exactly. If Automix jumps over
     * that value but lands inside the advertised range, start dropping from
     * the first block we actually received instead of leaving the request
     * permanently inactive. */
    if (!r->active && from_delta >= 0 && until_delta < 0) {
      r->active = true;
      if (event_count < AP2_MAX_DEFERRED_FLUSH) {
        events[event_count].index = i;
        events[event_count].event = 1U;
        events[event_count].from_seq = r->from_seq;
        events[event_count].from_rtp = r->from_rtp;
        events[event_count].until_seq = r->until_seq;
        events[event_count].until_rtp = r->until_rtp;
        event_count++;
      }
    }

    if (until_delta >= 0) {
      /* If from_seq was never observed and transport jumped straight to (or
       * beyond) until_seq, the boundary block is exactly the block that used
       * to pin the AAC processor as a multi-second future current_block.
       * Consume that one anomalous boundary block as part of recovery so the
       * processor can immediately inspect the following transport block.
       * Normal FLUSH endpoints are unchanged. */
      const bool skipped_range = !r->active && from_delta > 0;
      const uint32_t event =
          skipped_range ? 4U : (until_delta == 0 ? 2U : 3U);
      if (event_count < AP2_MAX_DEFERRED_FLUSH) {
        events[event_count].index = i;
        events[event_count].event = event;
        events[event_count].from_seq = r->from_seq;
        events[event_count].from_rtp = r->from_rtp;
        events[event_count].until_seq = r->until_seq;
        events[event_count].until_rtp = r->until_rtp;
        events[event_count].drops = r->diag_drop_count;
        events[event_count].first_drop_seq = r->diag_first_drop_seq;
        events[event_count].first_drop_rtp = r->diag_first_drop_rtp;
        events[event_count].last_drop_seq = r->diag_last_drop_seq;
        events[event_count].last_drop_rtp = r->diag_last_drop_rtp;
        event_count++;
      }
      if (skipped_range) {
        drop = true;
      }
      r->active = false;
      r->in_use = false;
      continue;
    }

    if (r->active) {
      if (r->diag_drop_count == 0) {
        r->diag_first_drop_seq = seq;
        r->diag_first_drop_rtp = rtp;
      }
      r->diag_drop_count++;
      r->diag_last_drop_seq = seq;
      r->diag_last_drop_rtp = rtp;
      drop = true;
    }
  }
  taskEXIT_CRITICAL(&s.state_mux);

  for (uint32_t i = 0; i < event_count; ++i) {
    if (events[i].event == 1U) {
      ESP_LOGI(TAG,
               "DFLUSH activate idx=%" PRIu32 " seq=%" PRIu32
               " rtp=%" PRIu32 " req_seq=%" PRIu32 "..%" PRIu32
               " req_rtp=%" PRIu32 "..%" PRIu32,
               events[i].index, seq, rtp, events[i].from_seq,
               events[i].until_seq, events[i].from_rtp, events[i].until_rtp);
    } else if (events[i].event == 4U) {
      ESP_LOGW(TAG,
               "DFLUSH skipped-range idx=%" PRIu32
               " seq=%" PRIu32 " rtp=%" PRIu32
               " req_seq=%" PRIu32 "..%" PRIu32
               " req_rtp=%" PRIu32 "..%" PRIu32
               " boundary_drop=1",
               events[i].index, seq, rtp, events[i].from_seq,
               events[i].until_seq, events[i].from_rtp, events[i].until_rtp);
    } else {
      ESP_LOGI(TAG,
               "DFLUSH %s idx=%" PRIu32 " seq=%" PRIu32 " rtp=%" PRIu32
               " req_seq=%" PRIu32 "..%" PRIu32
               " req_rtp=%" PRIu32 "..%" PRIu32
               " dropped=%" PRIu32
               " first=%" PRIu32 "/%" PRIu32
               " last=%" PRIu32 "/%" PRIu32,
               events[i].event == 2U ? "until" : "overshoot",
               events[i].index, seq, rtp, events[i].from_seq,
               events[i].until_seq, events[i].from_rtp, events[i].until_rtp,
               events[i].drops, events[i].first_drop_seq,
               events[i].first_drop_rtp, events[i].last_drop_seq,
               events[i].last_drop_rtp);
    }
  }
  return drop;
}

/* Diagnostic only: when a deferred FLUSH is pending, report any transport
 * sequence/RTP discontinuity before FLUSH state is applied. This proves
 * whether fromSeq was actually delivered by the sender or skipped in the
 * compressed TCP stream. */
static void deferred_flush_diag_transport(uint32_t seq, uint32_t rtp,
                                          uint32_t prev_seq, uint32_t prev_rtp,
                                          bool have_prev,
                                          uint32_t frame_samples) {
  if (!have_prev) return;

  const uint32_t expected_seq = (prev_seq + 1U) & 0x007fffffU;
  const uint32_t expected_rtp = prev_rtp + frame_samples;
  const int32_t seq_gap = seq23_delta(seq, expected_seq);
  const int32_t rtp_gap = rtp_delta(rtp, expected_rtp);
  if (seq_gap == 0 && rtp_gap == 0) return;

  ap2_flush_request_t pending[AP2_MAX_DEFERRED_FLUSH];
  uint32_t pending_index[AP2_MAX_DEFERRED_FLUSH];
  uint32_t count = 0;
  taskENTER_CRITICAL(&s.state_mux);
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    if (!s.deferred_flush[i].in_use) continue;
    if (count < AP2_MAX_DEFERRED_FLUSH) {
      pending[count] = s.deferred_flush[i];
      pending_index[count] = i;
      count++;
    }
  }
  taskEXIT_CRITICAL(&s.state_mux);

  for (uint32_t i = 0; i < count; ++i) {
    ESP_LOGW(TAG,
             "DFLUSH transport gap idx=%" PRIu32
             " prev=%" PRIu32 "/%" PRIu32
             " expected=%" PRIu32 "/%" PRIu32
             " current=%" PRIu32 "/%" PRIu32
             " seq_gap=%" PRId32 " rtp_gap=%" PRId32
             " req_seq=%" PRIu32 "..%" PRIu32
             " req_rtp=%" PRIu32 "..%" PRIu32 " active=%u",
             pending_index[i], prev_seq, prev_rtp, expected_seq, expected_rtp,
             seq, rtp, seq_gap, rtp_gap, pending[i].from_seq,
             pending[i].until_seq, pending[i].from_rtp, pending[i].until_rtp,
             pending[i].active ? 1U : 0U);
  }
}

static bool immediate_flush_should_drop(uint32_t seq) {
  bool drop = false;
  bool completed = false;
  uint32_t until_seq = 0;
  uint32_t start_seq = 0;
  int64_t start_us = 0;
  uint64_t start_drop = 0;
  size_t start_fifo = 0;
  uint64_t start_socket = 0;
  uint64_t start_read = 0;

  taskENTER_CRITICAL(&s.state_mux);
  if (s.immediate_flush_requested) {
    if (!s.immediate_flush_has_endpoint) {
      s.immediate_flush_requested = false;
      s.immediate_flush_diag_active = false;
    } else if (seq23_delta(seq, s.immediate_flush_until_seq) >= 0) {
      until_seq = s.immediate_flush_until_seq;
      start_seq = s.immediate_flush_start_seq;
      start_us = s.immediate_flush_start_us;
      start_drop = s.immediate_flush_start_drop;
      start_fifo = s.immediate_flush_start_fifo;
      start_socket = s.immediate_flush_start_socket_bytes;
      start_read = s.immediate_flush_start_fifo_read;
      completed = s.immediate_flush_diag_active;
      s.immediate_flush_requested = false;
      s.immediate_flush_has_endpoint = false;
      s.immediate_flush_diag_active = false;

      /* Shairport Sync parity: deferred requests remain live while an
       * endpoint-bounded immediate flush is in progress, because their ranges
       * may be consumed by the same purged transport blocks. Only when the
       * immediate endpoint is reached are any remaining deferred requests
       * cancelled. */
      for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
        s.deferred_flush[i].in_use = false;
        s.deferred_flush[i].active = false;
      }
    } else {
      drop = true;
    }
  }
  taskEXIT_CRITICAL(&s.state_mux);

  if (completed) {
    ap2_buffered_transport_stats_t ts = {0};
    if (s.transport) ap2_buffered_transport_get_stats(s.transport, &ts);
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    uint64_t dropped = s.diag.timeline_drop - start_drop;
    uint64_t drained = ts.fifo_bytes_read - start_read;
    uint64_t received = ts.socket_bytes - start_socket;
    ESP_LOGI(TAG,
             "FLUSH complete seq=%" PRIu32 " until=%" PRIu32
             " elapsed=%.1fms dropped=%" PRIu64
             " fifo=%u/%uKiB start=%uKiB drained=%" PRIu64
             "KiB recv=%" PRIu64 "KiB",
             seq, until_seq, (double)elapsed_us / 1000.0, dropped,
             (unsigned)(ts.fifo_occupancy / 1024U),
             (unsigned)(ts.fifo_high_water / 1024U),
             (unsigned)(start_fifo / 1024U), drained / 1024U,
             received / 1024U);
  }
  return drop;
}

static bool buffered_flush_should_drop(uint32_t seq, uint32_t rtp) {
  /* Do not use short-circuit `immediate || deferred` here. Shairport Sync
   * deliberately services deferred flush state even while an immediate flush
   * is actively dropping the same block. This keeps deferred activation and
   * termination semantics in transport order. */
  bool immediate_drop = immediate_flush_should_drop(seq);
  bool deferred_drop = deferred_flush_should_drop(seq, rtp);
  return immediate_drop || deferred_drop;
}

/* Diagnostic only: preserve the transport predecessor for the decoded block
 * and emit a richer snapshot only for discontinuities of at least four AAC
 * frames. This catches the rare multi-second positive hole without adding
 * per-packet logging or changing any transport/playout decision. */
static void diag_large_aac_timestamp_gap(
    uint32_t seq, uint32_t rtp, uint32_t decoded_expected,
    int32_t timestamp_gap, uint32_t transport_prev_seq,
    uint32_t transport_prev_rtp, bool have_transport_prev,
    uint32_t frame_samples, uint32_t wanted, int sample_rate,
    uint32_t generation) {
  const int64_t abs_gap = timestamp_gap < 0 ? -(int64_t)timestamp_gap
                                             : (int64_t)timestamp_gap;
  const int64_t threshold = (int64_t)frame_samples * 4LL;
  if (abs_gap < threshold) return;

  uint32_t expected_seq = 0;
  uint32_t expected_rtp = 0;
  int32_t seq_gap = 0;
  int32_t rtp_gap = 0;
  if (have_transport_prev) {
    expected_seq = (transport_prev_seq + 1U) & 0x007fffffU;
    expected_rtp = transport_prev_rtp + frame_samples;
    seq_gap = seq23_delta(seq, expected_seq);
    rtp_gap = rtp_delta(rtp, expected_rtp);
  }

  ap2_flush_request_t pending[AP2_MAX_DEFERRED_FLUSH];
  uint32_t pending_index[AP2_MAX_DEFERRED_FLUSH];
  uint32_t pending_count = 0;
  uint32_t active_count = 0;
  bool immediate_requested = false;
  bool immediate_has_endpoint = false;
  taskENTER_CRITICAL(&s.state_mux);
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    if (!s.deferred_flush[i].in_use) continue;
    if (pending_count < AP2_MAX_DEFERRED_FLUSH) {
      pending[pending_count] = s.deferred_flush[i];
      pending_index[pending_count] = i;
      pending_count++;
    }
    if (s.deferred_flush[i].active) active_count++;
  }
  immediate_requested = s.immediate_flush_requested;
  immediate_has_endpoint = s.immediate_flush_has_endpoint;
  taskEXIT_CRITICAL(&s.state_mux);

  ap2_buffered_transport_stats_t ts = {0};
  if (s.transport) ap2_buffered_transport_get_stats(s.transport, &ts);
  const size_t fifo_capacity =
      s.transport ? ap2_buffered_transport_capacity(s.transport) : 0U;
  const int sr = sample_rate > 0 ? sample_rate : 44100;
  const int32_t decoded_ahead_ms =
      (int32_t)(((int64_t)rtp_delta(decoded_expected, wanted) * 1000LL) / sr);
  const int32_t incoming_lead_ms =
      (int32_t)(((int64_t)rtp_delta(rtp, wanted) * 1000LL) / sr);

  ESP_LOGW(TAG,
           "AAC GAPDIAG seq=%" PRIu32 " rtp=%" PRIu32
           " dec_expected=%" PRIu32 " gap=%" PRId32
           " tprev_valid=%u tprev=%" PRIu32 "/%" PRIu32
           " texpected=%" PRIu32 "/%" PRIu32
           " seq_gap=%" PRId32 " rtp_gap=%" PRId32
           " wanted=%" PRIu32 " dec_ahead=%" PRId32 "ms"
           " incoming_lead=%" PRId32 "ms dflush=%" PRIu32 "/%" PRIu32
           " iflush=%u/%u fifo=%u/%uK gen=%" PRIu32,
           seq, rtp, decoded_expected, timestamp_gap,
           have_transport_prev ? 1U : 0U, transport_prev_seq,
           transport_prev_rtp, expected_seq, expected_rtp, seq_gap, rtp_gap,
           wanted, decoded_ahead_ms, incoming_lead_ms, pending_count,
           active_count, immediate_requested ? 1U : 0U,
           immediate_has_endpoint ? 1U : 0U,
           (unsigned)(ts.fifo_occupancy / 1024U),
           (unsigned)(fifo_capacity / 1024U), generation);

  for (uint32_t i = 0; i < pending_count; ++i) {
    ESP_LOGW(TAG,
             "AAC GAPDIAG flush idx=%" PRIu32
             " req_seq=%" PRIu32 "..%" PRIu32
             " req_rtp=%" PRIu32 "..%" PRIu32 " active=%u",
             pending_index[i], pending[i].from_seq, pending[i].until_seq,
             pending[i].from_rtp, pending[i].until_rtp,
             pending[i].active ? 1U : 0U);
  }
}

static void update_transport_continuity(uint32_t seq, uint32_t rtp,
                                        uint32_t *prev_seq,
                                        uint32_t *prev_rtp,
                                        bool *have_prev,
                                        uint32_t frame_samples) {
  if (*have_prev) {
    if (seq != ((*prev_seq + 1U) & 0x007fffffU)) s.diag.seq_gap++;
    if ((uint32_t)(rtp - *prev_rtp) != frame_samples) s.diag.rtp_gap++;
  }
  *prev_seq = seq;
  *prev_rtp = rtp;
  *have_prev = true;
  s.diag.last_seq = seq;
  s.diag.last_rtp = rtp;
}

static void pcm_process_common_eq(int16_t *pcm, size_t frames, int channels,
                                  int sample_rate) {
  /* This is the codec boundary: AAC and ALAC are fully independent up to
   * decoded PCM. From here both formats use the same EQ implementation. */
  audio_eq_process(pcm, frames, channels, sample_rate);
}

static bool pcm_store_with_backpressure(uint32_t rtp, const int16_t *pcm,
                                        size_t frames, int channels,
                                        uint32_t generation) {
  while (s.rx_running) {
    timing_snapshot_t snap;
    snapshot_state(&snap);
    if (snap.generation != generation || snap.stream_type != AUDIO_STREAM_BUFFERED)
      return false;
    uint32_t wanted = 0;
    bool wanted_valid = wanted_rtp_now(&snap, &wanted);
    if (wanted_valid && rtp_delta(rtp + (uint32_t)frames, wanted) <= 0) {
      s.diag.stale_predecode++;
      s.public_stats.late_frames++;
      return false;
    }
    if (pcm_rtp_ring_write(s.pcm_ring, rtp, pcm, frames, channels, generation,
                           wanted, wanted_valid)) return true;
    /* Decoded player buffer full/colliding: stop consuming the TCP FIFO. The
     * reader will eventually fill its bounded FIFO and TCP will throttle the
     * sender. No audio is deliberately dropped for transient pressure. */
    vTaskDelay(1);
  }
  return false;
}

static TickType_t delay_ticks_at_least_one(uint32_t delay_ms) {
  TickType_t ticks = pdMS_TO_TICKS(delay_ms);
  return ticks > 0 ? ticks : (TickType_t)1;
}

static void ap2_buffered_processor_task(void *arg) {
  (void)arg;
  aac_decoder_t *decoder = NULL;
  uint32_t decoder_format_generation = 0;
  uint32_t previous_seq = 0, previous_rtp = 0;
  uint32_t expected_timestamp = 0;
  bool have_previous_transport = false;
  bool have_decoded_sequence = false;
  bool mute_next_aac = true;
  uint32_t decode_generation = 0;

  /* Shairport-style processor ownership state. A block stays current until it
   * is explicitly dropped by FLUSH/staleness or successfully handed to the
   * decoded-player path. We never fetch the next TCP block merely because
   * playback timing is temporarily unavailable. */
  bool play_enabled = false;
  bool current_block_valid = false;
  bool need_new_block = true;
  uint16_t current_data_len = 0;
  size_t current_packet_len = 0;
  uint32_t current_seq = 0;
  uint32_t current_rtp = 0;
  uint32_t current_ssrc = 0;
  uint32_t current_transport_prev_seq = 0;
  uint32_t current_transport_prev_rtp = 0;
  bool current_have_transport_prev = false;
  uint64_t packets_played_in_sequence = 0;
  uint32_t observed_generation = 0;

  ESP_LOGI(TAG, "Shairport-style buffered processor core=%d prio=%u",
           xPortGetCoreID(), (unsigned)AP2_DECODE_PRIORITY);

  while (s.rx_running) {
    timing_snapshot_t state_snap;
    snapshot_state(&state_snap);
    bool control_play_enabled =
        state_snap.stream_type == AUDIO_STREAM_BUFFERED && state_snap.playing;

    if (control_play_enabled != play_enabled) {
      ESP_LOGI(TAG,
               "BSTATE play %u->%u gen=%" PRIu32
               " current=%u need_new=%u played=%" PRIu64,
               play_enabled ? 1U : 0U, control_play_enabled ? 1U : 0U,
               state_snap.generation, current_block_valid ? 1U : 0U,
               need_new_block ? 1U : 0U, packets_played_in_sequence);
      if (!play_enabled && control_play_enabled) {
        /* Shairport Sync sets new_audio_block_needed=1 on every play start.
         * Do the same: a block held while stopped belongs to the old stopped
         * sequence and must not become the first block of the resumed one. */
        current_block_valid = false;
        need_new_block = true;
        packets_played_in_sequence = 0;
      } else if (play_enabled && !control_play_enabled) {
        packets_played_in_sequence = 0;
      }
      play_enabled = control_play_enabled;
    }

    if (observed_generation != state_snap.generation) {
      observed_generation = state_snap.generation;
      packets_played_in_sequence = 0;
    }

    if (need_new_block || !current_block_valid) {
      uint8_t lb[2];
      ssize_t n = ap2_buffered_transport_read_exact(s.transport, lb, sizeof(lb));
      if (n != 2) {
        if (s.rx_running) vTaskDelay(delay_ticks_at_least_one(5));
        continue;
      }
      current_data_len = ((uint16_t)lb[0] << 8) | lb[1];
      if (current_data_len < 14U || current_data_len > AP2_PACKET_MAX + 2U) {
        ESP_LOGW(TAG, "invalid buffered block length=%u", (unsigned)current_data_len);
        need_new_block = true;
        current_block_valid = false;
        continue;
      }
      current_packet_len = (size_t)current_data_len - 2U;
      if (ap2_buffered_transport_read_exact(s.transport, s.packet,
                                             current_packet_len) !=
          (ssize_t)current_packet_len) {
        need_new_block = true;
        current_block_valid = false;
        continue;
      }

      current_seq = be32(s.packet) & 0x007fffffU;
      current_rtp = be32(s.packet + 4);
      current_ssrc = be32(s.packet + 8);
      (void)current_ssrc;
      current_block_valid = true;
      need_new_block = false;

      s.diag.rx++;
      taskENTER_CRITICAL(&s.state_mux);
      s.buffered_session_seq = current_seq;
      s.buffered_session_rx++;
      s.buffered_session_seq_valid = true;
      taskEXIT_CRITICAL(&s.state_mux);
      s.public_stats.packets_received++;
      s.public_stats.last_seq = (uint16_t)(current_seq & 0xffffU);
      s.public_stats.last_timestamp = current_rtp;

      snapshot_state(&state_snap);
      uint32_t frame_samples = state_snap.format.frame_size > 0 ?
          (uint32_t)state_snap.format.frame_size : 1024U;
      current_transport_prev_seq = previous_seq;
      current_transport_prev_rtp = previous_rtp;
      current_have_transport_prev = have_previous_transport;
      deferred_flush_diag_transport(current_seq, current_rtp, previous_seq,
                                    previous_rtp, have_previous_transport,
                                    frame_samples);
      update_transport_continuity(current_seq, current_rtp, &previous_seq,
                                  &previous_rtp, &have_previous_transport,
                                  frame_samples);
    }

    /* FLUSH is transport-order state. Service it even while playback is
     * stopped or an anchor is not yet usable. A dropped current block is then
     * replaced by the next transport block. */
    if (buffered_flush_should_drop(current_seq, current_rtp)) {
      s.diag.timeline_drop++;
      current_block_valid = false;
      need_new_block = true;
      continue;
    }

    /* Shairport keeps the current block while play/timing is unavailable.
     * This is the key ownership invariant: do not read ahead in the processor;
     * let the bounded raw FIFO/TCP backpressure absorb the transition. */
    snapshot_state(&state_snap);
    if (!play_enabled || state_snap.stream_type != AUDIO_STREAM_BUFFERED ||
        !state_snap.playing || !state_snap.anchor_valid ||
        state_snap.timeline_reset_pending || !ptp_clock_is_locked()) {
      vTaskDelay(delay_ticks_at_least_one(20));
      continue;
    }

    uint32_t frame_samples = state_snap.format.frame_size > 0 ?
        (uint32_t)state_snap.format.frame_size : 1024U;
    uint32_t wanted = 0;
    if (!wanted_rtp_now(&state_snap, &wanted)) {
      vTaskDelay(delay_ticks_at_least_one(20));
      continue;
    }

    int sr = state_snap.format.sample_rate > 0 ? state_snap.format.sample_rate : 44100;
    int32_t lead = rtp_delta(current_rtp, wanted);
    int32_t max_lead =
        (int32_t)(((int64_t)sr * AP2_BUFFERED_LEAD_MS) / 1000LL);
    if (lead > max_lead) {
      vTaskDelay(delay_ticks_at_least_one(5));
      continue;
    }
    if (lead + (int32_t)frame_samples <= 0) {
      s.diag.stale_predecrypt++;
      s.public_stats.late_frames++;
      current_block_valid = false;
      need_new_block = true;
      continue;
    }

    timing_snapshot_t snap = state_snap;
    if (decode_generation != snap.generation) {
      decode_generation = snap.generation;
      decoder_format_generation = 0;
      have_decoded_sequence = false;
      mute_next_aac = true;
      expected_timestamp = 0;
      packets_played_in_sequence = 0;
      audio_eq_reset_state();
    }

    int dec_len = audio_crypto_decrypt_buffered(&s.encrypt, s.packet,
                                                 current_packet_len,
                                                 s.decrypt_buf, AP2_PACKET_MAX);
    if (dec_len < 0) {
      s.diag.decrypt_error++;
      s.public_stats.decrypt_errors++;
      current_block_valid = false;
      need_new_block = true;
      continue;
    }
    if (dec_len == 0) {
      s.diag.empty_payload++;
      current_block_valid = false;
      need_new_block = true;
      continue;
    }

    if (!decoder || decoder_format_generation != snap.format_generation) {
      if (decoder) aac_decoder_destroy(decoder);
      decoder = NULL;
      aac_decoder_config_t cfg = {
          .sample_rate = snap.format.sample_rate,
          .channels = snap.format.channels,
          .bits_per_sample = snap.format.bits_per_sample,
      };
      decoder = aac_decoder_create(&cfg);
      if (!decoder) {
        s.diag.decode_error++;
        s.public_stats.packets_dropped++;
        current_block_valid = false;
        need_new_block = true;
        vTaskDelay(1);
        continue;
      }
      decoder_format_generation = snap.format_generation;
      have_decoded_sequence = false;
      mute_next_aac = true;
      ESP_LOGI(TAG, "AAC decoder ready %dHz %dch", snap.format.sample_rate,
               snap.format.channels);
    }

    int32_t timestamp_gap = 0;
    if (have_decoded_sequence) {
      timestamp_gap = rtp_delta(current_rtp, expected_timestamp);
      if (timestamp_gap != 0) {
        mute_next_aac = true;
        ESP_LOGI(TAG,
                 "AAC timestamp discontinuity seq=%" PRIu32 " rtp=%" PRIu32
                 " expected=%" PRIu32 " gap=%" PRId32,
                 current_seq, current_rtp, expected_timestamp, timestamp_gap);
        diag_large_aac_timestamp_gap(
            current_seq, current_rtp, expected_timestamp, timestamp_gap,
            current_transport_prev_seq, current_transport_prev_rtp,
            current_have_transport_prev, frame_samples, wanted, sr,
            snap.generation);
      }
    }

    aac_decode_info_t info = {0};
    int frames = aac_decoder_decode(decoder, s.decrypt_buf, (size_t)dec_len,
                                    s.decode_pcm, AP2_PCM_CAPACITY_FRAMES, &info);
    if (frames < 0) {
      s.diag.decode_error++;
      s.public_stats.packets_dropped++;
      current_block_valid = false;
      need_new_block = true;
      continue;
    }
    if (frames == 0) {
      current_block_valid = false;
      need_new_block = true;
      vTaskDelay(1);
      continue;
    }

    if (!have_decoded_sequence || mute_next_aac) {
      memset(s.decode_pcm, 0,
             (size_t)frames * (size_t)info.channels * sizeof(int16_t));
      ESP_LOGI(TAG,
               "AAC block muted seq=%" PRIu32 " rtp=%" PRIu32 " reason=%s",
               current_seq, current_rtp,
               !have_decoded_sequence ? "sequence-start" : "discontinuity");
      mute_next_aac = false;
    }

    expected_timestamp = current_rtp + (uint32_t)frames;
    have_decoded_sequence = true;

    if (packets_played_in_sequence == 0) {
      int64_t anchor_us = 0;
      uint32_t anchor_gen = 0;
      taskENTER_CRITICAL(&s.state_mux);
      anchor_us = s.anchor_set_local_us;
      anchor_gen = s.anchor_set_generation;
      taskEXIT_CRITICAL(&s.state_mux);
      if (anchor_gen == snap.generation && anchor_us > 0) {
        int32_t ms = (int32_t)((esp_timer_get_time() - anchor_us) / 1000LL);
        s.diag.first_decode_from_anchor_ms = ms;
        s.diag.first_decode_generation = snap.generation;
        ESP_LOGI(TAG,
                 "BSTATE FIRST_DECODE gen=%" PRIu32
                 " anchor_to_decode=%" PRId32 "ms seq=%" PRIu32
                 " rtp=%" PRIu32,
                 snap.generation, ms, current_seq, current_rtp);
      }
    }

    pcm_process_common_eq(s.decode_pcm, (size_t)frames, info.channels,
                          snap.format.sample_rate);
    if (pcm_store_with_backpressure(current_rtp, s.decode_pcm, (size_t)frames,
                                    info.channels, snap.generation)) {
      s.diag.decoded++;
      s.public_stats.packets_decoded++;
      s.diag.last_decoded_end_rtp = current_rtp + (uint32_t)frames;
      packets_played_in_sequence++;
      current_block_valid = false;
      need_new_block = true;
      vTaskDelay(1);
      continue;
    }

    /* Generation/timeline changed while the decoded PCM was waiting for room.
     * The decoded block no longer belongs to the active player sequence. */
    current_block_valid = false;
    need_new_block = true;
  }

  if (decoder) aac_decoder_destroy(decoder);
  s.processor_task = NULL;
  vTaskDelete(NULL);
}

static bool frame_is_fully_stale(uint32_t rtp, const timing_snapshot_t *snap,
                                 int64_t *end_delta_us) {
  uint32_t wanted;
  if (!wanted_rtp_now(snap, &wanted)) {
    return false;
  }
  uint32_t frame_samples = snap->format.frame_size > 0
                               ? (uint32_t)snap->format.frame_size
                               : 1024U;
  uint32_t end_rtp = rtp + frame_samples;
  int32_t remaining = rtp_delta(end_rtp, wanted);
  if (end_delta_us) {
    int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
    *end_delta_us = ((int64_t)remaining * 1000000LL) / sr;
  }
  return remaining <= 0;
}

static bool realtime_playout_deadline(uint32_t rtp,
                                      int64_t *time_to_play_us, void *ctx) {
  (void)ctx;
  if (!time_to_play_us) return false;

  timing_snapshot_t snap;
  snapshot_state(&snap);
  if (snap.stream_type != AUDIO_STREAM_REALTIME || !snap.playing ||
      !snap.anchor_valid || snap.timeline_reset_pending) {
    return false;
  }

  uint64_t target_time_ns = 0;
  if (!rtp_to_presentation_ns(&snap, rtp, &target_time_ns)) return false;

  const int64_t now_time_ns = (int64_t)presentation_now_ns(&snap);
  *time_to_play_us = ((int64_t)target_time_ns - now_time_ns) / 1000LL;
  return true;
}

static bool realtime_pcm_sink(uint32_t rtp, int16_t *pcm, size_t frames,
                              int channels, void *ctx) {
  (void)ctx;
  if (!pcm || frames == 0 || channels != 2) {
    return false;
  }

  /* Realtime ALAC is decoded immediately by the UDP receiver. This sink does
   * not run EQ and does not write the final playout ring; it only places raw
   * PCM at its exact RTP address so later/RTX packets can fill holes without
   * blocking newer packets. */
  timing_snapshot_t snap;
  snapshot_state(&snap);
  if (snap.stream_type != AUDIO_STREAM_REALTIME) {
    return false;
  }

  /* Never invent RTP<->PTP from packet arrival time. Before the first real
   * sender anchor, realtime PCM may be stored by RTP address in the prepared
   * generation, but staging/playout remain stopped until D7/SETRATE makes
   * anchor_valid true. After an unqualified FLUSH (timeline reset pending),
   * reject provisional PCM until the sender supplies a fresh real anchor. */
  if (snap.timeline_reset_pending) {
    s.diag.realtime_sink_no_ptp++;
    return false;
  }

  if (snap.anchor_valid && frame_is_fully_stale(rtp, &snap, NULL)) {
    s.diag.realtime_sink_stale++;
    s.diag.stale_predecode++;
    s.public_stats.late_frames++;
    s.public_stats.packets_dropped++;
    return false;
  }

  /* Once the chronological EQ cursor has passed an RTP range, a late RTX
   * cannot repair the stateful filter history. Drop it without touching raw
   * or final PCM. */
  bool cursor_valid = false;
  uint32_t cursor = 0;
  taskENTER_CRITICAL(&s.state_mux);
  if (s.realtime_stage_cursor_valid &&
      s.realtime_stage_generation == snap.generation) {
    cursor_valid = true;
    cursor = s.realtime_stage_cursor_rtp;
  }
  taskEXIT_CRITICAL(&s.state_mux);
  if (cursor_valid && rtp_delta(rtp + (uint32_t)frames, cursor) <= 0) {
    s.realtime_stage_late_fill++;
    s.public_stats.packets_received++;
    s.public_stats.last_timestamp = rtp;
    return true;
  }

  uint32_t wanted = 0;
  bool wanted_valid = wanted_rtp_now(&snap, &wanted);
  if (!s.realtime_stage_ring ||
      !pcm_rtp_ring_write(s.realtime_stage_ring, rtp, pcm, frames, channels,
                          snap.generation, wanted, wanted_valid)) {
    s.diag.realtime_sink_ring++;
    s.diag.pcm_write_error++;
    s.public_stats.packets_dropped++;
    return false;
  }

  taskENTER_CRITICAL(&s.state_mux);
  if (!s.realtime_stage_cursor_valid ||
      s.realtime_stage_generation != snap.generation) {
    s.realtime_stage_cursor_rtp = rtp;
    s.realtime_stage_generation = snap.generation;
    s.realtime_stage_cursor_valid = true;
  }
  taskEXIT_CRITICAL(&s.state_mux);

  /* Wake ordered staging only when new RAW PCM has actually been published.
   * Counting task notifications coalesce bursts and cannot lose a wakeup if
   * the write races with the task entering its blocked state. */
  realtime_stage_kick();

  s.public_stats.packets_received++;
  s.public_stats.last_timestamp = rtp;
  return true;
}

static inline void atomic_min_i32(volatile int32_t *dst, int32_t value) {
  int32_t cur = __atomic_load_n(dst, __ATOMIC_RELAXED);
  while (value < cur &&
         !__atomic_compare_exchange_n(dst, &cur, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

static inline void atomic_max_u32(volatile uint32_t *dst, uint32_t value) {
  uint32_t cur = __atomic_load_n(dst, __ATOMIC_RELAXED);
  while (value > cur &&
         !__atomic_compare_exchange_n(dst, &cur, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

static void realtime_stage_task(void *arg) {
  (void)arg;
  int16_t *pcm = heap_caps_malloc(
      (size_t)AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!pcm) {
    pcm = malloc((size_t)AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
  }
  if (!pcm) {
    ESP_LOGE(TAG, "ALAC staging PCM allocation failed");
    __atomic_store_n(&s.realtime_stage_idle, true, __ATOMIC_RELEASE);
    s.realtime_stage_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG,
           "ALAC staging: raw RTP PCM -> ordered EQ -> final PCM ring, core=%d prio=%u",
           xPortGetCoreID(), (unsigned)AP2_RT_STAGE_PRIORITY);

  uint32_t local_generation = 0;
  uint32_t missing_cursor = 0;
  int64_t missing_since_us = 0;
  __atomic_store_n(&s.realtime_stage_idle, true, __ATOMIC_RELEASE);

  while (s.engine_running) {
    if (!__atomic_load_n(&s.realtime_stage_running, __ATOMIC_ACQUIRE)) {
      __atomic_store_n(&s.realtime_stage_idle, true, __ATOMIC_RELEASE);
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    __atomic_store_n(&s.realtime_stage_idle, false, __ATOMIC_RELEASE);

    timing_snapshot_t snap;
    snapshot_state(&snap);
    if (snap.stream_type != AUDIO_STREAM_REALTIME || !snap.anchor_valid ||
        snap.timeline_reset_pending || snap.format.frame_size <= 0 ||
        snap.format.frame_size > (int)AP2_PCM_CAPACITY_FRAMES) {
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    uint32_t cursor = 0;
    bool cursor_valid = false;
    taskENTER_CRITICAL(&s.state_mux);
    if (s.realtime_stage_cursor_valid &&
        s.realtime_stage_generation == snap.generation) {
      cursor = s.realtime_stage_cursor_rtp;
      cursor_valid = true;
    }
    taskEXIT_CRITICAL(&s.state_mux);
    if (!cursor_valid) {
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    const uint32_t frames = (uint32_t)snap.format.frame_size;
    int64_t time_to_play_us = 0;
    const bool have_deadline =
        realtime_playout_deadline(cursor, &time_to_play_us, NULL);
    if (have_deadline) {
      int32_t ahead = time_to_play_us > INT32_MAX ? INT32_MAX :
                      time_to_play_us < INT32_MIN ? INT32_MIN :
                      (int32_t)time_to_play_us;
      __atomic_store_n(&s.realtime_stage_ahead_us, ahead, __ATOMIC_RELAXED);
      atomic_min_i32(&s.realtime_stage_min_ahead_us, ahead);
    }

    bool have = pcm_rtp_ring_read(s.realtime_stage_ring, cursor, frames,
                                  snap.generation, pcm);
    if (!have) {
      const int64_t now_us = esp_timer_get_time();
      if (missing_since_us == 0 || missing_cursor != cursor) {
        missing_cursor = cursor;
        missing_since_us = now_us;
        s.realtime_stage_missing++;
      }

      if (!have_deadline || time_to_play_us > AP2_RT_STAGE_COMMIT_MARGIN_US) {
        /* No polling: sleep until RAW PCM/RTX publication changes the ring,
         * or until the exact remaining recovery window reaches the existing
         * commit margin. If there is no valid deadline yet, a timing/state
         * transition will explicitly notify this task. */
        TickType_t wait_ticks = portMAX_DELAY;
        if (have_deadline) {
          wait_ticks = realtime_stage_wait_ticks(
              time_to_play_us - AP2_RT_STAGE_COMMIT_MARGIN_US);
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
        continue;
      }

      /* Recovery time is exhausted. Preserve RTP duration and EQ chronology
       * with one silent ALAC-sized PCM block. A later RTX is too late to
       * rewrite filter history and is discarded by realtime_pcm_sink(). */
      memset(pcm, 0, (size_t)frames * 2U * sizeof(int16_t));
      s.realtime_stage_silence++;
    } else if (missing_since_us != 0 && missing_cursor == cursor) {
      s.realtime_stage_recovered++;
    }
    if (missing_since_us != 0 && missing_cursor == cursor) {
      const int64_t waited = esp_timer_get_time() - missing_since_us;
      if (waited > 0) {
        atomic_max_u32(&s.realtime_stage_wait_max_us,
                       waited > UINT32_MAX ? UINT32_MAX : (uint32_t)waited);
      }
    }
    missing_since_us = 0;

    if (local_generation != snap.generation) {
      audio_eq_reset_state();
      local_generation = snap.generation;
      s.realtime_eq_generation = snap.generation;
    }

    pcm_process_common_eq(pcm, frames, 2, snap.format.sample_rate);

    /* Final ring is only the ordered, EQ-processed playout store. The raw
     * ring above is the reorder/jitter store for realtime ALAC. */
    if (!pcm_rtp_ring_write(s.pcm_ring, cursor, pcm, frames, 2,
                            snap.generation, 0U, false)) {
      s.diag.pcm_write_error++;
      vTaskDelay(1);
      continue;
    }

    s.diag.decoded++;
    s.diag.last_decoded_end_rtp = cursor + frames;
    s.public_stats.packets_decoded++;

    taskENTER_CRITICAL(&s.state_mux);
    if (s.realtime_stage_generation == snap.generation &&
        s.realtime_stage_cursor_valid &&
        s.realtime_stage_cursor_rtp == cursor) {
      s.realtime_stage_cursor_rtp = cursor + frames;
    }
    taskEXIT_CRITICAL(&s.state_mux);
  }

  __atomic_store_n(&s.realtime_stage_idle, true, __ATOMIC_RELEASE);
  free(pcm);
  s.realtime_stage_task = NULL;
  vTaskDelete(NULL);
}


static bool completion_sync_us(const timing_snapshot_t *snap,
                               const audio_playout_completion_t *done,
                               int32_t *sync_us_out) {
  if (!snap || !done || !sync_us_out || done->generation != snap->generation ||
      !snap->anchor_valid || snap->timeline_reset_pending) {
    return false;
  }

  uint64_t target_end_ns = 0;
  if (!rtp_to_presentation_ns(snap, done->rtp + done->frames, &target_end_ns)) {
    return false;
  }

  const int64_t done_time_ns = completion_presentation_ns(snap, done);
  *sync_us_out = (int32_t)(((int64_t)target_end_ns - done_time_ns) / 1000LL);
  return true;
}

static void process_i2s_completions(const timing_snapshot_t *snap) {
  audio_playout_completion_t done;
  while (audio_playout_poll_completion(&done)) {
    int32_t sync_us = 0;
    if (!completion_sync_us(snap, &done, &sync_us)) {
      continue;
    }

    s.diag.output_sync_raw_us = sync_us;
    if (!s.diag.output_sync_valid ||
        s.diag.output_sync_generation != done.generation) {
      s.diag.output_sync_us = sync_us;
      s.diag.output_sync_generation = done.generation;
      s.diag.output_sync_valid = true;
      s.diag.startup_sync_start_us = sync_us;
      s.diag.startup_sync_generation = done.generation;
      int64_t anchor_us = 0;
      uint32_t anchor_gen = 0;
      taskENTER_CRITICAL(&s.state_mux);
      anchor_us = s.anchor_set_local_us;
      anchor_gen = s.anchor_set_generation;
      taskEXIT_CRITICAL(&s.state_mux);
      s.diag.startup_anchor_to_sync_ms =
          (anchor_gen == done.generation && anchor_us > 0)
              ? (int32_t)((done.done_local_us - anchor_us) / 1000LL)
              : -1;
      __atomic_store_n(&s.diag.startup_sync_log_pending, 1U, __ATOMIC_RELEASE);
    } else {
      s.diag.output_sync_us += (sync_us - s.diag.output_sync_us) / 8;
    }

    const int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
    s.diag.output_sync_frames =
        (int32_t)(((int64_t)s.diag.output_sync_us * sr) / 1000000LL);
    s.diag.dma_tagged_completions++;
  }
}

static inline uint32_t abs_i16_u32(int16_t v) {
  return v == INT16_MIN ? 32768U : (uint32_t)(v < 0 ? -v : v);
}

static void apply_output_volume(int16_t *pcm, uint32_t frames,
                                int32_t *current_q15) {
  if (!pcm || !current_q15 || frames == 0U) return;
  int32_t target = __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
  if (target < 0) target = 0;
  if (target > 32768) target = 32768;
  const int32_t start = *current_q15;
  uint32_t peak_in = 0, peak_out = 0;
  uint64_t rail_in = 0, clip_out = 0;
  const int64_t dg = (int64_t)target - (int64_t)start;
  for (uint32_t f = 0; f < frames; ++f) {
    const int32_t gain = start + (int32_t)((dg * (int64_t)(f + 1U)) / (int64_t)frames);
    for (uint32_t ch = 0; ch < 2U; ++ch) {
      const uint32_t i = f * 2U + ch;
      const int16_t in = pcm[i];
      const uint32_t ai = abs_i16_u32(in);
      if (ai > peak_in) peak_in = ai;
      if (in == INT16_MAX || in == INT16_MIN) rail_in++;
      int64_t y = (int64_t)in * (int64_t)gain;
      y += y >= 0 ? 16384 : -16384;
      y >>= 15;
      if (y > INT16_MAX) { y = INT16_MAX; clip_out++; }
      else if (y < INT16_MIN) { y = INT16_MIN; clip_out++; }
      pcm[i] = (int16_t)y;
      const uint32_t ao = abs_i16_u32(pcm[i]);
      if (ao > peak_out) peak_out = ao;
    }
  }
  *current_q15 = target;
  uint32_t old_peak = __atomic_load_n(&s.diag.pcm_peak_in, __ATOMIC_RELAXED);
  while (peak_in > old_peak &&
         !__atomic_compare_exchange_n(&s.diag.pcm_peak_in, &old_peak, peak_in,
                                      false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
  old_peak = __atomic_load_n(&s.diag.pcm_peak_out, __ATOMIC_RELAXED);
  while (peak_out > old_peak &&
         !__atomic_compare_exchange_n(&s.diag.pcm_peak_out, &old_peak, peak_out,
                                      false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
  __atomic_add_fetch(&s.diag.pcm_rail_in, rail_in, __ATOMIC_RELAXED);
  __atomic_add_fetch(&s.diag.pcm_clip_out, clip_out, __ATOMIC_RELAXED);
  __atomic_store_n(&s.diag.volume_q15, target, __ATOMIC_RELAXED);
}

static void ap2_playout_task(void *arg) {
  (void)arg;
  int16_t *block = heap_caps_malloc(
      AUDIO_PLAYOUT_FRAMES * 2U * sizeof(int16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!block) {
    block = malloc(AUDIO_PLAYOUT_FRAMES * 2U * sizeof(int16_t));
  }
  if (!block) {
    ESP_LOGE(TAG, "playout block allocation failed");
    s.playout_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  typedef enum {
    PLAYOUT_STOPPED = 0,
    PLAYOUT_PRIMING = 1,
    PLAYOUT_RUNNING = 2,
  } playout_state_t;

  uint32_t cursor_rtp = 0; /* next block to submit after two preloaded blocks */
  uint32_t cursor_generation = 0;
  int32_t volume_current_q15 = __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
  playout_state_t state = PLAYOUT_STOPPED;
  int32_t servo_ppm = 0;
  int32_t servo_target_ppm = 0;
  int64_t pid_last_calc_us = 0;
  int64_t pid_last_tune_us = 0;
  uint32_t servo_generation = 0;
  double pid_integral_ms_s = 0.0;
  double pid_prev_error_ms = 0.0;
  double pid_d_filtered_ms_s = 0.0;
  bool pid_prev_valid = false;
  s.diag.playout_state = PLAYOUT_STOPPED;

  while (s.engine_running) {
    if (__atomic_exchange_n(&s.playout_servo_reset_requested, false,
                            __ATOMIC_ACQ_REL)) {
      /* A codec/session boundary is stronger than a normal timeline change.
       * Do not inherit the previous stream's learned crystal correction.
       * Keep ordinary AAC track/seek generations untouched so the mature
       * buffered path retains its current behavior. */
      audio_playout_flush();
      esp_err_t re = audio_playout_reset_tune();

      servo_ppm = 0;
      servo_target_ppm = 0;
      servo_generation = 0;
      pid_integral_ms_s = 0.0;
      pid_prev_error_ms = 0.0;
      pid_d_filtered_ms_s = 0.0;
      pid_prev_valid = false;
      pid_last_calc_us = esp_timer_get_time();
      pid_last_tune_us = pid_last_calc_us;

      s.diag.servo_ppm = 0;
      s.diag.servo_target_ppm = 0;
      s.diag.servo_slope_us_per_s = 0;
      s.diag.servo_mclk_hz = audio_playout_get_nominal_mclk_hz();
      s.diag.output_sync_valid = false;
      s.diag.dma_pipeline_blocks = 0;
      state = PLAYOUT_STOPPED;
      s.diag.playout_state = state;
      s.diag.playout_servo_session_resets++;
      if (re != ESP_OK) {
        s.diag.playout_servo_reset_errors++;
        /* Keep only the exceptional failure in the realtime task. */
        ESP_LOGW(TAG, "PLAYOUT SERVO RESET failed: %s", esp_err_to_name(re));
      }
    }

    if (s.i2s_flush_requested) {
      s.i2s_flush_requested = false;
      audio_playout_flush();
      s.diag.dma_pipeline_blocks = 0;
      s.diag.output_sync_valid = false;
      state = PLAYOUT_STOPPED;
      s.diag.playout_state = state;
      s.diag.playout_flushes++;
    }

    timing_snapshot_t snap;
    snapshot_state(&snap);
    process_i2s_completions(&snap);

    uint32_t desired_rtp = 0;
    bool timeline_ok = false;
    if (snap.playing && snap.anchor_valid && !snap.timeline_reset_pending &&
        timing_clock_ready(&snap)) {
      timeline_ok = wanted_rtp_now(&snap, &desired_rtp);
    }

    if (!timeline_ok) {
      s.diag.dma_pipeline_blocks = 0;
      state = PLAYOUT_STOPPED;
      s.diag.playout_state = state;
      vTaskDelay(1);
      continue;
    }

    if (cursor_generation != snap.generation || state == PLAYOUT_STOPPED) {
      cursor_generation = snap.generation;
      servo_generation = snap.generation;
      pid_last_calc_us = esp_timer_get_time();
      pid_last_tune_us = pid_last_calc_us;
      /* Keep the learned I/frequency bias across ordinary track/anchor changes
       * inside the same session, but reset D. A hard codec/session boundary
       * is handled above and explicitly returns both hardware tune and PID to 0. */
      pid_prev_valid = false;
      pid_d_filtered_ms_s = 0.0;
      servo_target_ppm = servo_ppm;
      s.diag.servo_target_ppm = servo_ppm;
      s.diag.servo_slope_us_per_s = 0;
      s.diag.dma_pipeline_blocks = 0;
      s.diag.output_sync_valid = false;
      state = PLAYOUT_PRIMING;
      s.diag.playout_state = state;
      s.diag.playout_resyncs++;
      audio_playout_flush(); /* READY/disabled for deterministic preload */
    }

    if (state == PLAYOUT_PRIMING) {
      const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
      const uint32_t prime_ms =
          snap.stream_type == AUDIO_STREAM_REALTIME ? AP2_REALTIME_PRIME_MS
                                                    : AP2_PLAYOUT_PRIME_MS;
      const uint32_t prime_frames =
          (uint32_t)(((uint64_t)sr * prime_ms) / 1000ULL);

      /* Wait until enough real PCM exists before starting the silent phase
       * probe.  Once I2S is enabled we intentionally never stop it between
       * measurement and the first real block, so the real block must already
       * be available for an immediate queue operation. */
      const uint32_t guard_start = desired_rtp;
      const uint32_t guard_frames =
          prime_frames + AP2_START_PRIME_GUARD_BLOCKS * AUDIO_PLAYOUT_FRAMES;
      if (!pcm_rtp_ring_has_range(s.pcm_ring, guard_start, guard_frames,
                                  snap.generation)) {
        s.diag.playout_prime_waits++;
        vTaskDelay(1);
        continue;
      }

      static int16_t silence[AUDIO_PLAYOUT_FRAMES * 2U];
      memset(silence, 0, sizeof(silence));

      /* The silence tags use generation 0 on purpose: their EOFs are only a
       * phase probe and must not become the generation's public SYNC START.
       * The first real block carries the real generation and therefore becomes
       * the first normal sync observation. */
      const uint32_t silence_rtp =
          desired_rtp + AP2_START_SILENCE_FUTURE_BLOCKS * AUDIO_PLAYOUT_FRAMES;
      uint64_t silence_start_time_ns = 0;
      if (!rtp_to_presentation_ns(&snap, silence_rtp, &silence_start_time_ns)) {
        s.diag.playout_prime_waits++;
        vTaskDelay(1);
        continue;
      }

      if (audio_playout_preload_tagged(silence, AUDIO_PLAYOUT_FRAMES,
                                       silence_rtp, 0U) != ESP_OK ||
          audio_playout_preload_tagged(silence, AUDIO_PLAYOUT_FRAMES,
                                       silence_rtp + AUDIO_PLAYOUT_FRAMES,
                                       0U) != ESP_OK) {
        audio_playout_flush();
        s.diag.playout_prime_waits++;
        vTaskDelay(1);
        continue;
      }

      wait_until_presentation_ns(&snap, silence_start_time_ns);

      timing_snapshot_t after_wait;
      snapshot_state(&after_wait);
      if (!after_wait.playing || !after_wait.anchor_valid ||
          after_wait.timeline_reset_pending ||
          after_wait.generation != snap.generation || s.i2s_flush_requested ||
          !timing_clock_ready(&after_wait) ||
          (snap.stream_type == AUDIO_STREAM_REALTIME &&
           (after_wait.anchor_local_ns != snap.anchor_local_ns ||
            after_wait.anchor_rtp != snap.anchor_rtp))) {
        audio_playout_flush();
          state = PLAYOUT_STOPPED;
        s.diag.playout_state = state;
        continue;
      }

      /* This is the only enable for the whole startup epoch. */
      if (audio_playout_enable() != ESP_OK) {
        audio_playout_flush();
        s.diag.playout_prime_waits++;
        vTaskDelay(1);
        continue;
      }

      /* Wait for EOF of the first silent block.  The second silent descriptor
       * is already running, leaving one full block (~5.8 ms) to calculate the
       * exact RTP sample for descriptor #3 and queue it without stopping I2S. */
      const int64_t align_deadline = esp_timer_get_time() + AP2_START_ALIGN_TIMEOUT_US;
      audio_playout_completion_t probe_done;
      bool have_probe = false;
      while (esp_timer_get_time() < align_deadline) {
        if (audio_playout_poll_completion(&probe_done)) {
          if (probe_done.generation == 0U && probe_done.rtp == silence_rtp) {
            have_probe = true;
            break;
          }
          /* No real-generation completion can exist yet. Ignore any stale
           * diagnostic item left over from a prior disabled epoch. */
        } else {
          taskYIELD();
        }
      }
      if (!have_probe) {
        audio_playout_flush();
        s.diag.playout_prime_waits++;
        vTaskDelay(1);
        continue;
      }

      /* Convert the measured EOF edge into the stream presentation clock.
       * For realtime ALAC this stays as the ISR's ESP-local timestamp; AAC
       * keeps the original local+PTP-offset conversion. Descriptor #3 starts
       * one block after descriptor #1 EOF because descriptor #2 is in flight. */
      const int64_t probe_done_time_ns = completion_presentation_ns(&snap, &probe_done);
      int64_t rate_scale_ppm = 1000000LL + (int64_t)servo_ppm;
      if (rate_scale_ppm < 900000LL) rate_scale_ppm = 900000LL;
      const uint64_t block_den = (uint64_t)sr * (uint64_t)rate_scale_ppm;
      const uint64_t block_ns =
          ((uint64_t)AUDIO_PLAYOUT_FRAMES * 1000000000ULL * 1000000ULL +
           block_den / 2ULL) / block_den;
      uint64_t real_boundary_time_ns =
          probe_done_time_ns > 0 ? (uint64_t)probe_done_time_ns + block_ns
                                 : silence_start_time_ns + 2ULL * block_ns;

      /* Positive test offset means intentionally play content earlier.  At a
       * fixed physical boundary that is equivalent to selecting the RTP sample
       * whose nominal presentation time lies test_offset in the future. */
      int64_t mapped_time_ns = (int64_t)real_boundary_time_ns +
                           (int64_t)CONFIG_AP2_PLAYOUT_TEST_OFFSET_US * 1000LL;
      if (mapped_time_ns < 0) mapped_time_ns = 0;

      /* Round to the nearest RTP sample instead of truncating.  This makes the
       * startup alignment resolution one sample (22.68 us at 44.1 kHz). */
      const uint64_t half_sample_ns = 500000000ULL / (uint64_t)sr;
      uint32_t real_start_rtp = 0;
      if (!wanted_rtp_at_presentation_ns(&snap, (uint64_t)mapped_time_ns + half_sample_ns,
                             &real_start_rtp)) {
        audio_playout_flush();
        s.diag.playout_prime_waits++;
        vTaskDelay(1);
        continue;
      }

      /* Revalidate the timeline after waiting for the probe EOF. */
      timing_snapshot_t align_snap;
      snapshot_state(&align_snap);
      if (!align_snap.playing || !align_snap.anchor_valid ||
          align_snap.timeline_reset_pending ||
          align_snap.generation != snap.generation || s.i2s_flush_requested ||
          !timing_clock_ready(&align_snap) ||
          (snap.stream_type == AUDIO_STREAM_REALTIME &&
           (align_snap.anchor_local_ns != snap.anchor_local_ns ||
            align_snap.anchor_rtp != snap.anchor_rtp))) {
        audio_playout_flush();
          state = PLAYOUT_STOPPED;
        s.diag.playout_state = state;
        continue;
      }

      bool ok = pcm_rtp_ring_read_256(s.pcm_ring, real_start_rtp,
                                      snap.generation, block);
      if (!ok) {
        /* Do not allow the already-running silent probe to leak into audible
         * timing indefinitely. A miss restarts the one-enable alignment epoch
         * after more PCM has arrived. */
        audio_playout_flush();
        s.diag.playout_prime_waits++;
        vTaskDelay(1);
        continue;
      }
      apply_output_volume(block, AUDIO_PLAYOUT_FRAMES, &volume_current_q15);
      if (audio_playout_write_tagged(block, AUDIO_PLAYOUT_FRAMES,
                                     real_start_rtp,
                                     snap.generation) != ESP_OK) {
        audio_playout_flush();
        s.diag.playout_prime_waits++;
        vTaskDelay(1);
        continue;
      }

      /* Report the raw one-enable startup phase only as a diagnostic.  It is
       * It is not used as a second-enable compensation. */
      uint64_t probe_target_end_ns = 0;
      int32_t probe_sync_us = 0;
      if (rtp_to_presentation_ns(&snap, silence_rtp + AUDIO_PLAYOUT_FRAMES,
                                 &probe_target_end_ns)) {
        probe_sync_us = (int32_t)(((int64_t)probe_target_end_ns -
                                   probe_done_time_ns) / 1000LL);
      }
      const int32_t align_samples =
          rtp_delta(real_start_rtp,
                    silence_rtp + 2U * AUDIO_PLAYOUT_FRAMES);

      cursor_rtp = real_start_rtp + AUDIO_PLAYOUT_FRAMES;
      s.diag.dma_pipeline_blocks = 2U; /* silent #2 + first real block */
      state = PLAYOUT_RUNNING;
      s.diag.playout_state = state;
      s.diag.playout_starts++;
      s.diag.startup_probe_sync_us = probe_sync_us;
      s.diag.startup_align_samples = align_samples;
      s.diag.startup_real_rtp = real_start_rtp;
      s.diag.startup_align_generation = snap.generation;
      __atomic_store_n(&s.diag.startup_align_log_pending, 1U, __ATOMIC_RELEASE);
      continue;
    }

    /* In RUNNING, cursor_rtp is the next future block to queue. The two DMA
     * descriptors are paced by their EOF interrupts. No guessed current+1
     * subtraction is used for sync any more; the ISR completion tags are the
     * source of truth. */
    s.diag.desired_cursor_err_frames = rtp_delta(cursor_rtp, desired_rtp);

    const uint64_t fetch_begin_ptp = presentation_now_ns(&snap);
    bool have_pcm = pcm_rtp_ring_read_256(
        s.pcm_ring, cursor_rtp, snap.generation, block);
    const uint64_t fetch_end_ptp = presentation_now_ns(&snap);
    if (!have_pcm) {
      memset(block, 0, AUDIO_PLAYOUT_FRAMES * 2U * sizeof(int16_t));
      s.diag.playout_underruns++;
      /* ap2_stats_task reports the same condition as AUDIO err U=... */
    }
    s.diag.tx_fetch_us =
        (uint32_t)((fetch_end_ptp - fetch_begin_ptp) / 1000ULL);
    apply_output_volume(block, AUDIO_PLAYOUT_FRAMES, &volume_current_q15);

    const esp_err_t write_err = audio_playout_write_tagged(
        block, AUDIO_PLAYOUT_FRAMES, cursor_rtp, snap.generation);
    process_i2s_completions(&snap);

    /* PID clock servo.
     *
     * Sign convention from tagged DMA EOF:
     *   SYNC < 0 : ESP is late  -> positive ppm (speed I2S up)
     *   SYNC > 0 : ESP is early -> negative pressure on ppm
     *
     * The PID math runs every 1 s, but i2s_channel_tune_rate() is allowed at
     * most every 5 s and only for a useful >=5 ppm change.  This separation
     * matters because IDF tuning needs disable->tune->enable, which can itself
     * perturb phase.  +/-1 ms is deliberately treated as GOOD: once there and
     * phase velocity is modest, the clock is held instead of chasing 0.000 ms.
     */
    const int64_t pid_now_us = esp_timer_get_time();
    if (write_err == ESP_OK && s.diag.output_sync_valid &&
        s.diag.output_sync_generation == snap.generation &&
        servo_generation == snap.generation &&
        pid_now_us - pid_last_calc_us >= AP2_PID_CALC_PERIOD_US) {
      const double dt_s = (double)(pid_now_us - pid_last_calc_us) / 1000000.0;
      pid_last_calc_us = pid_now_us;

      /* Controller error is opposite to SYNC: negative SYNC (late) must
       * request positive ppm. */
      const double sync_ms = (double)s.diag.output_sync_us / 1000.0;
      const double error_ms = -sync_ms;

      double d_raw_ms_s = 0.0;
      if (pid_prev_valid && dt_s > 0.001) {
        d_raw_ms_s = (error_ms - pid_prev_error_ms) / dt_s;
        pid_d_filtered_ms_s += AP2_PID_D_ALPHA *
            (d_raw_ms_s - pid_d_filtered_ms_s);
      } else {
        pid_d_filtered_ms_s = 0.0;
      }
      pid_prev_error_ms = error_ms;
      pid_prev_valid = true;
      /* Keep log sign intuitive: positive d means SYNC is moving upward. */
      s.diag.servo_slope_us_per_s =
          (int32_t)(-pid_d_filtered_ms_s * 1000.0);

      const int32_t abs_sync_us = s.diag.output_sync_us < 0
          ? -s.diag.output_sync_us : s.diag.output_sync_us;
      const bool in_deadband = abs_sync_us <= AP2_PID_DEADBAND_US;

      /* In the 1..2 ms soft band reduce proportional aggression.  Outside
       * 2 ms use full P.  This prevents unnecessary hunting once we are already
       * close enough for multiroom use. */
      double p_error_ms = error_ms;
      if (abs_sync_us > AP2_PID_DEADBAND_US &&
          abs_sync_us <= AP2_PID_SOFTBAND_US) {
        p_error_ms *= 0.45;
      }

      const double p_term = AP2_PID_KP_PPM_PER_MS * p_error_ms;
      const double d_term = AP2_PID_KD_PPM_PER_MS_PER_S * pid_d_filtered_ms_s;

      /* Candidate integral update with anti-windup.  We integrate outside the
       * +/-1 ms good zone.  If P+I+D is already saturated in the same direction
       * as the error, freeze I; if the error would pull us out of saturation,
       * allow it to unwind. */
      double i_term = AP2_PID_KI_PPM_PER_MS_S * pid_integral_ms_s;
      double unsat = p_term + i_term + d_term;
      bool allow_i = !in_deadband;
      if (allow_i) {
        if ((unsat >= AP2_PID_MAX_PPM && error_ms > 0.0) ||
            (unsat <= -AP2_PID_MAX_PPM && error_ms < 0.0)) {
          allow_i = false;
        }
      }
      if (allow_i) {
        pid_integral_ms_s += error_ms * dt_s;
        const double i_limit_state = AP2_PID_I_TERM_LIMIT_PPM /
                                     AP2_PID_KI_PPM_PER_MS_S;
        if (pid_integral_ms_s > i_limit_state) pid_integral_ms_s = i_limit_state;
        if (pid_integral_ms_s < -i_limit_state) pid_integral_ms_s = -i_limit_state;
        i_term = AP2_PID_KI_PPM_PER_MS_S * pid_integral_ms_s;
      }

      double command = p_term + i_term + d_term;
      if (command > AP2_PID_MAX_PPM) command = AP2_PID_MAX_PPM;
      if (command < -AP2_PID_MAX_PPM) command = -AP2_PID_MAX_PPM;
      int32_t next_target = (int32_t)(command >= 0.0 ? command + 0.5 : command - 0.5);

      /* GOOD zone: if phase is not racing through it, keep the learned clock.
       * D still remains alive, so a clear passage through the band will be seen
       * on the next calculation rather than being hidden forever. */
      const double sync_slope_ms_s = -pid_d_filtered_ms_s;
      if (in_deadband && sync_slope_ms_s > -0.080 && sync_slope_ms_s < 0.080) {
        next_target = servo_ppm;
      }
      servo_target_ppm = next_target;
      s.diag.servo_target_ppm = servo_target_ppm;
    }

    if (write_err == ESP_OK && s.diag.output_sync_valid &&
        s.diag.output_sync_generation == snap.generation &&
        servo_generation == snap.generation &&
        pid_now_us - pid_last_tune_us >= AP2_PID_TUNE_PERIOD_US) {
      int32_t delta = servo_target_ppm - servo_ppm;
      int32_t abs_delta = delta < 0 ? -delta : delta;
      if (abs_delta >= AP2_PID_MIN_TUNE_PPM) {
        if (delta > AP2_PID_MAX_JUMP_PPM) delta = AP2_PID_MAX_JUMP_PPM;
        if (delta < -AP2_PID_MAX_JUMP_PPM) delta = -AP2_PID_MAX_JUMP_PPM;
        const int32_t next_ppm = servo_ppm + delta;

        audio_playout_tune_info_t ti = {0};
        esp_err_t te = audio_playout_tune_ppm(next_ppm, &ti);
        pid_last_tune_us = pid_now_us;
        if (te == ESP_OK) {
          servo_ppm = next_ppm;
          s.diag.servo_ppm = servo_ppm;
          s.diag.servo_mclk_hz = ti.curr_mclk_hz;
          s.diag.servo_updates++;
        } else {
          s.diag.servo_errors++;
        }
      }
    }

    if (write_err == ESP_OK) {
      s.diag.playout_blocks++;
      s.diag.last_playout_rtp = cursor_rtp;
      cursor_rtp += AUDIO_PLAYOUT_FRAMES;
      s.diag.dma_pipeline_blocks = 2U;
    } else {
      /* A failed write breaks the exact tag<->descriptor FIFO relationship.
       * Flush and re-prime rather than pretending the software cursor moved. */
      audio_playout_flush();
      s.diag.dma_pipeline_blocks = 0;
      s.diag.output_sync_valid = false;
      state = PLAYOUT_PRIMING;
      s.diag.playout_state = state;
      s.diag.playout_resyncs++;
      vTaskDelay(1);
    }
  }

  audio_playout_flush();
  free(block);
  s.playout_task = NULL;
  vTaskDelete(NULL);
}

static void ap2_stats_task(void *arg) {
  (void)arg;
  diag_stats_t prev = {0};
  pcm_rtp_ring_stats_t pcm_prev = {0};
  audio_playout_diag_t pdiag_prev = {0};
  realtime_receiver_diag_t rt_prev = {0};
  uint32_t stg_missing_prev = 0;
  uint32_t stg_recovered_prev = 0;
  uint32_t stg_silence_prev = 0;
  uint32_t stg_late_prev = 0;
  unsigned idle_periods = 0;
  __atomic_store_n(&s.realtime_stage_min_ahead_us, INT32_MAX, __ATOMIC_RELAXED);
  __atomic_store_n(&s.realtime_stage_wait_max_us, 0U, __ATOMIC_RELAXED);
  prev = s.diag;
  pcm_rtp_ring_get_stats(s.pcm_ring, &pcm_prev);
  audio_playout_get_diag(&pdiag_prev);
  realtime_receiver_get_diag(&rt_prev, false);
  stg_missing_prev = __atomic_load_n(&s.realtime_stage_missing, __ATOMIC_RELAXED);
  stg_recovered_prev = __atomic_load_n(&s.realtime_stage_recovered, __ATOMIC_RELAXED);
  stg_silence_prev = __atomic_load_n(&s.realtime_stage_silence, __ATOMIC_RELAXED);
  stg_late_prev = __atomic_load_n(&s.realtime_stage_late_fill, __ATOMIC_RELAXED);
  ESP_LOGI(TAG, "stats session started on core %d", xPortGetCoreID());

  while (s.engine_running &&
         __atomic_load_n(&s.stats_session_running, __ATOMIC_ACQUIRE)) {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(AP2_STATS_PERIOD_MS));
    if (!__atomic_load_n(&s.stats_session_running, __ATOMIC_ACQUIRE)) break;
    timing_snapshot_t snap;
    snapshot_state(&snap);
    diag_stats_t now = s.diag;
    const bool have_align_log =
        __atomic_exchange_n(&s.diag.startup_align_log_pending, 0U, __ATOMIC_ACQ_REL) != 0U;
    const bool have_sync_start_log =
        __atomic_exchange_n(&s.diag.startup_sync_log_pending, 0U, __ATOMIC_ACQ_REL) != 0U;
    if (have_align_log) {
      const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
      ESP_LOGI(TAG,
               "ALIGN gen=%" PRIu32 " probe=%+.2fms shift=%" PRId32
               " samples (%+.3fms) real_rtp=%" PRIu32 " | deferred-log",
               now.startup_align_generation,
               (double)now.startup_probe_sync_us / 1000.0,
               now.startup_align_samples,
               (double)now.startup_align_samples * 1000.0 / (double)sr,
               now.startup_real_rtp);
    }
    if (have_sync_start_log) {
      const int32_t sync_us = now.startup_sync_start_us;
      ESP_LOGI(TAG,
               "SYNC START gen=%" PRIu32 " %+.2f ms (%s) anchor_to_i2s=%" PRId32
               "ms | deferred-log",
               now.startup_sync_generation, (double)sync_us / 1000.0,
               sync_us > 0 ? "ESP early" :
               (sync_us < 0 ? "ESP late" : "aligned"),
               now.startup_anchor_to_sync_ms);
    }
    now.pcm_peak_in = __atomic_exchange_n(&s.diag.pcm_peak_in, 0U, __ATOMIC_RELAXED);
    now.pcm_peak_out = __atomic_exchange_n(&s.diag.pcm_peak_out, 0U, __ATOMIC_RELAXED);
    pcm_rtp_ring_stats_t pcm = {0};
    pcm_rtp_ring_get_stats(s.pcm_ring, &pcm);

    bool active = now.rx != prev.rx || now.decoded != prev.decoded ||
                  now.stale_predecrypt != prev.stale_predecrypt ||
                  now.stale_predecode != prev.stale_predecode ||
                  now.decode_error != prev.decode_error ||
                  now.decode_reacquires != prev.decode_reacquires ||
                  now.pcm_write_error != prev.pcm_write_error ||
                  pcm.future_collisions != pcm_prev.future_collisions ||
                  now.playout_blocks != prev.playout_blocks ||
                  now.playout_underruns != prev.playout_underruns ||
                  now.playout_resyncs != prev.playout_resyncs ||
                  now.playout_starts != prev.playout_starts ||
                  __atomic_load_n(&s.realtime_stage_missing, __ATOMIC_RELAXED) != stg_missing_prev ||
                  __atomic_load_n(&s.realtime_stage_recovered, __ATOMIC_RELAXED) != stg_recovered_prev ||
                  __atomic_load_n(&s.realtime_stage_silence, __ATOMIC_RELAXED) != stg_silence_prev ||
                  __atomic_load_n(&s.realtime_stage_late_fill, __ATOMIC_RELAXED) != stg_late_prev ||
                  have_align_log || have_sync_start_log;
    if (!active && ++idle_periods < 5U) {
      continue;
    }
    idle_periods = 0;

    int pcm_ahead_ms = -1;
    uint32_t wanted = 0;
    if (wanted_rtp_now(&snap, &wanted)) {
      int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
      if (now.last_decoded_end_rtp != 0) {
        pcm_ahead_ms = (int)(((int64_t)rtp_delta(
            now.last_decoded_end_rtp, wanted) * 1000LL) / sr);
      }
    }
    audio_playout_diag_t pdiag = {0};
    audio_playout_get_diag(&pdiag);
    ap2_buffered_transport_stats_t transport = {0};
    if (s.transport) ap2_buffered_transport_get_stats(s.transport, &transport);
    const size_t fifo_capacity =
        s.transport ? ap2_buffered_transport_capacity(s.transport) : 0U;
    /* The old compressed AAC RTP ring no longer exists. Raw compressed audio is
     * represented by FIFO=... below, so do not print a fake millisecond value. */

    /* Compact runtime log. SYNC comes from the EOF ISR for an RTP-tagged
     * DMA block, not from a guessed queue depth. Keep only timing/buffer/error
     * diagnostics here; volume/level telemetry is intentionally omitted. */
    const double sync_ms = now.output_sync_valid
        ? (double)now.output_sync_us / 1000.0 : 0.0;
    const uint64_t drops =
        (now.timeline_drop - prev.timeline_drop) +
        0;
    const uint64_t collisions =
        (pcm.future_collisions - pcm_prev.future_collisions);
    /* DMA diagnostics are reported as per-period deltas, not cumulative
     * counters. The two values are: EOF callbacks with no matching RTP tag,
     * and tagged completions that could not be queued because the completion
     * queue was full. Counters are reset by audio_playout_flush(), so handle
     * a reset without unsigned underflow. */
    const uint64_t dma_untagged_delta =
        pdiag.untagged_completions >= pdiag_prev.untagged_completions
            ? pdiag.untagged_completions - pdiag_prev.untagged_completions
            : pdiag.untagged_completions;
    const uint64_t dma_overflow_delta =
        pdiag.completion_overflows >= pdiag_prev.completion_overflows
            ? pdiag.completion_overflows - pdiag_prev.completion_overflows
            : pdiag.completion_overflows;
    const uint64_t rt_drop_no_ptp =
        now.realtime_sink_no_ptp - prev.realtime_sink_no_ptp;
    const uint64_t rt_drop_stale =
        now.realtime_sink_stale - prev.realtime_sink_stale;
    const uint64_t rt_drop_ring =
        now.realtime_sink_ring - prev.realtime_sink_ring;
    if (rt_drop_no_ptp || rt_drop_stale || rt_drop_ring) {
      ESP_LOGI(TAG,
               "RT sink drop reason: no_ptp=%" PRIu64 " stale=%" PRIu64
               " ring=%" PRIu64,
               rt_drop_no_ptp, rt_drop_stale, rt_drop_ring);
    }
    const uint64_t underrun_delta =
        now.playout_underruns - prev.playout_underruns;
    const uint64_t resync_delta =
        now.playout_resyncs - prev.playout_resyncs;
    const uint64_t decode_delta = now.decode_error - prev.decode_error;

    /* Keep the normal 2 s heartbeat compact. Exceptional conditions are
     * emitted as a separate warning only when they actually occur. */
    if (underrun_delta || resync_delta || drops || collisions || decode_delta ||
        dma_untagged_delta || dma_overflow_delta) {
      ESP_LOGW(TAG,
               "AUDIO err U=%" PRIu64 " R=%" PRIu64 " drop=%" PRIu64
               " coll=%" PRIu64 " dec=%" PRIu64 " dma=%" PRIu64 "/%" PRIu64,
               underrun_delta, resync_delta, drops, collisions, decode_delta,
               dma_untagged_delta, dma_overflow_delta);
    }

    if (snap.stream_type == AUDIO_STREAM_REALTIME) {
      realtime_receiver_diag_t rt = {0};
      realtime_receiver_get_diag(&rt, true);
      ptp_stats_t ptp_diag = {0};
      ptp_clock_get_stats(&ptp_diag);
      ptp_realtime_snapshot_t ptp_rt = {0};
      ptp_clock_get_realtime_snapshot(&ptp_rt);
      const double ptp_raw_filter_delta_ms =
          (double)ptp_diag.raw_filter_delta_ns / 1000000.0;
#define DELTA32(cur, old) ((cur) >= (old) ? (cur) - (old) : (cur))
      const uint32_t miss_delta = DELTA32(rt.missing_packets, rt_prev.missing_packets);
      const uint32_t nack_delta = DELTA32(rt.nack_requests, rt_prev.nack_requests);
      const uint32_t rtx_delta = DELTA32(rt.retransmit_packets, rt_prev.retransmit_packets);
      const uint32_t retry_delta = DELTA32(rt.resend_retries, rt_prev.resend_retries);
      const uint32_t give_delta = DELTA32(rt.resend_giveups, rt_prev.resend_giveups);
      const uint32_t d7_bad_delta = DELTA32(rt.d7_malformed, rt_prev.d7_malformed);
      const uint32_t pt84_bad_delta = DELTA32(rt.sync_malformed, rt_prev.sync_malformed);

      bool map_valid = false;
      double map_delta_ms = 0.0;
      if (rt.last_d7_local_ns != 0U && snap.anchor_valid &&
          snap.anchor_local_ns != 0U && !snap.timeline_reset_pending) {
        const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
        const int32_t ds = rtp_delta(rt.last_d7_frame1, snap.anchor_rtp);
        const int64_t mapped_local_ns =
            (int64_t)snap.anchor_local_ns +
            ((int64_t)ds * 1000000000LL) / (int64_t)sr;
        map_delta_ms =
            (double)(mapped_local_ns - (int64_t)rt.last_d7_local_ns) / 1000000.0;
        map_valid = true;
      }

      const uint32_t stg_missing =
          __atomic_load_n(&s.realtime_stage_missing, __ATOMIC_RELAXED);
      const uint32_t stg_recovered =
          __atomic_load_n(&s.realtime_stage_recovered, __ATOMIC_RELAXED);
      const uint32_t stg_silence =
          __atomic_load_n(&s.realtime_stage_silence, __ATOMIC_RELAXED);
      const uint32_t stg_late =
          __atomic_load_n(&s.realtime_stage_late_fill, __ATOMIC_RELAXED);
      const int32_t stg_ahead =
          __atomic_load_n(&s.realtime_stage_ahead_us, __ATOMIC_RELAXED);
      int32_t stg_min_ahead = __atomic_exchange_n(
          &s.realtime_stage_min_ahead_us, INT32_MAX, __ATOMIC_RELAXED);
      const uint32_t stg_wait_max = __atomic_exchange_n(
          &s.realtime_stage_wait_max_us, 0U, __ATOMIC_RELAXED);
      if (stg_min_ahead == INT32_MAX) stg_min_ahead = stg_ahead;
      const uint32_t sil_delta = DELTA32(stg_silence, stg_silence_prev);
      const uint32_t late_delta = DELTA32(stg_late, stg_late_prev);

      const uint32_t pool_err = DELTA32(rt.data_pool_waits, rt_prev.data_pool_waits);
      const uint32_t work_err = DELTA32(rt.work_queue_drops, rt_prev.work_queue_drops);
      const uint32_t rtx_err = DELTA32(rt.rtx_pool_drops, rt_prev.rtx_pool_drops);
      const uint32_t ev_err = DELTA32(rt.resend_event_drops, rt_prev.resend_event_drops);
      const uint32_t miss_err =
          DELTA32(rt.missing_tracker_overflow, rt_prev.missing_tracker_overflow);
      if (pool_err || work_err || rtx_err || ev_err || miss_err ||
          d7_bad_delta || pt84_bad_delta) {
        ESP_LOGW(TAG,
                 "ALAC transport err pool=%" PRIu32 " work=%" PRIu32
                 " rtx=%" PRIu32 " ev=%" PRIu32 " miss=%" PRIu32
                 " d7bad=%" PRIu32 " pt84bad=%" PRIu32,
                 pool_err, work_err, rtx_err, ev_err, miss_err,
                 d7_bad_delta, pt84_bad_delta);
      }

      if (now.playout_state == 2 && now.output_sync_valid) {
        if (map_valid) {
          ESP_LOGI(TAG,
                   "ALAC sync=%+.2fms ppm=%+" PRId32 "/%+" PRId32 " pcm=%dms map=%+.2fms ptpD=%+.2fms gmReady=%d gmAge=%lums"
                   " | miss=%" PRIu32 " nack=%" PRIu32 " rtx=%" PRIu32 " retry=%" PRIu32
                   " give=%" PRIu32 " ia=%.0fms q=%" PRIu32
                   " | sil=%" PRIu32 " late=%" PRIu32
                   " stgMin=%.0fms wait=%.0fms",
                   sync_ms, now.servo_ppm, now.servo_target_ppm, pcm_ahead_ms,
                   map_delta_ms, ptp_raw_filter_delta_ms, ptp_rt.master_ready ? 1 : 0, (unsigned long)ptp_rt.mastership_age_ms, miss_delta, nack_delta, rtx_delta, retry_delta, give_delta,
                   (double)rt.interval_max_interarrival_us / 1000.0,
                   rt.work_queue_depth, sil_delta, late_delta,
                   (double)stg_min_ahead / 1000.0,
                   (double)stg_wait_max / 1000.0);
        } else {
          ESP_LOGI(TAG,
                   "ALAC sync=%+.2fms ppm=%+" PRId32 "/%+" PRId32 " pcm=%dms map=-- ptpD=%+.2fms gmReady=%d gmAge=%lums"
                   " | miss=%" PRIu32 " nack=%" PRIu32 " rtx=%" PRIu32 " retry=%" PRIu32
                   " give=%" PRIu32 " ia=%.0fms q=%" PRIu32
                   " | sil=%" PRIu32 " late=%" PRIu32
                   " stgMin=%.0fms wait=%.0fms",
                   sync_ms, now.servo_ppm, now.servo_target_ppm, pcm_ahead_ms,
                   ptp_raw_filter_delta_ms, ptp_rt.master_ready ? 1 : 0, (unsigned long)ptp_rt.mastership_age_ms, miss_delta, nack_delta, rtx_delta, retry_delta, give_delta,
                   (double)rt.interval_max_interarrival_us / 1000.0,
                   rt.work_queue_depth, sil_delta, late_delta,
                   (double)stg_min_ahead / 1000.0,
                   (double)stg_wait_max / 1000.0);
        }
      } else {
        ESP_LOGI(TAG,
                 "ALAC sync=-- pcm=%dms map=%s ptpD=%+.2fms gmReady=%d gmAge=%lums | miss=%" PRIu32
                 " nack=%" PRIu32 " rtx=%" PRIu32 " retry=%" PRIu32 " give=%" PRIu32
                 " ia=%.0fms q=%" PRIu32 " | sil=%" PRIu32
                 " late=%" PRIu32 " stgMin=%.0fms wait=%.0fms | %s",
                 pcm_ahead_ms, map_valid ? "ok" : "--", ptp_raw_filter_delta_ms,
                 ptp_rt.master_ready ? 1 : 0, (unsigned long)ptp_rt.mastership_age_ms, miss_delta, nack_delta, rtx_delta, retry_delta, give_delta,
                 (double)rt.interval_max_interarrival_us / 1000.0,
                 rt.work_queue_depth, sil_delta, late_delta,
                 (double)stg_min_ahead / 1000.0,
                 (double)stg_wait_max / 1000.0,
                 now.playout_state == 1 ? "PRIME" : "STOP");
      }

      rt_prev = rt;
      stg_missing_prev = stg_missing;
      stg_recovered_prev = stg_recovered;
      stg_silence_prev = stg_silence;
      stg_late_prev = stg_late;
#undef DELTA32
    } else {
      if (now.playout_state == 2 && now.output_sync_valid) {
        ESP_LOGI(TAG,
                 "AAC sync=%+.2fms ppm=%+" PRId32 "/%+" PRId32 " pcm=%dms fifo=%u/%uK",
                 sync_ms, now.servo_ppm, now.servo_target_ppm, pcm_ahead_ms,
                 (unsigned)(transport.fifo_occupancy / 1024U),
                 (unsigned)(fifo_capacity / 1024U));
      } else {
        ESP_LOGI(TAG,
                 "AAC sync=-- pcm=%dms fifo=%u/%uK | %s",
                 pcm_ahead_ms,
                 (unsigned)(transport.fifo_occupancy / 1024U),
                 (unsigned)(fifo_capacity / 1024U),
                 now.playout_state == 1 ? "PRIME" : "STOP");
      }
    }
    prev = now;
    pcm_prev = pcm;
    pdiag_prev = pdiag;
  }

  s.stats_task = NULL;
  ESP_LOGI(TAG, "stats session stopped");
  vTaskDeleteWithCaps(NULL);
}

static void stats_session_stop(void) {
  __atomic_store_n(&s.stats_session_running, false, __ATOMIC_RELEASE);
  TaskHandle_t task = s.stats_task;
  if (task) xTaskNotifyGive(task);
}

static void stats_session_start(void) {
  if (s.stats_task) {
    /* A just-stopped session is woken by notification and normally exits
     * immediately. Never make audio depend on logging; if it has not exited
     * yet, this stream simply starts without stats rather than blocking RT. */
    if (!__atomic_load_n(&s.stats_session_running, __ATOMIC_ACQUIRE)) {
      xTaskNotifyGive(s.stats_task);
      for (int i = 0; i < 8 && s.stats_task; ++i) taskYIELD();
    }
    if (s.stats_task) return;
  }
  __atomic_store_n(&s.stats_session_running, true, __ATOMIC_RELEASE);
  if (xTaskCreatePinnedToCoreWithCaps(
          ap2_stats_task, "ap2_stats", AP2_STATS_STACK, NULL,
          AP2_STATS_PRIORITY, &s.stats_task, AP2_NETWORK_CORE,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    s.stats_task = NULL;
    __atomic_store_n(&s.stats_session_running, false, __ATOMIC_RELEASE);
    ESP_LOGW(TAG,
             "stats session task create failed; audio continues "
             "internalFree=%u internalLargest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
}

esp_err_t audio_receiver_init(void) {
  s.diag.volume_q15 = __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);

  ESP_LOGI(TAG,
           "PSRAM before audio alloc: total=%u KiB free=%u KiB largest=%u KiB",
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));
  if (!s.packet) {
    s.packet = heap_caps_malloc(AP2_PACKET_MAX,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.packet) {
      s.packet = malloc(AP2_PACKET_MAX);
    }
  }
  if (!s.decrypt_buf) {
    s.decrypt_buf = heap_caps_malloc(AP2_PACKET_MAX,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.decrypt_buf) {
      s.decrypt_buf = malloc(AP2_PACKET_MAX);
    }
  }
  if (!s.decode_pcm) {
    s.decode_pcm = heap_caps_malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.decode_pcm) {
      s.decode_pcm = malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
    }
  }
  if (!s.pcm_ring && pcm_rtp_ring_create(&s.pcm_ring) != ESP_OK) {
    return ESP_ERR_NO_MEM;
  }
  if (!s.realtime_stage_ring &&
      pcm_rtp_ring_create(&s.realtime_stage_ring) != ESP_OK) {
    return ESP_ERR_NO_MEM;
  }
  if (!s.packet || !s.decrypt_buf || !s.decode_pcm || !s.pcm_ring ||
      !s.realtime_stage_ring) {
    return ESP_ERR_NO_MEM;
  }

  pcm_rtp_ring_set_generation(s.pcm_ring, s.generation);
  pcm_rtp_ring_set_generation(s.realtime_stage_ring, s.generation);
  __atomic_store_n(&s.realtime_stage_min_ahead_us, INT32_MAX, __ATOMIC_RELAXED);
  ESP_LOGI(TAG,
           "PSRAM after PCM ring: total=%u KiB free=%u KiB largest=%u KiB",
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));
  if (!s.transport) {
    ap2_buffered_transport_config_t tcfg = {
        .fifo_bytes = AP2_BUFFERED_FIFO_REQUEST_BYTES,
        .task_core = AP2_NETWORK_CORE,
        .task_priority = AP2_RX_PRIORITY,
        .task_stack = AP2_RX_STACK,
    };
    ESP_RETURN_ON_ERROR(ap2_buffered_transport_create(&s.transport, &tcfg),
                        TAG, "buffered transport create failed");
  }
  ESP_LOGI(TAG,
           "PSRAM after raw FIFO: total=%u KiB free=%u KiB largest=%u KiB fifo=%u KiB",
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(ap2_buffered_transport_capacity(s.transport) / 1024U));

  ESP_RETURN_ON_ERROR(audio_playout_init(), TAG, "I2S playout init failed");
  s.engine_running = true;
  if (xTaskCreatePinnedToCore(ap2_playout_task, "ap2_playout",
                              AP2_PLAYOUT_STACK, NULL, AP2_PLAYOUT_PRIORITY,
                              &s.playout_task, AP2_DECODE_CORE) != pdPASS) {
    return ESP_FAIL;
  }
  ESP_LOGI(TAG,
           "PTP/RTP playout task started core=%d prio=%u block=%u prime=%ums",
           AP2_DECODE_CORE, (unsigned)AP2_PLAYOUT_PRIORITY,
           (unsigned)AUDIO_PLAYOUT_FRAMES, (unsigned)AP2_PLAYOUT_PRIME_MS);
  if (xTaskCreatePinnedToCore(realtime_stage_task, "alac_stage",
                              AP2_RT_STAGE_STACK, NULL, AP2_RT_STAGE_PRIORITY,
                              &s.realtime_stage_task, AP2_DECODE_CORE) != pdPASS) {
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "AP2 buffered: Shairport-style TCP FIFO -> state/flush/timing processor -> AAC decode -> PCM RTP ring");
  ESP_LOGI(TAG, "AP2 buffered: no compressed AAC RTP ring, no +45s admission gate, no normal-path REACQUIRE");
  ESP_LOGI(TAG, "AP2 buffered: raw FIFO=%u KiB (requested=%u KiB), decoded target=%u ms",
           (unsigned)(ap2_buffered_transport_capacity(s.transport) / 1024U),
           (unsigned)(AP2_BUFFERED_FIFO_REQUEST_BYTES / 1024U),
           (unsigned)AP2_PCM_TARGET_MS);
  ESP_LOGI(TAG, "AP2 buffered: future audio is held/backpressured, FLUSH uses 23-bit sender sequence ranges");
  ESP_LOGI(TAG, "AP2 output: existing exact PTP/RTP PCM playout and I2S PID preserved");
  ESP_LOGI(TAG, "ALAC: immediate decode -> raw RTP PCM ring -> ordered EQ -> final PCM ring");
  return ESP_OK;
}

static void realtime_stage_stop_and_wait(void) {
  __atomic_store_n(&s.realtime_stage_running, false, __ATOMIC_RELEASE);
  realtime_stage_kick();

  /* Session-bound handshake only. Never put a mutex around audio_eq_process():
   * the control path merely waits until the persistent staging task has
   * observed the stop flag and is outside its current EQ/write iteration. */
  for (int i = 0; s.realtime_stage_task &&
                  !__atomic_load_n(&s.realtime_stage_idle, __ATOMIC_ACQUIRE) &&
                  i < 20; ++i) {
    vTaskDelay(1);
  }
  if (s.realtime_stage_task &&
      !__atomic_load_n(&s.realtime_stage_idle, __ATOMIC_ACQUIRE)) {
    ESP_LOGW(TAG, "ALAC staging did not become idle before codec switch");
  }
}

void audio_receiver_set_format(const audio_format_t *f) {
  if (!f) {
    return;
  }
  taskENTER_CRITICAL(&s.state_mux);
  s.format = *f;
  s.format_generation++;
  if (s.format_generation == 0) {
    s.format_generation = 1;
  }
  taskEXIT_CRITICAL(&s.state_mux);
  ESP_LOGI(TAG, "FORMAT codec=%s sr=%d ch=%d bits=%d frame=%d",
           f->codec, f->sample_rate, f->channels, f->bits_per_sample,
           f->frame_size);
}

void audio_receiver_set_encryption(const audio_encrypt_t *e) {
  if (e) {
    s.encrypt = *e;
  } else {
    memset(&s.encrypt, 0, sizeof(s.encrypt));
  }
  ESP_LOGI(TAG, "ENCRYPT type=%d key_len=%u", (int)s.encrypt.type,
           (unsigned)s.encrypt.key_len);
}

void audio_receiver_set_stream_type(audio_stream_type_t t) {
  taskENTER_CRITICAL(&s.state_mux);
  s.stream_type = t;
  taskEXIT_CRITICAL(&s.state_mux);
}

esp_err_t audio_receiver_start_buffered(uint16_t port) {
  timing_snapshot_t fmt_snap;
  snapshot_state(&fmt_snap);
  if (strcmp(fmt_snap.format.codec, "AAC") != 0 ||
      fmt_snap.format.sample_rate != 44100 || fmt_snap.format.channels != 2 ||
      fmt_snap.format.bits_per_sample != 16 || fmt_snap.format.frame_size != 1024) {
    ESP_LOGE(TAG,
             "Buffered format rejected codec=%s sr=%d ch=%d bits=%d frame=%d",
             fmt_snap.format.codec, fmt_snap.format.sample_rate,
             fmt_snap.format.channels, fmt_snap.format.bits_per_sample,
             fmt_snap.format.frame_size);
    return ESP_ERR_NOT_SUPPORTED;
  }

  realtime_stage_stop_and_wait();
  if (!s.transport) return ESP_ERR_INVALID_STATE;
  if (s.rx_running) return ESP_OK;

  /* New buffered codec session: compressed FIFO/control sequence state must
   * never inherit anything from a previous AAC -> ALAC -> AAC transition. */
  ap2_buffered_transport_clear(s.transport);
  taskENTER_CRITICAL(&s.state_mux);
  s.buffered_session_seq = 0;
  s.buffered_session_rx = 0;
  s.buffered_session_seq_valid = false;
  s.immediate_flush_requested = false;
  s.immediate_flush_has_endpoint = false;
  s.immediate_flush_diag_active = false;
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    s.deferred_flush[i].in_use = false;
    s.deferred_flush[i].active = false;
  }
  taskEXIT_CRITICAL(&s.state_mux);

  uint16_t bound = port;
  ESP_RETURN_ON_ERROR(ap2_buffered_transport_start(s.transport, port, &bound),
                      TAG, "buffered transport start failed");
  s.port = bound;
  s.rx_running = true;
  if (xTaskCreatePinnedToCore(ap2_buffered_processor_task, "ap2_buf_proc",
                              AP2_PROCESS_STACK, NULL, AP2_DECODE_PRIORITY,
                              &s.processor_task, AP2_BUFFERED_PROCESSOR_CORE) != pdPASS) {
    s.rx_running = false;
    ap2_buffered_transport_stop(s.transport);
    return ESP_FAIL;
  }
  stats_session_start();
  ESP_LOGI(TAG, "AP2 buffered listener port=%u", (unsigned)s.port);
  return ESP_OK;
}

esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port) {
  if (s.stream_type == AUDIO_STREAM_BUFFERED) {
    return audio_receiver_start_buffered(tcp_port);
  }

  if (s.stream_type == AUDIO_STREAM_REALTIME) {
    timing_snapshot_t fmt_snap;
    snapshot_state(&fmt_snap);
    if (strcmp(fmt_snap.format.codec, "ALAC") != 0 ||
        fmt_snap.format.sample_rate != 44100 || fmt_snap.format.channels != 2 ||
        fmt_snap.format.bits_per_sample != 16 || fmt_snap.format.frame_size != 352) {
      ESP_LOGE(TAG,
               "Realtime format rejected codec=%s sr=%d ch=%d bits=%d frame=%d",
               fmt_snap.format.codec, fmt_snap.format.sample_rate,
               fmt_snap.format.channels, fmt_snap.format.bits_per_sample,
               fmt_snap.format.frame_size);
      return ESP_ERR_NOT_SUPPORTED;
    }
    realtime_stage_stop_and_wait();
    uint32_t rt_gen = 0;
    taskENTER_CRITICAL(&s.state_mux);
    /* A codec/session boundary already marked a discontinuity. Publish the
     * fresh PCM generation now, before UDP starts, but deliberately leave the
     * timing anchor invalid. This lets early ALAC packets be retained by RTP
     * address while playout waits exclusively for a real D7/SETRATE anchor. */
    rt_gen = commit_anchor_epoch_locked();
    s.anchor_valid = false;
    s.rt_media_rebase_valid = false;
    s.rt_media_rebase_clock_id = 0;
    s.rt_media_rebase_epoch = 0;
    s.rt_media_rebase_bias_ns = 0;
    s.realtime_stage_cursor_valid = false;
    s.realtime_stage_generation = rt_gen;
    taskEXIT_CRITICAL(&s.state_mux);
    __atomic_store_n(&s.realtime_stage_running, true, __ATOMIC_RELEASE);
    realtime_stage_kick();
    ESP_LOGI(TAG,
             "REALTIME epoch prepared gen=%" PRIu32
             " waiting for sender D7/SETRATE anchor (no arrival-time fallback)",
             rt_gen);

    realtime_receiver_config_t cfg = {
        .format = fmt_snap.format,
        .encrypt = s.encrypt,
        .pcm_sink = realtime_pcm_sink,
        .pcm_sink_ctx = NULL,
        .deadline_cb = realtime_playout_deadline,
        .deadline_ctx = NULL,
    };
    esp_err_t err = realtime_receiver_start(data_port, control_port, &cfg);
    if (err != ESP_OK) {
      __atomic_store_n(&s.realtime_stage_running, false, __ATOMIC_RELEASE);
      realtime_stage_kick();
    } else {
      stats_session_start();
    }
    return err;
  }

  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_receiver_start(uint16_t data_port, uint16_t control_port) {
  (void)data_port;
  (void)control_port;
  return ESP_ERR_NOT_SUPPORTED;
}

void audio_receiver_stop(void) {
  stats_session_stop();
  /* A codec switch is a hard session boundary. Stop both possible producers
   * first; no compressed, decoded or timing state is allowed to bleed into
   * the next AAC/ALAC SETUP. */
  realtime_stage_stop_and_wait();
  realtime_receiver_stop();
  taskENTER_CRITICAL(&s.state_mux);
  s.realtime_stage_cursor_valid = false;
  taskEXIT_CRITICAL(&s.state_mux);
  s.rx_running = false;

  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  mark_timeline_discontinuity();
  __atomic_store_n(&s.playout_servo_reset_requested, true, __ATOMIC_RELEASE);

  if (s.transport) {
    ap2_buffered_transport_stop(s.transport);
  }

  /* The buffered processor exits when rx_running becomes false. Wait only on
   * the control path so its decoder cannot still publish PCM while the next
   * codec session is being configured. */
  for (int i = 0; s.processor_task && i < 100; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (s.transport) {
    ap2_buffered_transport_clear(s.transport);
  }

  taskENTER_CRITICAL(&s.state_mux);
  s.buffered_session_seq = 0;
  s.buffered_session_rx = 0;
  s.buffered_session_seq_valid = false;
  s.immediate_flush_requested = false;
  s.immediate_flush_has_endpoint = false;
  s.immediate_flush_diag_active = false;
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    s.deferred_flush[i].in_use = false;
    s.deferred_flush[i].active = false;
  }
  taskEXIT_CRITICAL(&s.state_mux);

  /* Do not reset the stateful EQ asynchronously from the control core. AAC
   * and the ALAC staging task each reset it on their next generation. */
  s.realtime_eq_generation = 0;
  s.port = 0;
}

void audio_receiver_stop_buffered_only(void) { audio_receiver_stop(); }
uint16_t audio_receiver_get_buffered_port(void) { return s.port; }

size_t audio_receiver_get_buffered_audio_buffer_size(void) {
  return ap2_buffered_transport_capacity(s.transport);
}

uint16_t audio_receiver_get_stream_port(void) { return s.port; }
void audio_receiver_set_volume_q15(int32_t volume_q15) {
  if (volume_q15 < 0) volume_q15 = 0;
  if (volume_q15 > 32768) volume_q15 = 32768;
  __atomic_store_n(&s_volume_target_q15, volume_q15, __ATOMIC_RELEASE);
}

int32_t audio_receiver_get_volume_q15(void) {
  return __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
}

void audio_receiver_get_stats(audio_stats_t *out) { if (out) *out = s.public_stats; }
void audio_receiver_flush(void) { mark_timeline_discontinuity(); }
void audio_receiver_seek_flush(void) { mark_timeline_discontinuity(); }

void audio_receiver_realtime_flush_to_rtp(uint32_t flush_rtp) {
  timing_snapshot_t snap;
  snapshot_state(&snap);
  if (snap.stream_type != AUDIO_STREAM_REALTIME) {
    audio_receiver_seek_flush();
    return;
  }

  /* RTP-Info names the first timestamp that may remain after FLUSH. Preserve
   * the validated sender timing map and current generation, invalidate only
   * older audio resident in the finite RTP-addressed stores, and move the
   * chronological staging floor forward to the same RTP boundary. */
  const uint32_t from_rtp = flush_rtp - PCM_RTP_RING_FRAMES;
  if (s.realtime_stage_ring) {
    pcm_rtp_ring_invalidate_range(s.realtime_stage_ring, from_rtp, flush_rtp,
                                  snap.generation);
  }
  if (s.pcm_ring) {
    pcm_rtp_ring_invalidate_range(s.pcm_ring, from_rtp, flush_rtp,
                                  snap.generation);
  }

  uint32_t old_cursor = 0;
  bool old_cursor_valid = false;
  taskENTER_CRITICAL(&s.state_mux);
  if (s.stream_type == AUDIO_STREAM_REALTIME &&
      s.generation == snap.generation && !s.timeline_reset_pending) {
    old_cursor_valid = s.realtime_stage_cursor_valid &&
                       s.realtime_stage_generation == snap.generation;
    if (old_cursor_valid) old_cursor = s.realtime_stage_cursor_rtp;
    if (!old_cursor_valid || rtp_delta(flush_rtp, old_cursor) > 0) {
      s.realtime_stage_cursor_rtp = flush_rtp;
      s.realtime_stage_generation = snap.generation;
      s.realtime_stage_cursor_valid = true;
    }
  }
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();

  /* An explicit FLUSH may cancel already queued DMA, but it must not change
   * the validated RTP<->presentation map. The normal playout task re-primes
   * against the same sender timeline and waits until desired_rtp reaches
   * flush_rtp. */
  s.i2s_flush_requested = true;
  if (old_cursor_valid) {
    ESP_LOGI(TAG,
             "REALTIME FLUSH preserve timing rtp=%" PRIu32
             " gen=%" PRIu32 " anchor=%d oldCursor=%" PRIu32,
             flush_rtp, snap.generation, snap.anchor_valid ? 1 : 0, old_cursor);
  } else {
    ESP_LOGI(TAG,
             "REALTIME FLUSH preserve timing rtp=%" PRIu32
             " gen=%" PRIu32 " anchor=%d oldCursor=--",
             flush_rtp, snap.generation, snap.anchor_valid ? 1 : 0);
  }
}

void audio_receiver_realtime_flush_wait_sender_anchor(void) {
  mark_timeline_discontinuity();
  ESP_LOGI(TAG,
           "REALTIME FLUSH no RTP boundary: local timing invalidated; "
           "waiting for D7/SETRATE (no arrival-time fallback)");
}

void audio_receiver_set_deferred_flush_range(uint32_t from_seq, uint32_t from_ts,
                                              uint32_t until_seq, uint32_t until_ts) {
  from_seq &= 0x007fffffU;
  until_seq &= 0x007fffffU;
  taskENTER_CRITICAL(&s.state_mux);
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    if (!s.deferred_flush[i].in_use) {
      s.deferred_flush[i] = (ap2_flush_request_t){
          .in_use = true, .active = false, .from_seq = from_seq,
          .from_rtp = from_ts, .until_seq = until_seq, .until_rtp = until_ts};
      taskEXIT_CRITICAL(&s.state_mux);
      /* Remove already-decoded PCM in the replaced range; rare O(N) control path. */
      pcm_rtp_ring_invalidate_range(s.pcm_ring, from_ts, until_ts, s.generation);
      ESP_LOGI(TAG, "deferred flush queued seq=%" PRIu32 "..%" PRIu32
                    " rtp=%" PRIu32 "..%" PRIu32,
               from_seq, until_seq, from_ts, until_ts);
      return;
    }
  }
  taskEXIT_CRITICAL(&s.state_mux);
  ESP_LOGW(TAG, "deferred flush queue full");
}

void audio_receiver_set_immediate_flush(uint32_t until_seq, uint32_t until_ts,
                                        bool has_endpoint) {
  ap2_buffered_transport_stats_t ts = {0};
  if (s.transport) ap2_buffered_transport_get_stats(s.transport, &ts);
  uint32_t current_seq = 0;
  uint64_t rx_count = 0;
  bool current_seq_valid = false;
  taskENTER_CRITICAL(&s.state_mux);
  current_seq = s.buffered_session_seq & 0x007fffffU;
  rx_count = s.buffered_session_rx;
  current_seq_valid = s.buffered_session_seq_valid;
  taskEXIT_CRITICAL(&s.state_mux);
  until_seq &= 0x007fffffU;
  int32_t distance = (has_endpoint && current_seq_valid && rx_count != 0)
      ? seq23_delta(until_seq, current_seq) : 0;
  bool endpoint_already_reached =
      has_endpoint && current_seq_valid && rx_count != 0 && distance <= 0;
  if (endpoint_already_reached) {
    ESP_LOGI(TAG,
             "FLUSH endpoint already reached current_seq=%" PRIu32
             " until=%" PRIu32 " until_rtp=%" PRIu32
             " seqdist=%" PRId32 "; clearing old raw FIFO",
             current_seq, until_seq, until_ts, distance);
    has_endpoint = false;
  }
  int64_t start_us = esp_timer_get_time();

  taskENTER_CRITICAL(&s.state_mux);
  s.immediate_flush_requested = true;
  s.immediate_flush_has_endpoint = has_endpoint;
  s.immediate_flush_until_seq = until_seq;
  s.immediate_flush_until_rtp = until_ts;
  s.immediate_flush_diag_active = has_endpoint;
  s.immediate_flush_start_us = start_us;
  s.immediate_flush_start_seq = current_seq;
  s.immediate_flush_start_drop = s.diag.timeline_drop;
  s.immediate_flush_start_fifo = ts.fifo_occupancy;
  s.immediate_flush_start_socket_bytes = ts.socket_bytes;
  s.immediate_flush_start_fifo_read = ts.fifo_bytes_read;
  /* Endpoint-bounded immediate FLUSH follows Shairport Sync: keep deferred
   * requests alive until the immediate endpoint is actually reached. A
   * no-endpoint flush is a full local reset, so preserving deferred ranges
   * would leave stale control state behind. */
  if (!has_endpoint) {
    for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
      s.deferred_flush[i].in_use = false;
      s.deferred_flush[i].active = false;
    }
  }
  taskEXIT_CRITICAL(&s.state_mux);

  if (has_endpoint) {
    ESP_LOGI(TAG,
             "FLUSH armed current_seq=%" PRIu32 " until=%" PRIu32
             " until_rtp=%" PRIu32 " seqdist=%" PRId32
             " fifo=%u/%uKiB",
             current_seq, until_seq, until_ts, distance,
             (unsigned)(ts.fifo_occupancy / 1024U),
             (unsigned)(ts.fifo_high_water / 1024U));
  } else {
    ESP_LOGI(TAG,
             "FLUSH immediate no endpoint: clearing raw FIFO occupancy=%uKiB",
             (unsigned)(ts.fifo_occupancy / 1024U));
  }

  mark_timeline_discontinuity();
  if (!has_endpoint && s.transport) ap2_buffered_transport_clear(s.transport);
}

void audio_receiver_pause(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  mark_timeline_discontinuity();
}

void audio_receiver_set_playout_latency_samples(uint32_t v) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playout_latency_samples = v;
  taskEXIT_CRITICAL(&s.state_mux);
}
uint32_t audio_receiver_get_hardware_latency_us(void) { return audio_playout_hardware_latency_us(); }

void audio_receiver_set_playing(bool p) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playing = p;
  taskEXIT_CRITICAL(&s.state_mux);
  if (!p) {
    mark_timeline_discontinuity();
  }
}

bool audio_receiver_is_playing(void) {
  bool playing;
  taskENTER_CRITICAL(&s.state_mux);
  playing = s.playing;
  taskEXIT_CRITICAL(&s.state_mux);
  return playing;
}

void audio_receiver_reset_timing(void) {
  mark_timeline_discontinuity();
  ptp_clock_clear();
}

void audio_receiver_set_client_control(uint32_t ip, uint16_t port) {
  realtime_receiver_set_client_control(ip, port);
}

void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t ptp_ns,
                                    uint32_t rtp) {
  if (clock_id) {
    ptp_clock_set_master_clock_id(clock_id);
  }
  uint32_t gen;
  bool committed;
  taskENTER_CRITICAL(&s.state_mux);
  committed = s.timeline_reset_pending;
  gen = commit_anchor_epoch_locked();
  s.anchor_clock_id = clock_id;
  s.anchor_ptp_ns = ptp_ns;
  s.anchor_local_ns = 0; /* buffered/AAC remains PTP-authoritative */
  s.anchor_rtp = rtp;
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  s.anchor_valid = true;
  s.anchor_set_local_us = esp_timer_get_time();
  s.anchor_set_generation = gen;
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();

  ESP_LOGI(TAG, "ANCHOR clock=%016" PRIx64 " ptp=%" PRIu64
                " rtp=%" PRIu32 " gen=%" PRIu32 "%s",
           clock_id, ptp_ns, rtp, gen,
           committed ? " (new timeline committed; waiting for PTP lock if needed)"
                     : " (anchor update; waiting for PTP lock if needed)");
}


bool audio_receiver_set_realtime_anchor_local(
    uint64_t clock_id, uint32_t gm_epoch, uint32_t mastership_age_ms,
    uint64_t remote_ptp_ns, uint64_t candidate_local_ns, uint32_t rtp,
    audio_realtime_anchor_result_t *result) {
  audio_realtime_anchor_result_t local_result = {0};
  if (!result) result = &local_result;
  memset(result, 0, sizeof(*result));
  if (candidate_local_ns == 0) return false;

  uint32_t gen = 0;
  bool committed = false;
  bool accepted = false;
  bool log_rebase = false;
  uint64_t effective_local_ns = candidate_local_ns;
  int64_t rebase_step_ns = 0;
  int64_t rebase_bias_ns = 0;

  taskENTER_CRITICAL(&s.state_mux);
  if (s.stream_type == AUDIO_STREAM_REALTIME) {
    const bool have_running_local_map =
        s.anchor_valid && !s.timeline_reset_pending && s.anchor_local_ns != 0U;
    const bool epoch_changed =
        have_running_local_map &&
        (!s.rt_media_rebase_valid || s.rt_media_rebase_epoch != gm_epoch ||
         s.rt_media_rebase_clock_id != clock_id);

    if (epoch_changed && mastership_age_ms < AP2_RT_GM_REBASE_SETTLE_MS) {
      result->deferred = true;
      taskEXIT_CRITICAL(&s.state_mux);
      return false;
    }

    if (!have_running_local_map) {
      /* Initial stream startup: no phase exists to preserve yet, so use the
       * sender/GM conversion directly and establish this epoch with zero
       * media bias. The existing ~400 ms PTP readiness remains unchanged. */
      s.rt_media_rebase_valid = true;
      s.rt_media_rebase_clock_id = clock_id;
      s.rt_media_rebase_epoch = gm_epoch;
      s.rt_media_rebase_bias_ns = 0;
      effective_local_ns = candidate_local_ns;
    } else if (epoch_changed) {
      /* New GM/mastership epoch while audio is already running. Preserve the
       * exact existing RTP<->local phase and calculate the one constant media
       * bias needed to express the new GM's D7 observations in that same
       * local timeline. This is a media rebase only: PTP and the PID are not
       * modified. */
      const int sr = s.format.sample_rate > 0 ? s.format.sample_rate : 44100;
      const int32_t drtp = rtp_delta(rtp, s.anchor_rtp);
      const int64_t predicted_local_ns =
          (int64_t)s.anchor_local_ns +
          ((int64_t)drtp * 1000000000LL) / (int64_t)sr;
      rebase_step_ns = (int64_t)candidate_local_ns - predicted_local_ns;
      rebase_bias_ns = -rebase_step_ns;
      const int64_t rebased = (int64_t)candidate_local_ns + rebase_bias_ns;
      if (rebased <= 0) {
        taskEXIT_CRITICAL(&s.state_mux);
        return false;
      }
      effective_local_ns = (uint64_t)rebased;
      s.rt_media_rebase_valid = true;
      s.rt_media_rebase_clock_id = clock_id;
      s.rt_media_rebase_epoch = gm_epoch;
      s.rt_media_rebase_bias_ns = rebase_bias_ns;
      result->rebased = true;
      log_rebase = true;
    } else if (s.rt_media_rebase_valid &&
               s.rt_media_rebase_epoch == gm_epoch &&
               s.rt_media_rebase_clock_id == clock_id) {
      /* Same stable GM epoch: retain the rebase offset but keep consuming
       * every D7, so sender timing/rate changes remain visible without ever
       * reintroducing the GM epoch phase step. */
      rebase_bias_ns = s.rt_media_rebase_bias_ns;
      const int64_t adjusted = (int64_t)candidate_local_ns + rebase_bias_ns;
      if (adjusted <= 0) {
        taskEXIT_CRITICAL(&s.state_mux);
        return false;
      }
      effective_local_ns = (uint64_t)adjusted;
    }

    committed = s.timeline_reset_pending;
    gen = commit_anchor_epoch_locked();
    s.anchor_clock_id = clock_id;
    s.anchor_ptp_ns = remote_ptp_ns; /* diagnostic/reference only for ALAC */
    s.anchor_local_ns = effective_local_ns;
    s.anchor_rtp = rtp;
    s.anchor_valid = true;
    s.anchor_set_local_us = esp_timer_get_time();
    s.anchor_set_generation = gen;
    accepted = true;

    result->effective_local_ns = effective_local_ns;
    result->rebase_step_ns = rebase_step_ns;
    result->rebase_bias_ns = s.rt_media_rebase_bias_ns;
  }
  taskEXIT_CRITICAL(&s.state_mux);

  if (accepted) {
    realtime_stage_kick();
  }

  if (log_rebase) {
    ESP_LOGI(TAG,
             "RT GM MEDIA REBASE clock=%016" PRIx64 " epoch=%" PRIu32
             " age=%lums step=%+.3fms bias=%+.3fms rtp=%" PRIu32,
             clock_id, gm_epoch, (unsigned long)mastership_age_ms,
             (double)rebase_step_ns / 1000000.0,
             (double)rebase_bias_ns / 1000000.0, rtp);
  }

  /* Continuous D7 refreshes are normal steady-state operation.  Keep INFO
   * logging for phase-significant events only; the 2 s ALAC health line
   * already reports map/sync state continuously. */
  if (accepted && (committed || log_rebase)) {
    ESP_LOGI(TAG,
             "RT ANCHOR clock=%016" PRIx64 " epoch=%" PRIu32
             " remotePTP=%" PRIu64 " rawLocal=%" PRIu64
             " local=%" PRIu64 " bias=%+.3fms rtp=%" PRIu32
             " gen=%" PRIu32 "%s",
             clock_id, gm_epoch, remote_ptp_ns, candidate_local_ns,
             effective_local_ns,
             (double)result->rebase_bias_ns / 1000000.0, rtp, gen,
             committed ? " (new timeline committed)" : " (GM rebase)");
  }
  return accepted;
}
