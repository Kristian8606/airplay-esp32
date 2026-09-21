#include "dns_server.h"
#include "spiram_task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "dns_server";

#define DNS_PORT          53
#define DNS_MAX_LEN       512
#define DNS_MAX_QUESTIONS 16
#define DNS_STOP_WAIT_MS  300

// DNS header structure
typedef struct __attribute__((packed)) {
  uint16_t id;
  uint16_t flags;
  uint16_t qdcount;
  uint16_t ancount;
  uint16_t nscount;
  uint16_t arcount;
} dns_header_t;

static int s_dns_socket = -1;
static TaskHandle_t s_dns_task = NULL;
static uint32_t s_redirect_ip = 0;
static volatile bool s_dns_stop_requested = false;

/* Return the first byte after QNAME, or NULL for a malformed/truncated name.
 * Compression pointers are accepted. Since the response preserves the DNS
 * header and complete question section at the same offsets, copied question
 * pointers remain valid. */
static const uint8_t *dns_name_end(const uint8_t *ptr, const uint8_t *end) {
  while (ptr < end) {
    const uint8_t label_len = *ptr;
    if (label_len == 0U) return ptr + 1;

    if ((label_len & 0xC0U) == 0xC0U) {
      return (ptr + 2 <= end) ? ptr + 2 : NULL;
    }
    if ((label_len & 0xC0U) != 0U || label_len > 63U) return NULL;
    if ((size_t)(end - ptr) < (size_t)label_len + 1U) return NULL;
    ptr += (size_t)label_len + 1U;
  }
  return NULL;
}

static bool dns_build_response(const uint8_t *request, size_t request_len,
                               uint8_t *response, size_t response_cap,
                               size_t *response_len) {
  if (!request || !response || !response_len ||
      request_len < sizeof(dns_header_t) || response_cap < sizeof(dns_header_t)) {
    return false;
  }

  dns_header_t req_header;
  memcpy(&req_header, request, sizeof(req_header));
  const uint16_t request_flags = ntohs(req_header.flags);
  const uint16_t qdcount = ntohs(req_header.qdcount);

  /* Only answer standard DNS queries. Never answer a response packet. */
  if ((request_flags & 0x8000U) != 0U ||
      (request_flags & 0x7800U) != 0U ||
      qdcount == 0U || qdcount > DNS_MAX_QUESTIONS) {
    return false;
  }

  memset(response, 0, sizeof(dns_header_t));
  dns_header_t *resp_header = (dns_header_t *)response;
  resp_header->id = req_header.id;
  /* QR=1, AA=1, RCODE=NOERROR. Preserve the client's RD bit; RA stays 0. */
  resp_header->flags = htons((uint16_t)(0x8400U | (request_flags & 0x0100U)));
  resp_header->qdcount = htons(qdcount);
  resp_header->nscount = 0;
  resp_header->arcount = 0; /* Deliberately drop EDNS/other additional records. */

  const uint8_t *src = request + sizeof(dns_header_t);
  const uint8_t *request_end = request + request_len;
  size_t out = sizeof(dns_header_t);
  uint16_t answer_name_offsets[DNS_MAX_QUESTIONS];
  uint16_t answer_count = 0;

  for (uint16_t i = 0; i < qdcount; ++i) {
    const uint8_t *question_start = src;
    const uint8_t *name_end = dns_name_end(src, request_end);
    if (!name_end || (size_t)(request_end - name_end) < 4U) return false;

    uint16_t qtype_net = 0;
    uint16_t qclass_net = 0;
    memcpy(&qtype_net, name_end, sizeof(qtype_net));
    memcpy(&qclass_net, name_end + 2, sizeof(qclass_net));
    const uint16_t qtype = ntohs(qtype_net);
    const uint16_t qclass = ntohs(qclass_net);
    const uint8_t *question_end = name_end + 4;
    const size_t question_len = (size_t)(question_end - question_start);

    if (out + question_len > response_cap) return false;
    const size_t qname_offset = out;
    if (qname_offset > 0x3FFFU) return false;
    memcpy(response + out, question_start, question_len);
    out += question_len;

    /* Captive portal redirects IPv4 A/IN queries. AAAA and other types get a
     * valid empty NOERROR response, avoiding bogus A data under another type. */
    if (qtype == 1U && qclass == 1U) {
      answer_name_offsets[answer_count++] = (uint16_t)qname_offset;
    }
    src = question_end;
  }

  for (uint16_t i = 0; i < answer_count; ++i) {
    if (out + 16U > response_cap) return false;
    uint8_t *ans = response + out;
    const uint16_t name_ptr = htons((uint16_t)(0xC000U | answer_name_offsets[i]));
    memcpy(ans, &name_ptr, sizeof(name_ptr));
    ans[2] = 0x00; ans[3] = 0x01; /* TYPE A */
    ans[4] = 0x00; ans[5] = 0x01; /* CLASS IN */
    ans[6] = 0x00; ans[7] = 0x00; ans[8] = 0x00; ans[9] = 0x3C; /* TTL 60 */
    ans[10] = 0x00; ans[11] = 0x04;
    memcpy(ans + 12, &s_redirect_ip, 4); /* already network byte order */
    out += 16U;
  }

  resp_header->ancount = htons(answer_count);
  *response_len = out;
  return true;
}

static void dns_server_task(void *pvParameters) {
  (void)pvParameters;
  uint8_t rx_buffer[DNS_MAX_LEN];
  uint8_t tx_buffer[DNS_MAX_LEN];
  struct sockaddr_in client_addr;

  ESP_LOGI(TAG, "DNS server task started");

  while (!s_dns_stop_requested) {
    const int sock = s_dns_socket;
    if (sock < 0) break;

    socklen_t addr_len = sizeof(client_addr);
    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                       (struct sockaddr *)&client_addr, &addr_len);
    if (len < 0) {
      if (s_dns_stop_requested || s_dns_socket < 0 || errno == EBADF ||
          errno == ENOTSOCK) {
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      ESP_LOGW(TAG, "recvfrom failed: %d", errno);
      break;
    }

    size_t resp_len = 0;
    if (!dns_build_response(rx_buffer, (size_t)len, tx_buffer,
                            sizeof(tx_buffer), &resp_len)) {
      continue;
    }

    if (sendto(sock, tx_buffer, resp_len, 0,
               (struct sockaddr *)&client_addr, addr_len) < 0 &&
        !s_dns_stop_requested) {
      ESP_LOGW(TAG, "sendto failed: %d", errno);
    }
  }

  ESP_LOGI(TAG, "DNS server task exiting");
  s_dns_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t dns_server_start(uint32_t redirect_ip) {
  if (s_dns_socket >= 0 || s_dns_task != NULL) {
    ESP_LOGW(TAG, "DNS server already running or still stopping");
    return ESP_OK;
  }

  s_redirect_ip = redirect_ip;
  s_dns_stop_requested = false;

  s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_dns_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    return ESP_FAIL;
  }

  struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
  setsockopt(s_dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  int opt = 1;
  setsockopt(s_dns_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in server_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  if (bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind socket: %d", errno);
    close(s_dns_socket);
    s_dns_socket = -1;
    return ESP_FAIL;
  }

  if (task_create_pinned_spiram(dns_server_task, "dns_server", 4096, NULL, 5,
                                &s_dns_task, 0, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create DNS server task");
    close(s_dns_socket);
    s_dns_socket = -1;
    s_dns_task = NULL;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "DNS server started, redirecting to " IPSTR,
           IP2STR((esp_ip4_addr_t *)&redirect_ip));
  return ESP_OK;
}

void dns_server_stop(void) {
  if (s_dns_socket < 0 && s_dns_task == NULL) return;

  s_dns_stop_requested = true;
  const int sock = s_dns_socket;
  s_dns_socket = -1;
  if (sock >= 0) {
    shutdown(sock, SHUT_RDWR);
    close(sock);
  }

  const int wait_steps = DNS_STOP_WAIT_MS / 10;
  for (int i = 0; s_dns_task != NULL && i < wait_steps; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s_dns_task != NULL) {
    ESP_LOGW(TAG, "DNS server task did not stop within %d ms", DNS_STOP_WAIT_MS);
  } else {
    ESP_LOGI(TAG, "DNS server stopped");
  }
}
