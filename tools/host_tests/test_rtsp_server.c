/* Host test for main/rtsp/rtsp_server.c (v4.1.45 play lock / connection model).
 *
 * The real rtsp_server.c runs on pthreads (FreeRTOS shim) and real loopback TCP
 * on port 7000. rtsp_dispatch() is replaced by a small stub with the same
 * structure as the real one: "PLAY" takes the play lock (outside the handler
 * mutex) and then starts "audio" under the handler mutex; everything else is a
 * non-playing request. The audio engine stub checks that it is never driven by
 * two connections at once and that a new owner only starts after the previous
 * owner's cleanup has stopped it.
 *
 * Covers: extra connections (/info probes) do not stop playback; a playing
 * connection takes over from the previous one; all slots busy -> the oldest
 * non-playing connection is evicted, never the player; takeover while the
 * owner is inside a slow handler; owner disconnect frees the lock. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "ptp_clock.h"
#include "rtsp_conn.h"
#include "rtsp_events.h"
#include "rtsp_handlers.h"
#include "rtsp_server.h"

/* ---- stubs for what rtsp_server.c links against ------------------------ */
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static rtsp_conn_t *g_audio_owner;   /* connection currently driving audio */
static atomic_int g_stops, g_starts, g_overlap, g_in_handler, g_max_in_handler;
static atomic_int g_nonowner_stop;
static atomic_int g_ptp_clears, g_ptp_peer_clears;

rtsp_conn_t *rtsp_conn_create(void) { return calloc(1, sizeof(rtsp_conn_t)); }
void rtsp_conn_free(rtsp_conn_t *c) { free(c); }
void rtsp_conn_set_volume(rtsp_conn_t *c, float db) { (void)c; (void)db; }
void rtsp_events_emit(rtsp_event_t e, const rtsp_event_data_t *d) { (void)e; (void)d; }
int rtsp_crypto_read_block(int s, rtsp_conn_t *c, uint8_t *b, size_t n) {
  (void)s; (void)c; (void)b; (void)n; return -1;
}
void rtsp_stop_event_port_task(void) {}
void audio_receiver_set_stream_type(audio_stream_type_t t) { (void)t; }
void audio_receiver_set_encryption(const audio_encrypt_t *e) { (void)e; }
void audio_receiver_set_volume_q15(int32_t v) { (void)v; }
void ptp_clock_set_peers(const ptp_clock_peer_t *peers, size_t count) {
  (void)peers;
  if (count == 0) atomic_fetch_add(&g_ptp_peer_clears, 1);
}
void ptp_clock_clear(void) { atomic_fetch_add(&g_ptp_clears, 1); }
void audio_receiver_stop(void) {
  pthread_mutex_lock(&g_m);
  if (!g_audio_owner) atomic_fetch_add(&g_nonowner_stop, 1);
  g_audio_owner = NULL;
  pthread_mutex_unlock(&g_m);
  atomic_fetch_add(&g_stops, 1);
}
const uint8_t *rtsp_find_header_end(const uint8_t *d, size_t n) {
  for (size_t i = 0; i + 3 < n; ++i)
    if (!memcmp(d + i, "\r\n\r\n", 4)) return d + i;
  return NULL;
}
int rtsp_parse_content_length(const char *r) { (void)r; return 0; }

/* Same shape as the real rtsp_dispatch(): play lock first, then the handler
 * mutex around the handler body. */
int rtsp_dispatch(int sock, rtsp_conn_t *conn, const uint8_t *raw, size_t len) {
  (void)len;
  char m[32] = {0};
  sscanf((const char *)raw, "%31s", m);
  const bool play = !strcmp(m, "PLAY");
  if (play && !conn->play_owner && !rtsp_server_acquire_play_lock(conn)) {
    send(sock, "453\r\n\r\n", 7, MSG_NOSIGNAL);
    return 0;
  }
  rtsp_handlers_lock();
  int n = atomic_fetch_add(&g_in_handler, 1) + 1;
  if (n > atomic_load(&g_max_in_handler)) atomic_store(&g_max_in_handler, n);
  if (play) {
    pthread_mutex_lock(&g_m);
    if (g_audio_owner && g_audio_owner != conn) atomic_fetch_add(&g_overlap, 1);
    g_audio_owner = conn;
    pthread_mutex_unlock(&g_m);
    atomic_fetch_add(&g_starts, 1);
  } else if (!strcmp(m, "SLOW")) {
    usleep(400 * 1000);                  /* owner busy inside a handler */
  } else {
    usleep(2 * 1000);                    /* /info, pairing, OPTIONS ... */
  }
  char reply[64];
  int rl = snprintf(reply, sizeof(reply), "200 %s owner=%d\r\n\r\n", m,
                    conn->play_owner ? 1 : 0);
  send(sock, reply, (size_t)rl, MSG_NOSIGNAL);
  atomic_fetch_sub(&g_in_handler, 1);
  rtsp_handlers_unlock();
  return 0;
}

/* rtsp_handlers.c is too large to link here; the three mutex functions are
 * copied verbatim in behaviour. */
#include "freertos/semphr.h"
static SemaphoreHandle_t s_dispatch_mutex;
esp_err_t rtsp_handlers_init(void) {
  if (!s_dispatch_mutex) s_dispatch_mutex = xSemaphoreCreateMutex();
  return s_dispatch_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}
void rtsp_handlers_lock(void) { xSemaphoreTake(s_dispatch_mutex, portMAX_DELAY); }
void rtsp_handlers_unlock(void) { xSemaphoreGive(s_dispatch_mutex); }

/* ---- test client -------------------------------------------------------- */
static int cli_connect(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(7000);
  a.sin_addr.s_addr = htonl(0x7f000001);
  for (int i = 0; i < 100; ++i) {
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) break;
    usleep(20 * 1000);
    close(s);
    s = socket(AF_INET, SOCK_STREAM, 0);
  }
  struct timeval tv = {5, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  usleep(30 * 1000);            /* let the server create the client task */
  return s;
}

/* Send one request, return the reply line ("" on close/timeout). */
static const char *req(int s, const char *method) {
  static __thread char buf[128];
  char out[64];
  int n = snprintf(out, sizeof(out), "%s * RTSP/1.0\r\nCSeq: 1\r\n\r\n", method);
  send(s, out, (size_t)n, MSG_NOSIGNAL);
  size_t got = 0;
  buf[0] = 0;
  while (got < sizeof(buf) - 1) {
    ssize_t r = recv(s, buf + got, sizeof(buf) - 1 - got, 0);
    if (r <= 0) { buf[got] = 0; return buf; }
    got += (size_t)r;
    buf[got] = 0;
    if (strstr(buf, "\r\n\r\n")) break;
  }
  char *e = strstr(buf, "\r\n");
  if (e) *e = 0;
  return buf;
}

static bool is_closed_by_server(int s) {
  char c;
  struct timeval tv = {3, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ssize_t r = recv(s, &c, 1, 0);
  return r == 0 || (r < 0 && errno == ECONNRESET);
}

static void wait_stops(int n) {
  for (int i = 0; i < 300 && atomic_load(&g_stops) < n; ++i) usleep(10 * 1000);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

struct slow_arg { int s; const char *reply; };
static void *slow_thread(void *p) {
  struct slow_arg *a = p;
  a->reply = strdup(req(a->s, "SLOW"));
  return NULL;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  CHECK(rtsp_server_start() == ESP_OK);
  usleep(100 * 1000);

  /* T1: a playing sender is not disturbed by other connections. */
  int A = cli_connect();
  CHECK(!strcmp(req(A, "PLAY"), "200 PLAY owner=1"));
  CHECK(atomic_load(&g_ptp_clears) == 1);
  CHECK(atomic_load(&g_ptp_peer_clears) == 1);
  for (int i = 0; i < 5; ++i) {
    int B = cli_connect();
    CHECK(!strcmp(req(B, "GET"), "200 GET owner=0"));
    CHECK(!strcmp(req(B, "OPTIONS"), "200 OPTIONS owner=0"));
    close(B);
    usleep(50 * 1000);
  }
  CHECK(atomic_load(&g_stops) == 0);
  CHECK(atomic_load(&g_ptp_clears) == 1);
  CHECK(atomic_load(&g_ptp_peer_clears) == 1);
  CHECK(!strcmp(req(A, "SET_PARAMETER"), "200 SET_PARAMETER owner=1"));
  printf("T1 probes/pairing on other connections keep playback: ok\n");

  /* T2: a second sender starts playing -> takes over; old one is closed after
   * its audio stop, and the new audio start never overlaps it. */
  int C = cli_connect();
  CHECK(!strcmp(req(C, "PLAY"), "200 PLAY owner=1"));
  CHECK(is_closed_by_server(A));
  CHECK(atomic_load(&g_stops) == 1);
  CHECK(atomic_load(&g_ptp_clears) == 2);
  CHECK(atomic_load(&g_ptp_peer_clears) == 2);
  CHECK(atomic_load(&g_overlap) == 0);
  close(A);
  printf("T2 takeover by a new playing connection: ok\n");

  /* T3: all slots busy -> the oldest NON-playing connection is evicted. */
  int D = cli_connect();
  CHECK(!strcmp(req(D, "GET"), "200 GET owner=0"));
  int E = cli_connect();
  CHECK(!strcmp(req(E, "GET"), "200 GET owner=0"));
  int F = cli_connect();                      /* 4th connection, 3 slots */
  CHECK(!strcmp(req(F, "GET"), "200 GET owner=0"));
  CHECK(is_closed_by_server(D));
  CHECK(!strcmp(req(C, "SET_PARAMETER"), "200 SET_PARAMETER owner=1"));
  CHECK(!strcmp(req(E, "GET"), "200 GET owner=0"));
  CHECK(atomic_load(&g_stops) == 1);
  close(D); close(E); close(F);
  usleep(100 * 1000);
  printf("T3 slot eviction never hits the playing connection: ok\n");

  /* T4: takeover while the owner is inside a slow handler. */
  struct slow_arg sa = {C, NULL};
  pthread_t th;
  pthread_create(&th, NULL, slow_thread, &sa);
  usleep(50 * 1000);
  int G = cli_connect();
  CHECK(!strcmp(req(G, "PLAY"), "200 PLAY owner=1"));
  pthread_join(th, NULL);
  /* The handler runs to completion, but the socket was already shut down by
   * the takeover (as Shairport cancels the principal conversation), so the
   * reply may not arrive. What matters: no overlap, audio stopped first. */
  CHECK(!strcmp(sa.reply, "") || !strcmp(sa.reply, "200 SLOW owner=1"));
  CHECK(sa.reply[0] == 0 || is_closed_by_server(C));
  CHECK(atomic_load(&g_stops) == 2);
  CHECK(atomic_load(&g_overlap) == 0);
  close(C);
  printf("T4 takeover waits for the owner's running handler: ok\n");

  /* T5: owner disconnect stops audio and frees the lock. */
  close(G);
  wait_stops(3);
  CHECK(atomic_load(&g_stops) == 3);
  int H = cli_connect();
  CHECK(!strcmp(req(H, "PLAY"), "200 PLAY owner=1"));
  CHECK(atomic_load(&g_stops) == 3);
  printf("T5 owner disconnect releases the play lock: ok\n");

  /* T6: stress - many probes in parallel with repeated takeovers. */
  for (int round = 0; round < 20; ++round) {
    int P = cli_connect();
    int Q = cli_connect();
    req(P, "GET");
    CHECK(!strcmp(req(Q, "PLAY"), "200 PLAY owner=1"));
    close(P);
    close(H);
    H = Q;
  }
  wait_stops(23);
  CHECK(atomic_load(&g_overlap) == 0);
  CHECK(atomic_load(&g_nonowner_stop) == 0);
  CHECK(atomic_load(&g_max_in_handler) == 1);
  printf("T6 20 takeovers with probes: no overlap, handlers serialised: ok\n");

  /* T7: internal RAM exhausted while playing (task creation fails) -> the new
   * sender still gets a client task (PSRAM stack) and can take over. */
  shim_task_create_fail_next = 1;
  const int caps_before = shim_task_create_with_caps_count;
  int R = cli_connect();
  CHECK(!strcmp(req(R, "PLAY"), "200 PLAY owner=1"));
  CHECK(shim_task_create_with_caps_count == caps_before + 1);
  CHECK(is_closed_by_server(H));
  close(H);
  H = R;
  CHECK(atomic_load(&g_overlap) == 0);
  printf("T7 takeover with internal RAM exhausted (PSRAM stack): ok\n");
  close(H);
  rtsp_server_stop();
  printf("rtsp_server: all tests passed\n");
  return 0;
}
