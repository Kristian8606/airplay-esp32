#include "audio_receiver.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aac_decoder.h"
#include "audio_eq.h"
#include "audio_crypto.h"
#include "audio_diag.h"
#include "ap2_buffered_fifo.h"
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
#include "freertos/semphr.h"
#include "network/ptp_clock.h"
#include "network/ptp_clock_engine.h"
#include "network/socket_utils.h"

#define AP2_PACKET_MAX             8192U
#define AP2_RX_STACK               4096U
#define AP2_PROCESS_STACK          6144U
#define AP2_PLAYOUT_STACK          4096U
#define AP2_RT_STAGE_STACK         4096U
#define AP2_STATUS_STACK             4096U
#define AP2_NETWORK_CORE           1
#define AP2_DECODE_CORE            1
#define AP2_BUFFERED_PROCESSOR_CORE 0
#define AP2_RX_PRIORITY            5
#define AP2_DECODE_PRIORITY        6
#define AP2_DECODE_YIELD_EVERY     8U  /* v4.1.19: 1 tick per 8 AAC blocks */
#define AP2_PLAYOUT_PRIORITY       8
#define AP2_RT_STAGE_PRIORITY      7
#define AP2_STATUS_PRIORITY          1
#define AP2_STATUS_CORE              0
#define AP2_STATUS_PERIOD_MS      2000U
/* v4.1.14 buffered timing watchdog: anchor + play but the PTP mapping never
 * qualifies. Normal lock takes < 1 s; give it 5 s, then log why and restart
 * the PTP estimator. At most AP2_TIMING_WD_MAX_RESETS per anchor timeline. */
/* Buffered start gate (v4.1.16).
 * v4.1.15 started at Shairport's 400 ms mastership without our lock. On the
 * board that produced a -10 ms start error: the nqptp-style filter
 * (ptp_clock_engine.c) still jumps to better samples during its first
 * PTP_ENGINE_STARTUP_NS = 1 s, and our fine correction (APLL servo) is too slow
 * to absorb such a step (Shairport absorbs it by frame stuffing). The PID then
 * wound up to +160 ppm and needed minutes to settle.
 * Now the FIRST mapping of an anchor needs the lock AND >= 1 s mastership, so
 * the filter's startup jumps happen before audio starts. Later anchors (skip,
 * pause, next track) are far past 1 s and start as fast as before.
 * Refreshes while playing: lock + 400 ms, as in v4.1.14. */
#define AP2_PTP_START_MASTERSHIP_MS 1000U
#define AP2_PTP_MASTERSHIP_MIN_MS   400U
#define AP2_PTP_SAMPLE_MAX_AGE_MS  5000U
/* v4.1.15 hard resync, Shairport "resync_threshold" semantics: if the output
 * is out of sync by more than this for AP2_RESYNC_HOLD_US, stop and re-prime
 * at the exact presentation point (brief silence), keeping the learned I2S
 * clock correction. Shairport uses 50 ms, but it also corrects small errors
 * quickly by frame stuffing; our fine correction is a slow APLL servo
 * (<= 160 ppm), so the threshold is lower. */
#define AP2_RESYNC_THRESHOLD_US      20000
#define AP2_RESYNC_HOLD_US         1500000LL
#define AP2_RESYNC_MIN_INTERVAL_US 5000000LL
#define AP2_TIMING_WD_TIMEOUT_MS  5000U
#define AP2_TIMING_WD_RETRY_MS    6000U
#define AP2_TIMING_WD_MAX_RESETS     3U
#define AP2_PCM_CAPACITY_FRAMES    1024U

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
#define AP2_START_REAL_BOUNDARY_BLOCKS      (AP2_START_SILENCE_FUTURE_BLOCKS + 2U)
#define AP2_START_ALIGN_TIMEOUT_US      30000LL
#define AP2_START_PRIME_GUARD_BLOCKS        8U
#define AP2_BUFFERED_START_RESERVE_MS      250U

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

/* v4.1.17 slow centring inside the +/-1 ms good zone (multiroom accuracy).
 * The PID holds the clock inside the zone, so sync used to park near +/-1 ms
 * and the crystal error was never learned precisely. Every window the
 * centring loop measures the real phase slope and steers the frequency so
 * that sync approaches 0 at <= AP2_CENTER_MAX_SLOPE_US_S and then stays flat.
 * 1 ppm of I2S rate change = 1 us/s of sync slope. */
#define AP2_CENTER_WINDOW_US         15000000LL /* slope measurement window */
#define AP2_CENTER_QUIET_US               200    /* |sync| below: aim for 0 slope */
#define AP2_CENTER_TIME_S                20.0   /* approach time constant (v4.1.19: was 60) */
#define AP2_CENTER_MAX_SLOPE_US_S        40.0   /* max wanted approach speed (v4.1.19: was 10) */
#define AP2_CENTER_MAX_STEP_PPM            40    /* max frequency step per window */
/* v4.1.18 hysteresis: centring mode is entered inside +/-1 ms and is kept
 * through brief excursions. It hands back to the PID only when |sync| stays
 * above 1.5 ms for 3 s, or immediately above 3 ms. The old code dropped out on
 * any momentary D-estimate spike, so a 15 s window never completed. */
#define AP2_CENTER_EXIT_US               1500
#define AP2_CENTER_EXIT_HOLD_US       3000000LL
#define AP2_CENTER_EXIT_HARD_US          3000
/* v4.1.19: a PTP filter step moves sync by >1 ms between two 1 s samples,
 * while real drift is < 0.1 ms/s. A window that contains such a jump gave a
 * false slope (-83 us/s in the board log). The jump is now subtracted from the
 * following samples of the window, so the slope stays the true drift and the
 * end-of-window phase still includes the step. */
#define AP2_CENTER_JUMP_US                400
/* Fraction of the computed correction applied per window. 0.5 was tried in
 * simulation for v4.1.19 and made phase recovery slower without reducing
 * retunes, so the full step is kept. */
#define AP2_CENTER_GAIN                   1.0
#define AP2_PCM_TARGET_MS           1000U
#define AP2_REALTIME_PRIME_MS        100U
#define AP2_RT_GM_REBASE_SETTLE_MS  1000U
#define AP2_BUFFERED_STORE_REQUEST_BYTES AP2_BUFFERED_AUDIO_BUFFER_REQUEST_BYTES
#define AP2_BUFFERED_LEAD_MS       (AP2_PCM_TARGET_MS + 100U)
#define AP2_PHASE_HISTORY_SAMPLES        32U

/* ALAC reorder release point. Missing PCM stays absent in the raw RTP ring
 * while retransmission runs independently. Only when the physical PTP
 * deadline is this close do we commit one frame of silence through EQ. */
#define AP2_RT_STAGE_COMMIT_MARGIN_US REALTIME_RECOVERY_FINAL_MARGIN_US

static const char *TAG = "audio_receiver";
static const char *STATUS_TAG = "audio_status";

/* Updated by RTSP control on Core0, consumed by playout on Core1. */
static volatile int32_t s_volume_target_q15 = 32768;

typedef struct {
  bool anchor_valid;
  bool playing;
  uint64_t anchor_clock_id;
  uint64_t anchor_ptp_ns;
  uint64_t anchor_local_ns; /* last valid ESP-local anchor for buffered holdover */
  uint64_t anchor_local_update_us; /* last qualified same-master refresh */
  bool anchor_local_valid;
  uint32_t anchor_rtp;
  uint32_t generation;          /* timing/playout epoch */
  uint32_t media_revision;      /* changes on every buffered anchor update */
  uint32_t pcm_generation;      /* buffered media-store epoch; realtime == generation */
  audio_format_t format;
  uint32_t format_generation;
  bool timeline_reset_pending;
  audio_stream_type_t stream_type;
  uint32_t playout_latency_samples;
} timing_snapshot_t;

typedef struct {
  /* Physical tagged-DMA phase used directly by the clock servo. */
  int32_t us;
  uint32_t generation;
  bool valid;

  /* Passive Shairport-style robust phase observation. It is never fed into
   * PID, cursor movement or playout decisions in this version. */
  int32_t phase_samples[AP2_PHASE_HISTORY_SAMPLES];
  uint8_t phase_count;
  uint8_t phase_index;
} output_sync_state_t;

typedef struct {
  audio_format_t format;
  audio_encrypt_t encrypt;
  audio_stream_type_t stream_type;

  uint16_t port;
  volatile bool engine_running;
  volatile bool rx_running;

  SemaphoreHandle_t publish_mutex; /* producers/FLUSH only; never I2S */
  ap2_buffered_fifo_t *transport;
  TaskHandle_t processor_task;
  TaskHandle_t playout_task;
  TaskHandle_t realtime_stage_task;
  TaskHandle_t status_task;
  SemaphoreHandle_t status_wake;
  SemaphoreHandle_t playout_wake;
  esp_timer_handle_t playout_timer;
  uint8_t *buffered_packet;
  uint8_t *decrypt_buf;
  int16_t *decode_pcm;
  int16_t *realtime_stage_pcm;
  uint8_t *codec_workspace;       /* shared AAC payload / ALAC large buffers */
  size_t codec_workspace_size;
  bool realtime_workspace_bound;
  pcm_rtp_ring_t *pcm_ring;          /* final EQ'd PCM -> PTP/I2S */
  pcm_rtp_ring_t *realtime_stage_ring; /* raw decoded ALAC, RTP-addressed */

  portMUX_TYPE state_mux;
  bool playing;
  bool anchor_valid;
  uint64_t anchor_clock_id;
  uint64_t anchor_ptp_ns;
  uint64_t anchor_local_ns; /* ESP monotonic time corresponding to anchor_rtp */
  uint64_t anchor_local_update_us; /* last qualified same-master local-anchor refresh */
  bool anchor_local_valid;
  uint32_t anchor_rtp;
  /* Realtime media-domain rebase. This is deliberately separate from PTP:
   * it only maps a new GM epoch onto the already-running local media phase. */
  bool rt_media_rebase_valid;
  uint64_t rt_media_rebase_clock_id;
  uint32_t rt_media_rebase_epoch;
  int64_t rt_media_rebase_bias_ns;
  int64_t rt_media_rebase_start_us;
  uint32_t generation;          /* timing/playout epoch */
  uint32_t media_revision;      /* changes on every buffered anchor update */
  uint32_t buffered_pcm_generation; /* survives buffered seek/anchor changes */
  uint32_t format_generation;
  bool timeline_reset_pending;
  uint32_t playout_latency_samples;
  volatile bool realtime_stage_running;
  volatile bool realtime_stage_idle;
  bool realtime_stage_cursor_valid;
  uint32_t realtime_stage_cursor_rtp;
  uint32_t realtime_stage_generation;
  /* Buffered transport control state belongs to one codec/session only.
   * FLUSH decisions must never reuse a sequence number from an older AAC
   * session after ALAC was active. */
  volatile bool i2s_flush_requested;
  volatile bool playout_servo_reset_requested;
  /* Hard stream/session boundaries are requested by the RTSP/control core but
   * executed by the single-owner playout task. The sequence pair provides a
   * bounded acknowledgement so the next codec SETUP cannot race ahead of an
   * unprocessed I2S flush/reset. */
  volatile uint32_t playout_quiesce_req;
  volatile uint32_t playout_quiesce_ack;
  output_sync_state_t output_sync;

  /* Only the existing 1 Hz PID loop publishes these. The 2 s status
   * task reads them; no packet/decode/I2S-completion instrumentation is added. */
  volatile bool status_sync_valid;
  volatile int32_t status_sync_us;
  volatile bool status_phase_valid;
  volatile int32_t status_phase_us;
  volatile int32_t status_servo_ppm;
  volatile uint32_t status_sync_generation;
} ap2_state_t;

static ap2_state_t s = {
    .stream_type = AUDIO_STREAM_NONE,
    .generation = 1,
    .buffered_pcm_generation = 1,
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

/* Remove a GM handover compensation at 50 us/second. This is phase
 * convergence, not another hardware-rate controller. The initial handover
 * stays continuous; a real old-epoch error is no longer preserved forever. */
static int64_t realtime_remaining_bias(int64_t bias_ns, int64_t start_us,
                                        int64_t now_us) {
  if (bias_ns == 0 || now_us <= start_us) return bias_ns;
  const uint64_t elapsed_us = (uint64_t)now_us - (uint64_t)start_us;
  const uint64_t magnitude = bias_ns < 0
      ? (uint64_t)(-(bias_ns + 1)) + 1U : (uint64_t)bias_ns;
  /* 50 ppm = 0.05 ns per microsecond, i.e. 1 ns per 20 us. */
  const uint64_t correction_ns = elapsed_us / 20U;
  if (correction_ns >= magnitude) return 0;
  return bias_ns < 0 ? bias_ns + (int64_t)correction_ns
                     : bias_ns - (int64_t)correction_ns;
}

static void playout_wake(void) {
  if (s.playout_wake) xSemaphoreGive(s.playout_wake);
}

static void playout_timer_callback(void *arg) {
  (void)arg;
  playout_wake();
}

static void media_control_wake(void) {
  ap2_buffered_fifo_notify(s.transport);
  playout_wake();
}

/* Pure read: only playout refreshes the cached PTP mapping. */
static void snapshot_state_locked(timing_snapshot_t *out) {
  out->anchor_valid = s.anchor_valid;
  out->playing = s.playing;
  out->anchor_clock_id = s.anchor_clock_id;
  out->anchor_ptp_ns = s.anchor_ptp_ns;
  out->anchor_local_ns = s.anchor_local_ns;
  out->anchor_local_update_us = s.anchor_local_update_us;
  out->anchor_local_valid = s.anchor_local_valid;
  out->anchor_rtp = s.anchor_rtp;
  out->generation = s.generation;
  out->media_revision = s.media_revision;
  out->pcm_generation =
      s.stream_type == AUDIO_STREAM_BUFFERED ? s.buffered_pcm_generation
                                             : s.generation;
  out->format = s.format;
  out->format_generation = s.format_generation;
  out->timeline_reset_pending = s.timeline_reset_pending;
  out->stream_type = s.stream_type;
  out->playout_latency_samples = s.playout_latency_samples;
}

static void snapshot_state(timing_snapshot_t *out) {
  taskENTER_CRITICAL(&s.state_mux);
  snapshot_state_locked(out);
  taskEXIT_CRITICAL(&s.state_mux);
}

/* Buffered AAC: may this PTP snapshot map the anchor's remote time to local
 * time? `initial` = the anchor has no local mapping yet (start/new anchor). */
static bool buffered_ptp_qualified(const ptp_clock_snapshot_t *ps,
                                   uint64_t anchor_clock_id, bool initial) {
  if (!ps || ps->realtime_mode || !ps->valid) return false;
  if (ps->grandmaster_clock_id == 0 ||
      ps->grandmaster_clock_id != anchor_clock_id)
    return false;
  if (!ps->locked) return false;
  if (ps->sample_age_ms > AP2_PTP_SAMPLE_MAX_AGE_MS) return false;
  return ps->mastership_age_ms >=
         (initial ? AP2_PTP_START_MASTERSHIP_MS : AP2_PTP_MASTERSHIP_MIN_MS);
}

/* First-audio measurement (v4.1.15): anchor commit time per generation. */
static volatile int64_t s_anchor_commit_us = 0;
static volatile uint32_t s_anchor_commit_gen = 0;

/* Single writer for PTP refresh; control still owns anchor publication. */
static void refresh_timing_snapshot(timing_snapshot_t *out) {
  uint64_t clock_id;
  uint32_t epoch;
  int64_t bias_ns, bias_start_us;
  taskENTER_CRITICAL(&s.state_mux);
  snapshot_state_locked(out);
  const bool was_local_valid = out->anchor_local_valid;
  clock_id = s.anchor_clock_id;
  epoch = s.rt_media_rebase_epoch;
  bias_ns = s.rt_media_rebase_bias_ns;
  bias_start_us = s.rt_media_rebase_start_us;
  taskEXIT_CRITICAL(&s.state_mux);

  if (out->stream_type == AUDIO_STREAM_BUFFERED && out->anchor_valid &&
      !out->timeline_reset_pending) {
    ptp_clock_snapshot_t ps = {0};
    ptp_clock_get_snapshot(&ps);
    if (!ps.realtime_mode) {
      const uint64_t now_us = (uint64_t)esp_timer_get_time();
      const bool same_master = ps.grandmaster_clock_id != 0 &&
                               ps.grandmaster_clock_id == out->anchor_clock_id;
      const bool qualified_master = buffered_ptp_qualified(
          &ps, out->anchor_clock_id, !out->anchor_local_valid);

      if (same_master && qualified_master) {
        /* Shairport Sync semantics: while the advertised master is the same
         * clock that owns the RTSP anchor, continuously refresh the local
         * representation and remember exactly when that mapping was last
         * known-good. A transient GM may not move this cached local point. */
        uint64_t local_anchor = 0;
        if (ptp_clock_engine_remote_to_local(
                out->anchor_ptp_ns, ps.filtered_offset_ns, &local_anchor)) {
          bool refreshed = false;
          taskENTER_CRITICAL(&s.state_mux);
          if (s.stream_type == AUDIO_STREAM_BUFFERED && s.anchor_valid &&
              !s.timeline_reset_pending && s.generation == out->generation &&
              s.media_revision == out->media_revision &&
              s.anchor_rtp == out->anchor_rtp &&
              s.anchor_clock_id == out->anchor_clock_id &&
              s.anchor_ptp_ns == out->anchor_ptp_ns) {
            s.anchor_local_ns = local_anchor;
            s.anchor_local_update_us = now_us;
            s.anchor_local_valid = true;
            refreshed = true;
          }
          taskEXIT_CRITICAL(&s.state_mux);
          if (refreshed) {
            out->anchor_local_ns = local_anchor;
            out->anchor_local_update_us = now_us;
            out->anchor_local_valid = true;
          }
        }
      } else if (!same_master && ps.grandmaster_clock_id != 0 &&
                 out->anchor_local_valid && out->anchor_local_ns != 0 &&
                 out->anchor_local_update_us != 0) {
        /* Shairport does not time handover from first mismatch observation.
         * It measures from the last successful local-anchor update by the old
         * master. That timestamp stops moving as soon as the GM differs. */
        const uint64_t stale_age_us =
            now_us >= out->anchor_local_update_us
                ? now_us - out->anchor_local_update_us
                : 0;
        const uint32_t stale_age_ms =
            stale_age_us / 1000ULL > UINT32_MAX
                ? UINT32_MAX
                : (uint32_t)(stale_age_us / 1000ULL);


        /* Only a qualified new GM may take ownership. Rebuild its remote
         * anchor from the exact same cached ESP-local media point. */
        if (ps.valid &&
            ptp_clock_engine_handover_ready(
                out->anchor_local_valid, ps.locked, out->anchor_clock_id,
                ps.grandmaster_clock_id, ps.mastership_age_ms, stale_age_ms)) {
          uint64_t new_remote_anchor = 0;
          if (ptp_clock_engine_local_to_remote(
                  out->anchor_local_ns, ps.filtered_offset_ns,
                  &new_remote_anchor)) {
            bool committed = false;
            taskENTER_CRITICAL(&s.state_mux);
            if (s.stream_type == AUDIO_STREAM_BUFFERED && s.anchor_valid &&
                !s.timeline_reset_pending && s.generation == out->generation &&
                s.media_revision == out->media_revision &&
                s.anchor_rtp == out->anchor_rtp &&
                s.anchor_clock_id == out->anchor_clock_id &&
                s.anchor_ptp_ns == out->anchor_ptp_ns &&
                s.anchor_local_valid &&
                s.anchor_local_ns == out->anchor_local_ns &&
                s.anchor_local_update_us == out->anchor_local_update_us) {
              s.anchor_clock_id = ps.grandmaster_clock_id;
              s.anchor_ptp_ns = new_remote_anchor;
              /* Deliberately preserve anchor_local_ns and its old update time.
               * On the next qualified snapshot the now-current GM refreshes
               * them, matching Shairport's two-step handover behaviour. */
              committed = true;
            }
            taskEXIT_CRITICAL(&s.state_mux);
            if (committed) {
              out->anchor_clock_id = ps.grandmaster_clock_id;
              out->anchor_ptp_ns = new_remote_anchor;
            }
          }
        }
      }
    }
  }

  if (out->stream_type == AUDIO_STREAM_REALTIME && out->anchor_valid &&
      !out->timeline_reset_pending && out->anchor_ptp_ns != 0U) {
    ptp_realtime_snapshot_t ps;
    ptp_clock_get_realtime_snapshot(&ps);
    uint64_t local_ns;
    if (ps.gm_change_count == epoch &&
        ptp_clock_realtime_snapshot_to_local(
            &ps, clock_id, out->anchor_ptp_ns, &local_ns)) {
      const int64_t remaining = realtime_remaining_bias(
          bias_ns, bias_start_us, esp_timer_get_time());
      /* All normal timestamps are ESP uptime values. Reject overflow instead
       * of publishing an invalid deadline if control input is malformed. */
      if ((remaining > 0 && local_ns > (uint64_t)(INT64_MAX - remaining)) ||
          (remaining < 0 && local_ns < (uint64_t)(-(remaining + 1)) + 1U)) {
        snapshot_state(out);
        return;
      }
      const int64_t effective = (int64_t)local_ns + remaining;
      if (effective <= 0) { snapshot_state(out); return; }
      out->anchor_local_ns = (uint64_t)effective;
      /* Cache the most recent valid local map for a temporary PTP outage or
       * GM acquisition. Never overwrite an anchor changed during the lookup. */
      taskENTER_CRITICAL(&s.state_mux);
      if (s.stream_type == out->stream_type && s.generation == out->generation &&
          s.anchor_valid && !s.timeline_reset_pending &&
          s.anchor_clock_id == clock_id && s.rt_media_rebase_epoch == epoch &&
          s.anchor_rtp == out->anchor_rtp && s.anchor_ptp_ns == out->anchor_ptp_ns) {
        s.anchor_local_ns = out->anchor_local_ns;
      }
      taskEXIT_CRITICAL(&s.state_mux);
    }
  }
  /* Return the committed map, including any control change during lookup. */
  snapshot_state(out);
  if (!was_local_valid && out->anchor_local_valid)
    ap2_buffered_fifo_notify(s.transport);
}

static uint32_t next_generation(uint32_t generation) {
  generation++;
  return generation ? generation : 1U;
}

/* Hard reset for buffered media storage only. Ordinary pause/seek/anchor
 * changes must not call this: they change the timing epoch, not the identity
 * of already received RTP-addressed PCM. */
static uint32_t reset_buffered_pcm_store(void) {
  taskENTER_CRITICAL(&s.state_mux);
  /* All identities used by the shared final ring come from the timing epoch
   * allocator. A hard media reset is also a hard timing boundary; incrementing
   * a separate AAC counter could reuse the preceding ALAC ring generation. */
  const uint32_t new_gen = s.generation = next_generation(s.generation);
  if (s.pcm_ring) {
    pcm_rtp_ring_set_generation(s.pcm_ring, new_gen);
  }
  s.buffered_pcm_generation = new_gen;
  taskEXIT_CRITICAL(&s.state_mux);
  return new_gen;
}

/* Pause/immediate FLUSH ends the current presentation timing now. TCP keeps
 * arriving into the raw FIFO while the anchor is invalid; the sequential
 * consumer alone decides whether each framed packet is discarded or decoded. */
static void mark_timeline_discontinuity(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.anchor_valid = false;
  s.timeline_reset_pending = true;
  s.anchor_local_update_us = 0;
  s.anchor_local_valid = false;
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  media_control_wake();
}

/* PAUSE/rate=0 is a presentation-clock stop, not by itself a compressed-media
 * discontinuity. AirPlay may emit many pause/resume anchor updates while the
 * user holds the seek buttons. Only FLUSH/FLUSHBUFFERED owns media-cursor
 * invalidation. Preserve an already-pending media reset from a preceding
 * FLUSH, but do not create a new one just because playback paused. */
static void pause_presentation_timing(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.anchor_valid = false;
  s.anchor_local_update_us = 0;
  s.anchor_local_valid = false;
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  media_control_wake();
}

static uint32_t commit_anchor_epoch_locked(void) {
  uint32_t gen = s.generation;
  if (s.timeline_reset_pending) {
    gen = next_generation(gen);
    /* Timing/playout epoch changes are not automatically PCM-store resets.
     * Buffered AAC may keep still-valid RTP-addressed PCM across an ordinary
     * pause/anchor refresh, but compressed packets behind the decoder are not a
     * rewind cache. Realtime ALAC still owns generation-scoped raw/final rings
     * because its recovery/EQ chronology is one continuous realtime epoch. */
    if (s.stream_type == AUDIO_STREAM_REALTIME) {
      if (s.pcm_ring) {
        pcm_rtp_ring_set_generation(s.pcm_ring, gen);
      }
      if (s.realtime_stage_ring) {
        pcm_rtp_ring_set_generation(s.realtime_stage_ring, gen);
      }
      s.realtime_stage_cursor_valid = false;
      s.realtime_stage_generation = gen;
    }
    s.generation = gen;
    s.timeline_reset_pending = false;
  }
  return gen;
}

static inline int32_t rtp_delta(uint32_t a, uint32_t b) {
  return (int32_t)(a - b);
}

/* Presentation-clock boundary. Both codecs schedule against ESP monotonic
 * time. Buffered AAC continuously converts its remote anchor to local time
 * while its anchor GM is current, then freezes that local anchor across GM
 * acquisition exactly like Shairport Sync. */
static bool timing_clock_ready(const timing_snapshot_t *snap) {
  if (!snap || !snap->anchor_valid || snap->timeline_reset_pending) return false;
  if (snap->stream_type == AUDIO_STREAM_BUFFERED)
    return snap->anchor_local_valid && snap->anchor_local_ns != 0;
  return snap->anchor_local_ns != 0;
}

static uint64_t presentation_now_ns(const timing_snapshot_t *snap) {
  (void)snap;
  return (uint64_t)esp_timer_get_time() * 1000ULL;
}

static uint64_t presentation_anchor_ns(const timing_snapshot_t *snap) {
  return snap ? snap->anchor_local_ns : 0;
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

/* Permanent audio-status policy:
 * - exactly one low-priority task owns periodic audio-status logging;
 * - the hot packet/decode path maintains no counters/timers just for logging;
 * - the playout task publishes sync/ppm only at its already-existing 1 Hz PID;
 * - one persistent task sleeps on a semaphore while paused; control never waits. */
static double status_frames_to_ms(uint32_t frames, int sample_rate) {
  const uint32_t sr = sample_rate > 0 ? (uint32_t)sample_rate : 44100U;
  return ((double)frames * 1000.0) / (double)sr;
}

static void status_publish_sync(int32_t sync_us, int32_t phase_us,
                                int32_t servo_ppm, uint32_t generation) {
  __atomic_store_n(&s.status_sync_us, sync_us, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_phase_us, phase_us, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_servo_ppm, servo_ppm, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_sync_generation, generation, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_phase_valid, true, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_sync_valid, true, __ATOMIC_RELEASE);
}

static void status_invalidate_sync(void) {
  __atomic_store_n(&s.status_phase_valid, false, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_sync_valid, false, __ATOMIC_RELEASE);
}

static int32_t robust_phase_center_us(const output_sync_state_t *sync) {
  if (!sync || !sync->valid || sync->phase_count == 0) return 0;
  if (sync->phase_count < 4U) return sync->us;

  int32_t sorted[AP2_PHASE_HISTORY_SAMPLES];
  const uint32_t n = sync->phase_count;
  for (uint32_t i = 0; i < n; ++i) sorted[i] = sync->phase_samples[i];
  for (uint32_t i = 1; i < n; ++i) {
    const int32_t v = sorted[i];
    uint32_t j = i;
    while (j > 0 && sorted[j - 1U] > v) {
      sorted[j] = sorted[j - 1U];
      --j;
    }
    sorted[j] = v;
  }
  return (int32_t)(((int64_t)sorted[1] + (int64_t)sorted[n - 2U]) / 2LL);
}

/* Buffered AAC timing watchdog, run from the low-priority status task.
 * Decoding is gated on anchor_local_valid, which needs a LOCKED PTP estimator
 * on the anchor's master for >= 400 ms. If that never happens the stream is
 * silent while the FIFO fills (first session after boot in v4.1.13 logs).
 * Log the exact failing condition and reset the estimator. */
static void buffered_timing_watchdog(const timing_snapshot_t *snap) {
  static uint32_t wd_generation = 0;
  static int64_t wd_since_us = 0;
  static int64_t wd_last_reset_us = 0;
  static uint32_t wd_resets = 0;

  const bool waiting = snap->stream_type == AUDIO_STREAM_BUFFERED &&
                       snap->playing && snap->anchor_valid &&
                       !snap->timeline_reset_pending &&
                       !snap->anchor_local_valid;
  const int64_t now_us = esp_timer_get_time();
  if (!waiting) {
    wd_since_us = 0;
    if (snap->generation != wd_generation) wd_resets = 0;
    wd_generation = snap->generation;
    return;
  }
  if (snap->generation != wd_generation || wd_since_us == 0) {
    if (snap->generation != wd_generation) wd_resets = 0;
    wd_generation = snap->generation;
    wd_since_us = now_us;
    wd_last_reset_us = 0;
    return;
  }
  const int64_t waited_ms = (now_us - wd_since_us) / 1000LL;
  if (waited_ms < (int64_t)AP2_TIMING_WD_TIMEOUT_MS) return;
  if (wd_last_reset_us != 0 &&
      (now_us - wd_last_reset_us) / 1000LL < (int64_t)AP2_TIMING_WD_RETRY_MS)
    return;
  if (wd_resets >= AP2_TIMING_WD_MAX_RESETS) return;

  ptp_clock_snapshot_t ps = {0};
  ptp_clock_get_snapshot(&ps);
  const char *reason =
      ps.realtime_mode ? "ptp-in-realtime-mode"
      : !ps.valid ? "no-ptp-samples"
      : ps.source_mixed ? "ptp-source-mixed"
      : ps.grandmaster_clock_id != snap->anchor_clock_id ? "gm-differs-from-anchor"
      : !ps.locked ? "ptp-not-locked"
      : ps.sample_age_ms > AP2_PTP_SAMPLE_MAX_AGE_MS ? "ptp-samples-stale"
      : ps.mastership_age_ms < AP2_PTP_START_MASTERSHIP_MS ? "mastership-too-young"
      : "anchor-map-failed";
  ESP_LOGW(TAG,
           "TIMING WATCHDOG: anchor waiting %lld ms, reason=%s | valid=%d locked=%d "
           "mixed=%d rt=%d src=%016llx gm=%016llx anchor=%016llx age=%lums "
           "samples=%lu sampleAge=%lums peers=%lu -> PTP reset %lu/%u",
           (long long)waited_ms, reason, ps.valid, ps.locked, ps.source_mixed,
           ps.realtime_mode, (unsigned long long)ps.source_clock_id,
           (unsigned long long)ps.grandmaster_clock_id,
           (unsigned long long)snap->anchor_clock_id,
           (unsigned long)ps.mastership_age_ms, (unsigned long)ps.sample_count,
           (unsigned long)ps.sample_age_ms, (unsigned long)ps.peer_count,
           (unsigned long)(wd_resets + 1U), (unsigned)AP2_TIMING_WD_MAX_RESETS);
  ptp_clock_clear();
  wd_resets++;
  wd_last_reset_us = now_us;
}

/* v4.1.18: the playout task never logs. It posts numbers into these
 * single-writer mailboxes (seqlock) and the low-priority audio_status_task
 * formats and prints them. A log line can block on UART/USB for milliseconds;
 * that must never happen on the task that feeds I2S. */
typedef struct {
  volatile uint32_t seq;   /* odd while the writer is updating */
  volatile uint32_t count; /* events posted so far */
  int32_t v[5];
  const char *str;
} playout_evt_t;

enum {
  PEVT_START = 0,
  PEVT_RESYNC,
  PEVT_CENTER,
  PEVT_TUNE_FAIL,
  PEVT_FLUSH_FAIL,
  PEVT_SERVO_RESET_FAIL,
  PEVT_COUNT
};
static playout_evt_t s_pevt[PEVT_COUNT];

static void pevt_post(int kind, int32_t a, int32_t b, int32_t c, int32_t d,
                      int32_t e, const char *str) {
  playout_evt_t *ev = &s_pevt[kind];
  const uint32_t q = ev->seq;
  __atomic_store_n(&ev->seq, q + 1U, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  ev->v[0] = a; ev->v[1] = b; ev->v[2] = c; ev->v[3] = d; ev->v[4] = e;
  ev->str = str;
  ev->count = ev->count + 1U;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  __atomic_store_n(&ev->seq, q + 2U, __ATOMIC_RELAXED);
}

/* Reader side (status task). Returns true once per new event; `missed` is the
 * number of additional events of this kind since the previous report. */
static bool pevt_take(int kind, uint32_t *last_count, int32_t v[5],
                      const char **str, uint32_t *missed) {
  playout_evt_t *ev = &s_pevt[kind];
  for (int tries = 0; tries < 3; ++tries) {
    const uint32_t q1 = __atomic_load_n(&ev->seq, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (q1 & 1U) continue;
    const uint32_t count = ev->count;
    for (int i = 0; i < 5; ++i) v[i] = ev->v[i];
    const char *sp = ev->str;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&ev->seq, __ATOMIC_RELAXED) != q1) continue;
    if (count == *last_count) return false;
    *missed = count - *last_count - 1U;
    *last_count = count;
    if (str) *str = sp;
    return true;
  }
  return false; /* writer busy; pick it up on the next cycle */
}

/* Format and print what the playout task posted (v4.1.18). */
static void status_print_playout_events(void) {
  static uint32_t last[PEVT_COUNT];
  int32_t v[5];
  const char *str = NULL;
  uint32_t missed = 0;
  if (pevt_take(PEVT_START, &last[PEVT_START], v, NULL, &missed))
    ESP_LOGI(TAG, "PLAYOUT START gen=%ld: first audio %ld ms after anchor "
                  "(ptp locked=%ld mastership=%ldms samples=%ld)",
             (long)v[0], (long)v[1], (long)v[2], (long)v[3], (long)v[4]);
  if (pevt_take(PEVT_RESYNC, &last[PEVT_RESYNC], v, NULL, &missed))
    ESP_LOGW(TAG, "RESYNC #%ld: sync error %+.2f ms beyond %.0f ms for %ld ms "
                  "-> re-prime at presentation point (I2S servo %+ld ppm kept)",
             (long)v[0], (double)v[1] / 1000.0,
             (double)AP2_RESYNC_THRESHOLD_US / 1000.0, (long)v[2], (long)v[3]);
  if (pevt_take(PEVT_CENTER, &last[PEVT_CENTER], v, NULL, &missed))
    ESP_LOGI(TAG, "SERVO CENTER: sync %+.2f ms, slope %+.1f us/s -> I2S %+ld ppm%s",
             (double)v[0] / 1000.0, (double)v[1] / 10.0, (long)v[2],
             missed ? " (+earlier steps)" : "");
  if (pevt_take(PEVT_TUNE_FAIL, &last[PEVT_TUNE_FAIL], v, NULL, &missed))
    ESP_LOGW(TAG, "I2S clock tune to %+ld ppm failed: %s (failures=%ld); "
                  "drift is NOT being corrected",
             (long)v[0], esp_err_to_name((esp_err_t)v[1]), (long)v[2]);
  if (pevt_take(PEVT_FLUSH_FAIL, &last[PEVT_FLUSH_FAIL], v, &str, &missed))
    ESP_LOGE(TAG, "PLAYOUT FLUSH failed (%s): %s (x%lu)", str ? str : "unknown",
             esp_err_to_name((esp_err_t)v[0]), (unsigned long)(missed + 1U));
  if (pevt_take(PEVT_SERVO_RESET_FAIL, &last[PEVT_SERVO_RESET_FAIL], v, NULL, &missed))
    ESP_LOGW(TAG, "PLAYOUT SERVO RESET failed: %s (x%lu)",
             esp_err_to_name((esp_err_t)v[0]), (unsigned long)(missed + 1U));
}

static void audio_status_task(void *arg) {
  (void)arg;
  bool stack_headroom_warned = false;
  while (s.engine_running) {
    status_print_playout_events();
    timing_snapshot_t snap;
    snapshot_state(&snap);
    const bool active = s.engine_running && snap.playing &&
        (snap.stream_type == AUDIO_STREAM_BUFFERED ||
         snap.stream_type == AUDIO_STREAM_REALTIME);
    if (xSemaphoreTake(s.status_wake, active ? pdMS_TO_TICKS(AP2_STATUS_PERIOD_MS)
                                            : portMAX_DELAY) == pdTRUE) {
      if (!s.engine_running) break;
      continue;
    }
    snapshot_state(&snap);
    if (!s.engine_running || !snap.playing ||
        (snap.stream_type != AUDIO_STREAM_BUFFERED &&
         snap.stream_type != AUDIO_STREAM_REALTIME)) continue;
    const audio_stream_type_t task_stream = snap.stream_type;
    buffered_timing_watchdog(&snap);

    const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
    uint32_t wanted = 0;
    const bool wanted_valid = wanted_rtp_now(&snap, &wanted);
    uint32_t pcm_frames = 0;
    if (wanted_valid && s.pcm_ring) {
      pcm_frames = pcm_rtp_ring_contiguous_frames(
          s.pcm_ring, wanted, snap.pcm_generation, PCM_RTP_RING_FRAMES);
    }

    const bool sync_valid =
        __atomic_load_n(&s.status_sync_valid, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&s.status_sync_generation, __ATOMIC_RELAXED) ==
            snap.generation;
    const bool phase_valid = sync_valid &&
        __atomic_load_n(&s.status_phase_valid, __ATOMIC_RELAXED);
    const int32_t sync_us =
        __atomic_load_n(&s.status_sync_us, __ATOMIC_RELAXED);
    const int32_t phase_us =
        __atomic_load_n(&s.status_phase_us, __ATOMIC_RELAXED);
    const int32_t correction_ppm =
        __atomic_load_n(&s.status_servo_ppm, __ATOMIC_RELAXED);

    ptp_clock_snapshot_t ps = {0};
    ptp_clock_get_snapshot(&ps);
    const uint32_t gm_short = (uint32_t)(ps.grandmaster_clock_id & 0xffffffffU);
    const double ptp_delta_ms = (double)ps.raw_filter_delta_ns / 1000000.0;

    if (task_stream == AUDIO_STREAM_BUFFERED) {
      ap2_buffered_fifo_usage_t usage = {0};
      ap2_buffered_fifo_get_usage(s.transport, &usage);

      char control_suffix[80] = "";
      if (usage.immediate_flush_active) {
        snprintf(control_suffix, sizeof(control_suffix),
                 " | flush=%lu", (unsigned long)usage.immediate_target_seq);
      } else if (usage.deferred_requests > 0) {
        snprintf(control_suffix, sizeof(control_suffix),
                 " | defer=%lu", (unsigned long)usage.deferred_requests);
      }

      if (sync_valid && ps.valid) {
        ESP_LOGI(STATUS_TAG,
                 "AAC | sync=%+.2fms | i2s=%+ldppm | ptpD=%+.2fms | "
                 "gm=%08lx%s | fifo=%u/%uKiB | pcm=%.0fms",
                 (double)sync_us / 1000.0, (long)correction_ppm,
                 ptp_delta_ms, (unsigned long)gm_short, control_suffix,
                 (unsigned)(usage.used_bytes / 1024U),
                 (unsigned)(usage.capacity_bytes / 1024U),
                 status_frames_to_ms(pcm_frames, sr));
      } else {
        ESP_LOGI(STATUS_TAG,
                 "AAC | sync=%s | i2s=%+ldppm | ptpD=%s | "
                 "gm=%08lx%s | fifo=%u/%uKiB | pcm=%.0fms",
                 sync_valid ? "valid" : "n/a", (long)correction_ppm,
                 ps.valid ? "valid" : "n/a", (unsigned long)gm_short,
                 control_suffix, (unsigned)(usage.used_bytes / 1024U),
                 (unsigned)(usage.capacity_bytes / 1024U),
                 status_frames_to_ms(pcm_frames, sr));
      }
    } else if (task_stream == AUDIO_STREAM_REALTIME) {
      realtime_receiver_usage_t usage = {0};
      realtime_receiver_get_usage(&usage);

      bool raw_cursor_valid = false;
      uint32_t raw_cursor = 0;
      taskENTER_CRITICAL(&s.state_mux);
      raw_cursor_valid = s.realtime_stage_cursor_valid &&
                         s.realtime_stage_generation == snap.generation;
      raw_cursor = s.realtime_stage_cursor_rtp;
      taskEXIT_CRITICAL(&s.state_mux);

      uint32_t raw_frames = 0;
      if (raw_cursor_valid && s.realtime_stage_ring) {
        raw_frames = pcm_rtp_ring_contiguous_frames(
            s.realtime_stage_ring, raw_cursor, snap.generation,
            PCM_RTP_RING_FRAMES);
      }
      if (sync_valid && phase_valid && ps.valid) {
        ESP_LOGI(STATUS_TAG,
                 "ALAC | sync=%+.2fms | phase=%+.2fms | i2s=%+ldppm | "
                 "ptpD=%+.2fms | gm=%08lx | epoch=%lu | frameq=%lu/%lu | "
                 "raw=%.0fms | pcm=%.0fms",
                 (double)sync_us / 1000.0, (double)phase_us / 1000.0,
                 (long)correction_ppm, ptp_delta_ms,
                 (unsigned long)gm_short, (unsigned long)ps.epoch,
                 (unsigned long)usage.work_queue_depth,
                 (unsigned long)usage.work_queue_capacity,
                 status_frames_to_ms(raw_frames, sr),
                 status_frames_to_ms(pcm_frames, sr));
      } else {
        ESP_LOGI(STATUS_TAG,
                 "ALAC | sync=%s | phase=%s | i2s=%+ldppm | ptpD=%s | "
                 "gm=%08lx | epoch=%lu | frameq=%lu/%lu | raw=%.0fms | pcm=%.0fms",
                 sync_valid ? "valid" : "n/a",
                 phase_valid ? "valid" : "n/a",
                 (long)correction_ppm, ps.valid ? "valid" : "n/a",
                 (unsigned long)gm_short, (unsigned long)ps.epoch,
                 (unsigned long)usage.work_queue_depth,
                 (unsigned long)usage.work_queue_capacity,
                 status_frames_to_ms(raw_frames, sr),
                 status_frames_to_ms(pcm_frames, sr));
      }
    }

    /* Keep this diagnostic cheap and rate-limited. On ESP-IDF the task stack
     * watermark is reported in bytes. Warn once while headroom is below 1 KiB
     * and re-arm only after there is comfortable margin again. */
    const UBaseType_t stack_headroom = uxTaskGetStackHighWaterMark(NULL);
    if (stack_headroom < 1024U) {
      if (!stack_headroom_warned) {
        ESP_LOGW(STATUS_TAG, "status task stack headroom low: %u bytes",
                 (unsigned)stack_headroom);
        stack_headroom_warned = true;
      }
    } else if (stack_headroom >= 1536U) {
      stack_headroom_warned = false;
    }
  }

  s.status_task = NULL;
  vTaskDelete(NULL);
}

static void audio_status_notify(void) {
  if (s.status_wake) xSemaphoreGive(s.status_wake);
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
  (void)snap;
  return done->done_local_us * 1000LL;
}

/* Pace DMA submission in ESP monotonic presentation time. */
static void wait_until_presentation_ns(const timing_snapshot_t *snap,
                                       uint64_t target_ns) {
  while (s.engine_running) {
    timing_snapshot_t current;
    snapshot_state(&current);
    if (!current.playing || !current.anchor_valid || current.timeline_reset_pending ||
        current.generation != snap->generation ||
        current.media_revision != snap->media_revision ||
        current.anchor_clock_id != snap->anchor_clock_id ||
        current.anchor_ptp_ns != snap->anchor_ptp_ns ||
        current.anchor_rtp != snap->anchor_rtp ||
        __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE)) break;
    const uint64_t now = presentation_now_ns(snap);
    if (now >= target_ns) break;
    const uint64_t remain_us = (target_ns - now) / 1000ULL;
    if (remain_us > 250U) {
      /* Wake slightly before the edge. Timer/control tokens are hints only;
       * re-read time and revision after every wake, including a stale token. */
      (void)esp_timer_stop(s.playout_timer);
      if (esp_timer_start_once(s.playout_timer, remain_us - 200U) == ESP_OK) {
        (void)xSemaphoreTake(s.playout_wake, pdMS_TO_TICKS(50));
        (void)esp_timer_stop(s.playout_timer);
      } else if (remain_us > (uint64_t)portTICK_PERIOD_MS * 1000U + 250U) {
        vTaskDelay(1);
      } else {
        esp_rom_delay_us(100);
      }
    } else {
      esp_rom_delay_us(remain_us > 40U ? 20U : 2U);
    }
  }
  (void)esp_timer_stop(s.playout_timer);
}

/* Buffered AirPlay 2 packet sequence arithmetic is used only for FLUSH and
 * decoder-continuity checks. Compressed media itself is consumed strictly
 * forward from the raw TCP byte FIFO. */
static inline int32_t seq23_delta(uint32_t a, uint32_t b) {
  uint32_t d = (a - b) & 0x007fffffU;
  if (d & 0x00400000U) d |= 0xff800000U;
  return (int32_t)d;
}


static void pcm_process_common_eq(int16_t *pcm, size_t frames, int channels,
                                  int sample_rate) {
  /* This is the codec boundary: AAC and ALAC are fully independent up to
   * decoded PCM. From here both formats use the same EQ implementation. */
  audio_eq_process(pcm, frames, channels, sample_rate);
}

/* The buffered AAC side now follows Shairport Sync's topology: the TCP task
 * owns only a raw byte FIFO, while this single consumer owns packet framing,
 * FLUSH decisions, timing, decrypt/decode and publication into the existing
 * RTP-addressed PCM ring. There is no compressed-media search or rebind. */
typedef enum {
  PCM_STORE_DROPPED = 0,
  PCM_STORE_PUBLISHED,
} pcm_store_result_t;

static pcm_store_result_t pcm_store_with_backpressure(
    uint32_t rtp, const int16_t *pcm, size_t frames, int channels,
    uint32_t pcm_generation) {
  if (!pcm || channels != 2 || frames == 0 || frames > PCM_RTP_SLOT_FRAMES)
    return PCM_STORE_DROPPED;

  while (s.rx_running) {
    xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
    timing_snapshot_t snap;
    snapshot_state(&snap);
    if (!s.rx_running || snap.stream_type != AUDIO_STREAM_BUFFERED ||
        snap.pcm_generation != pcm_generation || snap.timeline_reset_pending) {
      xSemaphoreGive(s.publish_mutex);
      return PCM_STORE_DROPPED;
    }
    if (!snap.playing || !snap.anchor_valid || !timing_clock_ready(&snap)) {
      xSemaphoreGive(s.publish_mutex);
      ap2_buffered_fifo_wait(s.transport, 10U);
      continue;
    }

    uint32_t wanted = 0;
    const bool wanted_valid = wanted_rtp_now(&snap, &wanted);
    const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
    const int32_t max_lead =
        (int32_t)(((int64_t)sr * AP2_BUFFERED_LEAD_MS) / 1000LL);
    if (!wanted_valid || rtp_delta(rtp, wanted) > max_lead) {
      xSemaphoreGive(s.publish_mutex);
      ap2_buffered_fifo_wait(s.transport, 10U);
      continue;
    }
    if (rtp_delta(rtp + (uint32_t)frames, wanted) <= 0) {
      xSemaphoreGive(s.publish_mutex);
      return PCM_STORE_DROPPED;
    }

    const bool stored = pcm_rtp_ring_write(s.pcm_ring, rtp, pcm, frames,
                                           channels, pcm_generation, wanted,
                                           wanted_valid);
    xSemaphoreGive(s.publish_mutex);
    if (stored) {
      playout_wake();
      return PCM_STORE_PUBLISHED;
    }
    vTaskDelay(1);
  }
  return PCM_STORE_DROPPED;
}

static void apply_deferred_flush_activations(
    const ap2_buffered_packet_decision_t *decision,
    uint32_t pcm_generation) {
  if (!decision || decision->activation_count == 0 || !s.pcm_ring) return;
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  timing_snapshot_t snap;
  snapshot_state(&snap);
  if (snap.stream_type == AUDIO_STREAM_BUFFERED &&
      snap.pcm_generation == pcm_generation) {
    for (uint8_t i = 0; i < decision->activation_count; ++i) {
      const ap2_buffered_flush_activation_t *a = &decision->activations[i];
      pcm_rtp_ring_invalidate_range(s.pcm_ring, a->from_rtp, a->until_rtp,
                                    pcm_generation);
    }
  }
  xSemaphoreGive(s.publish_mutex);
  playout_wake();
}

static void reset_aac_decode_history(aac_decoder_t **decoder,
                                     uint32_t *decoder_format_generation,
                                     bool *have_sequence,
                                     uint32_t *expected_timestamp,
                                     uint32_t *expected_seq) {
  if (*decoder && !aac_decoder_reset(*decoder)) {
    aac_decoder_destroy(*decoder);
    *decoder = NULL;
    *decoder_format_generation = 0;
  }
  audio_eq_reset_state();
  *have_sequence = false;
  *expected_timestamp = 0;
  *expected_seq = 0;
}

/* Pure centring decision (host-tested). Returns the frequency step in ppm to
 * add to the current I2S correction, or 0. `slope_us_s` is the measured sync
 * slope over the last window while the clock was NOT being retuned. */
static int32_t servo_center_step(int32_t sync_us, double slope_us_s) {
  double wanted_slope = 0.0;
  if (sync_us > AP2_CENTER_QUIET_US || sync_us < -AP2_CENTER_QUIET_US) {
    wanted_slope = -(double)sync_us / AP2_CENTER_TIME_S;
    if (wanted_slope > AP2_CENTER_MAX_SLOPE_US_S)
      wanted_slope = AP2_CENTER_MAX_SLOPE_US_S;
    if (wanted_slope < -AP2_CENTER_MAX_SLOPE_US_S)
      wanted_slope = -AP2_CENTER_MAX_SLOPE_US_S;
  }
  double step = AP2_CENTER_GAIN * (wanted_slope - slope_us_s); /* +1 ppm -> +1 us/s */
  if (step > AP2_CENTER_MAX_STEP_PPM) step = AP2_CENTER_MAX_STEP_PPM;
  if (step < -AP2_CENTER_MAX_STEP_PPM) step = -AP2_CENTER_MAX_STEP_PPM;
  const int32_t out = (int32_t)(step >= 0.0 ? step + 0.5 : step - 0.5);
  /* Below the physical retune threshold a step would never be applied. */
  if (out < AP2_PID_MIN_TUNE_PPM && out > -AP2_PID_MIN_TUNE_PPM) return 0;
  return out;
}

/* Linear fade-in over the first AP2_FADE_IN_FRAMES of a block (~5.8 ms at
 * 44.1 kHz). Used on the first audible block after a muted restart block. */
#define AP2_FADE_IN_FRAMES 256U
static void fade_in_pcm_block(int16_t *pcm, size_t frames, int channels) {
  if (!pcm || frames == 0U || channels <= 0) return;
  const size_t n = frames < AP2_FADE_IN_FRAMES ? frames : AP2_FADE_IN_FRAMES;
  for (size_t f = 0; f < n; ++f) {
    const int32_t g = (int32_t)(((f + 1U) * 32768U) / (n + 1U)); /* Q15 */
    for (int ch = 0; ch < channels; ++ch) {
      int16_t *x = &pcm[f * (size_t)channels + (size_t)ch];
      *x = (int16_t)(((int32_t)*x * g) >> 15);
    }
  }
}

static void ap2_buffered_processor_task(void *arg) {
  (void)arg;
  aac_decoder_t *decoder = NULL;
  uint32_t decoder_format_generation = 0;
  uint32_t expected_timestamp = 0;
  uint32_t expected_seq = 0;
  bool have_decoded_sequence = false;
  bool decoder_history_dirty = true;
  bool mute_next_aac_block = true;
  uint32_t decode_burst = 0;
  /* After the muted block, ramp the next one in: jumping from digital zero
   * straight to full-scale signal is itself a click. */
  bool fade_in_next_aac_block = false;
  uint32_t decoder_timeline_generation = 0;
  bool have_packet = false;
  uint8_t consecutive_decrypt_failures = 0;
  ap2_buffered_packet_t packet = {0};

  audio_eq_reset_state();
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_AAC_PROCESSOR,
                                    xPortGetCoreID(), AP2_DECODE_PRIORITY, 0U);

  while (s.rx_running) {
    timing_snapshot_t snap;
    snapshot_state(&snap);
    if (decoder_timeline_generation != snap.generation) {
      /* A committed presentation timeline is a new AAC sequence.  Keep codec
       * history aligned by decoding its first packet normally, but publish
       * silence for that one AAC block just as Shairport-Sync does.  This
       * prevents stale codec/filter history from becoming an audible startup
       * transient without changing the RTP cursor. */
      decoder_timeline_generation = snap.generation;
      decoder_history_dirty = true;
      mute_next_aac_block = true;
    }
    const bool normal_consume_ready =
        snap.stream_type == AUDIO_STREAM_BUFFERED && snap.playing &&
        snap.anchor_valid && !snap.timeline_reset_pending &&
        timing_clock_ready(&snap);

    /* Match Shairport's separation: while normal playback is paused the raw
     * TCP reader keeps filling the FIFO, but the packet consumer does not walk
     * forward unless an immediate FLUSH explicitly requires sequential drain.
     * Avoid even reading the FLUSH mutex on the normal hot path. */
    if (!have_packet) {
      const bool flush_draining = normal_consume_ready
          ? false
          : ap2_buffered_fifo_immediate_flush_active(s.transport);
      if (!normal_consume_ready && !flush_draining) {
        ap2_buffered_fifo_wait(s.transport, 10U);
        continue;
      }

      const esp_err_t read_err = ap2_buffered_fifo_read_packet(
          s.transport, s.buffered_packet, AP2_PACKET_MAX, &packet);
      if (read_err != ESP_OK) {
        if (read_err == ESP_ERR_INVALID_SIZE) {
          /* A length error destroys byte-stream framing. The FIFO layer has
           * already aborted this client so accept() can establish a clean
           * stream; reset codec history as well. */
          decoder_history_dirty = true;
          mute_next_aac_block = true;
          consecutive_decrypt_failures = 0;
        }
        if (s.rx_running) ap2_buffered_fifo_wait(s.transport, 10U);
        continue;
      }
      have_packet = true;
      AUDIO_DIAG_TRANSPORT_AAC_RX_BLOCK((uint32_t)packet.len);
    }

    /* Re-evaluate control state even while this same packet is being held for
     * presentation time. A new FLUSH therefore acts on the current packet
     * directly, exactly as in Shairport's new_audio_block_needed loop. */
    ap2_buffered_packet_decision_t decision = {0};
    ap2_buffered_fifo_classify_packet(s.transport, &packet, &decision);
    if (decision.activation_count) {
      apply_deferred_flush_activations(&decision, snap.pcm_generation);
    }
    if (decision.discontinuity) {
      decoder_history_dirty = true;
      mute_next_aac_block = true;
    }
    if (decision.immediate_completed) {
      ESP_LOGI(TAG, "AAC FLUSH complete target=%" PRIu32 " at seq=%" PRIu32 "%s",
               decision.immediate_target_seq, packet.seq,
               decision.immediate_overshoot ? " overshoot" : "");
    }
    if (decision.drop) {
      have_packet = false;
      continue;
    }

    snapshot_state(&snap);
    if (snap.stream_type != AUDIO_STREAM_BUFFERED || !snap.playing ||
        !snap.anchor_valid || snap.timeline_reset_pending ||
        !timing_clock_ready(&snap)) {
      ap2_buffered_fifo_wait(s.transport, 10U);
      continue;
    }

    uint32_t wanted = 0;
    if (!wanted_rtp_now(&snap, &wanted)) {
      ap2_buffered_fifo_wait(s.transport, 10U);
      continue;
    }
    const uint32_t frame_samples = snap.format.frame_size > 0
                                       ? (uint32_t)snap.format.frame_size
                                       : 1024U;
    const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
    const int32_t max_lead =
        (int32_t)(((int64_t)sr * AP2_BUFFERED_LEAD_MS) / 1000LL);
    const int32_t lead = rtp_delta(packet.rtp, wanted);

    if ((int64_t)lead + (int64_t)frame_samples <= 0) {
      /* Sequential stream, but this block is already entirely behind the
       * presentation point. Consume it and rebuild decoder history at the next
       * usable AAC block; never search ahead. */
      decoder_history_dirty = true;
      mute_next_aac_block = true;
      have_packet = false;
      continue;
    }
    if (lead > max_lead) {
      /* Shairport holds one compressed packet outside its raw FIFO until it is
       * close enough to the decoded-buffer target. TCP ingress keeps filling
       * independently and naturally backpressures when the FIFO is full. */
      ap2_buffered_fifo_wait(s.transport, 10U);
      continue;
    }

    if (decoder_history_dirty) {
      reset_aac_decode_history(&decoder, &decoder_format_generation,
                               &have_decoded_sequence, &expected_timestamp,
                               &expected_seq);
      decoder_history_dirty = false;
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
        vTaskDelay(1);
        continue;
      }
      decoder_format_generation = snap.format_generation;
      have_decoded_sequence = false;
      expected_timestamp = 0;
      expected_seq = 0;
      mute_next_aac_block = true;
      audio_eq_reset_state();
      AUDIO_DIAG_CODEC_AAC_READY((uint32_t)snap.format.sample_rate,
                                 (uint32_t)snap.format.channels);
    }

    if (have_decoded_sequence) {
      const int32_t timestamp_gap = rtp_delta(packet.rtp, expected_timestamp);
      const int32_t sequence_gap = seq23_delta(packet.seq, expected_seq);
      if (timestamp_gap != 0 || sequence_gap != 0) {
        reset_aac_decode_history(&decoder, &decoder_format_generation,
                                 &have_decoded_sequence, &expected_timestamp,
                                 &expected_seq);
        mute_next_aac_block = true;
        if (!decoder) {
          have_packet = false;
          continue;
        }
      }
    }

    const int dec_len = audio_crypto_decrypt_buffered(
        &s.encrypt, s.buffered_packet, packet.len,
        s.decrypt_buf + AAC_DECODER_INPUT_HEADROOM, AP2_PACKET_MAX);
    if (dec_len <= 0) {
      decoder_history_dirty = true;
      mute_next_aac_block = true;
      have_packet = false;
      if (++consecutive_decrypt_failures >= 3U) {
        ESP_LOGW(TAG,
                 "AAC decrypt/auth failed 3 consecutive blocks; aborting buffered TCP client");
        ap2_buffered_fifo_abort_client(s.transport);
        consecutive_decrypt_failures = 0;
      }
      continue;
    }
    consecutive_decrypt_failures = 0;

    aac_decode_info_t info = {0};
    const int frames = aac_decoder_decode(
        decoder, s.decrypt_buf + AAC_DECODER_INPUT_HEADROOM,
        (size_t)dec_len, s.decode_pcm, AP2_PCM_CAPACITY_FRAMES, &info);
    if (frames <= 0) {
      decoder_history_dirty = true;
      mute_next_aac_block = true;
      have_packet = false;
      continue;
    }

    expected_timestamp = packet.rtp + (uint32_t)frames;
    expected_seq = (packet.seq + 1U) & 0x007fffffU;
    have_decoded_sequence = true;

    pcm_process_common_eq(s.decode_pcm, (size_t)frames, info.channels,
                          snap.format.sample_rate);

    if (mute_next_aac_block) {
      /* Preserve decoder and EQ history, but never expose the first AAC block
       * of a new/discontinuous sequence to the PCM ring.  1024 samples at
       * 44.1 kHz is ~23 ms and the RTP duration is deliberately preserved. */
      memset(s.decode_pcm, 0,
             (size_t)frames * (size_t)info.channels * sizeof(int16_t));
      mute_next_aac_block = false;
      fade_in_next_aac_block = true;
    } else if (fade_in_next_aac_block) {
      fade_in_pcm_block(s.decode_pcm, (size_t)frames, info.channels);
      fade_in_next_aac_block = false;
    }

    const pcm_store_result_t stored = pcm_store_with_backpressure(
        packet.rtp, s.decode_pcm, (size_t)frames, info.channels,
        snap.pcm_generation);
    if (stored != PCM_STORE_PUBLISHED) {
      decoder_history_dirty = true;
      mute_next_aac_block = true;
    }
    have_packet = false;
    /* v4.1.19: after a new anchor this task (core 0, prio 6) refills the whole
     * ~1.1 s PCM lead back-to-back and starved the RTSP task (core 0, prio 5)
     * for 0.3-0.4 s on every track change. Give up the CPU for one tick every
     * few blocks; in steady state one block is decoded per ~23 ms anyway. */
    if (++decode_burst >= AP2_DECODE_YIELD_EVERY) {
      decode_burst = 0;
      vTaskDelay(1);
    }
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

static bool realtime_pcm_sink_locked(uint32_t rtp, int16_t *pcm, size_t frames,
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
    return false;
  }

  if (snap.anchor_valid && frame_is_fully_stale(rtp, &snap, NULL)) {
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
    return true;
  }

  uint32_t wanted = 0;
  bool wanted_valid = wanted_rtp_now(&snap, &wanted);
  if (!s.realtime_stage_ring ||
      !pcm_rtp_ring_write(s.realtime_stage_ring, rtp, pcm, frames, channels,
                          snap.generation, wanted, wanted_valid)) {
    return false;
  }

  taskENTER_CRITICAL(&s.state_mux);
  if (s.generation != snap.generation || s.timeline_reset_pending ||
      s.stream_type != AUDIO_STREAM_REALTIME) {
    taskEXIT_CRITICAL(&s.state_mux);
    return false;
  }
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
  return true;
}

static bool realtime_pcm_sink(uint32_t rtp, int16_t *pcm, size_t frames,
                              int channels, void *ctx) {
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  bool ok = realtime_pcm_sink_locked(rtp, pcm, frames, channels, ctx);
  xSemaphoreGive(s.publish_mutex);
  return ok;
}


static void realtime_stage_task(void *arg) {
  (void)arg;
  int16_t *pcm = s.realtime_stage_pcm;

  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_ALAC_STAGE,
                                    xPortGetCoreID(), AP2_RT_STAGE_PRIORITY, 0U);

  uint32_t local_generation = 0;
  __atomic_store_n(&s.realtime_stage_idle, true, __ATOMIC_RELEASE);

  while (s.engine_running) {
    /* Stop acknowledgement and permission to enter EQ are one state change.
     * Control cannot observe old idle=true after this iteration was admitted. */
    taskENTER_CRITICAL(&s.state_mux);
    const bool stage_running =
        __atomic_load_n(&s.realtime_stage_running, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s.realtime_stage_idle, !stage_running, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL(&s.state_mux);
    if (!stage_running) {
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

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
    bool have = pcm_rtp_ring_read(s.realtime_stage_ring, cursor, frames,
                                  snap.generation, pcm);
    if (!have) {
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
    }

    if (local_generation != snap.generation) {
      audio_eq_reset_state();
      local_generation = snap.generation;
    }

    pcm_process_common_eq(pcm, frames, 2, snap.format.sample_rate);

    bool published = false;
    while (s.engine_running) {
      xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
      taskENTER_CRITICAL(&s.state_mux);
      const bool current =
          __atomic_load_n(&s.realtime_stage_running, __ATOMIC_ACQUIRE) &&
          s.stream_type == AUDIO_STREAM_REALTIME &&
          s.generation == snap.generation && !s.timeline_reset_pending &&
          s.realtime_stage_generation == snap.generation &&
          s.realtime_stage_cursor_valid && s.realtime_stage_cursor_rtp == cursor;
      taskEXIT_CRITICAL(&s.state_mux);
      if (!current) {
        xSemaphoreGive(s.publish_mutex);
        break;
      }
      published = pcm_rtp_ring_write(s.pcm_ring, cursor, pcm, frames, 2,
                                      snap.generation, 0U, false);
      if (published) {
        taskENTER_CRITICAL(&s.state_mux);
        if (s.realtime_stage_generation == snap.generation &&
            s.realtime_stage_cursor_valid && s.realtime_stage_cursor_rtp == cursor)
          s.realtime_stage_cursor_rtp = cursor + frames;
        taskEXIT_CRITICAL(&s.state_mux);
      }
      xSemaphoreGive(s.publish_mutex);
      if (published) break;
      /* Retry the processed samples. Do not run a stateful EQ twice. */
      vTaskDelay(1);
    }
    if (!published) {
      /* The processed block was canceled by FLUSH/stop/generation change.
       * Its EQ history cannot become the next chronology's initial state. */
      local_generation = 0;
      continue;
    }

  }

  __atomic_store_n(&s.realtime_stage_idle, true, __ATOMIC_RELEASE);
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
    if (!completion_sync_us(snap, &done, &sync_us)) continue;
    if (!s.output_sync.valid ||
        s.output_sync.generation != done.generation) {
      s.output_sync.us = sync_us;
      s.output_sync.generation = done.generation;
      s.output_sync.valid = true;
      s.output_sync.phase_count = 0;
      s.output_sync.phase_index = 0;
    } else {
      /* Keep the existing EMA exactly as the PID input. */
      s.output_sync.us += (sync_us - s.output_sync.us) / 8;
    }

    /* Passive robust phase history only. No controller reads these raw samples. */
    s.output_sync.phase_samples[s.output_sync.phase_index] = sync_us;
    s.output_sync.phase_index =
        (uint8_t)((s.output_sync.phase_index + 1U) % AP2_PHASE_HISTORY_SAMPLES);
    if (s.output_sync.phase_count < AP2_PHASE_HISTORY_SAMPLES)
      s.output_sync.phase_count++;
  }
}



/* Volume scaling is the LAST requantisation to 16 bit, so it is where TPDF
 * dither belongs. The Q15 product carries 15 fractional bits; adding +-1 LSB
 * triangular noise before rounding decorrelates the rounding error from the
 * signal (no harmonic distortion on fades / quiet passages). Unity gain is
 * bit-perfect and is never dithered; exact-zero input samples stay exactly
 * zero (digital silence stays silent). Only the playout task calls this, so
 * the PRNG state needs no locking. */
static uint32_t s_vol_dither_state[2] = {0x9E3779B9U, 0x85EBCA6BU};

static inline uint32_t vol_dither_rng(uint32_t *st) {
  uint32_t x = *st;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *st = x;
  return x;
}

/* Difference of two independent uniform values in [0, 32767]: TPDF noise
 * spanning (-1 LSB, +1 LSB) expressed in Q15 units. */
static inline int32_t vol_dither_tpdf_q15(uint32_t *st) {
  const int32_t a = (int32_t)(vol_dither_rng(st) >> 17);
  const int32_t b = (int32_t)(vol_dither_rng(st) >> 17);
  return a - b;
}

static void apply_output_volume(int16_t *pcm, uint32_t frames,
                                int32_t *current_q15) {
  if (!pcm || !current_q15 || frames == 0U) return;
  int32_t target = __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
  if (target < 0) target = 0;
  if (target > 32768) target = 32768;
  const int32_t start = *current_q15;
  if (start == target) {
    if (target == 32768) return;
    if (target == 0) {
      memset(pcm, 0, (size_t)frames * 2U * sizeof(*pcm));
      return;
    }
    for (uint32_t i = 0; i < frames * 2U; ++i) {
      const int32_t x = pcm[i];
      if (x == 0) continue;
      int32_t y = (x * target + vol_dither_tpdf_q15(&s_vol_dither_state[i & 1U]) +
                   16384) >> 15;
      if (y > INT16_MAX) y = INT16_MAX;
      else if (y < INT16_MIN) y = INT16_MIN;
      pcm[i] = (int16_t)y;
    }
    return;
  }
  const int64_t dg = (int64_t)target - (int64_t)start;
  for (uint32_t f = 0; f < frames; ++f) {
    const int32_t gain =
        start + (int32_t)((dg * (int64_t)(f + 1U)) / (int64_t)frames);
    for (uint32_t ch = 0; ch < 2U; ++ch) {
      const uint32_t i = f * 2U + ch;
      if (pcm[i] == 0) continue;
      int64_t y = (int64_t)pcm[i] * (int64_t)gain +
                  vol_dither_tpdf_q15(&s_vol_dither_state[ch]);
      y = (y + 16384) >> 15;
      if (y > INT16_MAX) y = INT16_MAX;
      else if (y < INT16_MIN) y = INT16_MIN;
      pcm[i] = (int16_t)y;
    }
  }
  *current_q15 = target;
}


/* I2S lifecycle is single-owner on the playout task.  A failed flush must
 * never be treated as a successful STOPPED boundary: the driver may still be
 * RUNNING or may still contain preloaded DMA.  Keep the control request
 * pending so the next playout iteration retries before any new PRIME work is
 * allowed to touch the channel. */
static bool playout_flush_checked(const char *reason) {
  const esp_err_t err = audio_playout_flush();
  if (err == ESP_OK) {
    return true;
  }
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  media_control_wake();
  pevt_post(PEVT_FLUSH_FAIL, (int32_t)err, 0, 0, 0, 0,
            reason ? reason : "unknown");
  return false;
}

/* Compare media maps only when the sender publishes a new buffered anchor.
 * Same-GM remote times exclude local PTP filter movement from this decision.
 * Across GMs compare the qualified local maps. At most one DMA block of phase
 * adjustment stays with the servo; a larger cursor move needs fresh PRIME. */
static bool buffered_anchor_moved(const timing_snapshot_t *previous,
                                   const timing_snapshot_t *current) {
  const bool same_clock = previous->anchor_clock_id == current->anchor_clock_id;
  const uint64_t before = same_clock ? previous->anchor_ptp_ns
                                     : previous->anchor_local_ns;
  const uint64_t after = same_clock ? current->anchor_ptp_ns
                                    : current->anchor_local_ns;
  const int64_t time_delta_us = after >= before
      ? (int64_t)((after - before) / 1000ULL)
      : -(int64_t)((before - after) / 1000ULL);
  const int sr = current->format.sample_rate > 0 ? current->format.sample_rate : 44100;
  const int64_t media_delta_us =
      ((int64_t)rtp_delta(current->anchor_rtp, previous->anchor_rtp) * 1000000LL) / sr;
  const int64_t shift_us = time_delta_us - media_delta_us;
  const int64_t block_us = ((int64_t)AUDIO_PLAYOUT_FRAMES * 1000000LL) / sr;
  return shift_us > block_us || shift_us < -block_us;
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
  timing_snapshot_t cursor_timing = {0};
  int32_t volume_current_q15 = __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
  playout_state_t state = PLAYOUT_STOPPED;
  int32_t servo_ppm = 0;
  int32_t servo_target_ppm = 0;
  int64_t pid_last_calc_us = 0;
  int64_t pid_last_tune_us = 0;
  uint32_t tune_fail_count = 0;
  int64_t resync_over_since_us = 0;
  int64_t resync_last_us = 0;
  uint32_t resync_count = 0;
  uint32_t start_logged_gen = 0;
  bool center_active = false;
  int32_t center_prev_sync_us = 0;
  bool center_prev_valid = false;
  int32_t center_jump_us = 0; /* sum of reference steps inside this window */
  int64_t center_out_since_us = 0;
  int64_t center_start_us = 0;
  /* Least-squares line through the 1 Hz sync samples of the window:
   * robust slope and a de-noised end-of-window sync estimate. */
  double center_n = 0.0, center_st = 0.0, center_sy = 0.0;
  double center_stt = 0.0, center_sty = 0.0;
  uint32_t servo_generation = 0;
  double pid_integral_ms_s = 0.0;
  double pid_prev_error_ms = 0.0;
  double pid_d_filtered_ms_s = 0.0;
  bool pid_prev_valid = false;

  while (s.engine_running) {
    if (__atomic_exchange_n(&s.playout_servo_reset_requested, false,
                            __ATOMIC_ACQ_REL)) {
      /* A codec/session boundary is stronger than a normal timeline change.
       * Do not inherit the previous stream's learned crystal correction.
       * Ordinary buffered cursor/anchor changes deliberately keep the learned
       * physical clock correction; presentation changes are not new hardware
       * clock sessions. */
      if (!playout_flush_checked("servo-reset")) {
        __atomic_store_n(&s.playout_servo_reset_requested, true,
                         __ATOMIC_RELEASE);
        vTaskDelay(1);
        continue;
      }
      esp_err_t re = audio_playout_reset_tune();

      if (re != ESP_OK) {
        __atomic_store_n(&s.playout_servo_reset_requested, true,
                         __ATOMIC_RELEASE);
        /* A hard boundary is not complete until the physical clock is back
         * at nominal.  Retry instead of ACKing a half-reset session. */
        pevt_post(PEVT_SERVO_RESET_FAIL, (int32_t)re, 0, 0, 0, 0, NULL);
        vTaskDelay(1);
        continue;
      }

      servo_ppm = 0;
      servo_target_ppm = 0;
      servo_generation = 0;
      center_active = false;
      pid_integral_ms_s = 0.0;
      pid_prev_error_ms = 0.0;
      pid_d_filtered_ms_s = 0.0;
      pid_prev_valid = false;
      pid_last_calc_us = esp_timer_get_time();
      pid_last_tune_us = pid_last_calc_us;

      s.output_sync.valid = false;
      status_invalidate_sync();
      state = PLAYOUT_STOPPED;
    }

    if (__atomic_exchange_n(&s.i2s_flush_requested, false, __ATOMIC_ACQ_REL) ||
        audio_playout_has_fault()) {
      if (!playout_flush_checked("control")) {
        vTaskDelay(1);
        continue;
      }
      s.output_sync.valid = false;
      status_invalidate_sync();
      state = PLAYOUT_STOPPED;
    }

    /* Acknowledge a hard boundary only after both pending control requests
     * have been consumed and the local state machine is stopped. This keeps
     * all I2S driver ownership on this task while giving RTSP a deterministic
     * hand-off point before a new codec stream is started. */
    const uint32_t quiesce_req =
        __atomic_load_n(&s.playout_quiesce_req, __ATOMIC_ACQUIRE);
    if (quiesce_req !=
            __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_RELAXED) &&
        state == PLAYOUT_STOPPED &&
        !audio_playout_is_enabled() &&
        !__atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE)) {
      __atomic_store_n(&s.playout_quiesce_ack, quiesce_req, __ATOMIC_RELEASE);
    }

    timing_snapshot_t snap;
    refresh_timing_snapshot(&snap);
    process_i2s_completions(&snap);

    uint32_t desired_rtp = 0;
    bool timeline_ok = false;
    if (snap.playing && snap.anchor_valid && !snap.timeline_reset_pending &&
        timing_clock_ready(&snap)) {
      timeline_ok = wanted_rtp_now(&snap, &desired_rtp);
    }

    if (!timeline_ok) {
      state = PLAYOUT_STOPPED;
      (void)xSemaphoreTake(s.playout_wake, 1);
      continue;
    }

    if (state == PLAYOUT_RUNNING && snap.stream_type == AUDIO_STREAM_BUFFERED &&
        cursor_generation == snap.generation &&
        cursor_timing.media_revision != snap.media_revision) {
      if (buffered_anchor_moved(&cursor_timing, &snap)) state = PLAYOUT_STOPPED;
      cursor_timing = snap;
    }

    /* v4.1.15 hard resync (Shairport resync_threshold semantics). */
    if (state == PLAYOUT_RUNNING && s.output_sync.valid &&
        s.output_sync.generation == snap.generation) {
      const int32_t err_us = s.output_sync.us;
      const int32_t abs_err_us = err_us < 0 ? -err_us : err_us;
      const int64_t now_us = esp_timer_get_time();
      if (abs_err_us > AP2_RESYNC_THRESHOLD_US) {
        if (resync_over_since_us == 0) {
          resync_over_since_us = now_us;
        } else if (now_us - resync_over_since_us >= AP2_RESYNC_HOLD_US &&
                   (resync_last_us == 0 ||
                    now_us - resync_last_us >= AP2_RESYNC_MIN_INTERVAL_US)) {
          resync_count++;
          pevt_post(PEVT_RESYNC, (int32_t)resync_count, err_us,
                    (int32_t)((now_us - resync_over_since_us) / 1000LL),
                    servo_ppm, 0, NULL);
          resync_last_us = now_us;
          resync_over_since_us = 0;
          state = PLAYOUT_STOPPED; /* re-prime below; servo_ppm is kept */
        }
      } else {
        resync_over_since_us = 0;
      }
    } else if (state != PLAYOUT_RUNNING) {
      resync_over_since_us = 0;
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
      center_active = false; /* new timeline: restart slope measurement */
      center_prev_valid = false;
      s.output_sync.valid = false;
      status_invalidate_sync();
      state = PLAYOUT_PRIMING;
      if (!playout_flush_checked("prime-start")) {
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }
    }

    if (state == PLAYOUT_PRIMING) {
      const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;

      /* Prime against the RTP that will actually become audible. The phase
       * probe starts AP2_START_SILENCE_FUTURE_BLOCKS blocks ahead of desired
       * RTP and consumes two silent DMA blocks before descriptor #3 can carry
       * real PCM, so checking from desired_rtp would spend almost the whole
       * short guard on the alignment procedure itself. Buffered AAC therefore
       * requires a real 250 ms contiguous reserve from the predicted first-real
       * boundary. Realtime ALAC keeps its existing guard/recovery prime because
       * its raw reorder path has different deadline semantics. */
      uint32_t guard_start = desired_rtp;
      uint32_t guard_frames =
          AP2_START_PRIME_GUARD_BLOCKS * AUDIO_PLAYOUT_FRAMES;
      if (snap.stream_type == AUDIO_STREAM_REALTIME) {
        guard_frames += (uint32_t)(((uint64_t)sr * AP2_REALTIME_PRIME_MS) /
                                   1000ULL);
      } else {
        guard_start = desired_rtp +
                      AP2_START_REAL_BOUNDARY_BLOCKS * AUDIO_PLAYOUT_FRAMES;
        guard_frames = (uint32_t)(((uint64_t)sr *
                                   AP2_BUFFERED_START_RESERVE_MS) / 1000ULL);
      }
      if (!pcm_rtp_ring_has_range(s.pcm_ring, guard_start, guard_frames,
                                  snap.pcm_generation)) {
        (void)xSemaphoreTake(s.playout_wake, 1);
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
        vTaskDelay(1);
        continue;
      }

      if (audio_playout_preload_tagged(silence, AUDIO_PLAYOUT_FRAMES,
                                       silence_rtp, 0U) != ESP_OK ||
          audio_playout_preload_tagged(silence, AUDIO_PLAYOUT_FRAMES,
                                       silence_rtp + AUDIO_PLAYOUT_FRAMES,
                                       0U) != ESP_OK) {
        (void)playout_flush_checked("prime-preload-fail");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      wait_until_presentation_ns(&snap, silence_start_time_ns);

      timing_snapshot_t after_wait;
      snapshot_state(&after_wait);
      if (!after_wait.playing || !after_wait.anchor_valid ||
          after_wait.timeline_reset_pending ||
          after_wait.generation != snap.generation ||
          (snap.stream_type == AUDIO_STREAM_BUFFERED &&
           after_wait.media_revision != snap.media_revision) ||
          __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
          !timing_clock_ready(&after_wait) ||
          (snap.stream_type == AUDIO_STREAM_REALTIME &&
           (after_wait.anchor_clock_id != snap.anchor_clock_id ||
            after_wait.anchor_ptp_ns != snap.anchor_ptp_ns ||
            after_wait.anchor_rtp != snap.anchor_rtp))) {
        (void)playout_flush_checked("prime-revalidate-1");
        state = PLAYOUT_STOPPED;
        continue;
      }

      /* This is the only enable for the whole startup epoch. */
      if (audio_playout_enable() != ESP_OK) {
        (void)playout_flush_checked("prime-enable-fail");
        state = PLAYOUT_STOPPED;
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
        if (audio_playout_wait_completion(&probe_done, 1U)) {
          if (probe_done.generation == 0U && probe_done.rtp == silence_rtp) {
            have_probe = true;
            break;
          }
          /* No real-generation completion can exist yet. Ignore any
           * stale completion left over from a prior disabled epoch. */
        } else if (audio_playout_has_fault()) {
          break;
        }
      }
      if (!have_probe) {
        (void)playout_flush_checked("prime-probe-timeout");
        state = PLAYOUT_STOPPED;
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

      /* Round the measured physical I2S boundary to the nearest RTP sample.
       * This keeps startup alignment resolution at one sample (22.68 us at
       * 44.1 kHz) without a diagnostic/manual presentation offset. */
      const uint64_t half_sample_ns = 500000000ULL / (uint64_t)sr;
      uint32_t real_start_rtp = 0;
      if (!wanted_rtp_at_presentation_ns(&snap,
                                           real_boundary_time_ns + half_sample_ns,
                                           &real_start_rtp)) {
        (void)playout_flush_checked("prime-map-real");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      /* Revalidate the timeline after waiting for the probe EOF. */
      timing_snapshot_t align_snap;
      snapshot_state(&align_snap);
      if (!align_snap.playing || !align_snap.anchor_valid ||
          align_snap.timeline_reset_pending ||
          align_snap.generation != snap.generation ||
          (snap.stream_type == AUDIO_STREAM_BUFFERED &&
           align_snap.media_revision != snap.media_revision) ||
          __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
          !timing_clock_ready(&align_snap) ||
          (snap.stream_type == AUDIO_STREAM_REALTIME &&
           (align_snap.anchor_clock_id != snap.anchor_clock_id ||
            align_snap.anchor_ptp_ns != snap.anchor_ptp_ns ||
            align_snap.anchor_rtp != snap.anchor_rtp))) {
        (void)playout_flush_checked("prime-revalidate-2");
        state = PLAYOUT_STOPPED;
        continue;
      }

      /* The predicted boundary above is intentionally checked before I2S is
       * enabled. Re-check from the measured physical boundary now that the
       * silent probe has resolved the exact RTP sample. If the prediction was
       * off by even a small amount, restart silently instead of entering
       * RUNNING with less than the requested buffered reserve. */
      if (snap.stream_type == AUDIO_STREAM_BUFFERED) {
        const uint32_t buffered_reserve_frames =
            (uint32_t)(((uint64_t)sr * AP2_BUFFERED_START_RESERVE_MS) /
                       1000ULL);
        if (!pcm_rtp_ring_has_range(s.pcm_ring, real_start_rtp,
                                    buffered_reserve_frames,
                                    snap.pcm_generation)) {
          (void)playout_flush_checked("prime-buffered-reserve-miss");
          state = PLAYOUT_STOPPED;
          vTaskDelay(1);
          continue;
        }
      }

      bool ok = pcm_rtp_ring_read_256(s.pcm_ring, real_start_rtp,
                                      snap.pcm_generation, block);
      if (!ok) {
        /* Do not allow the already-running silent probe to leak into audible
         * timing indefinitely. A miss restarts the one-enable alignment epoch
         * after more PCM has arrived. */
        (void)playout_flush_checked("prime-pcm-miss");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }
      apply_output_volume(block, AUDIO_PLAYOUT_FRAMES, &volume_current_q15);
      if (audio_playout_write_tagged(block, AUDIO_PLAYOUT_FRAMES,
                                     real_start_rtp,
                                     snap.generation) != ESP_OK) {
        (void)playout_flush_checked("prime-write-fail");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      /* The first real write is blocking. Control/Core0 can publish a pause,
       * FLUSH or a newer buffered anchor while Core1 is inside the driver.
       * Revalidate once more before making this startup public as RUNNING.
       * The real block may have reached DMA, but a cancelled epoch is flushed
       * immediately and never becomes the accepted playout generation. */
      timing_snapshot_t commit_snap;
      snapshot_state(&commit_snap);
      if (!commit_snap.playing || !commit_snap.anchor_valid ||
          commit_snap.timeline_reset_pending ||
          commit_snap.generation != snap.generation ||
          (snap.stream_type == AUDIO_STREAM_BUFFERED &&
           commit_snap.media_revision != snap.media_revision) ||
          __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
          __atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE) ||
          !timing_clock_ready(&commit_snap) ||
          (snap.stream_type == AUDIO_STREAM_REALTIME &&
           (commit_snap.anchor_clock_id != snap.anchor_clock_id ||
            commit_snap.anchor_ptp_ns != snap.anchor_ptp_ns ||
            commit_snap.anchor_rtp != snap.anchor_rtp))) {
        (void)playout_flush_checked("prime-commit-cancel");
        s.output_sync.valid = false;
        state = PLAYOUT_STOPPED;
        continue;
      }

      cursor_rtp = real_start_rtp + AUDIO_PLAYOUT_FRAMES;
      cursor_timing = commit_snap;
      state = PLAYOUT_RUNNING;
      if (snap.stream_type == AUDIO_STREAM_BUFFERED &&
          start_logged_gen != snap.generation &&
          s_anchor_commit_gen == snap.generation && s_anchor_commit_us != 0) {
        start_logged_gen = snap.generation;
        ptp_clock_snapshot_t ps = {0};
        ptp_clock_get_snapshot(&ps);
        pevt_post(PEVT_START, (int32_t)snap.generation,
                  (int32_t)((esp_timer_get_time() - s_anchor_commit_us) / 1000LL),
                  ps.locked ? 1 : 0, (int32_t)ps.mastership_age_ms,
                  (int32_t)ps.sample_count, NULL);
      }
      continue;
    }

    /* In RUNNING, cursor_rtp is the next future block to queue. The two DMA
     * descriptors are paced by their EOF interrupts. No guessed current+1
     * subtraction is used for sync any more; the ISR completion tags are the
     * source of truth. */
    bool have_pcm = pcm_rtp_ring_read_256(
        s.pcm_ring, cursor_rtp, snap.pcm_generation, block);
    if (!have_pcm && snap.stream_type == AUDIO_STREAM_BUFFERED) {
      /* AutoMix/revision recovery can leave a small RTP-addressed validity
       * hole even though the surrounding AAC packets are complete. Keep the
       * normal exact read as the zero-overhead fast path. Only on a real miss,
       * retry with sparse concealment so N missing samples become exactly N
       * silent samples instead of discarding the whole 256-frame block. */
#if defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER
      /* Diagnostic-only work is deliberately inside the category guard. When
       * BUFFER diagnostics are off there is no extra contiguous scan, local
       * state, missing-frame count, or hook in the playout hot path. */
      uint32_t diag_missing_frames = 0U;
      const uint32_t diag_contiguous_before_gap =
          pcm_rtp_ring_contiguous_frames(s.pcm_ring, cursor_rtp,
                                         snap.pcm_generation,
                                         AUDIO_PLAYOUT_FRAMES);
      const bool diag_conceal_ok = pcm_rtp_ring_read_256_conceal(
          s.pcm_ring, cursor_rtp, snap.pcm_generation, block,
          &diag_missing_frames);

      /* A RUNNING-loop snapshot can become stale immediately if Core0
       * publishes PAUSE/FLUSH/new anchor while this task is doing the PCM
       * lookup. Classify a hard miss from fresh state so the diagnostics can
       * distinguish an audible while-playing underrun from a cancelled
       * transition. This work exists only with BUFFER diagnostics enabled. */
      uint32_t diag_transition = 0U;
      if (!diag_conceal_ok) {
        timing_snapshot_t diag_now;
        snapshot_state(&diag_now);
        diag_transition =
            !diag_now.playing || !diag_now.anchor_valid ||
            diag_now.timeline_reset_pending ||
            diag_now.generation != snap.generation ||
            diag_now.media_revision != snap.media_revision ||
            __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&s.playout_quiesce_req, __ATOMIC_ACQUIRE) !=
                __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE);
      }
      AUDIO_DIAG_BUFFER_PLAYOUT_MISS(diag_conceal_ok ? 1U : 0U,
                                     diag_missing_frames,
                                     diag_contiguous_before_gap,
                                     diag_transition);
      if (diag_conceal_ok) have_pcm = true;
#else
      if (pcm_rtp_ring_read_256_conceal(
              s.pcm_ring, cursor_rtp, snap.pcm_generation, block, NULL)) {
        have_pcm = true;
      }
#endif
    }
    if (!have_pcm) {
      memset(block, 0, AUDIO_PLAYOUT_FRAMES * 2U * sizeof(int16_t));
    }
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
     * phase velocity is modest, the PID stops acting and a slow centring loop
     * (v4.1.17, AP2_CENTER_*) steers towards 0 ms with at most one small step
     * per 15 s window, based on a least-squares slope of the measured sync.
     */
    const int64_t pid_now_us = esp_timer_get_time();
    if (write_err == ESP_OK && s.output_sync.valid &&
        s.output_sync.generation == snap.generation &&
        servo_generation == snap.generation &&
        pid_now_us - pid_last_calc_us >= AP2_PID_CALC_PERIOD_US) {
      const double dt_s = (double)(pid_now_us - pid_last_calc_us) / 1000000.0;
      pid_last_calc_us = pid_now_us;

      /* Controller error is opposite to SYNC: negative SYNC (late) must
       * request positive ppm. */
      const double sync_ms = (double)s.output_sync.us / 1000.0;
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

      const int32_t abs_sync_us = s.output_sync.us < 0
          ? -s.output_sync.us : s.output_sync.us;
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
      /* v4.1.18 hysteresis around the centring mode. */
      if (center_active) {
        if (abs_sync_us > AP2_CENTER_EXIT_HARD_US) {
          center_active = false;
        } else if (abs_sync_us > AP2_CENTER_EXIT_US) {
          if (center_out_since_us == 0) {
            center_out_since_us = pid_now_us;
          } else if (pid_now_us - center_out_since_us >= AP2_CENTER_EXIT_HOLD_US) {
            center_active = false;
          }
        } else {
          center_out_since_us = 0;
        }
      }
      const bool center_enter = !center_active && in_deadband &&
          sync_slope_ms_s > -0.080 && sync_slope_ms_s < 0.080;
      if (center_active || center_enter) {
        /* Hold the learned clock, then centre slowly (v4.1.17/18). */
        bool restart_window = false;
        if (center_enter) {
          next_target = servo_ppm; /* entering the zone: drop pending PID step */
          center_active = true;
          center_out_since_us = 0;
          restart_window = true;
          /* PID frequency memory = the clock we are now holding. */
          pid_integral_ms_s = (double)servo_ppm / AP2_PID_KI_PPM_PER_MS_S;
          const double i_lim = AP2_PID_I_TERM_LIMIT_PPM / AP2_PID_KI_PPM_PER_MS_S;
          if (pid_integral_ms_s > i_lim) pid_integral_ms_s = i_lim;
          if (pid_integral_ms_s < -i_lim) pid_integral_ms_s = -i_lim;
        } else {
          next_target = servo_target_ppm;
          if (servo_target_ppm != servo_ppm) {
            /* A step is still waiting for the 5 s retune slot: measure the
             * slope only once the clock really runs at the new rate. */
            restart_window = true;
          } else {
            if (center_prev_valid &&
                (s.output_sync.us - center_prev_sync_us > AP2_CENTER_JUMP_US ||
                 center_prev_sync_us - s.output_sync.us > AP2_CENTER_JUMP_US)) {
              /* A reference step, not drift: keep it out of the slope. */
              center_jump_us += s.output_sync.us - center_prev_sync_us;
            }
            const double t_s = (double)(pid_now_us - center_start_us) / 1000000.0;
            const double y = (double)(s.output_sync.us - center_jump_us);
            center_n += 1.0; center_st += t_s; center_sy += y;
            center_stt += t_s * t_s; center_sty += t_s * y;
          }
          const double den = center_n * center_stt - center_st * center_st;
          if (!restart_window && center_n >= 5.0 && den > 0.0 &&
              pid_now_us - center_start_us >= AP2_CENTER_WINDOW_US) {
            const double slope_us_s = (center_n * center_sty - center_st * center_sy) / den;
            const double icpt = (center_sy - slope_us_s * center_st) / center_n;
            const double t_end = (double)(pid_now_us - center_start_us) / 1000000.0;
            const int32_t sync_fit_us =
                (int32_t)(icpt + slope_us_s * t_end) + center_jump_us;
            const int32_t step = servo_center_step(sync_fit_us, slope_us_s);
            if (step != 0) {
              int32_t t = servo_ppm + step;
              if (t > AP2_PID_MAX_PPM) t = AP2_PID_MAX_PPM;
              if (t < -AP2_PID_MAX_PPM) t = -AP2_PID_MAX_PPM;
              next_target = t;
              /* Keep the PID's frequency memory consistent with the learned
               * clock, so leaving the zone does not jump back. */
              pid_integral_ms_s = (double)t / AP2_PID_KI_PPM_PER_MS_S;
              const double i_limit_state = AP2_PID_I_TERM_LIMIT_PPM /
                                           AP2_PID_KI_PPM_PER_MS_S;
              if (pid_integral_ms_s > i_limit_state) pid_integral_ms_s = i_limit_state;
              if (pid_integral_ms_s < -i_limit_state) pid_integral_ms_s = -i_limit_state;
              pevt_post(PEVT_CENTER, sync_fit_us,
                        (int32_t)(slope_us_s * 10.0), t, 0, 0, NULL);
            }
            restart_window = true;
          }
        }
        if (restart_window) {
          center_start_us = pid_now_us;
          center_n = center_st = center_sy = center_stt = center_sty = 0.0;
          center_jump_us = 0;
        }
        center_prev_sync_us = s.output_sync.us;
        center_prev_valid = true;
      }
      servo_target_ppm = next_target;
      status_publish_sync(s.output_sync.us, robust_phase_center_us(&s.output_sync),
                          servo_ppm, snap.generation);
    }

    if (write_err == ESP_OK && s.output_sync.valid &&
        s.output_sync.generation == snap.generation &&
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
          tune_fail_count = 0;
        } else {
          /* Previously silent: a failing tune means drift is not corrected. */
          tune_fail_count++;
          pevt_post(PEVT_TUNE_FAIL, next_ppm, (int32_t)te,
                    (int32_t)tune_fail_count, 0, 0, NULL);
        }
      }
    }

    if (write_err == ESP_OK) {
      cursor_rtp += AUDIO_PLAYOUT_FRAMES;
    } else {
      /* A failed write breaks the exact tag<->descriptor FIFO relationship.
       * Flush and re-prime rather than pretending the software cursor moved. */
      (void)playout_flush_checked("running-write-fail");
      s.output_sync.valid = false;
      state = PLAYOUT_STOPPED;
      vTaskDelay(1);
    }
  }

  (void)playout_flush_checked("task-exit");
  free(block);
  s.playout_task = NULL;
  vTaskDelete(NULL);
}




esp_err_t audio_receiver_init(void) {
  if (!s.publish_mutex) s.publish_mutex = xSemaphoreCreateMutex();
  if (!s.status_wake) s.status_wake = xSemaphoreCreateBinary();
  if (!s.playout_wake) s.playout_wake = xSemaphoreCreateBinary();
  if (!s.publish_mutex || !s.status_wake || !s.playout_wake) return ESP_ERR_NO_MEM;
  if (!s.playout_timer) {
    const esp_timer_create_args_t timer_args = {
        .callback = playout_timer_callback,
        .name = "audio_start",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s.playout_timer), TAG,
                        "playout timer create failed");
  }

  AUDIO_DIAG_BUFFER_PSRAM(
      AUDIO_DIAG_PSRAM_BEFORE_AUDIO,
      (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U),
      (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
      (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U),
      0U);

  /* Allocate the one large codec backing store first while PSRAM is least
   * fragmented. Buffered AAC uses the full configured store as its contiguous
   * circular byte buffer.
   * Realtime ALAC reuses the beginning of the same bytes for its raw PCM ring
   * and DATA/RTX pools, but only after the buffered transport is fully idle. */
  if (!s.codec_workspace) {
    s.codec_workspace_size = AP2_BUFFERED_STORE_REQUEST_BYTES;
    s.codec_workspace = heap_caps_malloc(
        s.codec_workspace_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.codec_workspace) s.codec_workspace = malloc(s.codec_workspace_size);
    if (!s.codec_workspace) return ESP_ERR_NO_MEM;
  }

  if (!s.buffered_packet) {
    s.buffered_packet = heap_caps_malloc(AP2_PACKET_MAX,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.buffered_packet) s.buffered_packet = malloc(AP2_PACKET_MAX);
  }

  if (!s.decrypt_buf) {
    s.decrypt_buf = heap_caps_malloc(AP2_PACKET_MAX + AAC_DECODER_INPUT_HEADROOM,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.decrypt_buf) {
      s.decrypt_buf = malloc(AP2_PACKET_MAX + AAC_DECODER_INPUT_HEADROOM);
    }
  }
  if (!s.decode_pcm) {
    s.decode_pcm = heap_caps_malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.decode_pcm) {
      s.decode_pcm = malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
    }
  }
  if (!s.realtime_stage_pcm) {
    s.realtime_stage_pcm = heap_caps_malloc(
        AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.realtime_stage_pcm)
      s.realtime_stage_pcm = malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
  }

  if (!s.transport) {
    ap2_buffered_fifo_config_t tcfg = {
        .buffer_bytes = AP2_BUFFERED_STORE_REQUEST_BYTES,
        .task_core = AP2_NETWORK_CORE,
        .task_priority = AP2_RX_PRIORITY,
        .task_stack = AP2_RX_STACK,
    };
    ESP_RETURN_ON_ERROR(ap2_buffered_fifo_create_with_storage(
                            &s.transport, &tcfg, s.codec_workspace,
                            s.codec_workspace_size),
                        TAG, "buffered FIFO create failed");
  }

  if (!s.pcm_ring && pcm_rtp_ring_create(&s.pcm_ring) != ESP_OK) {
    return ESP_ERR_NO_MEM;
  }

  const size_t rt_stage_bytes = pcm_rtp_ring_storage_bytes();
  const size_t rt_pool_bytes = realtime_receiver_packet_workspace_size();
  if (rt_stage_bytes > s.codec_workspace_size ||
      rt_pool_bytes > s.codec_workspace_size - rt_stage_bytes) {
    ESP_LOGE(TAG,
             "shared codec workspace too small: have=%u KiB ALAC needs=%u KiB",
             (unsigned)(s.codec_workspace_size / 1024U),
             (unsigned)((rt_stage_bytes + rt_pool_bytes) / 1024U));
    return ESP_ERR_NO_MEM;
  }

  if (!s.realtime_stage_ring &&
      pcm_rtp_ring_create_with_storage(
          &s.realtime_stage_ring, s.codec_workspace, rt_stage_bytes) != ESP_OK) {
    return ESP_ERR_NO_MEM;
  }
  if (!s.realtime_workspace_bound) {
    ESP_RETURN_ON_ERROR(
        realtime_receiver_set_packet_workspace(
            s.codec_workspace + rt_stage_bytes,
            s.codec_workspace_size - rt_stage_bytes),
        TAG, "ALAC shared packet workspace bind failed");
    s.realtime_workspace_bound = true;
  }
  if (!s.buffered_packet || !s.decrypt_buf || !s.decode_pcm ||
      !s.realtime_stage_pcm || !s.pcm_ring || !s.realtime_stage_ring) {
    return ESP_ERR_NO_MEM;
  }

  pcm_rtp_ring_set_generation(s.pcm_ring, s.buffered_pcm_generation);
  pcm_rtp_ring_set_generation(s.realtime_stage_ring, s.generation);
  AUDIO_DIAG_BUFFER_PSRAM(
      AUDIO_DIAG_PSRAM_AFTER_SHARED,
      (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U),
      (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
      (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U),
      0U);
  AUDIO_DIAG_BUFFER_WORKSPACE(
      (uint32_t)(s.codec_workspace_size / 1024U),
      (uint32_t)(ap2_buffered_fifo_capacity(s.transport) / 1024U),
      (uint32_t)((rt_stage_bytes + rt_pool_bytes) / 1024U));
  AUDIO_DIAG_BUFFER_PSRAM(
      AUDIO_DIAG_PSRAM_AFTER_STORES,
      (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U),
      (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
      (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U),
      (uint32_t)(ap2_buffered_fifo_capacity(s.transport) / 1024U));

  ESP_RETURN_ON_ERROR(audio_playout_init(), TAG, "I2S playout init failed");
  s.engine_running = true;
  if (!s.status_task &&
      xTaskCreatePinnedToCore(audio_status_task, "audio_status", AP2_STATUS_STACK,
                              NULL, AP2_STATUS_PRIORITY, &s.status_task,
                              AP2_STATUS_CORE) != pdPASS) {
    ESP_LOGW(TAG, "audio status task create failed");
  }
  if (!s.playout_task) {
    if (xTaskCreatePinnedToCore(ap2_playout_task, "ap2_playout",
                                AP2_PLAYOUT_STACK, NULL, AP2_PLAYOUT_PRIORITY,
                                &s.playout_task, AP2_DECODE_CORE) != pdPASS) {
      return ESP_FAIL;
    }
    AUDIO_DIAG_LIFECYCLE_TASK_STARTED(
        AUDIO_DIAG_TASK_PLAYOUT, AP2_DECODE_CORE, AP2_PLAYOUT_PRIORITY,
        AUDIO_PLAYOUT_FRAMES);
  }
  if (!s.realtime_stage_task) {
    if (xTaskCreatePinnedToCore(realtime_stage_task, "alac_stage",
                                AP2_RT_STAGE_STACK, NULL, AP2_RT_STAGE_PRIORITY,
                                &s.realtime_stage_task, AP2_DECODE_CORE) != pdPASS) {
      return ESP_FAIL;
    }
  }

  return ESP_OK;
}

bool audio_receiver_is_initialized(void) {
  return s.engine_running && s.codec_workspace && s.transport && s.pcm_ring &&
         s.realtime_stage_ring;
}

esp_err_t audio_receiver_release_for_wifi_scan(void) {
  if (!audio_receiver_is_initialized()) return ESP_OK;

  /* First reach the same hard media boundary used for codec/session changes.
   * The RTSP server is stopped by the caller, so no new producer can appear
   * while the stores are being dismantled. */
  audio_receiver_stop();

  if (s.processor_task || !realtime_receiver_is_idle() ||
      (s.transport && !ap2_buffered_fifo_is_idle(s.transport))) {
    ESP_LOGE(TAG, "WiFi scan release refused: media producer still active");
    return ESP_ERR_TIMEOUT;
  }

  /* Stop the long-lived workers before freeing any object they can observe. */
  s.engine_running = false;
  __atomic_store_n(&s.realtime_stage_running, false, __ATOMIC_RELEASE);
  audio_status_notify();
  playout_wake();
  realtime_stage_kick();
  if (s.playout_timer) (void)esp_timer_stop(s.playout_timer);

  for (int i = 0; (s.playout_task || s.realtime_stage_task || s.status_task) &&
                  i < 100; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s.playout_task || s.realtime_stage_task || s.status_task) {
    ESP_LOGE(TAG,
             "WiFi scan release timed out: playout=%p stage=%p status=%p",
             (void *)s.playout_task, (void *)s.realtime_stage_task,
             (void *)s.status_task);
    /* No large allocation has been freed yet. Restore the normal engine
     * lifecycle rather than returning a half-stopped receiver to the web
     * server. */
    (void)audio_receiver_init();
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = realtime_receiver_clear_packet_workspace();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to detach realtime shared workspace: %s",
             esp_err_to_name(err));
    (void)audio_receiver_init();
    return err;
  }
  s.realtime_workspace_bound = false;

  if (s.transport) {
    err = ap2_buffered_fifo_destroy(s.transport);
    if (err != ESP_OK) {
      /* destroy() leaves the FIFO object intact when its reader still owns
       * the shared backing store. Rebind/restart the engine and, critically,
       * do not free codec_workspace underneath that reader. */
      ESP_LOGE(TAG, "WiFi scan release: FIFO destroy failed: %s",
               esp_err_to_name(err));
      (void)audio_receiver_init();
      return err;
    }
    s.transport = NULL;
  }
  if (s.realtime_stage_ring) {
    pcm_rtp_ring_destroy(s.realtime_stage_ring);
    s.realtime_stage_ring = NULL;
  }
  if (s.pcm_ring) {
    pcm_rtp_ring_destroy(s.pcm_ring);
    s.pcm_ring = NULL;
  }

  free(s.buffered_packet);
  s.buffered_packet = NULL;
  free(s.decrypt_buf);
  s.decrypt_buf = NULL;
  free(s.decode_pcm);
  s.decode_pcm = NULL;
  free(s.realtime_stage_pcm);
  s.realtime_stage_pcm = NULL;
  free(s.codec_workspace);
  s.codec_workspace = NULL;
  s.codec_workspace_size = 0;

  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  s.anchor_valid = false;
  s.anchor_local_valid = false;
  s.realtime_stage_cursor_valid = false;
  s.stream_type = AUDIO_STREAM_NONE;
  s.port = 0;
  taskEXIT_CRITICAL(&s.state_mux);

  ESP_LOGI(TAG, "Audio memory released for WiFi scan");
  return ESP_OK;
}

static bool realtime_stage_stop_and_wait(void) {
  taskENTER_CRITICAL(&s.state_mux);
  __atomic_store_n(&s.realtime_stage_running, false, __ATOMIC_RELEASE);
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();
  for (int i = 0; s.realtime_stage_task &&
                  !__atomic_load_n(&s.realtime_stage_idle, __ATOMIC_ACQUIRE) &&
                  i < 20; ++i) {
    vTaskDelay(1);
  }
  const bool idle = !s.realtime_stage_task ||
      __atomic_load_n(&s.realtime_stage_idle, __ATOMIC_ACQUIRE);
  if (!idle) ESP_LOGW(TAG, "ALAC staging stop timed out; codec start refused");
  return idle;
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
  bool changed;
  taskENTER_CRITICAL(&s.state_mux);
  changed = s.stream_type != t;
  s.stream_type = t;
  taskEXIT_CRITICAL(&s.state_mux);
  if (changed) audio_status_notify();
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

  if (!realtime_stage_stop_and_wait() || !realtime_receiver_is_idle())
    return ESP_ERR_INVALID_STATE;
  if (!s.transport) return ESP_ERR_INVALID_STATE;
  if (s.rx_running) return ESP_OK;
  if (s.processor_task) {
    ESP_LOGE(TAG,
             "buffered processor from previous session is still active; refusing duplicate task");
    return ESP_ERR_INVALID_STATE;
  }

  /* New buffered codec session: both compressed and decoded media storage are
   * hard-reset. This is deliberately stronger than a seek/anchor change. */
  ap2_buffered_fifo_clear(s.transport);
  reset_buffered_pcm_store();

  uint16_t bound = port;
  ESP_RETURN_ON_ERROR(ap2_buffered_fifo_start(s.transport, port, &bound),
                      TAG, "buffered FIFO start failed");
  s.port = bound;
  s.rx_running = true;
  if (xTaskCreatePinnedToCore(ap2_buffered_processor_task, "ap2_buf_proc",
                              AP2_PROCESS_STACK, NULL, AP2_DECODE_PRIORITY,
                              &s.processor_task, AP2_BUFFERED_PROCESSOR_CORE) != pdPASS) {
    s.rx_running = false;
    ap2_buffered_fifo_notify(s.transport);
    ap2_buffered_fifo_stop(s.transport);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "AP2 buffered listener port=%u", (unsigned)s.port);
  return ESP_OK;
}

esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port) {
  const uint32_t quiesce_req =
      __atomic_load_n(&s.playout_quiesce_req, __ATOMIC_ACQUIRE);
  const uint32_t quiesce_ack =
      __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE);
  if (quiesce_req != quiesce_ack ||
      __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE)) {
    ESP_LOGE(TAG,
             "audio start refused: previous playout boundary not quiesced "
             "req=%" PRIu32 " ack=%" PRIu32 " flush=%d servoReset=%d",
             quiesce_req, quiesce_ack,
             __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ? 1 : 0,
             __atomic_load_n(&s.playout_servo_reset_requested,
                             __ATOMIC_ACQUIRE) ? 1 : 0);
    return ESP_ERR_INVALID_STATE;
  }

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

    /* AAC compressed payload pages and ALAC large realtime buffers are the
     * same physical PSRAM. Never let ALAC reuse those bytes until the TCP
     * reader and buffered decoder have completely released their ownership. */
    if (s.transport && !ap2_buffered_fifo_is_idle(s.transport)) {
      ESP_LOGE(TAG,
               "realtime start refused: buffered transport still owns shared codec workspace");
      return ESP_ERR_INVALID_STATE;
    }
    if (s.transport) ap2_buffered_fifo_clear(s.transport);

    /* SETUP may be followed by RECORD for the same live stream. Preserve
     * its anchor, cursor and EQ history; only a stopped stream needs startup. */
    if (realtime_receiver_is_running()) {
      return !s.rx_running && !s.processor_task && s.realtime_stage_task &&
                     __atomic_load_n(&s.realtime_stage_running, __ATOMIC_ACQUIRE)
                 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s.rx_running || s.processor_task || !realtime_receiver_is_idle() ||
        !s.realtime_stage_task ||
        !realtime_stage_stop_and_wait()) return ESP_ERR_INVALID_STATE;
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
    AUDIO_DIAG_LIFECYCLE_REALTIME_EPOCH(rt_gen);

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

static bool wait_playout_quiesced(uint32_t request, uint32_t timeout_ms) {
  if (!s.playout_task || !s.engine_running) return true;
  const int64_t deadline_us =
      esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
  while (esp_timer_get_time() < deadline_us) {
    if (__atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE) == request) {
      return true;
    }
    vTaskDelay(1);
  }
  return __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE) == request;
}

void audio_receiver_stop(void) {
  /* A codec switch is a hard session boundary. Stop both possible producers
   * first; no compressed, decoded or timing state is allowed to bleed into
   * the next AAC/ALAC SETUP. */
  (void)realtime_stage_stop_and_wait();
  realtime_receiver_stop();
  taskENTER_CRITICAL(&s.state_mux);
  s.realtime_stage_cursor_valid = false;
  taskEXIT_CRITICAL(&s.state_mux);
  s.rx_running = false;
  ap2_buffered_fifo_notify(s.transport);

  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  status_invalidate_sync();
  mark_timeline_discontinuity();
  audio_status_notify();
  __atomic_store_n(&s.playout_servo_reset_requested, true, __ATOMIC_RELEASE);
  const uint32_t quiesce_request =
      __atomic_add_fetch(&s.playout_quiesce_req, 1U, __ATOMIC_ACQ_REL);

  /* The comment above defines this as a hard boundary, so make that true in
   * execution too: do not let the next RTSP SETUP overtake the Core1 I2S
   * flush/reset. The wait is normally only a few milliseconds and is bounded
   * so a genuine I2S stall is surfaced rather than hanging the control task. */
  if (!wait_playout_quiesced(quiesce_request, 250U)) {
    ESP_LOGE(TAG,
             "PLAYOUT quiesce timeout req=%" PRIu32 " ack=%" PRIu32
             " flush=%d servoReset=%d",
             quiesce_request,
             __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE),
             __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ? 1 : 0,
             __atomic_load_n(&s.playout_servo_reset_requested,
                             __ATOMIC_ACQUIRE) ? 1 : 0);
  }

  if (s.transport) {
    ap2_buffered_fifo_stop(s.transport);
  }

  /* The buffered processor exits when rx_running becomes false. Wait only on
   * the control path so its decoder cannot still publish PCM while the next
   * codec session is being configured. */
  for (int i = 0; s.processor_task && i < 100; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s.processor_task) {
    ESP_LOGE(TAG,
             "buffered processor did not stop within shutdown window; next buffered start will be rejected");
  }

  if (s.transport) {
    ap2_buffered_fifo_clear(s.transport);
  }

  /* Do not reset the stateful EQ asynchronously from the control core. AAC
   * and the ALAC staging task each reset it on their next generation. */
  s.port = 0;
}

void audio_receiver_stop_buffered_only(void) { audio_receiver_stop(); }
uint16_t audio_receiver_get_buffered_port(void) { return s.port; }

size_t audio_receiver_get_buffered_audio_buffer_size(void) {
  return ap2_buffered_fifo_capacity(s.transport);
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
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  snapshot_state(&snap);
  if (snap.stream_type != AUDIO_STREAM_REALTIME) {
    xSemaphoreGive(s.publish_mutex);
    return;
  }
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
  xSemaphoreGive(s.publish_mutex);
  realtime_stage_kick();

  /* An explicit FLUSH may cancel already queued DMA, but it must not change
   * the validated RTP<->presentation map. The normal playout task re-primes
   * against the same sender timeline and waits until desired_rtp reaches
   * flush_rtp. */
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  media_control_wake();
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

esp_err_t audio_receiver_set_deferred_flush_range(uint32_t from_seq, uint32_t from_ts,
                                                   uint32_t until_seq, uint32_t until_ts) {
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  from_seq &= 0x007fffffU;
  until_seq &= 0x007fffffU;

  /* Shairport semantics: register a future sequential cut only. The raw TCP
   * FIFO is untouched here; the single packet consumer activates the rule when
   * it actually encounters fromSeq and discards until untilSeq/overshoot. */
  if (s.transport) {
    esp_err_t err = ap2_buffered_fifo_add_deferred_flush(
        s.transport, from_seq, from_ts, until_seq, until_ts);
    if (err != ESP_OK) {
      xSemaphoreGive(s.publish_mutex);
      return err;
    }
  }

  xSemaphoreGive(s.publish_mutex);
  return ESP_OK;
}

void audio_receiver_set_immediate_flush(uint32_t until_seq, uint32_t until_ts,
                                        bool has_endpoint) {
  AUDIO_DIAG_FLUSH_IMMEDIATE_BEGIN();
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  AUDIO_DIAG_FLUSH_IMMEDIATE_PUBLISH_ACQUIRED();
  until_seq &= 0x007fffffU;

  /* Stop presentation immediately. This changes only the timing epoch; media
   * invalidation below is independent and never stops TCP ingestion. */
  mark_timeline_discontinuity();

  if (s.transport) {
    ap2_buffered_fifo_set_immediate_flush(s.transport, until_seq,
                                                       until_ts, has_endpoint);
  }
  AUDIO_DIAG_FLUSH_IMMEDIATE_TRANSPORT_DONE();

  timing_snapshot_t flush_snap;
  snapshot_state(&flush_snap);
  if (has_endpoint) {
    /* Immediate FLUSH discards PCM before the supplied RTP endpoint. Later PCM
     * remains only as still-valid forward media; backward seek is handled as a
     * new presentation and does not depend on played compressed history. */
    pcm_rtp_ring_invalidate_before(s.pcm_ring, until_ts,
                                   flush_snap.pcm_generation);
  } else {
    /* No endpoint is an explicit full media flush. This is one of the few
     * buffered control operations allowed to invalidate the entire PCM store. */
    reset_buffered_pcm_store();
  }
  AUDIO_DIAG_FLUSH_IMMEDIATE_PCM_DONE();

  xSemaphoreGive(s.publish_mutex);
  AUDIO_DIAG_FLUSH_IMMEDIATE_END(has_endpoint ? 1U : 0U);
}

void audio_receiver_pause(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  status_invalidate_sync();
  pause_presentation_timing();
  audio_status_notify();
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
  media_control_wake();
  if (!p) {
    status_invalidate_sync();
    mark_timeline_discontinuity();
  }
  audio_status_notify();
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
  ptp_clock_snapshot_t ps = {0};
  ptp_clock_get_snapshot(&ps);
  uint64_t local_ns = 0;
  const bool local_valid = buffered_ptp_qualified(&ps, clock_id, true) &&
      ptp_clock_engine_remote_to_local(ptp_ns, ps.filtered_offset_ns, &local_ns) &&
      local_ns != 0;
  const uint64_t local_update_us = local_valid ? (uint64_t)esp_timer_get_time() : 0;
  uint32_t gen;
  bool committed;
  taskENTER_CRITICAL(&s.state_mux);
  committed = s.timeline_reset_pending;
  gen = commit_anchor_epoch_locked();
  s.media_revision = next_generation(s.media_revision);
  s.anchor_clock_id = clock_id;
  s.anchor_ptp_ns = ptp_ns;
  /* Publish both sides of the map together. No transient clock loss for a
   * qualified same-GM refresh, including while AAC is publishing an AU. */
  s.anchor_local_ns = local_valid ? local_ns : 0;
  s.anchor_local_update_us = local_update_us;
  s.anchor_local_valid = local_valid;
  s.anchor_rtp = rtp;
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  s.anchor_valid = true;
  taskEXIT_CRITICAL(&s.state_mux);
  if (committed) {
    s_anchor_commit_us = esp_timer_get_time();
    s_anchor_commit_gen = gen;
  }
  /* Shairport-style separation: SETRATEANCHORTIME updates presentation timing
   * only. The compressed FIFO has no RTP cursor to search or rebind. */
  if (s.transport) ap2_buffered_fifo_notify(s.transport);
  playout_wake();
  realtime_stage_kick();

  timing_snapshot_t anchor_snap;
  snapshot_state(&anchor_snap);
  ESP_LOGI(TAG, "ANCHOR clock=%016" PRIx64 " ptp=%" PRIu64
                " rtp=%" PRIu32 " gen=%" PRIu32 " pcm_gen=%" PRIu32 "%s",
           clock_id, ptp_ns, rtp, gen, anchor_snap.pcm_generation,
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
       * exact existing RTP<->local phase and calculate the initial media
       * bias needed to express the new GM's D7 observations in that same
       * local timeline. The bias then converges to zero at 50 ppm;
       * PTP estimation and the hardware PID remain independent. */
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
      s.rt_media_rebase_start_us = esp_timer_get_time();
      result->rebased = true;
      log_rebase = true;
    } else if (s.rt_media_rebase_valid &&
               s.rt_media_rebase_epoch == gm_epoch &&
               s.rt_media_rebase_clock_id == clock_id) {
      /* Same GM: follow the current filtered PTP offset and gradually retire
       * the temporary handover bias. Ordinary D7 updates do not restart decay. */
      rebase_bias_ns = realtime_remaining_bias(
          s.rt_media_rebase_bias_ns, s.rt_media_rebase_start_us,
          esp_timer_get_time());
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
    s.anchor_ptp_ns = remote_ptp_ns; /* retained remote anchor for live PTP conversion */
    s.anchor_local_ns = effective_local_ns;
    s.anchor_rtp = rtp;
    s.anchor_valid = true;
    accepted = true;

    result->effective_local_ns = effective_local_ns;
    result->rebase_step_ns = rebase_step_ns;
    result->rebase_bias_ns = realtime_remaining_bias(
        s.rt_media_rebase_bias_ns, s.rt_media_rebase_start_us,
        esp_timer_get_time());
  }
  taskEXIT_CRITICAL(&s.state_mux);

  if (accepted) {
    playout_wake();
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

  /* Continuous D7 refreshes are normal steady-state operation.
   * Keep INFO logging for phase-significant events only. */
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
