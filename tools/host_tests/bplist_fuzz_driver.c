#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *b, size_t n);
static uint8_t cur[8192]; static size_t curn;
static void on_alarm(int s){ (void)s; FILE*f=fopen("hang.bin","wb"); fwrite(cur,1,curn,f); fclose(f); fprintf(stderr,"HANG\n"); _exit(3);}
int main(int argc,char**argv){
  signal(SIGALRM,on_alarm); srand((unsigned)getpid());
  long iters = atol(argv[1]);
  static uint8_t seeds[64][4096]; size_t seedn[64]; int ns=0;
  for(int a=2;a<argc && ns<64;a++){FILE*f=fopen(argv[a],"rb"); if(!f)continue; seedn[ns]=fread(seeds[ns],1,4096,f); fclose(f); ns++;}
  for(long it=0; it<iters; it++){
    int si=rand()%ns; curn=seedn[si]; memcpy(cur,seeds[si],curn);
    int muts=1+rand()%8;
    for(int m=0;m<muts;m++){
      int k=rand()%4; size_t pos = curn? rand()%curn : 0;
      if(k==0 && curn) cur[pos]^=1u<<(rand()%8);
      else if(k==1 && curn) cur[pos]=(uint8_t)rand();
      else if(k==2 && curn) { static const uint8_t magic[]={0x0F,0x1F,0x13,0x12,0xFF,0x80,0x7F,0x00,0xDF,0xAF,0x4F,0x5F,0x6F}; cur[pos]=magic[rand()%sizeof magic]; }
      else if(k==3 && curn>40){ size_t t=curn-32+rand()%32; cur[t]=(uint8_t)rand(); }
    }
    uint8_t *b=malloc(curn?curn:1); memcpy(b,cur,curn);
    alarm(2); LLVMFuzzerTestOneInput(b,curn); alarm(0); free(b);
  }
  printf("ok %ld iterations\n", iters); return 0;
}
