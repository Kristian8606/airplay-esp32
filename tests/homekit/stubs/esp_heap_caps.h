#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
static inline void *heap_caps_calloc(size_t n,size_t s,int c){(void)c;return calloc(n,s);}
static inline void *heap_caps_malloc(size_t n,int c){(void)c;return malloc(n);}
#define heap_caps_free free
