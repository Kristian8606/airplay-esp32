#pragma once
#include <stdlib.h>
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return malloc(1);}
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void){return malloc(1);}
static inline int xSemaphoreTake(SemaphoreHandle_t s,unsigned t){(void)s;(void)t;return 1;}
static inline int xSemaphoreGive(SemaphoreHandle_t s){(void)s;return 1;}
static inline void vSemaphoreDelete(SemaphoreHandle_t s){free(s);}
