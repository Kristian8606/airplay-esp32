#include "rtsp_datastream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "plist.h"
#include "sodium.h"
#include "rtsp_crypto.h"
#include "rtsp_protocol_trace.h"
#include "socket_utils.h"

static const char *TAG = "rtsp_datastream";

#define DATASTREAM_STACK_SIZE 6144
#define DATASTREAM_HEADER_SIZE 32U
#define DATASTREAM_CAPTURE_MAX 16384U
#define DATASTREAM_MESSAGE_MAX (16U * 1024U * 1024U)
#define DATASTREAM_CRYPTO_FRAME_MAX (2U + RTSP_ENCRYPTED_BLOCK_MAX + 16U)
/* select() period: how quickly the task notices ctx->stop. stop() runs inside
 * RTSP SETUP/TEARDOWN handlers, before the reply, so it must not wait ~1 s
 * (lwIP's shutdown() is not guaranteed to wake a select on a listener). */
#define DATASTREAM_POLL_US 100000

struct rtsp_datastream {
  int listen_socket;
  int client_socket;
  uint16_t port;
  uint32_t expected_client_ip;
  volatile bool stop;
  TaskHandle_t task;
  uint8_t encrypt_key[32];
  uint8_t decrypt_key[32];
  uint64_t encrypt_nonce;
  uint64_t decrypt_nonce;
  uint64_t seed;
  char label[24];

  uint8_t header[DATASTREAM_HEADER_SIZE];
  size_t header_len;
  uint32_t message_size;
  uint32_t payload_remaining;
  uint64_t message_seq;
  bool message_sync;
  char message_type[5];
  char message_command[5];
  uint8_t *payload;
  size_t payload_len;
  bool capture_payload;
};

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const uint8_t *p) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

static void write_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void write_be64(uint8_t *p, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    p[i] = (uint8_t)v;
    v >>= 8;
  }
}

static void copy_fourcc(char out[5], const uint8_t *p) {
  for (size_t i = 0; i < 4; ++i) {
    const uint8_t c = p[i];
    out[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
  }
  out[4] = '\0';
}

static bool send_reply(rtsp_datastream_t *ctx, uint64_t seq) {
  uint8_t reply[DATASTREAM_HEADER_SIZE] = {0};
  write_be32(reply, DATASTREAM_HEADER_SIZE);
  memcpy(reply + 4, "rply", 4); /* remaining 8 bytes of message type stay 0 */
  write_be64(reply + 20, seq);
  if (rtsp_crypto_seal_send(ctx->client_socket, ctx->encrypt_key,
                            &ctx->encrypt_nonce, reply, sizeof(reply)) != 0) {
    ESP_LOGW(TAG, "%s: could not send DataStream rply seq=%" PRIu64,
             ctx->label, seq);
    return false;
  }
  return true;
}

static void message_complete(rtsp_datastream_t *ctx) {
  const size_t full_payload_len = ctx->message_size - DATASTREAM_HEADER_SIZE;
  ESP_LOGI(TAG, "%s: %s/%s seq=%" PRIu64 " payload=%u captured=%u%s",
           ctx->label, ctx->message_type, ctx->message_command,
           ctx->message_seq, (unsigned)full_payload_len,
           (unsigned)ctx->payload_len,
           ctx->payload_len < full_payload_len ? " (prefix only)" : "");

  if (ctx->payload_len >= 8 && memcmp(ctx->payload, "bplist00", 8) == 0 &&
      ctx->payload_len == full_payload_len) {
    char *desc = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (desc) {
      if (bplist_describe(ctx->payload, ctx->payload_len, desc, 4096) > 0) {
        ESP_LOGI(TAG, "%s plist: %s", ctx->label, desc);
      } else {
        ESP_LOGW(TAG, "%s: DataStream payload looked like bplist but did not parse",
                 ctx->label);
      }
      heap_caps_free(desc);
    }
  }
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
  rtsp_protocol_trace_datastream_payload(ctx->label, ctx->payload,
                                         ctx->payload_len,
                                         full_payload_len);
#endif

  if (ctx->message_sync) {
    (void)send_reply(ctx, ctx->message_seq);
  }

  ctx->header_len = 0;
  ctx->message_size = 0;
  ctx->payload_remaining = 0;
  ctx->message_seq = 0;
  ctx->message_sync = false;
  ctx->message_type[0] = '\0';
  ctx->message_command[0] = '\0';
  ctx->payload_len = 0;
  ctx->capture_payload = false;
}

/* Feed decrypted HAP-channel plaintext into the AirPlay DataStream framing.
 * Large payloads are consumed without buffering: acknowledgements only need
 * the 32-byte header sequence number, and this keeps artwork/metadata blobs
 * from consuming the ESP32's limited RAM. */
static bool feed_plain(rtsp_datastream_t *ctx, const uint8_t *data,
                       size_t len) {
  while (len > 0) {
    if (ctx->header_len < DATASTREAM_HEADER_SIZE) {
      size_t need = DATASTREAM_HEADER_SIZE - ctx->header_len;
      size_t take = len < need ? len : need;
      memcpy(ctx->header + ctx->header_len, data, take);
      ctx->header_len += take;
      data += take;
      len -= take;
      if (ctx->header_len < DATASTREAM_HEADER_SIZE) continue;

      ctx->message_size = read_be32(ctx->header);
      if (ctx->message_size < DATASTREAM_HEADER_SIZE ||
          ctx->message_size > DATASTREAM_MESSAGE_MAX) {
        ESP_LOGW(TAG, "%s: invalid DataStream message size %u", ctx->label,
                 (unsigned)ctx->message_size);
        return false;
      }
      copy_fourcc(ctx->message_type, ctx->header + 4);
      copy_fourcc(ctx->message_command, ctx->header + 16);
      ctx->message_seq = read_be64(ctx->header + 20);
      ctx->message_sync = memcmp(ctx->header + 4, "sync", 4) == 0;
      ctx->payload_remaining = ctx->message_size - DATASTREAM_HEADER_SIZE;
      ctx->payload_len = 0;
      ctx->capture_payload = true; /* capture up to DATASTREAM_CAPTURE_MAX */
      if (ctx->payload_remaining == 0) {
        message_complete(ctx);
        continue;
      }
    }

    size_t take = len < ctx->payload_remaining ? len : ctx->payload_remaining;
    if (ctx->capture_payload && take > 0 &&
        ctx->payload_len < DATASTREAM_CAPTURE_MAX) {
      size_t copy = take;
      const size_t room = DATASTREAM_CAPTURE_MAX - ctx->payload_len;
      if (copy > room) copy = room;
      memcpy(ctx->payload + ctx->payload_len, data, copy);
      ctx->payload_len += copy;
    }
    data += take;
    len -= take;
    ctx->payload_remaining -= (uint32_t)take;
    if (ctx->payload_remaining == 0) message_complete(ctx);
  }
  return true;
}

static bool consume_encrypted(rtsp_datastream_t *ctx, uint8_t *rx,
                              size_t *rx_len, uint8_t *plain) {
  const ssize_t n = recv(ctx->client_socket, rx + *rx_len,
                         DATASTREAM_CRYPTO_FRAME_MAX - *rx_len, 0);
  if (n <= 0) {
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
  }
  *rx_len += (size_t)n;

  while (*rx_len >= 2) {
    const size_t block_len = (size_t)rx[0] | ((size_t)rx[1] << 8);
    if (block_len == 0 || block_len > RTSP_ENCRYPTED_BLOCK_MAX) {
      ESP_LOGW(TAG, "%s: invalid encrypted frame length %u", ctx->label,
               (unsigned)block_len);
      return false;
    }
    const size_t frame_len = 2 + block_len + 16;
    if (*rx_len < frame_len) break;

    const int plain_len = rtsp_crypto_open(ctx->decrypt_key,
                                           &ctx->decrypt_nonce, rx, frame_len,
                                           plain);
    if (plain_len < 0) {
      ESP_LOGW(TAG, "%s: DataStream decrypt/authentication failed", ctx->label);
      return false;
    }
    if (!feed_plain(ctx, plain, (size_t)plain_len)) return false;

    memmove(rx, rx + frame_len, *rx_len - frame_len);
    *rx_len -= frame_len;
  }
  return true;
}

static void datastream_task(void *arg) {
  rtsp_datastream_t *ctx = (rtsp_datastream_t *)arg;
  uint8_t *rx = heap_caps_malloc(DATASTREAM_CRYPTO_FRAME_MAX,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t *plain = heap_caps_malloc(RTSP_ENCRYPTED_BLOCK_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ctx->payload = heap_caps_malloc(DATASTREAM_CAPTURE_MAX,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rx || !plain || !ctx->payload) {
    ESP_LOGE(TAG, "%s: out of memory starting DataStream task", ctx->label);
    ctx->stop = true;
  }

  while (!ctx->stop && ctx->listen_socket >= 0) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ctx->listen_socket, &fds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = DATASTREAM_POLL_US};
    int ret = select(ctx->listen_socket + 1, &fds, NULL, NULL, &tv);
    if (ctx->stop) break;
    if (ret < 0) {
      if (errno != EINTR) ESP_LOGW(TAG, "%s: listener select errno=%d", ctx->label, errno);
      break;
    }
    if (ret == 0 || !FD_ISSET(ctx->listen_socket, &fds)) continue;

    struct sockaddr_in addr = {0};
    socklen_t addr_len = sizeof(addr);
    int client = accept(ctx->listen_socket, (struct sockaddr *)&addr, &addr_len);
    if (client < 0) {
      if (!ctx->stop) ESP_LOGW(TAG, "%s: accept errno=%d", ctx->label, errno);
      continue;
    }
    if (ctx->expected_client_ip != 0 &&
        addr.sin_addr.s_addr != ctx->expected_client_ip) {
      ESP_LOGW(TAG, "%s: rejected DataStream client from unexpected IP", ctx->label);
      close(client);
      continue;
    }

    if (ctx->client_socket >= 0) close(ctx->client_socket);
    ctx->client_socket = client;
    struct timeval rcv_tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    ctx->encrypt_nonce = 0;
    ctx->decrypt_nonce = 0;
    ctx->header_len = 0;
    ctx->payload_remaining = 0;
    ESP_LOGI(TAG, "%s: encrypted DataStream client connected port=%u seed=%" PRIu64,
             ctx->label, ctx->port, ctx->seed);

    size_t rx_len = 0;
    while (!ctx->stop && ctx->client_socket >= 0) {
      fd_set cfds;
      FD_ZERO(&cfds);
      FD_SET(ctx->client_socket, &cfds);
      struct timeval ctv = {.tv_sec = 0, .tv_usec = DATASTREAM_POLL_US};
      ret = select(ctx->client_socket + 1, &cfds, NULL, NULL, &ctv);
      if (ctx->stop) break;
      if (ret < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (ret == 0) continue;
      if (FD_ISSET(ctx->client_socket, &cfds) &&
          !consume_encrypted(ctx, rx, &rx_len, plain)) {
        break;
      }
    }

    if (ctx->client_socket >= 0) {
      close(ctx->client_socket);
      ctx->client_socket = -1;
    }
    ESP_LOGI(TAG, "%s: DataStream client disconnected", ctx->label);
  }

  if (ctx->client_socket >= 0) {
    close(ctx->client_socket);
    ctx->client_socket = -1;
  }
  if (ctx->listen_socket >= 0) {
    close(ctx->listen_socket);
    ctx->listen_socket = -1;
  }
  heap_caps_free(ctx->payload);
  ctx->payload = NULL;
  heap_caps_free(rx);
  heap_caps_free(plain);
  ctx->task = NULL;
  vTaskDeleteWithCaps(NULL);
}

esp_err_t rtsp_datastream_start(const hap_session_t *session, uint64_t seed,
                                uint32_t expected_client_ip,
                                const char *label,
                                rtsp_datastream_t **out_stream,
                                uint16_t *out_port) {
  if (!session || !out_stream || !out_port) return ESP_ERR_INVALID_ARG;
  if (*out_stream) rtsp_datastream_stop(out_stream);

  rtsp_datastream_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return ESP_ERR_NO_MEM;
  ctx->listen_socket = -1;
  ctx->client_socket = -1;
  ctx->seed = seed;
  ctx->expected_client_ip = expected_client_ip;
  snprintf(ctx->label, sizeof(ctx->label), "%s",
           label ? label : "DataStream");

  esp_err_t err = hap_derive_datastream_keys(session, seed,
                                             ctx->encrypt_key,
                                             ctx->decrypt_key);
  if (err != ESP_OK) {
    free(ctx);
    return err;
  }

  ctx->listen_socket = socket_utils_bind_tcp_listener(0, 1, false, &ctx->port);
  if (ctx->listen_socket < 0 || ctx->port == 0) {
    if (ctx->listen_socket >= 0) close(ctx->listen_socket);
    sodium_memzero(ctx, sizeof(*ctx));
    free(ctx);
    return ESP_FAIL;
  }

  BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
      datastream_task, "ap2_datastream", DATASTREAM_STACK_SIZE, ctx, 5,
      &ctx->task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ret != pdPASS) {
    close(ctx->listen_socket);
    sodium_memzero(ctx, sizeof(*ctx));
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  *out_port = ctx->port;
  *out_stream = ctx;
  ESP_LOGI(TAG, "%s: listener ready on TCP port %u seed=%" PRIu64,
           ctx->label, ctx->port, seed);
  return ESP_OK;
}

void rtsp_datastream_stop(rtsp_datastream_t **stream) {
  if (!stream || !*stream) return;
  rtsp_datastream_t *ctx = *stream;
  ctx->stop = true;
  if (ctx->client_socket >= 0) shutdown(ctx->client_socket, SHUT_RDWR);
  if (ctx->listen_socket >= 0) shutdown(ctx->listen_socket, SHUT_RDWR);

  int ticks = 100; /* up to 1 s; normally one poll period */
  while (ctx->task != NULL && ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (ctx->task != NULL) {
    /* Do not free memory still owned by a task. Closing both descriptors is
     * enough to make the select/accept loop exit on the next scheduler turn. */
    ESP_LOGW(TAG, "%s: task did not stop within timeout; context retained",
             ctx->label);
    return;
  }

  if (ctx->client_socket >= 0) close(ctx->client_socket);
  if (ctx->listen_socket >= 0) close(ctx->listen_socket);
  sodium_memzero(ctx, sizeof(*ctx));
  free(ctx);
  *stream = NULL;
}
