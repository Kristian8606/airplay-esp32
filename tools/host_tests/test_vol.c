/* Host test for apply_output_volume() (extracted from audio_receiver.c by
 * extract_volume.py): bit-perfect unity, silent blocks stay silent, mute,
 * ramp endpoints/monotonicity, TPDF dither statistics. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
void vol_new(int16_t*,uint32_t,int32_t*,int32_t);
#define F 256
static int fails=0;
#define CHECK(c,...) do{if(!(c)){fails++;printf("FAIL: " __VA_ARGS__);printf("\n");}}while(0)
int main(void){
  int16_t b[F*2], r[F*2]; int32_t cur;
  srand(1);
  /* 1 unity bit-perfect */
  for(int i=0;i<F*2;i++) r[i]=b[i]=(int16_t)(rand()-RAND_MAX/2);
  cur=32768; vol_new(b,F,&cur,32768); CHECK(!memcmp(b,r,sizeof b),"unity not bit-perfect");
  /* 2 silence stays silent at any gain, incl. ramps */
  memset(b,0,sizeof b); cur=10000; vol_new(b,F,&cur,20000);
  for(int i=0;i<F*2;i++) CHECK(b[i]==0,"silence ramp nonzero");
  CHECK(cur==20000,"cur not updated on silence");
  memset(b,0,sizeof b); vol_new(b,F,&cur,20000); for(int i=0;i<F*2;i++) CHECK(b[i]==0,"silence steady nonzero");
  /* 3 mute -> exact zeros */
  for(int i=0;i<F*2;i++) b[i]=1000;
  cur=0; vol_new(b,F,&cur,0);
  for(int i=0;i<F*2;i++) CHECK(b[i]==0,"mute not zero");
  /* 4 ramp to mute ends at exact zero */
  for(int i=0;i<F*2;i++) b[i]=30000;
  cur=16384; vol_new(b,F,&cur,0);
  CHECK(b[F*2-1]==0&&b[F*2-2]==0,"ramp end not zero");
  /* 5 ramp endpoints & monotonic, no overflow at full scale */
  for(int i=0;i<F*2;i++) b[i]=(i&1)?-32768:32767;
  cur=0; vol_new(b,F,&cur,32768);
  CHECK(abs(b[F*2-2]-32767)<=1 && abs(b[F*2-1]+32768)<=1,"ramp end %d %d",b[F*2-2],b[F*2-1]);
  for(int f=1;f<F;f++) CHECK(b[2*f]>=b[2*f-2]-1,"ramp not monotonic at %d",f);
  /* 7 dither statistics on a steady -12 dB gain: error vs ideal, mean ~0, within +-1.5 LSB */
  double sum=0,sum2=0; long n=0; int32_t cc=8192;
  for(int k=0;k<4000;k++){
    for(int i=0;i<F*2;i++) r[i]=b[i]=(int16_t)(20000*sin((k*F*2+i)*0.001234)+ (i%7));
    vol_new(b,F,&cc,8192);
    for(int i=0;i<F*2;i++){ double e=b[i]-r[i]*8192.0/32768.0; sum+=e; sum2+=e*e; n++; CHECK(fabs(e)<=1.51,"err %f",e); if(fails>5) return 1;}
  }
  double mean=sum/n, rms=sqrt(sum2/n-mean*mean);
  printf("dither: mean error %.4f LSB, rms %.3f LSB (TPDF+rounding ideal ~0.50)\n",mean,rms);
  CHECK(fabs(mean)<0.01,"biased mean %f",mean); CHECK(rms>0.4&&rms<0.65,"rms %f",rms);
  /* 8 low-level signal: no per-sample gating -> quiet signal crossing zero gets continuous dither */
  int zeros_dithered=0; cc=16384;
  for(int i=0;i<F*2;i++) b[i]=(int16_t)((i%4)-1); /* -1,0,1,2 pattern incl. zeros */
  vol_new(b,F,&cc,16384); for(int i=0;i<F*2;i+=4) if(b[i+1]!=0) zeros_dithered++;
  printf("low-level block: %d of %d zero samples received dither\n",zeros_dithered,F*2/4);
  printf(fails?"VOLUME FAILURES %d\n":"ALL VOLUME TESTS PASSED\n",fails);
  return fails!=0;
}
