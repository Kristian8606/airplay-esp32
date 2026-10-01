#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "network/socket_utils.h"

struct shim_sem { pthread_mutex_t m; pthread_cond_t c; int count; int is_mutex; };
static struct shim_sem *mk(int is_mutex) {
  struct shim_sem *s = calloc(1, sizeof(*s));
  pthread_mutex_init(&s->m, NULL); pthread_cond_init(&s->c, NULL);
  s->is_mutex = is_mutex; s->count = is_mutex ? 1 : 0; return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return mk(1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return mk(0); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
  pthread_mutex_lock(&s->m);
  if (ticks == portMAX_DELAY) {
    while (s->count == 0) pthread_cond_wait(&s->c, &s->m);
  } else {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    long ns = ts.tv_nsec + (long)ticks * 1000000L;
    ts.tv_sec += ns / 1000000000L; ts.tv_nsec = ns % 1000000000L;
    while (s->count == 0)
      if (pthread_cond_timedwait(&s->c, &s->m, &ts) == ETIMEDOUT) break;
    if (s->count == 0) { pthread_mutex_unlock(&s->m); return pdFALSE; }
  }
  s->count = 0; pthread_mutex_unlock(&s->m); return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  pthread_mutex_lock(&s->m); s->count = 1; pthread_cond_signal(&s->c);
  pthread_mutex_unlock(&s->m); return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
int xPortGetCoreID(void) { return 0; }
void vTaskDelay(TickType_t t) { usleep((t ? t : 1) * 1000); }
TickType_t xTaskGetTickCount(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (TickType_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
struct targ { void (*fn)(void *); void *arg; };
static void *tramp(void *p) { struct targ a = *(struct targ *)p; free(p); a.fn(a.arg); return NULL; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack,
                                   void *arg, int prio, TaskHandle_t *out, int core) {
  (void)name; (void)stack; (void)prio; (void)core;
  if (shim_task_create_fail_next > 0) {
    shim_task_create_fail_next--;
    return 0;
  }
  struct targ *a = malloc(sizeof(*a)); a->fn = fn; a->arg = arg;
  pthread_t *t = malloc(sizeof(*t));
  if (out) *out = t;                       /* published before the thread runs */
  if (pthread_create(t, NULL, tramp, a) != 0) return 0;
  pthread_detach(*t);
  return pdPASS;
}
void vTaskDelete(TaskHandle_t t) { (void)t; pthread_exit(NULL); }
int shim_task_create_fail_next;
int shim_task_create_with_caps_count;
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name,
                                           uint32_t stack, void *arg, int prio,
                                           TaskHandle_t *out, int core,
                                           unsigned caps) {
  (void)caps;
  shim_task_create_with_caps_count++;
  const int saved = shim_task_create_fail_next;
  shim_task_create_fail_next = 0;
  const BaseType_t r = xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, core);
  shim_task_create_fail_next = saved;
  return r;
}
void vTaskDeleteWithCaps(TaskHandle_t t) { vTaskDelete(t); }
int socket_utils_bind_tcp_listener(uint16_t port, int backlog, bool nonblock, uint16_t *bound) {
  (void)nonblock;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct timeval tv = {0, 200000}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(0x7f000001);
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, backlog)) { close(s); return -1; }
  socklen_t l = sizeof(a); getsockname(s, (struct sockaddr *)&a, &l);
  *bound = ntohs(a.sin_port); return s;
}


