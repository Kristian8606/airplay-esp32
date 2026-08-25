#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_receiver.h"
#include "esp_err.h"

typedef bool (*realtime_pcm_sink_t)(uint32_t rtp, int16_t *pcm,
                                    size_t frames, int channels, void *ctx);

/* Called when the realtime RTP receiver can no longer recover continuity
 * inside its bounded reorder window. The owner must invalidate the current
 * PCM/timing epoch; the first subsequently decoded packet will establish a
 * fresh realtime anchor. */
typedef void (*realtime_resync_cb_t)(void *ctx);

/* Return the remaining time until the first sample at `rtp` reaches the
 * physical playout deadline. False means that no valid PTP/RTP timeline is
 * available yet (for example before the first realtime local anchor). */
typedef bool (*realtime_deadline_cb_t)(uint32_t rtp,
                                       int64_t *time_to_play_us, void *ctx);


typedef struct {
  uint32_t rx_packets;
  uint32_t gap_events;
  uint32_t missing_packets;
  uint32_t nack_requests;
  uint32_t retransmit_packets;
  uint32_t retransmit_bad;
  uint32_t reorder_late;
  uint32_t reorder_overwrite;
  uint32_t gap_skips;
  uint32_t resend_retries;
  uint32_t resend_giveups;
  uint32_t hard_resyncs;
  uint32_t select_errors;
  uint32_t recv_errors;
  uint32_t nack_send_errors;
  uint32_t processing_samples;
  uint64_t processing_sum_us;
  uint32_t interval_max_processing_us;
  uint32_t interval_max_control_us;
  uint32_t interval_max_interarrival_us;
  uint16_t interval_max_gap_packets;
  uint32_t rtx_latency_samples;
  uint64_t rtx_latency_sum_us;
  uint32_t rtx_latency_min_us;
  uint32_t rtx_latency_max_us;

  /* Passive PT=84 sync diagnostics. These fields are observational only and
   * never feed the realtime playout timeline. AirPlay sync packets carry two
   * RTP timestamps plus a 32.32 source/network time value. */
  uint32_t sync_packets;
  uint32_t sync_malformed;
  uint16_t last_sync_flags;
  uint32_t last_sync_rtp_less_latency;
  uint32_t last_sync_rtp;
  uint32_t last_sync_latency_frames;
  uint32_t last_sync_time_seconds;
  uint32_t last_sync_time_fraction;
} realtime_receiver_diag_t;

typedef struct {
  audio_format_t format;
  audio_encrypt_t encrypt;
  realtime_pcm_sink_t pcm_sink;
  void *pcm_sink_ctx;
  realtime_resync_cb_t resync_cb;
  void *resync_ctx;
  realtime_deadline_cb_t deadline_cb;
  void *deadline_ctx;
} realtime_receiver_config_t;

esp_err_t realtime_receiver_start(uint16_t data_port, uint16_t control_port,
                                  const realtime_receiver_config_t *config);
void realtime_receiver_stop(void);
bool realtime_receiver_is_running(void);
void realtime_receiver_get_diag(realtime_receiver_diag_t *out, bool reset_interval_peaks);
void realtime_receiver_set_client_control(uint32_t client_ip, uint16_t client_control_port);
