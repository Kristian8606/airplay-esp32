#pragma once
#include "FreeRTOS.h"
static inline UBaseType_t uxTaskPriorityGet(void *t){(void)t;return 5;}
static inline int xTaskCreate(void (*f)(void*), const char *n,unsigned s,void*a,unsigned p,void*t){(void)n;(void)s;(void)p;(void)t;f(a);return pdPASS;}
static inline void vTaskDelete(void *t){(void)t;}
