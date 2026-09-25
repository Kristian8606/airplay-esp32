#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * Temporary AirPlay diagnostics facade.
 *
 * Production audio_status is intentionally NOT part of this module.
 * Every temporary counter/timestamp/array/buffer/helper/task must either live
 * in audio_diag.c or be enclosed by the matching CONFIG_AIRPLAY_DIAG_* guard.
 *
 * When the master switch/category is disabled, the macros below erase both the
 * call and its arguments at preprocessing time. Expensive expressions are not
 * evaluated and no diagnostic state is linked into the firmware.
 */

#if defined(CONFIG_AIRPLAY_DIAG_ACTIVE) && CONFIG_AIRPLAY_DIAG_ACTIVE
esp_err_t audio_diag_init(void);
#define AUDIO_DIAG_INIT() audio_diag_init()
#else
#define AUDIO_DIAG_INIT() (ESP_OK)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
typedef enum {
  AUDIO_DIAG_TASK_AAC_PROCESSOR = 1,
  AUDIO_DIAG_TASK_ALAC_STAGE,
  AUDIO_DIAG_TASK_PLAYOUT,
  AUDIO_DIAG_TASK_RT_DATA,
  AUDIO_DIAG_TASK_RT_CTRL,
  AUDIO_DIAG_TASK_RT_WORK,
  AUDIO_DIAG_TASK_RT_RESEND,
  AUDIO_DIAG_TASK_TCP_READER,
} audio_diag_task_id_t;
#endif

#if defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER
typedef enum {
  AUDIO_DIAG_PSRAM_BEFORE_AUDIO = 1,
  AUDIO_DIAG_PSRAM_AFTER_SHARED,
  AUDIO_DIAG_PSRAM_AFTER_STORES,
} audio_diag_psram_stage_t;

typedef enum {
  AUDIO_DIAG_PCM_RING_FINAL = 1,
  AUDIO_DIAG_PCM_RING_ALAC_STAGE,
} audio_diag_pcm_ring_id_t;
#endif

#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
typedef enum {
  AUDIO_DIAG_SYNC_RX_SYNC = 1,
  AUDIO_DIAG_SYNC_RX_FOLLOWUP,
  AUDIO_DIAG_SYNC_RX_ANNOUNCE,
  AUDIO_DIAG_SYNC_REJECTED_SOURCE,
  AUDIO_DIAG_SYNC_OUTLIER,
  AUDIO_DIAG_SYNC_PAIR_OK,
  AUDIO_DIAG_SYNC_ORPHAN_FOLLOWUP,
  AUDIO_DIAG_SYNC_PAIR_MISMATCH,
} audio_diag_sync_counter_t;
#endif

#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
void audio_diag_lifecycle_task_started(audio_diag_task_id_t task_id, int core,
                                       uint32_t priority, uint32_t extra);
void audio_diag_lifecycle_realtime_stopped(void);
void audio_diag_lifecycle_realtime_epoch(uint32_t generation);
#define AUDIO_DIAG_LIFECYCLE_TASK_STARTED(...) \
  audio_diag_lifecycle_task_started(__VA_ARGS__)
#define AUDIO_DIAG_LIFECYCLE_REALTIME_STOPPED() \
  audio_diag_lifecycle_realtime_stopped()
#define AUDIO_DIAG_LIFECYCLE_REALTIME_EPOCH(...) \
  audio_diag_lifecycle_realtime_epoch(__VA_ARGS__)
#else
#define AUDIO_DIAG_LIFECYCLE_TASK_STARTED(...) do {} while (0)
#define AUDIO_DIAG_LIFECYCLE_REALTIME_STOPPED() do {} while (0)
#define AUDIO_DIAG_LIFECYCLE_REALTIME_EPOCH(...) do {} while (0)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER
void audio_diag_buffer_psram(audio_diag_psram_stage_t stage,
                             uint32_t total_kib, uint32_t free_kib,
                             uint32_t largest_kib, uint32_t store_kib);
void audio_diag_buffer_workspace(uint32_t workspace_kib, uint32_t aac_kib,
                                 uint32_t alac_kib);
void audio_diag_buffer_pcm_ring(audio_diag_pcm_ring_id_t ring_id,
                                uint32_t slots, uint32_t slot_frames,
                                uint32_t ring_frames, uint32_t bytes);
void audio_diag_buffer_packet_workspace(uint32_t data_kib, uint32_t rtx_kib,
                                        uint32_t total_kib);
/* Buffered AAC final-ring miss diagnostics. This hook is called only after
 * the normal exact 256-frame read has failed. */
void audio_diag_buffer_playout_miss(uint32_t conceal_ok,
                                    uint32_t missing_frames,
                                    uint32_t contiguous_before_gap,
                                    uint32_t transition);
#define AUDIO_DIAG_BUFFER_PSRAM(...) audio_diag_buffer_psram(__VA_ARGS__)
#define AUDIO_DIAG_BUFFER_WORKSPACE(...) audio_diag_buffer_workspace(__VA_ARGS__)
#define AUDIO_DIAG_BUFFER_PCM_RING(...) audio_diag_buffer_pcm_ring(__VA_ARGS__)
#define AUDIO_DIAG_BUFFER_PACKET_WORKSPACE(...) \
  audio_diag_buffer_packet_workspace(__VA_ARGS__)
#define AUDIO_DIAG_BUFFER_PLAYOUT_MISS(...) \
  audio_diag_buffer_playout_miss(__VA_ARGS__)
#else
#define AUDIO_DIAG_BUFFER_PSRAM(...) do {} while (0)
#define AUDIO_DIAG_BUFFER_WORKSPACE(...) do {} while (0)
#define AUDIO_DIAG_BUFFER_PCM_RING(...) do {} while (0)
#define AUDIO_DIAG_BUFFER_PACKET_WORKSPACE(...) do {} while (0)
#define AUDIO_DIAG_BUFFER_PLAYOUT_MISS(...) do {} while (0)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_CODEC) && CONFIG_AIRPLAY_DIAG_CODEC
void audio_diag_codec_aac_ready(uint32_t sample_rate, uint32_t channels);
#define AUDIO_DIAG_CODEC_AAC_READY(...) audio_diag_codec_aac_ready(__VA_ARGS__)
#else
#define AUDIO_DIAG_CODEC_AAC_READY(...) do {} while (0)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
void audio_diag_playout_i2s(uint32_t mclk_hz, uint32_t dma_desc,
                            uint32_t dma_frames, uint32_t isr_iram_safe);
#define AUDIO_DIAG_PLAYOUT_I2S(...) audio_diag_playout_i2s(__VA_ARGS__)
#else
#define AUDIO_DIAG_PLAYOUT_I2S(...) do {} while (0)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_TRANSPORT) && CONFIG_AIRPLAY_DIAG_TRANSPORT
void audio_diag_transport_socket_buffer(uint32_t requested, uint32_t actual);
void audio_diag_transport_ports(uint32_t data_port, uint32_t control_port);
void audio_diag_transport_retransmit_target(uint32_t ip_be, uint32_t port);
/* Buffered AAC/TCP hot-path statistics. All state is owned by audio_diag.c.
 * These hooks are compiled out completely with TRANSPORT diagnostics off. */
void audio_diag_transport_aac_session_reset(void);
void audio_diag_transport_aac_rx_block(uint32_t payload_bytes);
void audio_diag_transport_aac_store_wait_begin(void);
void audio_diag_transport_aac_store_wait_end(void);
/* Decoder-cursor diagnostics. Normal exact hits stay O(1); these hooks only
 * count why the cursor waited or entered the recovery-only descriptor scan. */
#define AUDIO_DIAG_TRANSPORT_SOCKET_BUFFER(...) \
  audio_diag_transport_socket_buffer(__VA_ARGS__)
#define AUDIO_DIAG_TRANSPORT_PORTS(...) audio_diag_transport_ports(__VA_ARGS__)
#define AUDIO_DIAG_TRANSPORT_RETRANSMIT_TARGET(...) \
  audio_diag_transport_retransmit_target(__VA_ARGS__)
#define AUDIO_DIAG_TRANSPORT_AAC_SESSION_RESET() \
  audio_diag_transport_aac_session_reset()
#define AUDIO_DIAG_TRANSPORT_AAC_RX_BLOCK(...) \
  audio_diag_transport_aac_rx_block(__VA_ARGS__)
#define AUDIO_DIAG_TRANSPORT_AAC_STORE_WAIT_BEGIN() \
  audio_diag_transport_aac_store_wait_begin()
#define AUDIO_DIAG_TRANSPORT_AAC_STORE_WAIT_END() \
  audio_diag_transport_aac_store_wait_end()
#else
#define AUDIO_DIAG_TRANSPORT_SOCKET_BUFFER(...) do {} while (0)
#define AUDIO_DIAG_TRANSPORT_PORTS(...) do {} while (0)
#define AUDIO_DIAG_TRANSPORT_RETRANSMIT_TARGET(...) do {} while (0)
#define AUDIO_DIAG_TRANSPORT_AAC_SESSION_RESET() do {} while (0)
#define AUDIO_DIAG_TRANSPORT_AAC_RX_BLOCK(...) do {} while (0)
#define AUDIO_DIAG_TRANSPORT_AAC_STORE_WAIT_BEGIN() do {} while (0)
#define AUDIO_DIAG_TRANSPORT_AAC_STORE_WAIT_END() do {} while (0)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
void audio_diag_sync_count(audio_diag_sync_counter_t counter);
void audio_diag_sync_gap_ns(int64_t gap_ns);
#define AUDIO_DIAG_SYNC_COUNT(...) audio_diag_sync_count(__VA_ARGS__)
#define AUDIO_DIAG_SYNC_GAP_NS(...) audio_diag_sync_gap_ns(__VA_ARGS__)
#else
#define AUDIO_DIAG_SYNC_COUNT(...) do {} while (0)
#define AUDIO_DIAG_SYNC_GAP_NS(...) do {} while (0)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_FLUSH) && CONFIG_AIRPLAY_DIAG_FLUSH
/* Seek / RTSP-control latency diagnostics. All timestamps and state live in
 * audio_diag.c; call sites compile to nothing when FLUSH diagnostics are off. */
void audio_diag_flush_rtsp_session_reset(int socket);
void audio_diag_flush_rtsp_request_begin(int socket, const char *method);
void audio_diag_flush_rtsp_request_end(int socket, const char *method);
void audio_diag_flush_control_rx_begin(int socket);
void audio_diag_flush_control_rx_header_done(int socket);
void audio_diag_flush_control_rx_payload_done(int socket);
void audio_diag_flush_control_rx_end(int socket, uint32_t plaintext_bytes);
void audio_diag_flush_status_wait_begin(void);
void audio_diag_flush_status_wait_end(uint32_t timed_out);
void audio_diag_flush_immediate_begin(void);
void audio_diag_flush_immediate_publish_acquired(void);
void audio_diag_flush_immediate_transport_done(void);
void audio_diag_flush_immediate_pcm_done(void);
void audio_diag_flush_immediate_end(void);
#define AUDIO_DIAG_FLUSH_RTSP_SESSION_RESET(...) \
  audio_diag_flush_rtsp_session_reset(__VA_ARGS__)
#define AUDIO_DIAG_FLUSH_RTSP_BEGIN(...) \
  audio_diag_flush_rtsp_request_begin(__VA_ARGS__)
#define AUDIO_DIAG_FLUSH_RTSP_END(...) \
  audio_diag_flush_rtsp_request_end(__VA_ARGS__)
#define AUDIO_DIAG_FLUSH_CONTROL_RX_BEGIN(...) \
  audio_diag_flush_control_rx_begin(__VA_ARGS__)
#define AUDIO_DIAG_FLUSH_CONTROL_RX_HEADER_DONE(...) \
  audio_diag_flush_control_rx_header_done(__VA_ARGS__)
#define AUDIO_DIAG_FLUSH_CONTROL_RX_PAYLOAD_DONE(...) \
  audio_diag_flush_control_rx_payload_done(__VA_ARGS__)
#define AUDIO_DIAG_FLUSH_CONTROL_RX_END(...) \
  audio_diag_flush_control_rx_end(__VA_ARGS__)
#define AUDIO_DIAG_FLUSH_STATUS_WAIT_BEGIN() \
  audio_diag_flush_status_wait_begin()
#define AUDIO_DIAG_FLUSH_STATUS_WAIT_END(...) \
  audio_diag_flush_status_wait_end(__VA_ARGS__)
#define AUDIO_DIAG_FLUSH_IMMEDIATE_BEGIN() \
  audio_diag_flush_immediate_begin()
#define AUDIO_DIAG_FLUSH_IMMEDIATE_PUBLISH_ACQUIRED() \
  audio_diag_flush_immediate_publish_acquired()
#define AUDIO_DIAG_FLUSH_IMMEDIATE_TRANSPORT_DONE() \
  audio_diag_flush_immediate_transport_done()
#define AUDIO_DIAG_FLUSH_IMMEDIATE_PCM_DONE() \
  audio_diag_flush_immediate_pcm_done()
#define AUDIO_DIAG_FLUSH_IMMEDIATE_END(...) \
  audio_diag_flush_immediate_end(__VA_ARGS__)
#else
#define AUDIO_DIAG_FLUSH_RTSP_SESSION_RESET(...) do {} while (0)
#define AUDIO_DIAG_FLUSH_RTSP_BEGIN(...) do {} while (0)
#define AUDIO_DIAG_FLUSH_RTSP_END(...) do {} while (0)
#define AUDIO_DIAG_FLUSH_CONTROL_RX_BEGIN(...) do {} while (0)
#define AUDIO_DIAG_FLUSH_CONTROL_RX_HEADER_DONE(...) do {} while (0)
#define AUDIO_DIAG_FLUSH_CONTROL_RX_PAYLOAD_DONE(...) do {} while (0)
#define AUDIO_DIAG_FLUSH_CONTROL_RX_END(...) do {} while (0)
#define AUDIO_DIAG_FLUSH_STATUS_WAIT_BEGIN() do {} while (0)
#define AUDIO_DIAG_FLUSH_STATUS_WAIT_END(...) do {} while (0)
#define AUDIO_DIAG_FLUSH_IMMEDIATE_BEGIN() do {} while (0)
#define AUDIO_DIAG_FLUSH_IMMEDIATE_PUBLISH_ACQUIRED() do {} while (0)
#define AUDIO_DIAG_FLUSH_IMMEDIATE_TRANSPORT_DONE() do {} while (0)
#define AUDIO_DIAG_FLUSH_IMMEDIATE_PCM_DONE() do {} while (0)
#define AUDIO_DIAG_FLUSH_IMMEDIATE_END(...) do {} while (0)
#endif

