#pragma once
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include "FreeRTOS.h"
typedef struct {pthread_mutex_t lock;pthread_cond_t cond;int binary,count;} *SemaphoreHandle_t;
static inline SemaphoreHandle_t make_sem(int binary){SemaphoreHandle_t s=calloc(1,sizeof(*s));pthread_mutex_init(&s->lock,0);pthread_cond_init(&s->cond,0);s->binary=binary;return s;}
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return make_sem(0);}
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void){return make_sem(1);}
static inline int xSemaphoreTake(SemaphoreHandle_t s,TickType_t ms){pthread_mutex_lock(&s->lock);if(!s->binary)return 1;struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_sec+=ms/1000;t.tv_nsec+=(ms%1000)*1000000L;if(t.tv_nsec>=1000000000L){t.tv_sec++;t.tv_nsec-=1000000000L;}while(!s->count){int e=ms==portMAX_DELAY?pthread_cond_wait(&s->cond,&s->lock):pthread_cond_timedwait(&s->cond,&s->lock,&t);if(e==ETIMEDOUT){pthread_mutex_unlock(&s->lock);return 0;}}s->count=0;pthread_mutex_unlock(&s->lock);return 1;}
static inline int xSemaphoreGive(SemaphoreHandle_t s){if(s->binary){pthread_mutex_lock(&s->lock);s->count=1;pthread_cond_signal(&s->cond);}pthread_mutex_unlock(&s->lock);return 1;}
static inline void vSemaphoreDelete(SemaphoreHandle_t s){pthread_mutex_destroy(&s->lock);pthread_cond_destroy(&s->cond);free(s);}
