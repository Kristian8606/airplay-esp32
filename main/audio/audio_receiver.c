#include "audio_receiver.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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
#include "freertos/semphr.h"
#include "network/ptp_clock.h"
#include "network/socket_utils.h"

#define AP2_PACKET_MAX             8192U
#define AP2_RX_STACK               4096U
#define AP2_PROCESS_STACK          6144U
#define AP2_PLAYOUT_STACK          4096U
#define AP2_RT_STAGE_STACK         4096U
#define AP2_DIAG_STACK             3072U
#define AP2_NETWORK_CORE           0
#define AP2_DECODE_CORE            1
#define AP2_BUFFERED_PROCESSOR_CORE 0
#define AP2_RX_PRIORITY            5
#define AP2_DECODE_PRIORITY        6
#define AP2_PLAYOUT_PRIORITY       8
#define AP2_RT_STAGE_PRIORITY      7
#define AP2_DIAG_PRIORITY          1
#define AP2_DIAG_CORE              0
#define AP2_DIAG_PERIOD_MS      2000U
#define AP2_PCM_CAPACITY_FRAMES    4096U

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
#define AP2_REALTIME_PRIME_MS        100U
#define AP2_RT_GM_REBASE_SETTLE_MS  1000U
#define AP2_BUFFERED_STORE_REQUEST_BYTES AP2_BUFFERED_AUDIO_BUFFER_REQUEST_BYTES
#define AP2_BUFFERED_LEAD_MS       (AP2_PCM_TARGET_MS + 100U)
#define AP2_CSTORE_DECODE_BURST          8U
#define AP2_CSTORE_REORDER_GUARD_MS    300U
#define AP2_DECODE_IDLE_TICKS            1U

/* ALAC reorder release point. Missing PCM stays absent in the raw RTP ring
 * while retransmission runs independently. Only when the physical PTP
 * deadline is this close do we commit one frame of silence through EQ. */
#define AP2_RT_STAGE_COMMIT_MARGIN_US REALTIME_RECOVERY_FINAL_MARGIN_US

static const char *TAG = "audio_shairport";
static const char *DIAG_TAG = "audio_status";

/* Updated by RTSP control on Core0, consumed by playout on Core1. */
static volatile int32_t s_volume_target_q15 = 32768;

typedef struct {
  bool anchor_valid;
  bool playing;
  uint64_t anchor_clock_id;
  uint64_t anchor_ptp_ns;
  uint64_t anchor_local_ns; /* current local conversion, cached during PTP holdover */
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
} output_sync_state_t;

typedef struct {
  audio_format_t format;
  audio_encrypt_t encrypt;
  audio_stream_type_t stream_type;

  uint16_t port;
  volatile bool engine_running;
  volatile bool rx_running;

  SemaphoreHandle_t publish_mutex; /* producers/FLUSH only; never I2S */
  ap2_buffered_transport_t *transport;
  TaskHandle_t processor_task;
  TaskHandle_t playout_task;
  TaskHandle_t realtime_stage_task;
  volatile bool diag_running;
  SemaphoreHandle_t diag_stop_sem;
  audio_stream_type_t diag_stream_type;
  uint8_t *packet;
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

  /* Only the existing 1 Hz PID loop publishes these. The 2 s diagnostic
   * task reads them; no packet/decode/I2S-completion instrumentation is added. */
  volatile bool diag_sync_valid;
  volatile int32_t diag_sync_us;
  volatile int32_t diag_servo_ppm;
  volatile uint32_t diag_sync_generation;
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

static void snapshot_state(timing_snapshot_t *out) {
  uint64_t clock_id;
  uint32_t epoch;
  int64_t bias_ns, bias_start_us;
  taskENTER_CRITICAL(&s.state_mux);
  out->anchor_valid = s.anchor_valid;
  out->playing = s.playing;
  out->anchor_clock_id = s.anchor_clock_id;
  out->anchor_ptp_ns = s.anchor_ptp_ns;
  out->anchor_local_ns = s.anchor_local_ns;
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
  clock_id = s.anchor_clock_id;
  epoch = s.rt_media_rebase_epoch;
  bias_ns = s.rt_media_rebase_bias_ns;
  bias_start_us = s.rt_media_rebase_start_us;
  taskEXIT_CRITICAL(&s.state_mux);

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
          (remaining < 0 && local_ns < (uint64_t)(-(remaining + 1)) + 1U))
        return;
      const int64_t effective = (int64_t)local_ns + remaining;
      if (effective <= 0) return;
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
}

static uint32_t next_generation(uint32_t generation) {
  generation++;
  return generation ? generation : 1U;
}

/* Hard reset for buffered media storage only. Ordinary pause/seek/anchor
 * changes must not call this: they change the timing epoch, not the identity
 * of already received RTP-addressed PCM. */
static uint32_t reset_buffered_pcm_store(void) {
  uint32_t new_gen;
  taskENTER_CRITICAL(&s.state_mux);
  new_gen = next_generation(s.buffered_pcm_generation);
  taskEXIT_CRITICAL(&s.state_mux);

  if (s.pcm_ring) {
    pcm_rtp_ring_set_generation(s.pcm_ring, new_gen);
  }

  taskENTER_CRITICAL(&s.state_mux);
  s.buffered_pcm_generation = new_gen;
  taskEXIT_CRITICAL(&s.state_mux);
  return new_gen;
}

/* Pause/immediate FLUSH ends the current presentation timeline now, but it does
 * not by itself destroy buffered AAC media. TCP keeps arriving while the
 * anchor is invalid; FLUSH rules decide media validity and the next anchor only
 * commits a fresh timing/playout epoch. */
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
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
}

static uint32_t commit_anchor_epoch_locked(void) {
  uint32_t gen = s.generation;
  if (s.timeline_reset_pending) {
    gen = next_generation(gen);
    /* Timing/playout epoch changes are not automatically media-store resets.
     * Buffered AAC keeps RTP-addressed PCM across pause/seek/anchor changes so
     * a backward seek can reuse samples we already decoded. Realtime ALAC still
     * owns generation-scoped raw/final rings because its recovery/EQ chronology
     * is tied to one continuous realtime epoch. */
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

/* Presentation-clock boundary.
 * Buffered AAC intentionally keeps the existing PTP-domain behaviour.
 * Realtime ALAC retains the remote D7 anchor. Each timing snapshot converts
 * it with the current filtered PTP offset, even between D7 packets. Staging,
 * playout and DMA/PID use that coherent snapshot in ESP monotonic time. */
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

/* Runtime diagnostics policy:
 * - exactly one low-priority task owns periodic audio-status logging;
 * - the hot packet/decode path maintains no counters/timers just for logging;
 * - the playout task publishes sync/ppm only at its already-existing 1 Hz PID;
 * - codec switch/pause destroys this task, so there is no dormant logger. */
static double diag_frames_to_ms(uint32_t frames, int sample_rate) {
  const uint32_t sr = sample_rate > 0 ? (uint32_t)sample_rate : 44100U;
  return ((double)frames * 1000.0) / (double)sr;
}

static void diag_publish_sync(int32_t sync_us, int32_t servo_ppm,
                              uint32_t generation) {
  __atomic_store_n(&s.diag_sync_us, sync_us, __ATOMIC_RELAXED);
  __atomic_store_n(&s.diag_servo_ppm, servo_ppm, __ATOMIC_RELAXED);
  __atomic_store_n(&s.diag_sync_generation, generation, __ATOMIC_RELAXED);
  __atomic_store_n(&s.diag_sync_valid, true, __ATOMIC_RELEASE);
}

static void diag_invalidate_sync(void) {
  __atomic_store_n(&s.diag_sync_valid, false, __ATOMIC_RELEASE);
}

static void audio_diag_task(void *arg) {
  const audio_stream_type_t task_stream = (audio_stream_type_t)(intptr_t)arg;

  for (;;) {
    if (xSemaphoreTake(s.diag_stop_sem, pdMS_TO_TICKS(AP2_DIAG_PERIOD_MS)) ==
        pdTRUE) {
      break;
    }

    timing_snapshot_t snap;
    snapshot_state(&snap);
    if (!snap.playing || snap.stream_type != task_stream || !s.engine_running) {
      break;
    }

    const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
    uint32_t wanted = 0;
    const bool wanted_valid = wanted_rtp_now(&snap, &wanted);
    uint32_t pcm_frames = 0;
    if (wanted_valid && s.pcm_ring) {
      pcm_frames = pcm_rtp_ring_contiguous_frames(
          s.pcm_ring, wanted, snap.pcm_generation, PCM_RTP_RING_FRAMES);
    }

    const bool sync_valid =
        __atomic_load_n(&s.diag_sync_valid, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&s.diag_sync_generation, __ATOMIC_RELAXED) ==
            snap.generation;
    const int32_t sync_us =
        __atomic_load_n(&s.diag_sync_us, __ATOMIC_RELAXED);
    const int32_t correction_ppm =
        __atomic_load_n(&s.diag_servo_ppm, __ATOMIC_RELAXED);
    const int32_t crystal_ppm = -correction_ppm;

    if (task_stream == AUDIO_STREAM_BUFFERED) {
      ap2_buffered_transport_usage_t usage = {0};
      ap2_buffered_transport_get_usage(s.transport, &usage);
      const unsigned used_pct = usage.capacity_bytes
          ? (unsigned)((usage.used_bytes * 100U) / usage.capacity_bytes)
          : 0U;
      if (sync_valid) {
        ESP_LOGI(DIAG_TAG,
                 "AAC sync=%+.2fms xtal~=%+ldppm i2s=%+ldppm "
                 "cbuf=%u%%(%u/%uKiB ready=%lu) pcm=%.2fms",
                 (double)sync_us / 1000.0, (long)crystal_ppm,
                 (long)correction_ppm, used_pct,
                 (unsigned)(usage.used_bytes / 1024U),
                 (unsigned)(usage.capacity_bytes / 1024U),
                 (unsigned long)usage.ready_packets,
                 diag_frames_to_ms(pcm_frames, sr));
      } else {
        ESP_LOGI(DIAG_TAG,
                 "AAC sync=n/a xtal~=n/a i2s=%+ldppm "
                 "cbuf=%u%%(%u/%uKiB ready=%lu) pcm=%.2fms",
                 (long)correction_ppm, used_pct,
                 (unsigned)(usage.used_bytes / 1024U),
                 (unsigned)(usage.capacity_bytes / 1024U),
                 (unsigned long)usage.ready_packets,
                 diag_frames_to_ms(pcm_frames, sr));
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
      if (sync_valid) {
        ESP_LOGI(DIAG_TAG,
                 "ALAC sync=%+.2fms xtal~=%+ldppm i2s=%+ldppm "
                 "frameq=%lu/%lu raw=%.2fms pcm=%.2fms",
                 (double)sync_us / 1000.0, (long)crystal_ppm,
                 (long)correction_ppm,
                 (unsigned long)usage.work_queue_depth,
                 (unsigned long)usage.work_queue_capacity,
                 diag_frames_to_ms(raw_frames, sr),
                 diag_frames_to_ms(pcm_frames, sr));
      } else {
        ESP_LOGI(DIAG_TAG,
                 "ALAC sync=n/a xtal~=n/a i2s=%+ldppm "
                 "frameq=%lu/%lu raw=%.2fms pcm=%.2fms",
                 (long)correction_ppm,
                 (unsigned long)usage.work_queue_depth,
                 (unsigned long)usage.work_queue_capacity,
                 diag_frames_to_ms(raw_frames, sr),
                 diag_frames_to_ms(pcm_frames, sr));
      }
    }
  }

  taskENTER_CRITICAL(&s.state_mux);
  s.diag_stream_type = AUDIO_STREAM_NONE;
  taskEXIT_CRITICAL(&s.state_mux);
  __atomic_store_n(&s.diag_running, false, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

static bool audio_diag_stop_and_wait(void) {
  if (!__atomic_load_n(&s.diag_running, __ATOMIC_ACQUIRE)) return true;

  if (s.diag_stop_sem) xSemaphoreGive(s.diag_stop_sem);
  for (uint32_t i = 0; i < 100U; ++i) {
    if (!__atomic_load_n(&s.diag_running, __ATOMIC_ACQUIRE)) return true;
    vTaskDelay(1);
  }
  ESP_LOGW(TAG, "audio status task stop timed out");
  return false;
}

static void audio_diag_start(audio_stream_type_t stream_type) {
  if (stream_type != AUDIO_STREAM_BUFFERED &&
      stream_type != AUDIO_STREAM_REALTIME) return;

  taskENTER_CRITICAL(&s.state_mux);
  const bool same_stream = s.diag_stream_type == stream_type;
  taskEXIT_CRITICAL(&s.state_mux);
  if (__atomic_load_n(&s.diag_running, __ATOMIC_ACQUIRE) && same_stream) return;

  if (!audio_diag_stop_and_wait() || !s.diag_stop_sem) return;
  while (xSemaphoreTake(s.diag_stop_sem, 0) == pdTRUE) {}

  taskENTER_CRITICAL(&s.state_mux);
  s.diag_stream_type = stream_type;
  taskEXIT_CRITICAL(&s.state_mux);
  __atomic_store_n(&s.diag_running, true, __ATOMIC_RELEASE);

  if (xTaskCreatePinnedToCore(audio_diag_task, "audio_status", AP2_DIAG_STACK,
                              (void *)(intptr_t)stream_type,
                              AP2_DIAG_PRIORITY, NULL,
                              AP2_DIAG_CORE) != pdPASS) {
    __atomic_store_n(&s.diag_running, false, __ATOMIC_RELEASE);
    taskENTER_CRITICAL(&s.state_mux);
    s.diag_stream_type = AUDIO_STREAM_NONE;
    taskEXIT_CRITICAL(&s.state_mux);
    ESP_LOGW(TAG, "audio status task create failed");
  }
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

/* Buffered AirPlay 2 addressable model: transport arrival, media validity,
 * decoder chronology and presentation timing are separate concerns. */
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

/* Lock order: publish_mutex -> transport or PCM writer mutex -> state_mux.
 * Never sleep for backpressure with publish_mutex held. I2S remains lock-free. */
static bool pcm_store_with_backpressure(uint32_t rtp, const int16_t *pcm,
                                        size_t frames, int channels,
                                        uint32_t pcm_generation,
                                        const ap2_buffered_packet_ref_t *pkt) {
  if (!pcm || !pkt || channels != 2 || frames == 0 ||
      frames > PCM_RTP_SLOT_FRAMES) return false;
  while (s.rx_running) {
    xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
    timing_snapshot_t snap;
    snapshot_state(&snap);
    if (!s.rx_running || snap.pcm_generation != pcm_generation ||
        snap.stream_type != AUDIO_STREAM_BUFFERED ||
        ap2_buffered_transport_ref_is_invalid(s.transport, pkt)) {
      xSemaphoreGive(s.publish_mutex);
      return false;
    }
    uint32_t wanted = 0;
    const bool wanted_valid = wanted_rtp_now(&snap, &wanted);
    if (wanted_valid && rtp_delta(rtp + (uint32_t)frames, wanted) <= 0) {
      xSemaphoreGive(s.publish_mutex);
      return false;
    }
    const bool stored = pcm_rtp_ring_write(s.pcm_ring, rtp, pcm, frames, channels,
                                           pcm_generation, wanted, wanted_valid);
    xSemaphoreGive(s.publish_mutex);
    if (stored) return true;
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
  uint32_t expected_timestamp = 0;
  uint32_t expected_seq = 0;
  bool have_decoded_sequence = false;
  bool play_enabled = false;
  bool decoder_history_dirty = false;

  /* EQ/AAC history belongs to media chronology, not to the PTP presentation
   * epoch.  Start a fresh codec-session history once here; later resets happen
   * only when the media cursor itself becomes discontinuous. */
  audio_eq_reset_state();

  ESP_LOGI(TAG,
           "addressable buffered processor core=%d prio=%u media_order=1 exact_hash=1",
           xPortGetCoreID(), (unsigned)AP2_DECODE_PRIORITY);

  while (s.rx_running) {
    bool made_progress = false;
    timing_snapshot_t state_snap;
    snapshot_state(&state_snap);
    const bool control_play_enabled =
        state_snap.stream_type == AUDIO_STREAM_BUFFERED && state_snap.playing;

    if (control_play_enabled != play_enabled) {
      play_enabled = control_play_enabled;
    }

    /* TCP publishes directly into the addressable READY index. There is no
     * metadata FIFO and no transport-order ownership phase here. */
    snapshot_state(&state_snap);
    if (!play_enabled || state_snap.stream_type != AUDIO_STREAM_BUFFERED ||
        !state_snap.playing || !state_snap.anchor_valid ||
        state_snap.timeline_reset_pending || !ptp_clock_is_locked()) {
      if (!made_progress) vTaskDelay(delay_ticks_at_least_one(5));
      continue;
    }

    /* Decode a bounded media-order burst. TCP publication is independent and
     * direct, so decoder work can never gate network ingestion except through
     * the intentional full-store backpressure point. */
    for (uint32_t decoded_now = 0; decoded_now < AP2_CSTORE_DECODE_BURST;
         ++decoded_now) {
      snapshot_state(&state_snap);
      if (!play_enabled || state_snap.stream_type != AUDIO_STREAM_BUFFERED ||
          !state_snap.playing || !state_snap.anchor_valid ||
          state_snap.timeline_reset_pending || !ptp_clock_is_locked()) {
        break;
      }

      if (decoder_history_dirty) {
        if (decoder && !aac_decoder_reset(decoder)) {
          aac_decoder_destroy(decoder);
          decoder = NULL;
          decoder_format_generation = 0;
        }
        audio_eq_reset_state();
        have_decoded_sequence = false;
        expected_timestamp = 0;
        expected_seq = 0;
        decoder_history_dirty = false;
      }

      const uint32_t frame_samples = state_snap.format.frame_size > 0
                                         ? (uint32_t)state_snap.format.frame_size
                                         : 1024U;
      uint32_t wanted = 0;
      if (!wanted_rtp_now(&state_snap, &wanted)) break;

      const int sr = state_snap.format.sample_rate > 0
                         ? state_snap.format.sample_rate
                         : 44100;
      const int32_t max_lead =
          (int32_t)(((int64_t)sr * AP2_BUFFERED_LEAD_MS) / 1000LL);

      timing_snapshot_t snap = state_snap;

      /* Presentation anchors are cursor moves, not decoder resets.  Keep AAC
       * and EQ chronology while the decoder's expected RTP is still inside the
       * active addressable window.  A seek/track/scrub that moves the playhead
       * outside that window breaks media continuity exactly once, independent
       * of how many PAUSE/FLUSH/SETRATE timing generations were involved. */
      if (have_decoded_sequence) {
        const int32_t expected_from_wanted =
            rtp_delta(expected_timestamp, wanted);
        const bool cursor_moved_past_decoder =
            (int64_t)expected_from_wanted + (int64_t)frame_samples <= 0;
        const bool decoder_is_other_neighbourhood =
            expected_from_wanted > max_lead + (int32_t)frame_samples;
        if (cursor_moved_past_decoder || decoder_is_other_neighbourhood) {
          /* Cursor moved out of the decoder neighbourhood. Reset
           * codec/EQ history once and reacquire from the live media window. */
          if (decoder && !aac_decoder_reset(decoder)) {
            aac_decoder_destroy(decoder);
            decoder = NULL;
            decoder_format_generation = 0;
          }
          audio_eq_reset_state();
          expected_timestamp = 0;
          expected_seq = 0;
          have_decoded_sequence = false;
        }
      }

      const int32_t reorder_guard =
          (int32_t)(((int64_t)sr * AP2_CSTORE_REORDER_GUARD_MS) / 1000LL);
      const int32_t expected_lead =
          have_decoded_sequence ? rtp_delta(expected_timestamp, wanted) : 0;
      const bool allow_recovery_scan =
          !have_decoded_sequence || expected_lead <= reorder_guard;

      bool forced_recovery = false;
      ap2_buffered_packet_ref_t pkt = {0};
      if (!ap2_buffered_transport_acquire_media_next(
              s.transport, wanted, expected_timestamp, expected_seq,
              have_decoded_sequence, allow_recovery_scan, frame_samples,
              max_lead, snap.media_revision, &forced_recovery, &pkt)) {
        /* Missing exact media above the reorder guard is normal
         * waiting. Hash miss -> unlock -> sleep -> retry. */
        break;
      }
      made_progress = true;

      if (ap2_buffered_transport_ref_is_invalid(s.transport, &pkt)) {
        decoder_history_dirty = true;
        ap2_buffered_transport_release(s.transport, &pkt);
        continue;
      }

      const int32_t lead = rtp_delta(pkt.rtp, wanted);
      if ((int64_t)lead + (int64_t)frame_samples <= 0) {
        ap2_buffered_transport_release(s.transport, &pkt);
        continue;
      }

      /* If the exact decoder continuation is missing but the PCM timeline still
       * has comfortable lead, do not immediately jump the AAC decoder forward.
       * Return the candidate to READY and give Apple time to publish a missing
       * replacement media block. Once the expected position is
       * within the guard of the playhead, progress wins over waiting. */
      if (have_decoded_sequence) {
        const int32_t media_gap = rtp_delta(pkt.rtp, expected_timestamp);
        if (media_gap > 0 && expected_lead > reorder_guard &&
            !forced_recovery) {
          if (!ap2_buffered_transport_defer_decode(s.transport, &pkt)) {
            /* A FLUSH may have invalidated the DECODING packet while the
             * reorder decision was being made. In that race, release its
             * ownership instead of leaving an invalid packet pinned. */
            ap2_buffered_transport_release(s.transport, &pkt);
          }
          vTaskDelay(1);
          break;
        }
      }

      if (pkt.packet_len > AP2_PACKET_MAX ||
          ap2_buffered_transport_copy_packet(s.transport, &pkt, s.packet,
                                             AP2_PACKET_MAX) !=
              (ssize_t)pkt.packet_len) {
        ap2_buffered_transport_release(s.transport, &pkt);
        continue;
      }

      int dec_len = audio_crypto_decrypt_buffered(&s.encrypt, s.packet,
                                                   pkt.packet_len,
                                                   s.decrypt_buf,
                                                   AP2_PACKET_MAX);
      if (dec_len < 0) {
        ap2_buffered_transport_release(s.transport, &pkt);
        continue;
      }
      if (dec_len == 0) {
        ap2_buffered_transport_release(s.transport, &pkt);
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
          ap2_buffered_transport_release(s.transport, &pkt);
          vTaskDelay(1);
          continue;
        }
        decoder_format_generation = snap.format_generation;
        have_decoded_sequence = false;
        expected_timestamp = 0;
        expected_seq = 0;
        audio_eq_reset_state();
        ESP_LOGI(TAG, "AAC decoder ready %dHz %dch", snap.format.sample_rate,
                 snap.format.channels);
      }

      int32_t timestamp_gap = 0;
      int32_t sequence_gap = 0;
      if (have_decoded_sequence) {
        timestamp_gap = rtp_delta(pkt.rtp, expected_timestamp);
        sequence_gap = seq23_delta(pkt.seq, expected_seq);
        if (timestamp_gap != 0 || sequence_gap != 0) {
          /* This is the actual media boundary.  Reset AAC overlap/history and
           * EQ delay state here, not on SETRATEANCHORTIME.  The current packet
           * becomes the first block of the newly selected addressable segment. */
          if (decoder && !aac_decoder_reset(decoder)) {
            aac_decoder_destroy(decoder);
            decoder = NULL;
            decoder_format_generation = 0;
            expected_timestamp = 0;
            expected_seq = 0;
            have_decoded_sequence = false;
            ap2_buffered_transport_release(s.transport, &pkt);
            continue;
          }
          audio_eq_reset_state();
          expected_timestamp = 0;
          expected_seq = 0;
          have_decoded_sequence = false;
        }
      }

      aac_decode_info_t info = {0};
      int frames = aac_decoder_decode(decoder, s.decrypt_buf, (size_t)dec_len,
                                      s.decode_pcm, AP2_PCM_CAPACITY_FRAMES,
                                      &info);
      if (frames < 0) {
        ap2_buffered_transport_release(s.transport, &pkt);
        continue;
      }
      if (frames == 0) {
        ap2_buffered_transport_release(s.transport, &pkt);
        vTaskDelay(1);
        continue;
      }

      expected_timestamp = pkt.rtp + (uint32_t)frames;
      expected_seq = (pkt.seq + 1U) & 0x007fffffU;
      have_decoded_sequence = true;


      pcm_process_common_eq(s.decode_pcm, (size_t)frames, info.channels,
                            snap.format.sample_rate);

      /* Declarative invalidation can race an in-flight decode. It never steals
       * DECODING pages, so re-check validity exactly at PCM publication. */
      if (ap2_buffered_transport_ref_is_invalid(s.transport, &pkt)) {
        decoder_history_dirty = true;
        ap2_buffered_transport_release(s.transport, &pkt);
        continue;
      }

      if (pcm_store_with_backpressure(pkt.rtp, s.decode_pcm, (size_t)frames,
                                      info.channels, snap.pcm_generation, &pkt)) {
        /* FLUSH and publication share publish_mutex. If FLUSH runs now, it
         * invalidates this PCM itself; no post-write rollback can erase a
         * replacement at the same RTP address. */
        ap2_buffered_transport_release(s.transport, &pkt);
        continue;
      }

      if (ap2_buffered_transport_ref_is_invalid(s.transport, &pkt))
        decoder_history_dirty = true;
      /* The PCM media epoch changed or this decoded block became stale while
       * waiting for a reusable address.  Compressed ownership is complete
       * regardless of whether publication survived. */
      ap2_buffered_transport_release(s.transport, &pkt);
    }

    /* CPU0 also hosts network/PTP/control work. Even with infinite READY
     * backlog this worker must yield after every bounded decode burst. */
    vTaskDelay(made_progress ? AP2_DECODE_IDLE_TICKS
                             : delay_ticks_at_least_one(5));
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

  ESP_LOGI(TAG,
           "ALAC staging: raw RTP PCM -> ordered EQ -> final PCM ring, core=%d prio=%u",
           xPortGetCoreID(), (unsigned)AP2_RT_STAGE_PRIORITY);

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
    } else {
      s.output_sync.us += (sync_us - s.output_sync.us) / 8;
    }
  }
}



static void apply_output_volume(int16_t *pcm, uint32_t frames,
                                int32_t *current_q15) {
  if (!pcm || !current_q15 || frames == 0U) return;
  int32_t target = __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
  if (target < 0) target = 0;
  if (target > 32768) target = 32768;
  const int32_t start = *current_q15;
  const int64_t dg = (int64_t)target - (int64_t)start;
  for (uint32_t f = 0; f < frames; ++f) {
    const int32_t gain = dg == 0 ? target :
        start + (int32_t)((dg * (int64_t)(f + 1U)) / (int64_t)frames);
    for (uint32_t ch = 0; ch < 2U; ++ch) {
      const uint32_t i = f * 2U + ch;
      int64_t y = (int64_t)pcm[i] * (int64_t)gain;
      if (y >= 0) y = (y + 16384) >> 15;
      else y = -(((-y) + 16384) >> 15);
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
  ESP_LOGE(TAG, "PLAYOUT FLUSH failed (%s): %s",
           reason ? reason : "unknown", esp_err_to_name(err));
  return false;
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
        ESP_LOGW(TAG, "PLAYOUT SERVO RESET failed: %s", esp_err_to_name(re));
        vTaskDelay(1);
        continue;
      }

      servo_ppm = 0;
      servo_target_ppm = 0;
      servo_generation = 0;
      pid_integral_ms_s = 0.0;
      pid_prev_error_ms = 0.0;
      pid_d_filtered_ms_s = 0.0;
      pid_prev_valid = false;
      pid_last_calc_us = esp_timer_get_time();
      pid_last_tune_us = pid_last_calc_us;

      s.output_sync.valid = false;
      diag_invalidate_sync();
      state = PLAYOUT_STOPPED;
    }

    if (__atomic_exchange_n(&s.i2s_flush_requested, false, __ATOMIC_ACQ_REL)) {
      if (!playout_flush_checked("control")) {
        vTaskDelay(1);
        continue;
      }
      s.output_sync.valid = false;
      diag_invalidate_sync();
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
    snapshot_state(&snap);
    process_i2s_completions(&snap);

    uint32_t desired_rtp = 0;
    bool timeline_ok = false;
    if (snap.playing && snap.anchor_valid && !snap.timeline_reset_pending &&
        timing_clock_ready(&snap)) {
      timeline_ok = wanted_rtp_now(&snap, &desired_rtp);
    }

    if (!timeline_ok) {
      state = PLAYOUT_STOPPED;
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
      s.output_sync.valid = false;
      diag_invalidate_sync();
      state = PLAYOUT_PRIMING;
      if (!playout_flush_checked("prime-start")) {
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }
    }

    if (state == PLAYOUT_PRIMING) {
      const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;

      /* The old FIFO decoder required a long fully-contiguous PCM run before
       * startup. In the addressable model that condition is too strong: a
       * single intentionally skipped/revised AAC AU can leave a sparse hole
       * while plenty of valid PCM already surrounds the playhead. Buffered AAC
       * therefore requires only the short deterministic I2S start guard; once
       * running, the existing exact-RTP playout handles any truly missing block
       * at its physical deadline instead of remaining in PRIME forever.
       * Realtime ALAC keeps its existing recovery prime because its raw reorder
       * path has different deadline semantics. */
      const uint32_t guard_start = desired_rtp;
      uint32_t guard_frames =
          AP2_START_PRIME_GUARD_BLOCKS * AUDIO_PLAYOUT_FRAMES;
      if (snap.stream_type == AUDIO_STREAM_REALTIME) {
        guard_frames += (uint32_t)(((uint64_t)sr * AP2_REALTIME_PRIME_MS) /
                                   1000ULL);
      }
      if (!pcm_rtp_ring_has_range(s.pcm_ring, guard_start, guard_frames,
                                  snap.pcm_generation)) {
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
        if (audio_playout_poll_completion(&probe_done)) {
          if (probe_done.generation == 0U && probe_done.rtp == silence_rtp) {
            have_probe = true;
            break;
          }
          /* No real-generation completion can exist yet. Ignore any
           * stale completion left over from a prior disabled epoch. */
        } else {
          taskYIELD();
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
      state = PLAYOUT_RUNNING;
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
      if (pcm_rtp_ring_read_256_conceal(
              s.pcm_ring, cursor_rtp, snap.pcm_generation, block, NULL)) {
        have_pcm = true;
      }
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
     * phase velocity is modest, the clock is held instead of chasing 0.000 ms.
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
      if (in_deadband && sync_slope_ms_s > -0.080 && sync_slope_ms_s < 0.080) {
        next_target = servo_ppm;
      }
      servo_target_ppm = next_target;
      diag_publish_sync(s.output_sync.us, servo_ppm, snap.generation);
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
        if (te == ESP_OK) servo_ppm = next_ppm;
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
  if (!s.diag_stop_sem) s.diag_stop_sem = xSemaphoreCreateBinary();
  if (!s.publish_mutex || !s.diag_stop_sem) return ESP_ERR_NO_MEM;

  ESP_LOGI(TAG,
           "PSRAM before audio alloc: total=%u KiB free=%u KiB largest=%u KiB",
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));

  /* Allocate the one large codec backing store first while PSRAM is least
   * fragmented. Buffered AAC uses all 5 MiB as its page payload store.
   * Realtime ALAC reuses the beginning of the same bytes for its raw PCM ring
   * and DATA/RTX pools, but only after the buffered transport is fully idle. */
  if (!s.codec_workspace) {
    s.codec_workspace_size = AP2_BUFFERED_STORE_REQUEST_BYTES;
    s.codec_workspace = heap_caps_malloc(
        s.codec_workspace_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.codec_workspace) s.codec_workspace = malloc(s.codec_workspace_size);
    if (!s.codec_workspace) return ESP_ERR_NO_MEM;
  }

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
  if (!s.realtime_stage_pcm) {
    s.realtime_stage_pcm = heap_caps_malloc(
        AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.realtime_stage_pcm)
      s.realtime_stage_pcm = malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
  }

  if (!s.transport) {
    ap2_buffered_transport_config_t tcfg = {
        .store_bytes = AP2_BUFFERED_STORE_REQUEST_BYTES,
        .task_core = AP2_NETWORK_CORE,
        .task_priority = AP2_RX_PRIORITY,
        .task_stack = AP2_RX_STACK,
    };
    ESP_RETURN_ON_ERROR(ap2_buffered_transport_create_with_payload_storage(
                            &s.transport, &tcfg, s.codec_workspace,
                            s.codec_workspace_size),
                        TAG, "buffered transport create failed");
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
  if (!s.packet || !s.decrypt_buf || !s.decode_pcm || !s.realtime_stage_pcm || !s.pcm_ring ||
      !s.realtime_stage_ring) {
    return ESP_ERR_NO_MEM;
  }

  pcm_rtp_ring_set_generation(s.pcm_ring, s.buffered_pcm_generation);
  pcm_rtp_ring_set_generation(s.realtime_stage_ring, s.generation);
  ESP_LOGI(TAG,
           "PSRAM after shared codec + final PCM: total=%u KiB free=%u KiB largest=%u KiB",
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));
  ESP_LOGI(TAG,
           "shared codec workspace=%u KiB: AAC payload=%u KiB; ALAC active use=%u KiB (raw PCM + DATA/RTX)",
           (unsigned)(s.codec_workspace_size / 1024U),
           (unsigned)(ap2_buffered_transport_capacity(s.transport) / 1024U),
           (unsigned)((rt_stage_bytes + rt_pool_bytes) / 1024U));
  ESP_LOGI(TAG,
           "PSRAM after audio stores: total=%u KiB free=%u KiB largest=%u KiB store=%u KiB",
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
           "PTP/RTP playout task started core=%d prio=%u block=%u buffered_guard=%ums realtime_prime=%ums",
           AP2_DECODE_CORE, (unsigned)AP2_PLAYOUT_PRIORITY,
           (unsigned)AUDIO_PLAYOUT_FRAMES,
           (unsigned)((AP2_START_PRIME_GUARD_BLOCKS * AUDIO_PLAYOUT_FRAMES *
                       1000U) / 44100U),
           (unsigned)AP2_REALTIME_PRIME_MS);
  if (xTaskCreatePinnedToCore(realtime_stage_task, "alac_stage",
                              AP2_RT_STAGE_STACK, NULL, AP2_RT_STAGE_PRIORITY,
                              &s.realtime_stage_task, AP2_DECODE_CORE) != pdPASS) {
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "AP2 buffered: TCP -> addressable compressed store -> media-order AAC scheduler -> PCM RTP ring");
  ESP_LOGI(TAG, "AP2 buffered: TCP payload recv writes directly into page-backed packet storage");
  ESP_LOGI(TAG, "AP2 buffered: compressed store=%u KiB (requested=%u KiB), decoded target=%u ms",
           (unsigned)(ap2_buffered_transport_capacity(s.transport) / 1024U),
           (unsigned)(AP2_BUFFERED_STORE_REQUEST_BYTES / 1024U),
           (unsigned)AP2_PCM_TARGET_MS);
  ESP_LOGI(TAG, "AP2 buffered: future packets remain addressable; only full store applies TCP backpressure");
  ESP_LOGI(TAG, "AP2 output: existing exact PTP/RTP PCM playout and I2S PID preserved");
  ESP_LOGI(TAG, "ALAC: immediate decode -> raw RTP PCM ring -> ordered EQ -> final PCM ring");
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
  if (changed) (void)audio_diag_stop_and_wait();
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
  ap2_buffered_transport_clear(s.transport);
  reset_buffered_pcm_store();

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
    if (s.transport && !ap2_buffered_transport_is_idle(s.transport)) {
      ESP_LOGE(TAG,
               "realtime start refused: buffered transport still owns shared codec workspace");
      return ESP_ERR_INVALID_STATE;
    }
    if (s.transport) ap2_buffered_transport_clear(s.transport);

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

  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  diag_invalidate_sync();
  (void)audio_diag_stop_and_wait();
  mark_timeline_discontinuity();
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
    ap2_buffered_transport_stop(s.transport);
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
    ap2_buffered_transport_clear(s.transport);
  }

  taskENTER_CRITICAL(&s.state_mux);
  taskEXIT_CRITICAL(&s.state_mux);

  /* Do not reset the stateful EQ asynchronously from the control core. AAC
   * and the ALAC staging task each reset it on their next generation. */
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

void audio_receiver_get_stats(audio_stats_t *out) { if (out) memset(out, 0, sizeof(*out)); }
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

  /* In the addressable model FLUSHBUFFERED is declarative invalidation, not a
   * packet-order state machine. Install [fromSeq, untilSeq) in the compressed
   * store so it applies retroactively and to later arrivals, and invalidate the
   * same sender-described media interval in decoded PCM. untilSeq itself is
   * deliberately preserved: our Automix logs show it can be the first packet
   * of replacement material, often restarting at flushFromTS. */
  if (s.transport) {
    esp_err_t err = ap2_buffered_transport_add_invalid_seq_range(
        s.transport, from_seq, until_seq);
    if (err != ESP_OK) {
      xSemaphoreGive(s.publish_mutex);
      return err;
    }
  }
  timing_snapshot_t flush_snap;
  snapshot_state(&flush_snap);
  pcm_rtp_ring_invalidate_range(s.pcm_ring, from_ts, until_ts,
                                flush_snap.pcm_generation);

  xSemaphoreGive(s.publish_mutex);
  return ESP_OK;
}

void audio_receiver_set_immediate_flush(uint32_t until_seq, uint32_t until_ts,
                                        bool has_endpoint) {
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  until_seq &= 0x007fffffU;

  /* Stop presentation immediately. This changes only the timing epoch; media
   * invalidation below is independent and never stops TCP ingestion. */
  mark_timeline_discontinuity();

  if (s.transport) {
    if (has_endpoint) {
      /* Declarative endpoint: everything before untilSeq belongs to the old
       * timeline. The rule remains active for future TCP arrivals until the
       * next anchor commits; untilSeq itself is preserved. */
      (void)ap2_buffered_transport_invalidate_before_seq(
          s.transport, until_seq);
    } else {
      /* No endpoint means all currently buffered/provisional material belongs
       * to the old timeline. Keep receiving TCP, but catalog new packets as
       * INVALID until the next anchor retires this rule. */
      (void)ap2_buffered_transport_invalidate_all(s.transport);
    }
  }

  timing_snapshot_t flush_snap;
  snapshot_state(&flush_snap);
  if (has_endpoint) {
    /* The protocol says immediate FLUSH discards buffered media up to the
     * supplied endpoint. Preserve later and historical PCM outside that
     * boundary; a backward anchor can therefore reuse still-valid samples. */
    pcm_rtp_ring_invalidate_before(s.pcm_ring, until_ts,
                                   flush_snap.pcm_generation);
  } else {
    /* No endpoint is an explicit full media flush. This is one of the few
     * buffered control operations allowed to invalidate the entire PCM store. */
    reset_buffered_pcm_store();
  }

  xSemaphoreGive(s.publish_mutex);
}

void audio_receiver_pause(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  diag_invalidate_sync();
  (void)audio_diag_stop_and_wait();
  mark_timeline_discontinuity();
}

void audio_receiver_set_playout_latency_samples(uint32_t v) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playout_latency_samples = v;
  taskEXIT_CRITICAL(&s.state_mux);
}
uint32_t audio_receiver_get_hardware_latency_us(void) { return audio_playout_hardware_latency_us(); }

void audio_receiver_set_playing(bool p) {
  audio_stream_type_t stream_type;
  taskENTER_CRITICAL(&s.state_mux);
  s.playing = p;
  stream_type = s.stream_type;
  taskEXIT_CRITICAL(&s.state_mux);
  if (p) {
    audio_diag_start(stream_type);
  } else {
    diag_invalidate_sync();
    (void)audio_diag_stop_and_wait();
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
  uint32_t revision;
  bool committed;
  ap2_buffered_transport_begin_media_update(s.transport);
  taskENTER_CRITICAL(&s.state_mux);
  committed = s.timeline_reset_pending;
  gen = commit_anchor_epoch_locked();
  revision = s.media_revision = next_generation(s.media_revision);
  s.anchor_clock_id = clock_id;
  s.anchor_ptp_ns = ptp_ns;
  s.anchor_local_ns = 0; /* buffered/AAC remains PTP-authoritative */
  s.anchor_rtp = rtp;
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  s.anchor_valid = true;
  taskEXIT_CRITICAL(&s.state_mux);
  ap2_buffered_transport_end_media_update(s.transport, revision);
  if (committed && s.transport) {
    /* Rules describe the previous control timeline. Matching packets already
     * marked INVALID stay invalid; only the rule table is retired so new
     * timeline traffic is not judged by an old FLUSHBUFFERED range. */
    ap2_buffered_transport_clear_invalidation_rules(s.transport);
  }
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
