#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <algorithm>
extern "C" {
#include "aac_decoder.h"
#include "pcm_fifo.h"
}
#ifndef LIMITER_EXPECTED
#define LIMITER_EXPECTED 1
#endif
int main(int argc,char**argv){assert(argc==4);int sr=atoi(argv[2]);FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);std::vector<uint8_t>b(n);assert(fread(b.data(),1,n,f)==n);fclose(f);
aac_decoder_config_t cfg={sr,2,24};aac_decoder_t*d=aac_decoder_create(&cfg);assert(d);
const uint32_t delay=LIMITER_EXPECTED?(sr*15/1000):0;
assert(aac_decoder_limiter_delay_frames(d)==delay);assert(!aac_decoder_tail_due(d,0,5760));assert(aac_decoder_tail_wait_ms(d,46,20)==46);
uint32_t rtp=0xfffff000u;size_t off=0;std::vector<float>out(4096);f=fopen(argv[3],"wb");assert(f);unsigned frames=0;
while(off+7<=n){size_t len=((b[off+3]&3)<<11)|(b[off+4]<<3)|(b[off+5]>>5);assert(len>=7&&off+len<=n);aac_decode_info_t info={};
 int got=aac_decoder_decode(d,b.data()+off,len,out.data(),2048,rtp,false,&info);assert(got>0&&info.channels==2);assert(info.limiter_delay_frames==delay);assert(info.output_rtp==rtp-delay);
 assert(aac_decoder_tail_wait_ms(d,46,20)==(LIMITER_EXPECTED?20u:46u));
 if(LIMITER_EXPECTED){uint32_t margin=3840+sr*40/1000;uint32_t wait=aac_decoder_tail_wait_ms(d,(2048u*1000)/sr,20);uint32_t lead=margin+1;assert(lead-wait*sr/1000>3840+480);uint32_t pending=rtp+got-delay;assert(!aac_decoder_tail_due(d,pending-5761,5760));assert(aac_decoder_tail_due(d,pending-5760,5760));}
 for(int k=0;k<got*2;k++)assert(isfinite(out[k]));fwrite(out.data(),sizeof(float),got*2,f);rtp+=got;off+=len;frames++;}
aac_decode_info_t tail={};int drained=aac_decoder_drain(d,out.data(),2048,&tail);assert(drained==(int)delay);
if(drained){assert(tail.output_rtp==rtp-delay);fwrite(out.data(),sizeof(float),drained*2,f);}
assert(aac_decoder_drain(d,out.data(),2048,&tail)==0);
fclose(f);assert(frames>10);aac_decoder_destroy(d);
/* First/muted AUs advance AAC history but neither their output nor their
 * lookahead tail may leak into the next unmuted AU. */
d=aac_decoder_create(&cfg);assert(d);off=0;rtp=48000;
for(int i=0;i<4;i++){size_t len=((b[off+3]&3)<<11)|(b[off+4]<<3)|(b[off+5]>>5);aac_decode_info_t info={};bool mute=i==0||i==2;uint32_t tag=rtp+(i>=2?10000:0);
 int got=aac_decoder_decode(d,b.data()+off,len,out.data(),2048,tag,mute,&info);assert(got>0);
 if(mute)for(int k=0;k<got*2;k++)assert(out[k]==0);
 if(!mute&&LIMITER_EXPECTED)for(unsigned k=0;k<delay*2;k++)assert(out[k]==0);
 rtp+=got;off+=len;}
aac_decoder_destroy(d);
/* Simulate a held-open TCP stream whose next AU is delayed: drain at the
 * DMA deadline, then resume a contiguous AU. Use the real PCM FIFO so the
 * new reset-generated prefix cannot erase the already published tail. */
d=aac_decoder_create(&cfg);assert(d);pcm_fifo_t *fifo;assert(pcm_fifo_create(&fifo)==ESP_OK);pcm_fifo_set_generation(fifo,1);off=0;rtp=0xfffff000u;uint32_t initial=rtp,total=0;unsigned au=0;
while(off+7<=n){size_t len=((b[off+3]&3)<<11)|(b[off+4]<<3)|(b[off+5]>>5);aac_decode_info_t info={};int got=aac_decoder_decode(d,b.data()+off,len,out.data(),2048,rtp,false,&info);assert(got>0);assert(pcm_fifo_write(fifo,info.output_rtp,out.data(),got,1,0,false));rtp+=got;total+=got;off+=len;au++;
 if(au==10&&LIMITER_EXPECTED){uint32_t due=rtp-delay-5760;assert(aac_decoder_tail_due(d,due,5760));int tail=aac_decoder_drain(d,out.data(),2048,&info);assert(tail==(int)delay);assert(pcm_fifo_write(fifo,info.output_rtp,out.data(),tail,1,0,false));assert(!aac_decoder_tail_due(d,due,5760));}}
aac_decode_info_t final={};int last=aac_decoder_drain(d,out.data(),2048,&final);assert(last==(int)delay);if(last)assert(pcm_fifo_write(fifo,final.output_rtp,out.data(),last,1,0,false));
std::vector<float>timeline(total*2);assert(pcm_fifo_read(fifo,initial,total,1,timeline.data())==total);std::string filename=std::string(argv[3])+".timeline";f=fopen(filename.c_str(),"wb");assert(f);fwrite(timeline.data(),sizeof(float),timeline.size(),f);fclose(f);pcm_fifo_destroy(fifo);aac_decoder_destroy(d);
printf("PASS wrapper limiter=%d sr=%d: actual delay, RTP wrap, mute/seek tail removal\n",LIMITER_EXPECTED,sr);return 0;}
