#pragma once
#include "FreeRTOS.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
typedef struct{void(*fn)(void*);void*arg;} HostTask;
static inline void *host_task(void *p){HostTask t=*(HostTask*)p;free(p);t.fn(t.arg);return 0;}
static inline int xTaskCreatePinnedToCore(void(*fn)(void*),const char*n,unsigned stack,void*arg,unsigned prio,TaskHandle_t*h,int core){(void)n;(void)stack;(void)prio;(void)core;HostTask*p=malloc(sizeof(*p));*p=(HostTask){fn,arg};*h=(void*)1;pthread_t th;if(pthread_create(&th,0,host_task,p)){free(p);*h=0;return 0;}pthread_detach(th);return pdPASS;}
static inline void vTaskDelay(unsigned ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,0);}
static inline void vTaskDelete(void*p){(void)p;pthread_exit(0);}
