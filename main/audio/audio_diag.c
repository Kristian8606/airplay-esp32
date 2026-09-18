#include "audio_diag.h"

#if defined(CONFIG_AIRPLAY_DIAG_ACTIVE) && CONFIG_AIRPLAY_DIAG_ACTIVE

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if (defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE) || \
    (defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER) || \
    (defined(CONFIG_AIRPLAY_DIAG_CODEC) && CONFIG_AIRPLAY_DIAG_CODEC) || \
    (defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT) || \
    (defined(CONFIG_AIRPLAY_DIAG_TRANSPORT) && CONFIG_AIRPLAY_DIAG_TRANSPORT) || \
    (defined(CONFIG_AIRPLAY_DIAG_FLUSH) && CONFIG_AIRPLAY_DIAG_FLUSH)
#define AUDIO_DIAG_EVENT_QUEUE_ACTIVE 1
#include "freertos/queue.h"
#else
#define AUDIO_DIAG_EVENT_QUEUE_ACTIVE 0
#endif

#if (defined(CONFIG_AIRPLAY_DIAG_TRANSPORT) && CONFIG_AIRPLAY_DIAG_TRANSPORT) || \
    (defined(CONFIG_AIRPLAY_DIAG_FLUSH) && CONFIG_AIRPLAY_DIAG_FLUSH)
#include "esp_timer.h"
#endif

#if defined(CONFIG_AIRPLAY_DIAG_FLUSH) && CONFIG_AIRPLAY_DIAG_FLUSH
#include <strings.h>
#endif

#define AUDIO_DIAG_STACK       3072U
#define AUDIO_DIAG_PRIORITY       1U
#define AUDIO_DIAG_CORE           0U
#define AUDIO_DIAG_POLL_MS     2000U
#define AUDIO_DIAG_EVENT_DEPTH   24U

static const char *TAG = "audio_diag";
static TaskHandle_t s_diag_task;

#if AUDIO_DIAG_EVENT_QUEUE_ACTIVE
typedef enum {
  DIAG_EVENT_NONE = 0,
  DIAG_EVENT_TASK_STARTED,
  DIAG_EVENT_REALTIME_STOPPED,
  DIAG_EVENT_REALTIME_EPOCH,
  DIAG_EVENT_PSRAM,
  DIAG_EVENT_WORKSPACE,
  DIAG_EVENT_PCM_RING,
  DIAG_EVENT_PACKET_WORKSPACE,
  DIAG_EVENT_AAC_READY,
  DIAG_EVENT_I2S,
  DIAG_EVENT_SOCKET_BUFFER,
  DIAG_EVENT_PORTS,
  DIAG_EVENT_RETRANSMIT_TARGET,
  DIAG_EVENT_FLUSH_RTSP,
  DIAG_EVENT_FLUSH_CONTROL_RX,
  DIAG_EVENT_FLUSH_STATUS_WAIT,
  DIAG_EVENT_FLUSH_IMMEDIATE,
} diag_event_kind_t;

typedef struct {
  uint16_t kind;
  uint16_t id;
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
} diag_event_t;

static StaticQueue_t s_event_queue_struct;
static uint8_t s_event_queue_storage[AUDIO_DIAG_EVENT_DEPTH * sizeof(diag_event_t)];
static QueueHandle_t s_event_queue;

static void diag_emit(diag_event_kind_t kind, uint16_t id,
                      uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  QueueHandle_t q = __atomic_load_n(&s_event_queue, __ATOMIC_ACQUIRE);
  if (!q) return;
  const diag_event_t ev = {
      .kind = (uint16_t)kind, .id = id, .a = a, .b = b, .c = c, .d = d,
  };
  /* Diagnostics are never allowed to backpressure audio/control code. */
  (void)xQueueSend(q, &ev, 0);
}
#endif



#if defined(CONFIG_AIRPLAY_DIAG_FLUSH) && CONFIG_AIRPLAY_DIAG_FLUSH
#define AUDIO_DIAG_FLUSH_RTSP_SLOTS       2U
#define AUDIO_DIAG_FLUSH_OP_SLOTS         2U
#define AUDIO_DIAG_FLUSH_CONTROL_RX_SLOW_US 500000U
#define AUDIO_DIAG_FLUSH_HANDLER_SLOW_US     50000U

typedef enum {
  AUDIO_DIAG_FLUSH_METHOD_NONE = 0,
  AUDIO_DIAG_FLUSH_METHOD_SETRATEANCHORTIME = 1,
  AUDIO_DIAG_FLUSH_METHOD_FLUSHBUFFERED = 2,
  AUDIO_DIAG_FLUSH_METHOD_FLUSH = 3,
  AUDIO_DIAG_FLUSH_METHOD_OPTIONS = 4,
  AUDIO_DIAG_FLUSH_METHOD_GET = 5,
  AUDIO_DIAG_FLUSH_METHOD_POST = 6,
  AUDIO_DIAG_FLUSH_METHOD_ANNOUNCE = 7,
  AUDIO_DIAG_FLUSH_METHOD_SETUP = 8,
  AUDIO_DIAG_FLUSH_METHOD_RECORD = 9,
  AUDIO_DIAG_FLUSH_METHOD_SET_PARAMETER = 10,
  AUDIO_DIAG_FLUSH_METHOD_GET_PARAMETER = 11,
  AUDIO_DIAG_FLUSH_METHOD_PAUSE = 12,
  AUDIO_DIAG_FLUSH_METHOD_TEARDOWN = 13,
  AUDIO_DIAG_FLUSH_METHOD_SETPEERS = 14,
  AUDIO_DIAG_FLUSH_METHOD_SETPEERSX = 15,
} audio_diag_flush_method_t;

typedef struct {
  int socket;
  int64_t request_start_us;
  int64_t last_handler_end_us;
  int64_t control_rx_start_us;
  int64_t control_rx_header_done_us;
  int64_t control_rx_payload_done_us;
  uint32_t idle_us;
  uint16_t method_id;
  bool in_use;
} flush_rtsp_slot_t;

typedef struct {
  TaskHandle_t task;
  int64_t begin_us;
  int64_t publish_acquired_us;
  int64_t transport_done_us;
  int64_t pcm_done_us;
  int64_t status_wait_start_us;
  bool in_use;
} flush_op_slot_t;

static portMUX_TYPE s_flush_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static flush_rtsp_slot_t s_flush_rtsp[AUDIO_DIAG_FLUSH_RTSP_SLOTS];
static flush_op_slot_t s_flush_ops[AUDIO_DIAG_FLUSH_OP_SLOTS];

static uint32_t clamp_elapsed_us(int64_t start_us, int64_t end_us) {
  if (start_us <= 0 || end_us <= start_us) return 0U;
  const int64_t elapsed = end_us - start_us;
  return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static uint16_t flush_method_id(const char *method) {
  if (!method) return AUDIO_DIAG_FLUSH_METHOD_NONE;
  if (strcasecmp(method, "SETRATEANCHORTIME") == 0) {
    return AUDIO_DIAG_FLUSH_METHOD_SETRATEANCHORTIME;
  }
  if (strcasecmp(method, "FLUSHBUFFERED") == 0) {
    return AUDIO_DIAG_FLUSH_METHOD_FLUSHBUFFERED;
  }
  if (strcasecmp(method, "FLUSH") == 0) {
    return AUDIO_DIAG_FLUSH_METHOD_FLUSH;
  }
  if (strcasecmp(method, "OPTIONS") == 0) return AUDIO_DIAG_FLUSH_METHOD_OPTIONS;
  if (strcasecmp(method, "GET") == 0) return AUDIO_DIAG_FLUSH_METHOD_GET;
  if (strcasecmp(method, "POST") == 0) return AUDIO_DIAG_FLUSH_METHOD_POST;
  if (strcasecmp(method, "ANNOUNCE") == 0) return AUDIO_DIAG_FLUSH_METHOD_ANNOUNCE;
  if (strcasecmp(method, "SETUP") == 0) return AUDIO_DIAG_FLUSH_METHOD_SETUP;
  if (strcasecmp(method, "RECORD") == 0) return AUDIO_DIAG_FLUSH_METHOD_RECORD;
  if (strcasecmp(method, "SET_PARAMETER") == 0) return AUDIO_DIAG_FLUSH_METHOD_SET_PARAMETER;
  if (strcasecmp(method, "GET_PARAMETER") == 0) return AUDIO_DIAG_FLUSH_METHOD_GET_PARAMETER;
  if (strcasecmp(method, "PAUSE") == 0) return AUDIO_DIAG_FLUSH_METHOD_PAUSE;
  if (strcasecmp(method, "TEARDOWN") == 0) return AUDIO_DIAG_FLUSH_METHOD_TEARDOWN;
  if (strcasecmp(method, "SETPEERS") == 0) return AUDIO_DIAG_FLUSH_METHOD_SETPEERS;
  if (strcasecmp(method, "SETPEERSX") == 0) return AUDIO_DIAG_FLUSH_METHOD_SETPEERSX;
  return AUDIO_DIAG_FLUSH_METHOD_NONE;
}

static flush_rtsp_slot_t *flush_rtsp_slot_locked(int socket, bool create) {
  flush_rtsp_slot_t *free_slot = NULL;
  for (size_t i = 0; i < AUDIO_DIAG_FLUSH_RTSP_SLOTS; ++i) {
    if (s_flush_rtsp[i].in_use && s_flush_rtsp[i].socket == socket) {
      return &s_flush_rtsp[i];
    }
    if (!s_flush_rtsp[i].in_use && !free_slot) free_slot = &s_flush_rtsp[i];
  }
  if (!create) return NULL;
  if (!free_slot) free_slot = &s_flush_rtsp[0];
  memset(free_slot, 0, sizeof(*free_slot));
  free_slot->socket = socket;
  free_slot->in_use = true;
  return free_slot;
}

static flush_op_slot_t *flush_op_slot_locked(TaskHandle_t task, bool create) {
  flush_op_slot_t *free_slot = NULL;
  for (size_t i = 0; i < AUDIO_DIAG_FLUSH_OP_SLOTS; ++i) {
    if (s_flush_ops[i].in_use && s_flush_ops[i].task == task) return &s_flush_ops[i];
    if (!s_flush_ops[i].in_use && !free_slot) free_slot = &s_flush_ops[i];
  }
  if (!create) return NULL;
  if (!free_slot) free_slot = &s_flush_ops[0];
  memset(free_slot, 0, sizeof(*free_slot));
  free_slot->task = task;
  free_slot->in_use = true;
  return free_slot;
}

void audio_diag_flush_rtsp_session_reset(int socket) {
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_rtsp_slot_t *slot = flush_rtsp_slot_locked(socket, true);
  if (slot) {
    memset(slot, 0, sizeof(*slot));
    slot->socket = socket;
    slot->in_use = true;
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);
}

void audio_diag_flush_rtsp_request_begin(int socket, const char *method) {
  const int64_t now_us = esp_timer_get_time();
  const uint16_t method_id = flush_method_id(method);
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_rtsp_slot_t *slot = flush_rtsp_slot_locked(socket, true);
  if (slot) {
    slot->request_start_us = now_us;
    slot->method_id = method_id;
    slot->idle_us = clamp_elapsed_us(slot->last_handler_end_us, now_us);
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);
}

void audio_diag_flush_rtsp_request_end(int socket, const char *method) {
  (void)method;
  const int64_t now_us = esp_timer_get_time();
  uint16_t method_id = AUDIO_DIAG_FLUSH_METHOD_NONE;
  uint32_t idle_us = 0U;
  uint32_t handler_us = 0U;
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_rtsp_slot_t *slot = flush_rtsp_slot_locked(socket, false);
  if (slot) {
    method_id = slot->method_id;
    idle_us = slot->idle_us;
    handler_us = clamp_elapsed_us(slot->request_start_us, now_us);
    slot->last_handler_end_us = now_us;
    slot->request_start_us = 0;
    slot->idle_us = 0U;
    slot->method_id = AUDIO_DIAG_FLUSH_METHOD_NONE;
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);
  const bool seek_method =
      method_id == AUDIO_DIAG_FLUSH_METHOD_SETRATEANCHORTIME ||
      method_id == AUDIO_DIAG_FLUSH_METHOD_FLUSHBUFFERED ||
      method_id == AUDIO_DIAG_FLUSH_METHOD_FLUSH;
  if (method_id != AUDIO_DIAG_FLUSH_METHOD_NONE &&
      (seek_method || handler_us >= AUDIO_DIAG_FLUSH_HANDLER_SLOW_US)) {
    diag_emit(DIAG_EVENT_FLUSH_RTSP, method_id, idle_us, handler_us, 0U, 0U);
  }
}

void audio_diag_flush_control_rx_begin(int socket) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_rtsp_slot_t *slot = flush_rtsp_slot_locked(socket, true);
  if (slot) {
    slot->control_rx_start_us = now_us;
    slot->control_rx_header_done_us = 0;
    slot->control_rx_payload_done_us = 0;
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);
}

void audio_diag_flush_control_rx_header_done(int socket) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_rtsp_slot_t *slot = flush_rtsp_slot_locked(socket, false);
  if (slot) slot->control_rx_header_done_us = now_us;
  portEXIT_CRITICAL(&s_flush_diag_mux);
}

void audio_diag_flush_control_rx_payload_done(int socket) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_rtsp_slot_t *slot = flush_rtsp_slot_locked(socket, false);
  if (slot) slot->control_rx_payload_done_us = now_us;
  portEXIT_CRITICAL(&s_flush_diag_mux);
}

void audio_diag_flush_control_rx_end(int socket, uint32_t plaintext_bytes) {
  const int64_t now_us = esp_timer_get_time();
  uint32_t header_us = 0U;
  uint32_t payload_us = 0U;
  uint32_t decrypt_us = 0U;
  uint32_t total_us = 0U;

  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_rtsp_slot_t *slot = flush_rtsp_slot_locked(socket, false);
  if (slot) {
    header_us = clamp_elapsed_us(slot->control_rx_start_us,
                                 slot->control_rx_header_done_us);
    payload_us = clamp_elapsed_us(slot->control_rx_header_done_us,
                                  slot->control_rx_payload_done_us);
    decrypt_us = clamp_elapsed_us(slot->control_rx_payload_done_us, now_us);
    total_us = clamp_elapsed_us(slot->control_rx_start_us, now_us);
    slot->control_rx_start_us = 0;
    slot->control_rx_header_done_us = 0;
    slot->control_rx_payload_done_us = 0;
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);

  if (total_us >= AUDIO_DIAG_FLUSH_CONTROL_RX_SLOW_US) {
    const uint16_t bytes = plaintext_bytes > UINT16_MAX
                               ? UINT16_MAX
                               : (uint16_t)plaintext_bytes;
    diag_emit(DIAG_EVENT_FLUSH_CONTROL_RX, bytes, header_us, payload_us,
              decrypt_us, total_us);
  }
}

void audio_diag_flush_status_wait_begin(void) {
  const int64_t now_us = esp_timer_get_time();
  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_op_slot_t *slot = flush_op_slot_locked(task, true);
  if (slot) slot->status_wait_start_us = now_us;
  portEXIT_CRITICAL(&s_flush_diag_mux);
}

void audio_diag_flush_status_wait_end(uint32_t timed_out) {
  const int64_t now_us = esp_timer_get_time();
  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  int64_t start_us = 0;
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_op_slot_t *slot = flush_op_slot_locked(task, false);
  if (slot) {
    start_us = slot->status_wait_start_us;
    slot->status_wait_start_us = 0;
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);
  diag_emit(DIAG_EVENT_FLUSH_STATUS_WAIT, 0U,
            clamp_elapsed_us(start_us, now_us), timed_out, 0U, 0U);
}

void audio_diag_flush_immediate_begin(void) {
  const int64_t now_us = esp_timer_get_time();
  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_op_slot_t *slot = flush_op_slot_locked(task, true);
  if (slot) {
    slot->begin_us = now_us;
    slot->publish_acquired_us = 0;
    slot->transport_done_us = 0;
    slot->pcm_done_us = 0;
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);
}

static void flush_op_mark(int which) {
  const int64_t now_us = esp_timer_get_time();
  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_op_slot_t *slot = flush_op_slot_locked(task, false);
  if (slot) {
    if (which == 1) slot->publish_acquired_us = now_us;
    else if (which == 2) slot->transport_done_us = now_us;
    else if (which == 3) slot->pcm_done_us = now_us;
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);
}

void audio_diag_flush_immediate_publish_acquired(void) { flush_op_mark(1); }
void audio_diag_flush_immediate_transport_done(void) { flush_op_mark(2); }
void audio_diag_flush_immediate_pcm_done(void) { flush_op_mark(3); }

void audio_diag_flush_immediate_end(uint32_t has_endpoint) {
  const int64_t now_us = esp_timer_get_time();
  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  int64_t begin_us = 0;
  int64_t acquired_us = 0;
  int64_t transport_done_us = 0;
  int64_t pcm_done_us = 0;
  portENTER_CRITICAL(&s_flush_diag_mux);
  flush_op_slot_t *slot = flush_op_slot_locked(task, false);
  if (slot) {
    begin_us = slot->begin_us;
    acquired_us = slot->publish_acquired_us;
    transport_done_us = slot->transport_done_us;
    pcm_done_us = slot->pcm_done_us;
    memset(slot, 0, sizeof(*slot));
  }
  portEXIT_CRITICAL(&s_flush_diag_mux);

  const uint32_t mutex_us = clamp_elapsed_us(begin_us, acquired_us);
  const uint32_t transport_us = clamp_elapsed_us(acquired_us, transport_done_us);
  const uint32_t pcm_us = clamp_elapsed_us(transport_done_us, pcm_done_us);
  const uint32_t total_us = clamp_elapsed_us(begin_us, now_us);
  diag_emit(DIAG_EVENT_FLUSH_IMMEDIATE, has_endpoint ? 1U : 0U,
            mutex_us, transport_us, pcm_us, total_us);
}
#endif


#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
typedef struct {
  uint32_t sync_rx;
  uint32_t followup_rx;
  uint32_t announce_rx;
  uint32_t rejected_source;
  uint32_t outlier;
  uint32_t pair_ok;
  uint32_t orphan_followup;
  uint32_t pair_mismatch;
  int64_t last_gap_ns;
} sync_diag_state_t;

static sync_diag_state_t s_sync;

void audio_diag_sync_count(audio_diag_sync_counter_t counter) {
  uint32_t *p = NULL;
  switch (counter) {
    case AUDIO_DIAG_SYNC_RX_SYNC: p = &s_sync.sync_rx; break;
    case AUDIO_DIAG_SYNC_RX_FOLLOWUP: p = &s_sync.followup_rx; break;
    case AUDIO_DIAG_SYNC_RX_ANNOUNCE: p = &s_sync.announce_rx; break;
    case AUDIO_DIAG_SYNC_REJECTED_SOURCE: p = &s_sync.rejected_source; break;
    case AUDIO_DIAG_SYNC_OUTLIER: p = &s_sync.outlier; break;
    case AUDIO_DIAG_SYNC_PAIR_OK: p = &s_sync.pair_ok; break;
    case AUDIO_DIAG_SYNC_ORPHAN_FOLLOWUP: p = &s_sync.orphan_followup; break;
    case AUDIO_DIAG_SYNC_PAIR_MISMATCH: p = &s_sync.pair_mismatch; break;
    default: return;
  }
  __atomic_fetch_add(p, 1U, __ATOMIC_RELAXED);
}

void audio_diag_sync_gap_ns(int64_t gap_ns) {
  __atomic_store_n(&s_sync.last_gap_ns, gap_ns, __ATOMIC_RELAXED);
}

static void diag_sync_poll(void) {
  const uint32_t sync_rx = __atomic_exchange_n(&s_sync.sync_rx, 0U, __ATOMIC_RELAXED);
  const uint32_t followup_rx = __atomic_exchange_n(&s_sync.followup_rx, 0U, __ATOMIC_RELAXED);
  const uint32_t announce_rx = __atomic_exchange_n(&s_sync.announce_rx, 0U, __ATOMIC_RELAXED);
  const uint32_t rejected = __atomic_exchange_n(&s_sync.rejected_source, 0U, __ATOMIC_RELAXED);
  const uint32_t outlier = __atomic_exchange_n(&s_sync.outlier, 0U, __ATOMIC_RELAXED);
  const uint32_t pair_ok = __atomic_exchange_n(&s_sync.pair_ok, 0U, __ATOMIC_RELAXED);
  const uint32_t orphan = __atomic_exchange_n(&s_sync.orphan_followup, 0U, __ATOMIC_RELAXED);
  const uint32_t mismatch = __atomic_exchange_n(&s_sync.pair_mismatch, 0U, __ATOMIC_RELAXED);
  const int64_t gap_ns = __atomic_load_n(&s_sync.last_gap_ns, __ATOMIC_RELAXED);
  if (sync_rx || followup_rx || announce_rx || rejected || outlier || pair_ok ||
      orphan || mismatch) {
    ESP_LOGI(TAG,
             "SYNC/2s sync=%lu followup=%lu announce=%lu pair=%lu orphan=%lu mismatch=%lu rejected=%lu outlier=%lu gap=%.2fms",
             (unsigned long)sync_rx, (unsigned long)followup_rx,
             (unsigned long)announce_rx, (unsigned long)pair_ok,
             (unsigned long)orphan, (unsigned long)mismatch,
             (unsigned long)rejected, (unsigned long)outlier,
             (double)gap_ns / 1000000.0);
  }
}
#endif

#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
void audio_diag_lifecycle_task_started(audio_diag_task_id_t task_id, int core,
                                       uint32_t priority, uint32_t extra) {
  diag_emit(DIAG_EVENT_TASK_STARTED, (uint16_t)task_id,
            (uint32_t)core, priority, extra, 0U);
}
void audio_diag_lifecycle_realtime_stopped(void) {
  diag_emit(DIAG_EVENT_REALTIME_STOPPED, 0U, 0U, 0U, 0U, 0U);
}
void audio_diag_lifecycle_realtime_epoch(uint32_t generation) {
  diag_emit(DIAG_EVENT_REALTIME_EPOCH, 0U, generation, 0U, 0U, 0U);
}
#endif

#if defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER
typedef struct {
  uint32_t read_miss;
  uint32_t conceal_ok;
  uint32_t hard_miss;
  uint32_t hard_play;
  uint32_t hard_transition;
  uint32_t missing_frames;
  uint32_t max_missing_frames;
  uint32_t min_contiguous_before_gap;
} buffer_playout_diag_state_t;

static portMUX_TYPE s_buffer_playout_mux = portMUX_INITIALIZER_UNLOCKED;
static buffer_playout_diag_state_t s_buffer_playout;

void audio_diag_buffer_psram(audio_diag_psram_stage_t stage,
                             uint32_t total_kib, uint32_t free_kib,
                             uint32_t largest_kib, uint32_t store_kib) {
  diag_emit(DIAG_EVENT_PSRAM, (uint16_t)stage, total_kib, free_kib,
            largest_kib, store_kib);
}
void audio_diag_buffer_workspace(uint32_t workspace_kib, uint32_t aac_kib,
                                 uint32_t alac_kib) {
  diag_emit(DIAG_EVENT_WORKSPACE, 0U, workspace_kib, aac_kib, alac_kib, 0U);
}
void audio_diag_buffer_pcm_ring(audio_diag_pcm_ring_id_t ring_id,
                                uint32_t slots, uint32_t slot_frames,
                                uint32_t ring_frames, uint32_t bytes) {
  diag_emit(DIAG_EVENT_PCM_RING, (uint16_t)ring_id, slots, slot_frames,
            ring_frames, bytes);
}
void audio_diag_buffer_packet_workspace(uint32_t data_kib, uint32_t rtx_kib,
                                        uint32_t total_kib) {
  diag_emit(DIAG_EVENT_PACKET_WORKSPACE, 0U, data_kib, rtx_kib, total_kib, 0U);
}

void audio_diag_buffer_playout_miss(uint32_t conceal_ok,
                                    uint32_t missing_frames,
                                    uint32_t contiguous_before_gap,
                                    uint32_t transition) {
  portENTER_CRITICAL(&s_buffer_playout_mux);
  s_buffer_playout.read_miss++;
  if (conceal_ok) {
    s_buffer_playout.conceal_ok++;
    if (UINT32_MAX - s_buffer_playout.missing_frames < missing_frames) {
      s_buffer_playout.missing_frames = UINT32_MAX;
    } else {
      s_buffer_playout.missing_frames += missing_frames;
    }
    if (missing_frames > s_buffer_playout.max_missing_frames) {
      s_buffer_playout.max_missing_frames = missing_frames;
    }
  } else {
    s_buffer_playout.hard_miss++;
    if (transition) {
      s_buffer_playout.hard_transition++;
    } else {
      s_buffer_playout.hard_play++;
    }
  }
  if (s_buffer_playout.read_miss == 1U ||
      contiguous_before_gap < s_buffer_playout.min_contiguous_before_gap) {
    s_buffer_playout.min_contiguous_before_gap = contiguous_before_gap;
  }
  portEXIT_CRITICAL(&s_buffer_playout_mux);
}

static void diag_buffer_poll(void) {
  uint32_t read_miss;
  uint32_t conceal_ok;
  uint32_t hard_miss;
  uint32_t hard_play;
  uint32_t hard_transition;
  uint32_t missing_frames;
  uint32_t max_missing_frames;
  uint32_t min_contiguous_before_gap;

  portENTER_CRITICAL(&s_buffer_playout_mux);
  read_miss = s_buffer_playout.read_miss;
  conceal_ok = s_buffer_playout.conceal_ok;
  hard_miss = s_buffer_playout.hard_miss;
  hard_play = s_buffer_playout.hard_play;
  hard_transition = s_buffer_playout.hard_transition;
  missing_frames = s_buffer_playout.missing_frames;
  max_missing_frames = s_buffer_playout.max_missing_frames;
  min_contiguous_before_gap = s_buffer_playout.min_contiguous_before_gap;
  memset(&s_buffer_playout, 0, sizeof(s_buffer_playout));
  portEXIT_CRITICAL(&s_buffer_playout_mux);

  if (read_miss) {
    ESP_LOGI(TAG,
             "BUFFER AAC/2s readMiss=%lu conceal=%lu hard=%lu hardPlay=%lu hardTransition=%lu missing=%luf maxMissing=%luf firstGapMin=%luf",
             (unsigned long)read_miss, (unsigned long)conceal_ok,
             (unsigned long)hard_miss, (unsigned long)hard_play,
             (unsigned long)hard_transition, (unsigned long)missing_frames,
             (unsigned long)max_missing_frames,
             (unsigned long)min_contiguous_before_gap);
  }
}
#endif

#if defined(CONFIG_AIRPLAY_DIAG_CODEC) && CONFIG_AIRPLAY_DIAG_CODEC
void audio_diag_codec_aac_ready(uint32_t sample_rate, uint32_t channels) {
  diag_emit(DIAG_EVENT_AAC_READY, 0U, sample_rate, channels, 0U, 0U);
}
#endif

#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
void audio_diag_playout_i2s(uint32_t mclk_hz, uint32_t dma_desc,
                            uint32_t dma_frames, uint32_t isr_iram_safe) {
  diag_emit(DIAG_EVENT_I2S, 0U, mclk_hz, dma_desc, dma_frames, isr_iram_safe);
}
#endif

#if defined(CONFIG_AIRPLAY_DIAG_TRANSPORT) && CONFIG_AIRPLAY_DIAG_TRANSPORT
typedef struct {
  uint32_t blocks;
  uint32_t payload_bytes;
  uint32_t max_gap_us;
  uint32_t wait_events;
  uint32_t wait_total_us;
  uint32_t wait_max_us;
  int64_t last_block_us;
  int64_t wait_start_us;
  int64_t window_start_us;
} transport_aac_diag_state_t;

static portMUX_TYPE s_transport_aac_mux = portMUX_INITIALIZER_UNLOCKED;
static transport_aac_diag_state_t s_transport_aac;

void audio_diag_transport_socket_buffer(uint32_t requested, uint32_t actual) {
  diag_emit(DIAG_EVENT_SOCKET_BUFFER, 0U, requested, actual, 0U, 0U);
}
void audio_diag_transport_ports(uint32_t data_port, uint32_t control_port) {
  diag_emit(DIAG_EVENT_PORTS, 0U, data_port, control_port, 0U, 0U);
}
void audio_diag_transport_retransmit_target(uint32_t ip_be, uint32_t port) {
  diag_emit(DIAG_EVENT_RETRANSMIT_TARGET, 0U, ip_be, port, 0U, 0U);
}

void audio_diag_transport_aac_session_reset(void) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_transport_aac_mux);
  memset(&s_transport_aac, 0, sizeof(s_transport_aac));
  s_transport_aac.window_start_us = now_us;
  portEXIT_CRITICAL(&s_transport_aac_mux);
}

void audio_diag_transport_aac_rx_block(uint32_t payload_bytes) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_transport_aac_mux);
  if (s_transport_aac.window_start_us == 0) s_transport_aac.window_start_us = now_us;
  if (s_transport_aac.last_block_us > 0 && now_us > s_transport_aac.last_block_us) {
    const int64_t gap = now_us - s_transport_aac.last_block_us;
    const uint32_t gap_us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    if (gap_us > s_transport_aac.max_gap_us) s_transport_aac.max_gap_us = gap_us;
  }
  s_transport_aac.last_block_us = now_us;
  s_transport_aac.blocks++;
  if (UINT32_MAX - s_transport_aac.payload_bytes < payload_bytes) {
    s_transport_aac.payload_bytes = UINT32_MAX;
  } else {
    s_transport_aac.payload_bytes += payload_bytes;
  }
  portEXIT_CRITICAL(&s_transport_aac_mux);
}

void audio_diag_transport_aac_store_wait_begin(void) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_transport_aac_mux);
  if (s_transport_aac.wait_start_us == 0) s_transport_aac.wait_start_us = now_us;
  portEXIT_CRITICAL(&s_transport_aac_mux);
}

void audio_diag_transport_aac_store_wait_end(void) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_transport_aac_mux);
  const int64_t start_us = s_transport_aac.wait_start_us;
  s_transport_aac.wait_start_us = 0;
  if (start_us > 0 && now_us > start_us) {
    const int64_t waited = now_us - start_us;
    const uint32_t wait_us = waited > UINT32_MAX ? UINT32_MAX : (uint32_t)waited;
    s_transport_aac.wait_events++;
    if (UINT32_MAX - s_transport_aac.wait_total_us < wait_us) {
      s_transport_aac.wait_total_us = UINT32_MAX;
    } else {
      s_transport_aac.wait_total_us += wait_us;
    }
    if (wait_us > s_transport_aac.wait_max_us) s_transport_aac.wait_max_us = wait_us;
  }
  portEXIT_CRITICAL(&s_transport_aac_mux);
}

static void diag_transport_poll(void) {
  const int64_t now_us = esp_timer_get_time();
  uint32_t blocks;
  uint32_t payload_bytes;
  uint32_t max_gap_us;
  uint32_t wait_events;
  uint32_t wait_total_us;
  uint32_t wait_max_us;
  uint32_t blocked_now_us = 0U;
  int64_t window_start_us;

  portENTER_CRITICAL(&s_transport_aac_mux);
  blocks = s_transport_aac.blocks;
  payload_bytes = s_transport_aac.payload_bytes;
  max_gap_us = s_transport_aac.max_gap_us;
  wait_events = s_transport_aac.wait_events;
  wait_total_us = s_transport_aac.wait_total_us;
  wait_max_us = s_transport_aac.wait_max_us;
  window_start_us = s_transport_aac.window_start_us;
  if (s_transport_aac.wait_start_us > 0 && now_us > s_transport_aac.wait_start_us) {
    const int64_t blocked = now_us - s_transport_aac.wait_start_us;
    blocked_now_us = blocked > UINT32_MAX ? UINT32_MAX : (uint32_t)blocked;
  }
  s_transport_aac.blocks = 0U;
  s_transport_aac.payload_bytes = 0U;
  s_transport_aac.max_gap_us = 0U;
  s_transport_aac.wait_events = 0U;
  s_transport_aac.wait_total_us = 0U;
  s_transport_aac.wait_max_us = 0U;
  s_transport_aac.window_start_us = now_us;
  portEXIT_CRITICAL(&s_transport_aac_mux);

  if (blocks || wait_events || blocked_now_us) {
    double window_s = 2.0;
    if (window_start_us > 0 && now_us > window_start_us) {
      window_s = (double)(now_us - window_start_us) / 1000000.0;
    }
#endif

#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
static const char *task_name(uint16_t id) {
  switch ((audio_diag_task_id_t)id) {
    case AUDIO_DIAG_TASK_AAC_PROCESSOR: return "AAC_PROCESSOR";
    case AUDIO_DIAG_TASK_ALAC_STAGE: return "ALAC_STAGE";
    case AUDIO_DIAG_TASK_PLAYOUT: return "PLAYOUT";
    case AUDIO_DIAG_TASK_RT_DATA: return "RT_DATA";
    case AUDIO_DIAG_TASK_RT_CTRL: return "RT_CTRL";
    case AUDIO_DIAG_TASK_RT_WORK: return "RT_WORK";
    case AUDIO_DIAG_TASK_RT_RESEND: return "RT_RESEND";
    case AUDIO_DIAG_TASK_TCP_READER: return "TCP_READER";
    default: return "UNKNOWN";
  }
}
#endif

#if defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER
static const char *psram_stage_name(uint16_t id) {
  switch ((audio_diag_psram_stage_t)id) {
    case AUDIO_DIAG_PSRAM_BEFORE_AUDIO: return "before-audio";
    case AUDIO_DIAG_PSRAM_AFTER_SHARED: return "after-shared";
    case AUDIO_DIAG_PSRAM_AFTER_STORES: return "after-stores";
    default: return "unknown";
  }
}

static const char *pcm_ring_name(uint16_t id) {
  switch ((audio_diag_pcm_ring_id_t)id) {
    case AUDIO_DIAG_PCM_RING_FINAL: return "FINAL";
    case AUDIO_DIAG_PCM_RING_ALAC_STAGE: return "ALAC_STAGE";
    default: return "UNKNOWN";
  }
}
#endif

#if defined(CONFIG_AIRPLAY_DIAG_FLUSH) && CONFIG_AIRPLAY_DIAG_FLUSH
static const char *flush_method_name(uint16_t id) {
  switch ((audio_diag_flush_method_t)id) {
    case AUDIO_DIAG_FLUSH_METHOD_SETRATEANCHORTIME: return "SETRATEANCHORTIME";
    case AUDIO_DIAG_FLUSH_METHOD_FLUSHBUFFERED: return "FLUSHBUFFERED";
    case AUDIO_DIAG_FLUSH_METHOD_FLUSH: return "FLUSH";
    case AUDIO_DIAG_FLUSH_METHOD_OPTIONS: return "OPTIONS";
    case AUDIO_DIAG_FLUSH_METHOD_GET: return "GET";
    case AUDIO_DIAG_FLUSH_METHOD_POST: return "POST";
    case AUDIO_DIAG_FLUSH_METHOD_ANNOUNCE: return "ANNOUNCE";
    case AUDIO_DIAG_FLUSH_METHOD_SETUP: return "SETUP";
    case AUDIO_DIAG_FLUSH_METHOD_RECORD: return "RECORD";
    case AUDIO_DIAG_FLUSH_METHOD_SET_PARAMETER: return "SET_PARAMETER";
    case AUDIO_DIAG_FLUSH_METHOD_GET_PARAMETER: return "GET_PARAMETER";
    case AUDIO_DIAG_FLUSH_METHOD_PAUSE: return "PAUSE";
    case AUDIO_DIAG_FLUSH_METHOD_TEARDOWN: return "TEARDOWN";
    case AUDIO_DIAG_FLUSH_METHOD_SETPEERS: return "SETPEERS";
    case AUDIO_DIAG_FLUSH_METHOD_SETPEERSX: return "SETPEERSX";
    default: return "OTHER";
  }
}
#endif

#if AUDIO_DIAG_EVENT_QUEUE_ACTIVE
static void log_event(const diag_event_t *ev) {
  switch ((diag_event_kind_t)ev->kind) {
#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
    case DIAG_EVENT_TASK_STARTED:
      ESP_LOGI(TAG, "LIFECYCLE task=%s core=%lu prio=%lu extra=%lu",
               task_name(ev->id), (unsigned long)ev->a,
               (unsigned long)ev->b, (unsigned long)ev->c);
      break;
    case DIAG_EVENT_REALTIME_STOPPED:
      ESP_LOGI(TAG, "LIFECYCLE realtime stopped");
      break;
    case DIAG_EVENT_REALTIME_EPOCH:
      ESP_LOGI(TAG, "LIFECYCLE realtime epoch gen=%lu awaiting sender anchor",
               (unsigned long)ev->a);
      break;
#endif
#if defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER
    case DIAG_EVENT_PSRAM:
      ESP_LOGI(TAG, "BUFFER PSRAM %s total=%luKiB free=%luKiB largest=%luKiB store=%luKiB",
               psram_stage_name(ev->id), (unsigned long)ev->a,
               (unsigned long)ev->b, (unsigned long)ev->c,
               (unsigned long)ev->d);
      break;
    case DIAG_EVENT_WORKSPACE:
      ESP_LOGI(TAG, "BUFFER workspace=%luKiB AAC=%luKiB ALAC=%luKiB",
               (unsigned long)ev->a, (unsigned long)ev->b,
               (unsigned long)ev->c);
      break;
    case DIAG_EVENT_PCM_RING:
      ESP_LOGI(TAG, "BUFFER PCM ring=%s slots=%lu slotFrames=%lu totalFrames=%lu bytes=%lu",
               pcm_ring_name(ev->id), (unsigned long)ev->a,
               (unsigned long)ev->b, (unsigned long)ev->c,
               (unsigned long)ev->d);
      break;
    case DIAG_EVENT_PACKET_WORKSPACE:
      ESP_LOGI(TAG, "BUFFER ALAC packets DATA=%luKiB RTX=%luKiB total=%luKiB",
               (unsigned long)ev->a, (unsigned long)ev->b,
               (unsigned long)ev->c);
      break;
#endif
#if defined(CONFIG_AIRPLAY_DIAG_CODEC) && CONFIG_AIRPLAY_DIAG_CODEC
    case DIAG_EVENT_AAC_READY:
      ESP_LOGI(TAG, "CODEC AAC decoder ready %luHz %luch",
               (unsigned long)ev->a, (unsigned long)ev->b);
      break;
#endif
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
    case DIAG_EVENT_I2S:
      ESP_LOGI(TAG, "PLAYOUT I2S mclk=%luHz dma=%lux%lu ISR_IRAM=%lu",
               (unsigned long)ev->a, (unsigned long)ev->b,
               (unsigned long)ev->c, (unsigned long)ev->d);
      break;
#endif
#if defined(CONFIG_AIRPLAY_DIAG_FLUSH) && CONFIG_AIRPLAY_DIAG_FLUSH
    case DIAG_EVENT_FLUSH_RTSP:
      ESP_LOGI(TAG,
               "FLUSH RTSP %s idle=%.2fms handler=%.2fms",
               flush_method_name(ev->id), (double)ev->a / 1000.0,
               (double)ev->b / 1000.0);
      break;
    case DIAG_EVENT_FLUSH_CONTROL_RX:
      ESP_LOGI(TAG,
               "FLUSH controlRx bytes=%u total=%.2fms headerWait=%.2fms payloadWait=%.2fms decrypt=%.2fms",
               (unsigned)ev->id, (double)ev->d / 1000.0,
               (double)ev->a / 1000.0, (double)ev->b / 1000.0,
               (double)ev->c / 1000.0);
      break;
    case DIAG_EVENT_FLUSH_STATUS_WAIT:
      ESP_LOGI(TAG, "FLUSH statusStop wait=%.2fms timeout=%lu",
               (double)ev->a / 1000.0, (unsigned long)ev->b);
      break;
    case DIAG_EVENT_FLUSH_IMMEDIATE:
      ESP_LOGI(TAG,
               "FLUSH immediate endpoint=%u total=%.2fms mutex=%.2fms transport=%.2fms pcm=%.2fms",
               (unsigned)ev->id, (double)ev->d / 1000.0,
               (double)ev->a / 1000.0, (double)ev->b / 1000.0,
               (double)ev->c / 1000.0);
      break;
#endif
#if defined(CONFIG_AIRPLAY_DIAG_TRANSPORT) && CONFIG_AIRPLAY_DIAG_TRANSPORT
    case DIAG_EVENT_SOCKET_BUFFER:
      ESP_LOGI(TAG, "TRANSPORT UDP SO_RCVBUF requested=%lu actual=%lu",
               (unsigned long)ev->a, (unsigned long)ev->b);
      break;
    case DIAG_EVENT_PORTS:
      ESP_LOGI(TAG, "TRANSPORT realtime ports data=%lu control=%lu",
               (unsigned long)ev->a, (unsigned long)ev->b);
      break;
    case DIAG_EVENT_RETRANSMIT_TARGET: {
      /* ev->a is sockaddr_in.sin_addr.s_addr (network byte order).
       * Read its object representation byte-wise so diagnostics do not depend
       * on inet_ntop()/POSIX socket declarations. Access through uint8_t is
       * valid for any object representation in C. */
      const uint8_t *ip = (const uint8_t *)&ev->a;
      ESP_LOGI(TAG,
               "TRANSPORT retransmit target=%u.%u.%u.%u:%lu",
               (unsigned)ip[0], (unsigned)ip[1],
               (unsigned)ip[2], (unsigned)ip[3],
               (unsigned long)ev->b);
      break;
    }
#endif
    default:
      break;
  }
}

#endif

static const char *enabled_categories(void) {
  return ""
#if defined(CONFIG_AIRPLAY_DIAG_TRANSPORT) && CONFIG_AIRPLAY_DIAG_TRANSPORT
      " TRANSPORT"
#endif
#if defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER
      " BUFFER"
#endif
#if defined(CONFIG_AIRPLAY_DIAG_FLUSH) && CONFIG_AIRPLAY_DIAG_FLUSH
      " FLUSH"
#endif
#if defined(CONFIG_AIRPLAY_DIAG_CODEC) && CONFIG_AIRPLAY_DIAG_CODEC
      " CODEC"
#endif
#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
      " SYNC"
#endif
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
      " PLAYOUT"
#endif
#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
      " LIFECYCLE"
#endif
      ;
}

static void poll_categories(void) {
#if defined(CONFIG_AIRPLAY_DIAG_TRANSPORT) && CONFIG_AIRPLAY_DIAG_TRANSPORT
  diag_transport_poll();
#endif
#if defined(CONFIG_AIRPLAY_DIAG_BUFFER) && CONFIG_AIRPLAY_DIAG_BUFFER
  diag_buffer_poll();
#endif
#if defined(CONFIG_AIRPLAY_DIAG_SYNC) && CONFIG_AIRPLAY_DIAG_SYNC
  diag_sync_poll();
#endif
}

static void audio_diag_task(void *arg) {
  (void)arg;
#if defined(CONFIG_AIRPLAY_DIAG_HIGH_RATE_TRACE) && CONFIG_AIRPLAY_DIAG_HIGH_RATE_TRACE
  ESP_LOGW(TAG, "ENABLED:%s HIGH_RATE_TRACE", enabled_categories());
#else
  ESP_LOGI(TAG, "ENABLED:%s", enabled_categories());
#endif

  TickType_t last_poll = xTaskGetTickCount();
  for (;;) {
    const TickType_t period = pdMS_TO_TICKS(AUDIO_DIAG_POLL_MS);
#if AUDIO_DIAG_EVENT_QUEUE_ACTIVE
    diag_event_t ev;
    const TickType_t now = xTaskGetTickCount();
    const TickType_t elapsed = now - last_poll;
    const TickType_t wait = elapsed >= period ? 0 : period - elapsed;
    if (xQueueReceive(s_event_queue, &ev, wait) == pdTRUE) {
      log_event(&ev);
      while (xQueueReceive(s_event_queue, &ev, 0) == pdTRUE) log_event(&ev);
    }
#else
    vTaskDelay(period);
#endif
    const TickType_t after = xTaskGetTickCount();
    if ((after - last_poll) >= period) {
      poll_categories();
      last_poll = after;
    }
  }
}

esp_err_t audio_diag_init(void) {
  if (__atomic_load_n(&s_diag_task, __ATOMIC_ACQUIRE) != NULL) return ESP_OK;

#if AUDIO_DIAG_EVENT_QUEUE_ACTIVE
  if (!s_event_queue) {
    QueueHandle_t q = xQueueCreateStatic(AUDIO_DIAG_EVENT_DEPTH,
                                         sizeof(diag_event_t),
                                         s_event_queue_storage,
                                         &s_event_queue_struct);
    if (!q) return ESP_ERR_NO_MEM;
    __atomic_store_n(&s_event_queue, q, __ATOMIC_RELEASE);
  }
#endif

  TaskHandle_t task = NULL;
  if (xTaskCreatePinnedToCore(audio_diag_task, "audio_diag", AUDIO_DIAG_STACK,
                              NULL, AUDIO_DIAG_PRIORITY, &task,
                              AUDIO_DIAG_CORE) != pdPASS) {
    ESP_LOGE(TAG, "diagnostic task create failed");
    return ESP_ERR_NO_MEM;
  }
  __atomic_store_n(&s_diag_task, task, __ATOMIC_RELEASE);
  return ESP_OK;
}

#endif /* CONFIG_AIRPLAY_DIAG_ACTIVE */
