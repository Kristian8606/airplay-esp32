#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_INTERNAL 0
static inline size_t heap_caps_get_free_size(int caps) { (void)caps; return 40960; }
static inline size_t heap_caps_get_largest_free_block(int caps) { (void)caps; return 32768; }
static inline size_t heap_caps_get_minimum_free_size(int caps) { (void)caps; return 40960; }
