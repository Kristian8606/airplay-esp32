#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "plist.h"
static void hex(const uint8_t *d, size_t n){ for(size_t i=0;i<n && i<8;i++) printf("%02x", d[i]); }
static void run(const uint8_t *b, size_t n) {
  const char *keys[] = {"shk","ekey","eiv","type","rate","rtpTime","networkTimeSecs","flushFromSeq","flushUntilSeq","name","deviceID","timingProtocol","k39","params","x",NULL};
  for (int i=0; keys[i]; i++) {
    uint8_t out[128]; size_t ol=0; int64_t iv=0; double dv=0; char s[64];
    int r1 = bplist_find_data(b,n,keys[i],out,sizeof out,&ol); printf("fd %s %d %zu ", keys[i], r1, r1?ol:0); if(r1) hex(out,ol);
    ol=0; int r2 = bplist_find_data_deep(b,n,keys[i],out,sizeof out,&ol); printf(" | dd %d %zu ", r2, r2?ol:0); if(r2) hex(out,ol);
    int r3 = bplist_find_int(b,n,keys[i],&iv); printf(" | fi %d %lld", r3, r3?(long long)iv:0);
    int r4 = bplist_find_real(b,n,keys[i],&dv); printf(" | fr %d %g", r4, r4?dv:0);
    int r5 = bplist_find_string(b,n,keys[i],s,sizeof s); printf(" | fs %d %s\n", r5, r5?s:"");
  }
  size_t c=0; int r = bplist_get_streams_count(b,n,&c); printf("streams %d %zu\n", r, r?c:0);
  for (size_t idx=0; idx<2; idx++) {
    int64_t t=-1; size_t a=0,e=0,k=0;
    r = bplist_get_stream_info(b,n,idx,&t,&a,&e,&k); printf("info[%zu] %d %lld %zu %zu %zu\n", idx, r, (long long)t, a,e,k);
    bplist_kv_info_t kv[16]; size_t kc=0;
    r = bplist_get_stream_kv_info(b,n,idx,kv,16,&kc); printf("kv[%zu] %d %zu:", idx, r, r?kc:0);
    if (r) for (size_t j=0;j<kc;j++) printf(" %s/%u/%zu/%lld", kv[j].key, kv[j].value_type, kv[j].value_len, (long long)kv[j].int_value);
    printf("\n");
  }
  for (int64_t st = 96; st <= 103; st += 7) {
    uint8_t ek[64],ev[16],sk[32]; size_t el=0,vl=0,sl=0;
    r = bplist_find_stream_crypto(b,n,st,ek,64,&el,ev,16,&vl,sk,32,&sl);
    printf("crypto %lld %d %zu %zu %zu\n",(long long)st,r,el,vl,sl);
  }
  for (int ext=0; ext<2; ext++) {
    bplist_peer_info_t p[4]; size_t pc=0;
    r = bplist_get_peer_list(b,n,ext,p,4,&pc); printf("peers ext=%d %d %zu:", ext, r, r?pc:0);
    if (r) for (size_t j=0;j<pc && j<4;j++){ printf(" [%zu", p[j].address_count); for(size_t q=0;q<p[j].address_count;q++) printf(" %s", p[j].addresses[q]); printf(" id=%d:%llx]", p[j].has_clock_id,(unsigned long long)p[j].clock_id);} 
    printf("\n");
  }
}
int main(int argc, char **argv) { setvbuf(stdout,NULL,_IONBF,0);
  for (int a=1; a<argc; a++) {
    FILE *f = fopen(argv[a],"rb"); if(!f) continue;
    static uint8_t buf[1<<20]; size_t n = fread(buf,1,sizeof buf,f); fclose(f);
    uint8_t *b = malloc(n ? n : 1); memcpy(b, buf, n);   /* exact-size heap copy for ASan */
    printf("== %s\n", argv[a]); run(b, n);
    { static char d[4096]; bplist_describe(b, n, d, sizeof d); printf("describe: %s\n", d); }
    free(b);
  }
  return 0;
}
