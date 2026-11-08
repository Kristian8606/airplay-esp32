#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct{int unused;} esp_aes_context;
static inline void esp_aes_init(esp_aes_context *c){(void)c;}
static inline void esp_aes_free(esp_aes_context *c){(void)c;}
static inline int esp_aes_setkey(esp_aes_context*c,const unsigned char*k,unsigned n){(void)c;(void)k;(void)n;return -1;}
static inline int esp_aes_crypt_ctr(esp_aes_context*c,size_t n,size_t*o,unsigned char*a,unsigned char*b,const unsigned char*i,unsigned char*u){(void)c;(void)n;(void)o;(void)a;(void)b;(void)i;(void)u;return -1;}
