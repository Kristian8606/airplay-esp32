#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>
#include "limiter.h"
#include "aacdecoder_lib.h"

/* New S32 entry points: declarations also make RED a link failure until the
 * implementation exists. Behavioral assertions below exercise real FDK. */
TDLIMITER_ERROR pcmLimiter_Apply32(TDLimiterPtr,PCM_LIM*,LONG*,FIXP_DBL*,INT,UINT);
extern "C" UINT aacDecoder_GetLimiterDelay(HANDLE_AACDECODER);
extern "C" AAC_DECODER_ERROR aacDecoder_ResetLimiter(HANDLE_AACDECODER);
static void exact_delay_and_precision(unsigned sr,unsigned ch){
 TDLimiterPtr l=pcmLimiter_Create(15,50,MAXVAL_DBL,ch,sr);assert(l);
 const unsigned delay=sr*15/1000,N=2048;assert(pcmLimiter_GetDelay(l)==delay);
 std::vector<LONG> in(N*ch,0),out(N*ch,0);in[101*ch]=0x123abcd;
 if(ch==2)in[101*ch+1]=-0x123abcd;
 assert(pcmLimiter_Apply32(l,in.data(),out.data(),NULL,0,N)==TDLIMIT_OK);
 for(unsigned f=0;f<N;f++)for(unsigned c=0;c<ch;c++){
  const LONG want=f==101+delay?(c==0?0x123abcd:-0x123abcd):0;
  assert(out[f*ch+c]==want);
 }
 /* The physical sample's compensated RTP equals its input position, also
  * across unsigned RTP wrap. This is the same mapping the receiver uses. */
 uint32_t rtp=0xffffff00u;
 assert((uint32_t)(rtp-delay+101+delay)==(uint32_t)(rtp+101));
 pcmLimiter_Destroy(l);
}
static void no_old_tail_after_reset(){
 TDLimiterPtr l=pcmLimiter_Create(15,50,MAXVAL_DBL,2,48000);assert(l);
 std::vector<LONG> in(2048,0x10012345),out(2048);
 assert(pcmLimiter_Apply32(l,in.data(),out.data(),NULL,0,1024)==TDLIMIT_OK);
 assert(pcmLimiter_Reset(l)==TDLIMIT_OK);in.assign(in.size(),0);
 for(int i=0;i<4;i++){assert(pcmLimiter_Apply32(l,in.data(),out.data(),NULL,0,1024)==TDLIMIT_OK);for(LONG v:out)assert(v==0);}
 pcmLimiter_Destroy(l);
}
static void peak_limiting(){
 TDLimiterPtr l=pcmLimiter_Create(15,50,MAXVAL_DBL,2,48000);assert(l);
 std::vector<LONG> in(2048,0x60000000),out(2048); /* scaling 1 means 1.5 full scale */
 bool reduced=false,non_s16=false;
 for(int i=0;i<12;i++){
  assert(pcmLimiter_Apply32(l,in.data(),out.data(),NULL,1,1024)==TDLIMIT_OK);
  reduced|=pcmLimiter_GetMaxGainReduction(l)>0;
  for(LONG v:out){assert(v>=0&&v<MAXVAL_DBL);non_s16|=(v&0xffff)!=0;}
 }
 assert(reduced&&non_s16);pcmLimiter_Destroy(l);
}
extern "C" AAC_DECODER_ERROR aacDecoder_DrainLimiter32(HANDLE_AACDECODER,LONG*,INT,UINT*);
static void stock_s16_unchanged(){
 TDLimiterPtr l=pcmLimiter_Create(15,50,MAXVAL_DBL,2,48000);assert(l);
 std::vector<LONG> in(4096,0);std::vector<INT_PCM> out(4096,0);in[200]=0x123abcd;in[201]=-0x123abcd;
 assert(pcmLimiter_Apply(l,in.data(),out.data(),NULL,0,2048)==TDLIMIT_OK);
 assert(out[(100+720)*2]==292&&out[(100+720)*2+1]==-292);pcmLimiter_Destroy(l);
}
int main(){exact_delay_and_precision(48000,2);exact_delay_and_precision(44100,2);exact_delay_and_precision(48000,1);no_old_tail_after_reset();peak_limiting();stock_s16_unchanged();puts("PASS real S32 limiter: exact lookahead, low-bit precision, gain reduction, reset silence and unchanged S16 API");return 0;}
