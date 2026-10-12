#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_out_sample.h"

/* I2S playout backend: two 256-frame DMA descriptors. Every application audio
 * block is tagged with its RTP/generation before it reaches DMA. The TX EOF
 * ISR records the local completion timestamp for that exact tag, allowing the
 * playout task to compare the physical DMA completion edge against AirPlay PTP.
 */

#define AUDIO_PLAYOUT_FRAMES 256U

typedef struct {
  uint32_t rtp;
  uint32_t generation;
  uint32_t frames;
  int64_t done_local_us;
} audio_playout_completion_t;

typedef struct {
  int32_t requested_ppm;
  int32_t actual_delta_mclk_hz;
  uint32_t curr_mclk_hz;
} audio_playout_tune_info_t;



esp_err_t audio_playout_init(void);
esp_err_t audio_playout_flush(void);

/* Preload is valid only while the TX channel is disabled/READY. It is used for
 * the first two blocks of an epoch so the channel starts with known audio in
 * both DMA descriptors instead of clocking zero-filled descriptors first. */
esp_err_t audio_playout_preload_tagged(const audio_out_sample_t *stereo,
                                       uint32_t frames,
                                       uint32_t rtp, uint32_t generation);

/* Enable after the first two blocks have been preloaded and the PTP start edge
 * has been reached. */
esp_err_t audio_playout_enable(void);

/* Normal steady-state write. The RTP tag is paired FIFO-wise with the TX EOF
 * callback for the descriptor that physically finished sending. */
esp_err_t audio_playout_write_tagged(const audio_out_sample_t *stereo,
                                     uint32_t frames,
                                     uint32_t rtp, uint32_t generation);

/* Drain one completed tagged DMA block. done_local_us comes from
 * esp_timer_get_time() inside the TX ISR. */
bool audio_playout_poll_completion(audio_playout_completion_t *out);

/* Sticky until a successful flush. Never use completions after a FIFO fault. */
bool audio_playout_has_fault(void);
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
/* Counts since the last call: DMA underruns, EOF tag faults. */
void audio_playout_diag_take(uint32_t *underruns, uint32_t *tag_faults);
#endif
/* Single playout owner only; timeout is in FreeRTOS ticks. */
bool audio_playout_wait_completion(audio_playout_completion_t *out,
                                    uint32_t timeout_ticks);

bool audio_playout_is_enabled(void);

/* Fine tune the TX master clock of a running channel: disable -> tune ->
 * enable (the IDF allows i2s_channel_tune_rate() only in the READY state).
 * Call only from the playout task between writes. target_ppm is relative to
 * the nominal MCLK measured at init (+/-160 ppm). */
esp_err_t audio_playout_tune_ppm(int32_t target_ppm,
                                 audio_playout_tune_info_t *out);

/* Same, but reloads the two DMA descriptors in play order before enable():
 * `playing` is the block the DMA was sending, `next` the block queued after
 * it (the last one written). Call right after a write returned, so only a
 * few frames of `playing` are repeated. halt_us = how long TX was stopped. */
esp_err_t audio_playout_tune_ppm_in_order(int32_t target_ppm,
                                          const audio_out_sample_t *playing,
                                          const audio_out_sample_t *next,
                                          uint32_t frames,
                                          audio_playout_tune_info_t *out,
                                          uint32_t *halt_us);
/* Reset the physical MCLK correction to nominal while preserving the current
 * enabled/disabled channel state. Intended for a hard audio-session boundary,
 * not for normal track/anchor changes inside one session. */
esp_err_t audio_playout_reset_tune(void);

/* True when retunes rewrite only the fractional MCLK divider register while
 * I2S keeps running (the normal case; checked at init). */
bool audio_playout_tune_is_live(void);
