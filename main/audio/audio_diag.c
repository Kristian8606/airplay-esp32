#include "audio_diag.h"

#if defined(CONFIG_AIRPLAY_DIAG_ACTIVE) && CONFIG_AIRPLAY_DIAG_ACTIVE

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AUDIO_DIAG_STACK       3072U
#define AUDIO_DIAG_PRIORITY       1U
#define AUDIO_DIAG_CORE           0U
#define AUDIO_DIAG_POLL_MS     2000U
#define AUDIO_DIAG_EVENT_DEPTH   24U

static const char *TAG = "audio_diag";
static TaskHandle_t s_diag_task;

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
void audio_diag_buffer_pcm_ring(uint32_t slots, uint32_t slot_frames,
                                uint32_t ring_frames, uint32_t bytes) {
  diag_emit(DIAG_EVENT_PCM_RING, 0U, slots, slot_frames, ring_frames, bytes);
}
void audio_diag_buffer_packet_workspace(uint32_t data_kib, uint32_t rtx_kib,
                                        uint32_t total_kib) {
  diag_emit(DIAG_EVENT_PACKET_WORKSPACE, 0U, data_kib, rtx_kib, total_kib, 0U);
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
void audio_diag_transport_socket_buffer(uint32_t requested, uint32_t actual) {
  diag_emit(DIAG_EVENT_SOCKET_BUFFER, 0U, requested, actual, 0U, 0U);
}
void audio_diag_transport_ports(uint32_t data_port, uint32_t control_port) {
  diag_emit(DIAG_EVENT_PORTS, 0U, data_port, control_port, 0U, 0U);
}
void audio_diag_transport_retransmit_target(uint32_t ip_be, uint32_t port) {
  diag_emit(DIAG_EVENT_RETRANSMIT_TARGET, 0U, ip_be, port, 0U, 0U);
}
#endif

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

static const char *psram_stage_name(uint16_t id) {
  switch ((audio_diag_psram_stage_t)id) {
    case AUDIO_DIAG_PSRAM_BEFORE_AUDIO: return "before-audio";
    case AUDIO_DIAG_PSRAM_AFTER_SHARED: return "after-shared";
    case AUDIO_DIAG_PSRAM_AFTER_STORES: return "after-stores";
    default: return "unknown";
  }
}

static void log_event(const diag_event_t *ev) {
  switch ((diag_event_kind_t)ev->kind) {
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
      ESP_LOGI(TAG, "BUFFER PCM ring slots=%lu slotFrames=%lu totalFrames=%lu bytes=%lu",
               (unsigned long)ev->a, (unsigned long)ev->b,
               (unsigned long)ev->c, (unsigned long)ev->d);
      break;
    case DIAG_EVENT_PACKET_WORKSPACE:
      ESP_LOGI(TAG, "BUFFER ALAC packets DATA=%luKiB RTX=%luKiB total=%luKiB",
               (unsigned long)ev->a, (unsigned long)ev->b,
               (unsigned long)ev->c);
      break;
    case DIAG_EVENT_AAC_READY:
      ESP_LOGI(TAG, "CODEC AAC decoder ready %luHz %luch",
               (unsigned long)ev->a, (unsigned long)ev->b);
      break;
    case DIAG_EVENT_I2S:
      ESP_LOGI(TAG, "PLAYOUT I2S mclk=%luHz dma=%lux%lu ISR_IRAM=%lu",
               (unsigned long)ev->a, (unsigned long)ev->b,
               (unsigned long)ev->c, (unsigned long)ev->d);
      break;
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
    default:
      break;
  }
}

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
    diag_event_t ev;
    const TickType_t now = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(AUDIO_DIAG_POLL_MS);
    const TickType_t elapsed = now - last_poll;
    const TickType_t wait = elapsed >= period ? 0 : period - elapsed;
    if (xQueueReceive(s_event_queue, &ev, wait) == pdTRUE) {
      log_event(&ev);
      while (xQueueReceive(s_event_queue, &ev, 0) == pdTRUE) log_event(&ev);
    }
    const TickType_t after = xTaskGetTickCount();
    if ((after - last_poll) >= period) {
      poll_categories();
      last_poll = after;
    }
  }
}

esp_err_t audio_diag_init(void) {
  if (__atomic_load_n(&s_diag_task, __ATOMIC_ACQUIRE) != NULL) return ESP_OK;

  if (!s_event_queue) {
    QueueHandle_t q = xQueueCreateStatic(AUDIO_DIAG_EVENT_DEPTH,
                                         sizeof(diag_event_t),
                                         s_event_queue_storage,
                                         &s_event_queue_struct);
    if (!q) return ESP_ERR_NO_MEM;
    __atomic_store_n(&s_event_queue, q, __ATOMIC_RELEASE);
  }

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
