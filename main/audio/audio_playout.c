#include "audio_playout.h"
#include "sdkconfig.h"

#include <stddef.h>
#include <inttypes.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "audio_diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led.h"

#define TAG "audio_playout"

#define I2S_DMA_DESC_NUM  2U
#define I2S_DMA_FRAME_NUM AUDIO_PLAYOUT_FRAMES
#define I2S_RATE_HZ       44100U
#define TX_TAG_Q_CAP      8U
#define TX_DONE_Q_CAP     8U

/* Allocate the I2S/GDMA channel from Core1 so its external interrupt is
 * serviced on the same core as the playout task, away from Core0 network/PTP
 * traffic. This helper task exists only during boot-time initialisation. */
#define AUDIO_PLAYOUT_INIT_CORE  1
#define AUDIO_PLAYOUT_INIT_STACK 4096U
#define AUDIO_PLAYOUT_INIT_PRIO  5U

typedef struct {
  uint32_t rtp;
  uint32_t generation;
  uint32_t frames;
} tx_tag_t;

static i2s_chan_handle_t s_tx;
static bool s_enabled;
/* True when READY-state DMA descriptors contain data loaded by preload but the
 * channel has not yet been enabled. IDF concatenates repeated preloads until
 * the DMA cache is full, so an aborted startup must explicitly consume/reset
 * this state before retrying. */
static bool s_preload_pending;
static uint32_t s_nominal_mclk_hz;
static int32_t s_tune_ppm;

/* SPSC queues: playout task -> TX ISR for tags; TX ISR -> playout task for
 * completions. Capacity is intentionally larger than the two DMA descriptors
 * so the producer can publish the next tag before a blocking write wakes on
 * the previous descriptor's EOF. */
static DRAM_ATTR tx_tag_t s_tag_q[TX_TAG_Q_CAP];
static DRAM_ATTR uint32_t s_tag_head;
static DRAM_ATTR uint32_t s_tag_tail;
static DRAM_ATTR audio_playout_completion_t s_done_q[TX_DONE_Q_CAP];
static DRAM_ATTR uint32_t s_done_head;
static DRAM_ATTR uint32_t s_done_tail;

static inline void queues_reset(void) {
  __atomic_store_n(&s_tag_head, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_tag_tail, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_done_head, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_done_tail, 0U, __ATOMIC_RELEASE);
}

static bool tag_push(uint32_t rtp, uint32_t generation, uint32_t frames) {
  const uint32_t head = __atomic_load_n(&s_tag_head, __ATOMIC_RELAXED);
  const uint32_t tail = __atomic_load_n(&s_tag_tail, __ATOMIC_ACQUIRE);
  const uint32_t next = (head + 1U) % TX_TAG_Q_CAP;
  if (next == tail) {
    return false;
  }
  s_tag_q[head].rtp = rtp;
  s_tag_q[head].generation = generation;
  s_tag_q[head].frames = frames;
  __atomic_store_n(&s_tag_head, next, __ATOMIC_RELEASE);
  return true;
}

static bool IRAM_ATTR tag_pop_isr(tx_tag_t *out) {
  const uint32_t tail = __atomic_load_n(&s_tag_tail, __ATOMIC_RELAXED);
  const uint32_t head = __atomic_load_n(&s_tag_head, __ATOMIC_ACQUIRE);
  if (tail == head) {
    return false;
  }
  /* Keep this copy explicit: an IRAM ISR must not acquire an out-of-line
   * memcpy helper from flash. */
  out->rtp = s_tag_q[tail].rtp;
  out->generation = s_tag_q[tail].generation;
  out->frames = s_tag_q[tail].frames;
  __atomic_store_n(&s_tag_tail, (tail + 1U) % TX_TAG_Q_CAP,
                   __ATOMIC_RELEASE);
  return true;
}

static bool IRAM_ATTR done_push_isr(const tx_tag_t *tag, int64_t local_us) {
  const uint32_t head = __atomic_load_n(&s_done_head, __ATOMIC_RELAXED);
  const uint32_t tail = __atomic_load_n(&s_done_tail, __ATOMIC_ACQUIRE);
  const uint32_t next = (head + 1U) % TX_DONE_Q_CAP;
  if (next == tail) {
    return false;
  }
  s_done_q[head].rtp = tag->rtp;
  s_done_q[head].generation = tag->generation;
  s_done_q[head].frames = tag->frames;
  s_done_q[head].done_local_us = local_us;
  __atomic_store_n(&s_done_head, next, __ATOMIC_RELEASE);
  return true;
}

static bool IRAM_ATTR on_sent(i2s_chan_handle_t handle,
                              i2s_event_data_t *event,
                              void *user_ctx) {
  (void)handle;
  (void)event;
  (void)user_ctx;

  tx_tag_t tag;
  if (!tag_pop_isr(&tag)) return false;

  /* esp_timer_get_time() is lock-free and documented for ISR use. Timestamp
   * the DMA EOF itself; conversion to PTP is done later in task context. */
  const int64_t done_local_us = esp_timer_get_time();
  (void)done_push_isr(&tag, done_local_us);
  return false;
}


static esp_err_t audio_playout_init_current_core(void) {
  if (s_tx) {
    return ESP_OK;
  }

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
  chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
  chan_cfg.auto_clear = true;

  esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
    return err;
  }

  i2s_std_config_t cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_RATE_HZ),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = CONFIG_I2S_SCK_IO,
          .bclk = CONFIG_I2S_BCK_IO,
          .ws = CONFIG_I2S_WS_IO,
          .dout = CONFIG_I2S_DO_IO,
          .din = I2S_GPIO_UNUSED,
      },
  };

  err = i2s_channel_init_std_mode(s_tx, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
    return err;
  }

  const i2s_event_callbacks_t callbacks = {
      .on_recv = NULL,
      .on_recv_q_ovf = NULL,
      .on_sent = on_sent,
      .on_send_q_ovf = NULL,
  };
  err = i2s_channel_register_event_callback(s_tx, &callbacks, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s callback registration failed: %s", esp_err_to_name(err));
    return err;
  }

  queues_reset();
  s_enabled = false;
  s_preload_pending = false;
  s_tune_ppm = 0;

  /* The channel is READY here, which is the state required by
   * i2s_channel_tune_rate(). Capture the actual nominal MCLK so ppm
   * corrections are relative to the hardware clock chosen by IDF. */
  i2s_tuning_info_t initial_tune = {0};
  err = i2s_channel_tune_rate(s_tx, NULL, &initial_tune);
  if (err == ESP_OK) {
    s_nominal_mclk_hz = initial_tune.curr_mclk_hz;
  } else {
    s_nominal_mclk_hz = I2S_RATE_HZ * 256U;
    ESP_LOGW(TAG, "initial tune query failed: %s; assuming MCLK=%" PRIu32,
             esp_err_to_name(err), s_nominal_mclk_hz);
  }

  /* Keep the channel READY/disabled. Preload both DMA descriptors before
   * the exact PTP start edge, then enables the channel. This removes the
   * zero-descriptor ambiguity that made earlier EOF counting unreliable. */
  AUDIO_DIAG_PLAYOUT_I2S(
      s_nominal_mclk_hz, (uint32_t)I2S_DMA_DESC_NUM,
      (uint32_t)I2S_DMA_FRAME_NUM,
#if CONFIG_I2S_ISR_IRAM_SAFE
      1U
#else
      0U
#endif
  );
  return ESP_OK;
}

typedef struct {
  SemaphoreHandle_t done;
  esp_err_t result;
} audio_playout_init_ctx_t;

static void audio_playout_init_task(void *arg) {
  audio_playout_init_ctx_t *ctx = (audio_playout_init_ctx_t *)arg;
  ctx->result = audio_playout_init_current_core();
  xSemaphoreGive(ctx->done);
  vTaskDelete(NULL);
}

esp_err_t audio_playout_init(void) {
  if (s_tx) {
    return ESP_OK;
  }

#if CONFIG_FREERTOS_UNICORE
  return audio_playout_init_current_core();
#else
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) {
    return ESP_ERR_NO_MEM;
  }

  audio_playout_init_ctx_t ctx = {
      .done = done,
      .result = ESP_FAIL,
  };

  BaseType_t created = xTaskCreatePinnedToCore(
      audio_playout_init_task, "i2s_init", AUDIO_PLAYOUT_INIT_STACK, &ctx,
      AUDIO_PLAYOUT_INIT_PRIO, NULL, AUDIO_PLAYOUT_INIT_CORE);
  if (created != pdPASS) {
    vSemaphoreDelete(done);
    return ESP_ERR_NO_MEM;
  }

  (void)xSemaphoreTake(done, portMAX_DELAY);
  const esp_err_t result = ctx.result;
  vSemaphoreDelete(done);
  return result;
#endif
}

esp_err_t audio_playout_flush(void) {
  if (!s_tx) {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_enabled) {
    esp_err_t de = i2s_channel_disable(s_tx);
    if (de == ESP_OK || de == ESP_ERR_INVALID_STATE) {
      s_enabled = false;
      s_preload_pending = false;
    } else {
      return de;
    }
  } else if (s_preload_pending) {
    /* There is no public "discard preload" API. A failed realtime timeline
     * revalidation can happen after both silent descriptors were preloaded but
     * before normal enable. Leaving them there makes every later preload append
     * into an already-full DMA cache and PRIME can then persist until reboot.
     * Cycle READY -> RUNNING -> READY with silence only; this consumes/resets
     * the driver's preloaded DMA state without exposing stale program audio. */
    esp_err_t ee = i2s_channel_enable(s_tx);
    if (ee == ESP_OK) {
      s_enabled = true;
      esp_err_t de = i2s_channel_disable(s_tx);
      if (de == ESP_OK || de == ESP_ERR_INVALID_STATE) {
        s_enabled = false;
        s_preload_pending = false;
      } else {
        return de;
      }
    } else {
      /* If enable says the driver is not READY, try to converge it to READY.
       * This is exceptional; converge the channel back to READY. */
      esp_err_t de = i2s_channel_disable(s_tx);
      if (de == ESP_OK || de == ESP_ERR_INVALID_STATE) {
        s_enabled = false;
        s_preload_pending = false;
      } else {
        return de;
      }
    }
  }

  queues_reset();
  return ESP_OK;
}

esp_err_t audio_playout_preload_tagged(const int16_t *stereo, uint32_t frames,
                                       uint32_t rtp, uint32_t generation) {
  if (!s_tx || !stereo || frames == 0U || s_enabled) {
    return ESP_ERR_INVALID_STATE;
  }
  size_t loaded = 0;
  const size_t bytes = (size_t)frames * 2U * sizeof(int16_t);
  esp_err_t err = i2s_channel_preload_data(s_tx, stereo, bytes, &loaded);
  if (loaded > 0U) s_preload_pending = true;
  if (err != ESP_OK || loaded != bytes) {
    return err == ESP_OK ? ESP_ERR_NO_MEM : err;
  }
  if (!tag_push(rtp, generation, frames)) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t audio_playout_enable(void) {
  if (!s_tx) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_enabled) {
    return ESP_OK;
  }
  esp_err_t err = i2s_channel_enable(s_tx);
  if (err == ESP_OK) {
    s_enabled = true;
    s_preload_pending = false;
  }
  return err;
}

esp_err_t audio_playout_write_tagged(const int16_t *stereo, uint32_t frames,
                                     uint32_t rtp, uint32_t generation) {
  if (!s_tx || !s_enabled || !stereo || frames == 0U) {
    return ESP_ERR_INVALID_STATE;
  }

  /* Publish the tag before entering the blocking write. With two descriptors
   * there are already outstanding tagged blocks while running, so an EOF that
   * wakes this write always consumes the oldest earlier tag, never this one. */
  if (!tag_push(rtp, generation, frames)) {
    return ESP_ERR_NO_MEM;
  }

  size_t written = 0;
  const size_t bytes = (size_t)frames * 2U * sizeof(int16_t);
  esp_err_t err = i2s_channel_write(s_tx, stereo, bytes, &written,
                                    portMAX_DELAY);


  if (err == ESP_OK && written == bytes) {
    /* RGB/VU is fed from the final I2S submission path. When disabled
     * in menuconfig this compiles to a no-op. */
    led_audio_feed(stereo, frames);
    return ESP_OK;
  }

  /* A failed write invalidates the FIFO relationship between application tags
   * and DMA descriptors. Caller will flush/re-prime the channel. */
  return err == ESP_OK ? ESP_FAIL : err;
}

bool audio_playout_poll_completion(audio_playout_completion_t *out) {
  if (!out) {
    return false;
  }
  const uint32_t tail = __atomic_load_n(&s_done_tail, __ATOMIC_RELAXED);
  const uint32_t head = __atomic_load_n(&s_done_head, __ATOMIC_ACQUIRE);
  if (tail == head) {
    return false;
  }
  *out = s_done_q[tail];
  __atomic_store_n(&s_done_tail, (tail + 1U) % TX_DONE_Q_CAP,
                   __ATOMIC_RELEASE);
  return true;
}

bool audio_playout_is_enabled(void) {
  return s_enabled;
}


esp_err_t audio_playout_reset_tune(void) {
  if (!s_tx || s_nominal_mclk_hz == 0U) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_tune_ppm == 0) {
    return ESP_OK;
  }

  /* ADDSUB is relative to the current MCLK. Query the actual clock first and
   * return it to the nominal value. This avoids retaining a few Hz of rounding
   * residue after several incremental ppm changes in the previous session. */
  i2s_tuning_info_t before = {0};
  esp_err_t query_err = i2s_channel_tune_rate(s_tx, NULL, &before);
  const int32_t step_delta_hz = query_err == ESP_OK
      ? (int32_t)s_nominal_mclk_hz - (int32_t)before.curr_mclk_hz
      : (int32_t)(-((int64_t)s_nominal_mclk_hz *
                    (int64_t)s_tune_ppm) / 1000000LL);
  const int32_t limit_hz = (int32_t)(
      ((int64_t)s_nominal_mclk_hz * 160LL) / 1000000LL) + 4;
  i2s_tuning_config_t cfg = {
      .tune_mode = I2S_TUNING_MODE_ADDSUB,
      .tune_mclk_val = step_delta_hz,
      .max_delta_mclk = limit_hz,
      .min_delta_mclk = -limit_hz,
  };
  i2s_tuning_info_t info = {0};
  esp_err_t err = i2s_channel_tune_rate(s_tx, &cfg, &info);
  if (err == ESP_OK) {
    s_tune_ppm = 0;
  }

  return err;
}

int32_t audio_playout_get_tune_ppm(void) {
  return s_tune_ppm;
}

uint32_t audio_playout_get_nominal_mclk_hz(void) {
  return s_nominal_mclk_hz;
}

esp_err_t audio_playout_tune_ppm(int32_t target_ppm,
                                 audio_playout_tune_info_t *out) {
  if (!s_tx || !s_enabled || s_nominal_mclk_hz == 0U) {
    return ESP_ERR_INVALID_STATE;
  }
  if (target_ppm > 160) target_ppm = 160;
  if (target_ppm < -160) target_ppm = -160;
  if (target_ppm == s_tune_ppm) {
    if (out) {
      out->requested_ppm = s_tune_ppm;
      out->curr_mclk_hz = (uint32_t)((int64_t)s_nominal_mclk_hz +
          ((int64_t)s_nominal_mclk_hz * s_tune_ppm) / 1000000LL);
      out->actual_delta_mclk_hz = (int32_t)out->curr_mclk_hz -
                                   (int32_t)s_nominal_mclk_hz;
    }
    return ESP_OK;
  }

  /* ADDSUB is relative to the CURRENT clock, so apply only the requested
   * ppm step. The max/min bounds remain relative to the initial MCLK. */
  const int32_t step_ppm = target_ppm - s_tune_ppm;
  const int32_t step_delta_hz = (int32_t)(
      ((int64_t)s_nominal_mclk_hz * step_ppm) / 1000000LL);
  const int32_t limit_hz = (int32_t)(
      ((int64_t)s_nominal_mclk_hz * 160LL) / 1000000LL) + 4;
  i2s_tuning_config_t cfg = {
      .tune_mode = I2S_TUNING_MODE_ADDSUB,
      .tune_mclk_val = step_delta_hz,
      .max_delta_mclk = limit_hz,
      .min_delta_mclk = -limit_hz,
  };
  i2s_tuning_info_t info = {0};

  /* i2s_channel_tune_rate() is the IDF runtime fine-tuning API. Keep
   * the active DMA ring running so clock correction cannot reset TX/GDMA
   * state or disturb the application tag-to-EOF chronology. */
  esp_err_t err = i2s_channel_tune_rate(s_tx, &cfg, &info);
  if (err == ESP_OK) {
    s_tune_ppm = target_ppm;
  }

  if (out) {
    out->requested_ppm = s_tune_ppm;
    out->curr_mclk_hz = info.curr_mclk_hz;
    out->actual_delta_mclk_hz = info.delta_mclk_hz;
  }
  return err;
}

uint32_t audio_playout_hardware_latency_us(void) {
  return (uint32_t)(((uint64_t)I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM *
                     1000000ULL) / I2S_RATE_HZ);
}
