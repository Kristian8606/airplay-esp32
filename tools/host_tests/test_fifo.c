/* Host test for ap2_buffered_fifo.c (v4.1.44): real loopback TCP, FreeRTOS
 * shims on pthreads. Checks framing, head/rest copy + discard, wrap-around,
 * partial (trickled) delivery, invalid length recovery, orderly-close
 * retention, reconnect epoch change, and drain speed copy vs discard. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ap2_buffered_fifo.h"

#define HEAD 12U
#define CAP 8192U

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* deterministic block content: header = seq (be32), rtp = seq*1024, ssrc; body pattern */
static size_t make_block(uint32_t seq, uint8_t *out /* includes 2-byte len */, uint32_t *rng) {
  *rng = *rng * 1664525u + 1013904223u;
  size_t body = 12 + (*rng >> 8) % 1500; /* 12..1511 bytes of block */
  size_t wire = body + 2;
  out[0] = (uint8_t)(wire >> 8); out[1] = (uint8_t)wire;
  uint8_t *b = out + 2;
  b[0] = 0x80; b[1] = (uint8_t)(seq >> 16); b[2] = (uint8_t)(seq >> 8); b[3] = (uint8_t)seq;
  uint32_t rtp = seq * 1024u;
  b[4] = rtp >> 24; b[5] = rtp >> 16; b[6] = rtp >> 8; b[7] = rtp;
  b[8] = 0x16; b[9] = 0; b[10] = 0; b[11] = 0;
  for (size_t i = 12; i < body; ++i) b[i] = (uint8_t)(seq * 31u + i);
  return wire;
}

static bool check_block(uint32_t seq, const uint8_t *blk, size_t len) {
  if (len < 12) return false;
  uint32_t s = ((uint32_t)blk[1] << 16) | ((uint32_t)blk[2] << 8) | blk[3];
  if (s != seq) return false;
  for (size_t i = 12; i < len; ++i) if (blk[i] != (uint8_t)(seq * 31u + i)) return false;
  return true;
}

static int connect_to(uint16_t port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(0x7f000001);
  for (int i = 0; i < 100; ++i) { if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) return s; usleep(10000); }
  return -1;
}

typedef struct { uint16_t port; uint32_t first, count; int trickle; int close_after; uint32_t seed; int fd_out; } sender_t;

static void *sender(void *p) {
  sender_t *sd = p;
  int s = connect_to(sd->port);
  sd->fd_out = s;
  uint8_t buf[2048];
  uint32_t rng = sd->seed;
  for (uint32_t i = 0; i < sd->count; ++i) {
    size_t n = make_block(sd->first + i, buf, &rng);
    if (sd->trickle) {
      for (size_t k = 0; k < n; ) {
        size_t chunk = 1 + (rng >> 20) % 7; rng = rng * 1664525u + 1013904223u;
        if (chunk > n - k) chunk = n - k;
        if (send(s, buf + k, chunk, 0) <= 0) return NULL;
        k += chunk;
      }
    } else {
      size_t k = 0;
      while (k < n) { ssize_t w = send(s, buf + k, n - k, 0); if (w <= 0) return NULL; k += (size_t)w; }
    }
  }
  if (sd->close_after) { shutdown(s, SHUT_WR); usleep(100000); close(s); }
  return NULL;
}

static ap2_buffered_fifo_t *mkfifo_(size_t cap, uint16_t *port) {
  static ap2_buffered_fifo_config_t cfg;
  cfg.buffer_bytes = cap; cfg.task_core = 1; cfg.task_priority = 4; cfg.task_stack = 4096;
  void *store = malloc(cap);
  ap2_buffered_fifo_t *f = NULL;
  assert(ap2_buffered_fifo_create_with_storage(&f, &cfg, store, cap) == ESP_OK);
  assert(ap2_buffered_fifo_start(f, 0, port) == ESP_OK);
  return f;
}

/* consume `count` blocks, discarding every `discard_every`-th body (0 = never) */
static uint32_t consume(ap2_buffered_fifo_t *f, uint32_t first, uint32_t count, int discard_every, uint32_t *epoch_out) {
  static uint8_t blk[CAP];
  uint32_t ok = 0;
  for (uint32_t i = 0; i < count; ++i) {
    size_t len = 0; uint32_t ep = 0;
    esp_err_t e = ap2_buffered_fifo_read_block_head(f, blk, HEAD, CAP, &len, &ep);
    if (e != ESP_OK) { fprintf(stderr, "head err %d at %u\n", e, i); break; }
    if (epoch_out) *epoch_out = ep;
    uint32_t seq = ((uint32_t)blk[1] << 16) | ((uint32_t)blk[2] << 8) | blk[3];
    CHECK(seq == first + i, "seq %u != %u", seq, first + i);
    bool discard = discard_every && (i % (uint32_t)discard_every) == 0;
    e = ap2_buffered_fifo_read_block_rest(f, discard ? NULL : blk + HEAD, len - HEAD, ep);
    CHECK(e == ESP_OK, "rest err %d", e);
    if (!discard) CHECK(check_block(first + i, blk, len), "content mismatch seq %u", first + i);
    ok++;
  }
  return ok;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  /* 1 + 2 + 4: bulk send, 16 KiB FIFO (heavy wrap + backpressure), copy & discard mix */
  {
    uint16_t port; ap2_buffered_fifo_t *f = mkfifo_(16384, &port);
    sender_t sd = {port, 1000, 20000, 0, 0, 7, -1}; pthread_t t; pthread_create(&t, NULL, sender, &sd);
    uint32_t n = consume(f, 1000, 20000, 3, NULL);
    CHECK(n == 20000, "bulk wrap consumed %u", n);
    pthread_join(t, NULL);
    ap2_buffered_fifo_stop(f); CHECK(ap2_buffered_fifo_destroy(f) == ESP_OK, "destroy");
    close(sd.fd_out);
    printf("T1 bulk/wrap/discard mix: %s\n", n == 20000 ? "ok" : "FAIL");
  }
  /* 3: trickled 1..7 byte chunks -> slow path (partial length, partial head, partial body) */
  {
    uint16_t port; ap2_buffered_fifo_t *f = mkfifo_(16384, &port);
    sender_t sd = {port, 5, 3000, 1, 0, 11, -1}; pthread_t t; pthread_create(&t, NULL, sender, &sd);
    uint32_t n = consume(f, 5, 3000, 2, NULL);
    CHECK(n == 3000, "trickle consumed %u", n);
    pthread_join(t, NULL);
    ap2_buffered_fifo_stop(f); ap2_buffered_fifo_destroy(f); close(sd.fd_out);
    printf("T2 trickle/partial blocks: %s\n", n == 3000 ? "ok" : "FAIL");
  }
  /* 6 + 7: orderly close keeps the backlog; reconnect starts a new epoch */
  {
    uint16_t port; ap2_buffered_fifo_t *f = mkfifo_(1 << 20, &port);
    sender_t sd = {port, 1, 400, 0, 1, 3, -1}; pthread_t t; pthread_create(&t, NULL, sender, &sd);
    pthread_join(t, NULL); usleep(200000); /* sender has closed; nothing consumed yet */
    ap2_buffered_fifo_usage_t u; ap2_buffered_fifo_get_usage(f, &u);
    CHECK(u.used_bytes > 0, "backlog dropped on FIN (used=%zu)", u.used_bytes);
    uint32_t ep1 = 0;
    uint32_t n = consume(f, 1, 400, 0, &ep1);
    CHECK(n == 400, "after FIN consumed %u of 400", n);
    /* second connection */
    sender_t sd2 = {port, 900, 50, 0, 0, 5, -1}; pthread_t t2; pthread_create(&t2, NULL, sender, &sd2);
    uint32_t ep2 = 0; uint32_t n2 = consume(f, 900, 50, 0, &ep2);
    CHECK(n2 == 50 && ep2 != ep1, "reconnect n=%u ep %u->%u", n2, ep1, ep2);
    pthread_join(t2, NULL);
    ap2_buffered_fifo_stop(f); ap2_buffered_fifo_destroy(f); close(sd2.fd_out);
    printf("T3 FIN retention + reconnect epoch: %s\n", (n == 400 && n2 == 50 && ep2 != ep1) ? "ok" : "FAIL");
  }
  /* 6b: a new connection while old backlog is still queued discards the old bytes */
  {
    uint16_t port; ap2_buffered_fifo_t *f = mkfifo_(1 << 20, &port);
    sender_t sd = {port, 1, 300, 0, 1, 3, -1}; pthread_t t; pthread_create(&t, NULL, sender, &sd);
    pthread_join(t, NULL); usleep(100000);
    uint32_t n = consume(f, 1, 10, 0, NULL); /* play a little of the old stream */
    sleep(2); /* let the reader reach the old FIN (it paces at ~400 KB/s) */
    sender_t sd2 = {port, 7000, 20, 0, 0, 9, -1}; pthread_t t2; pthread_create(&t2, NULL, sender, &sd2);
    pthread_join(t2, NULL); usleep(100000);
    uint32_t n2 = consume(f, 7000, 20, 0, NULL); /* must start at the new stream */
    CHECK(n == 10 && n2 == 20, "old %u new %u", n, n2);
    ap2_buffered_fifo_stop(f); ap2_buffered_fifo_destroy(f); close(sd2.fd_out);
    printf("T4 new connection replaces old backlog: %s\n", (n == 10 && n2 == 20) ? "ok" : "FAIL");
  }
  /* 5: invalid length aborts only the client; a new connection works */
  {
    uint16_t port; ap2_buffered_fifo_t *f = mkfifo_(16384, &port);
    int s = connect_to(port);
    uint8_t bad[4] = {0x00, 0x05, 1, 2}; send(s, bad, 4, 0);
    static uint8_t blk[CAP]; size_t len; uint32_t ep;
    esp_err_t e = ap2_buffered_fifo_read_block_head(f, blk, HEAD, CAP, &len, &ep);
    CHECK(e == ESP_ERR_INVALID_SIZE, "bad length err %d", e);
    close(s);
    sender_t sd = {port, 42, 30, 0, 0, 1, -1}; pthread_t t; pthread_create(&t, NULL, sender, &sd);
    uint32_t n = consume(f, 42, 30, 0, NULL);
    CHECK(n == 30, "after abort %u", n);
    pthread_join(t, NULL);
    ap2_buffered_fifo_stop(f); ap2_buffered_fifo_destroy(f); close(sd.fd_out);
    printf("T5 invalid length -> abort + reconnect: %s\n", (e == ESP_ERR_INVALID_SIZE && n == 30) ? "ok" : "FAIL");
  }
  /* stop while the consumer is blocked in read_block_head */
  {
    uint16_t port; ap2_buffered_fifo_t *f = mkfifo_(16384, &port);
    int s = connect_to(port); usleep(50000);
    pthread_t t; struct { ap2_buffered_fifo_t *f; esp_err_t e; } ctx = {f, 0};
    void *rd(void *p) { typeof(ctx) *c = p; static uint8_t b[CAP]; size_t l; uint32_t ep;
      c->e = ap2_buffered_fifo_read_block_head(c->f, b, HEAD, CAP, &l, &ep); return NULL; }
    pthread_create(&t, NULL, rd, &ctx); usleep(100000);
    double t0 = now_s(); ap2_buffered_fifo_stop(f); pthread_join(t, NULL); double dt = now_s() - t0;
    CHECK(ctx.e == ESP_ERR_INVALID_STATE && dt < 1.5, "stop unblock e=%d dt=%.2f", ctx.e, dt);
    ap2_buffered_fifo_destroy(f); close(s);
    printf("T6 stop unblocks reader: %s (%.0f ms)\n", ctx.e == ESP_ERR_INVALID_STATE ? "ok" : "FAIL", dt * 1000);
  }
  /* RX diagnostics: sender idle -> reader blocked in recv, nothing unread */
  {
    uint16_t port; ap2_buffered_fifo_t *f = mkfifo_(1 << 20, &port);
    ap2_buffered_fifo_rx_diag_t d0; ap2_buffered_fifo_get_rx_diag(f, &d0);
    sender_t sd = {port, 1, 10, 0, 0, 3, -1}; pthread_t t; pthread_create(&t, NULL, sender, &sd);
    pthread_join(t, NULL); usleep(400000);
    ap2_buffered_fifo_rx_diag_t d; ap2_buffered_fifo_get_rx_diag(f, &d);
    const bool ok = !d0.connected && d.connected && d.state == AP2_FIFO_RX_RECV &&
                    d.idle_ms >= 300 && d.state_ms >= 300 && d.socket_unread == 0;
    CHECK(ok, "rx diag connected=%d state=%s idle=%u state_ms=%u unread=%d",
          d.connected, ap2_buffered_fifo_rx_state_name(d.state), d.idle_ms,
          d.state_ms, d.socket_unread);
    ap2_buffered_fifo_stop(f); ap2_buffered_fifo_destroy(f); close(sd.fd_out);
    printf("T8 rx diag (idle sender -> recv, 0 unread): %s\n", ok ? "ok" : "FAIL");
  }
  /* 8: drain speed of a full 6 MiB backlog: copy every body vs discard bodies */
  for (int mode = 0; mode < 2; ++mode) {
    uint16_t port; ap2_buffered_fifo_t *f = mkfifo_(6u << 20, &port);
    uint32_t blocks = 7000; /* ~5.3 MB */
    sender_t sd = {port, 1, blocks, 0, 0, 99, -1}; pthread_t t; pthread_create(&t, NULL, sender, &sd);
    pthread_join(t, NULL);
    ap2_buffered_fifo_usage_t u;
    size_t last = (size_t)-1;
    for (int i = 0; i < 120; ++i) { /* wait until the paced reader has everything */
      usleep(500000); ap2_buffered_fifo_get_usage(f, &u);
      if (u.used_bytes == last) break;
      last = u.used_bytes;
    }
    double t0 = now_s();
    uint32_t n = consume(f, 1, blocks, mode ? 1 : 0, NULL);
    double dt = now_s() - t0;
    printf("T7 drain %u blocks (%.1f MB queued) with %s: %.1f ms\n", n, u.used_bytes / 1e6,
           mode ? "discard (v4.1.44)" : "copy (old)", dt * 1000);
    CHECK(n == blocks, "drain n=%u", n);
    ap2_buffered_fifo_stop(f); ap2_buffered_fifo_destroy(f); close(sd.fd_out);
  }
  printf(fails ? "FAILURES: %d\n" : "ALL FIFO TESTS PASSED\n", fails);
  return fails != 0;
}
