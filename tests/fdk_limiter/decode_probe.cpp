#include <assert.h>
#include <stdio.h>
#include <vector>
#include "aacdecoder_lib.h"
int main(int argc,char**argv){assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);std::vector<UCHAR>b(n);assert(fread(b.data(),1,n,f)==n);fclose(f);
HANDLE_AACDECODER h=aacDecoder_Open(TT_MP4_ADTS,1);assert(h);assert(aacDecoder_SetParam(h,AAC_PCM_LIMITER_ENABLE,1)==AAC_DEC_OK);assert(aacDecoder_SetParam(h,AAC_PCM_LIMITER_ATTACK_TIME,15)==AAC_DEC_OK);assert(aacDecoder_SetParam(h,AAC_CONCEAL_METHOD,1)==AAC_DEC_OK);
size_t off=0;unsigned frames=0;std::vector<LONG>pcm(8*2048);while(off+7<=n){size_t len=((b[off+3]&3)<<11)|(b[off+4]<<3)|(b[off+5]>>5);assert(len>=7&&off+len<=n);UCHAR*in=b.data()+off;UINT size=len,valid=len;assert(aacDecoder_Fill(h,&in,&size,&valid)==AAC_DEC_OK);AAC_DECODER_ERROR e=aacDecoder_DecodeFrame32(h,pcm.data(),pcm.size(),0);if(e!=AAC_DEC_OK)fprintf(stderr,"DecodeFrame32 with limiter returned 0x%x\n",e);assert(e==AAC_DEC_OK);off+=len;frames++;}assert(frames>10);aacDecoder_Close(h);printf("PASS actual AAC S32 decode with FDK limiter, %u frames\n",frames);}
