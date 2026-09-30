#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "plist.h"
int LLVMFuzzerTestOneInput(const uint8_t *b, size_t n) {
  uint8_t out[128]; size_t ol; int64_t iv; double dv; char s[64];
  const char *keys[] = {"shk","type","streams","Addresses","name",NULL};
  for (int i=0; keys[i]; i++) {
    bplist_find_data(b,n,keys[i],out,sizeof out,&ol);
    bplist_find_data_deep(b,n,keys[i],out,sizeof out,&ol);
    bplist_find_int(b,n,keys[i],&iv); bplist_find_real(b,n,keys[i],&dv);
    bplist_find_string(b,n,keys[i],s,sizeof s);
  }
  size_t c; bplist_get_streams_count(b,n,&c);
  int64_t t; size_t a,e,k; bplist_get_stream_info(b,n,0,&t,&a,&e,&k);
  bplist_kv_info_t kv[16]; size_t kc; bplist_get_stream_kv_info(b,n,0,kv,16,&kc);
  uint8_t ek[64],ev[16],sk[32]; size_t el,vl,sl;
  bplist_find_stream_crypto(b,n,103,ek,64,&el,ev,16,&vl,sk,32,&sl);
  bplist_peer_info_t p[4]; size_t pc;
  bplist_get_peer_list(b,n,0,p,4,&pc); bplist_get_peer_list(b,n,1,p,4,&pc);
  char d[256]; bplist_describe(b,n,d,sizeof d); char d1[1]; bplist_describe(b,n,d1,1);
  return 0;
}
