/* Tests production request handlers and actual Ed25519/X25519/AEAD.
 * Hardware ports are inert: any attempted audio operation is counted. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
#include <time.h>
#include "hap_private.h"
#include "srp.h"
#include "nvs.h"
#include "fixtures.h"
#include "../../components/ap2_core/src/rtsp/rtsp_core.c"
int audio_calls, playing_starts, buffered_starts, stop_calls;
bool engine_playing;
static rtsp_conn_t *fail_event, *closed_conn;
static uint32_t selected_peer;
static int clock_calls, group_calls;static char group[48];
static size_t largest_alloc;
void *rtsp_port_malloc(size_t n){if(n>largest_alloc)largest_alloc=n;return malloc(n);}
void rtsp_port_free(void*p){free(p);}
void rtsp_port_lock(void){}
void rtsp_port_unlock(void){}
uint32_t rtsp_port_now_ms(void){return 0;}
esp_err_t rtsp_port_settings_get_volume(float*v){*v=-24;return ESP_OK;}
esp_err_t rtsp_port_settings_set_volume(float v){return ESP_OK;}
esp_err_t rtsp_port_settings_persist_volume(void){return ESP_OK;}
esp_err_t rtsp_port_settings_get_name(char*n,size_t c){snprintf(n,c,"Speaker");return ESP_OK;}
void rtsp_port_device_mac(uint8_t m[6]){memset(m,1,6);}
esp_err_t rtsp_port_event_open(rtsp_conn_t*c,const uint8_t*w,const uint8_t*r,uint16_t*p){if(c==fail_event)return ESP_FAIL;*p=9000;return ESP_OK;}
void rtsp_port_event_close(rtsp_conn_t*c){}
void rtsp_port_conn_close(rtsp_conn_t*c){closed_conn=c;}
esp_err_t rtsp_port_udp_pair(uint16_t*a,uint16_t*b){*a=9001;*b=9002;return ESP_OK;}
void rtsp_remote_detach(void){}
void rtsp_remote_conn_gone(const rtsp_conn_t*c){}
void apple_clock_select(uint32_t ip,bool r){clock_calls++;selected_peer=ip;}
void apple_clock_release(void){clock_calls++;}
void amp_control_session_connected(void){}
void amp_control_session_disconnected(void){}
void rtsp_events_emit(rtsp_event_t e){}
void mdns_airplay_update_flags(void){}
void mdns_airplay_set_group(const char*g,bool l){group_calls++;snprintf(group,sizeof(group),"%s",g?g:"");}
int ap2_fairplay_setup(const uint8_t*a,size_t b,uint8_t*c,size_t d){return -1;}
struct ap2_srp{int unused;};
ap2_srp_t *ap2_srp_new(void){return calloc(1,sizeof(ap2_srp_t));}
void ap2_srp_delete(ap2_srp_t*s){free(s);}
esp_err_t ap2_srp_start(ap2_srp_t*s,const char*u,const char*p){return ESP_FAIL;}
const uint8_t *ap2_srp_salt(ap2_srp_t*s){static uint8_t k[16];return k;}
const uint8_t *ap2_srp_public_key(ap2_srp_t*s,size_t*n){static uint8_t k[384];*n=sizeof(k);return k;}
esp_err_t ap2_srp_verify_client(ap2_srp_t*s,const uint8_t*k,size_t n,const uint8_t*p,size_t l){return ESP_FAIL;}
const uint8_t *ap2_srp_proof(ap2_srp_t*s){static uint8_t k[64];return k;}
const uint8_t *ap2_srp_session_key(ap2_srp_t*s,size_t*n){static uint8_t k[64];*n=sizeof(k);return k;}
static struct{char key[32];size_t len;uint8_t data[4096];} blobs[4];
esp_err_t nvs_open(const char*n,int mode,nvs_handle_t*h){*h=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*p,size_t*n){for(int i=0;i<4;i++)if(!strcmp(k,blobs[i].key)){if(*n<blobs[i].len)return ESP_ERR_INVALID_SIZE;*n=blobs[i].len;memcpy(p,blobs[i].data,*n);return ESP_OK;}return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*p,size_t n){for(int i=0;i<4;i++)if(!blobs[i].key[0]||!strcmp(k,blobs[i].key)){assert(n<=4096);snprintf(blobs[i].key,32,"%s",k);blobs[i].len=n;memcpy(blobs[i].data,p,n);return ESP_OK;}return ESP_FAIL;}
esp_err_t nvs_commit(nvs_handle_t h){return ESP_OK;}
void nvs_close(nvs_handle_t h){}
static rtsp_msg_t request(const char*method,const char*path,const uint8_t*b,size_t n){rtsp_msg_t m={0};m.start[0]=method;m.start[1]=path;m.start[2]="RTSP/1.0";m.body=(uint8_t*)b;m.body_len=n;return m;}
static reply_t dispatch(rtsp_conn_t*c,const char*method,const char*path,const uint8_t*b,size_t n){rtsp_msg_t m=request(method,path,b,n);reply_t r={.code=200};handle(c,&m,&r);return r;}
static int error(const rtsp_conn_t*c,const reply_t*r){size_t n=0;const uint8_t*p=ap2_tlv8_find(c->body,r->body_len,AP2_TLV_ERROR,&n);return p?*p:0;}
static void management(void){
 rtsp_conn_t*c=rtsp_conn_new(0,NULL);c->hap=ap2_hap_session_new();uint8_t pk[32]={9};const uint8_t id[]="admin";
 uint8_t b[128];ap2_tlv8_writer_t w;ap2_tlv8_writer_init(&w,b,sizeof(b));ap2_tlv8_put(&w,AP2_TLV_IDENTIFIER,id,5);ap2_tlv8_put(&w,AP2_TLV_PUBLIC_KEY,pk,32);/* No Permissions in bootstrap request. */
 reply_t a=dispatch(c,"POST","/pair-add",b,w.size);assert(error(c,&a)==AP2_TLV_ERROR_AUTHENTICATION);assert(ap2_pairings_count()==0);
 c->encrypted=true;c->paired=true;c->hap->pair_setup_transient=true;
 a=dispatch(c,"POST","/pair-add",b,w.size);assert(!error(c,&a));assert(ap2_pairings_has_admin());assert(ap2_hap_controller(c->hap,NULL,NULL,NULL));
 rtsp_conn_t*u=rtsp_conn_new(0,NULL);u->hap=ap2_hap_session_new();u->encrypted=true;u->paired=true;u->hap->pair_setup_transient=true;
 a=dispatch(u,"POST","/pair-add",b,w.size);assert(!error(u,&a));assert(!ap2_hap_controller(u->hap,NULL,NULL,NULL)); /* Idempotent ack grants no admin rights. */
 a=dispatch(u,"POST","/pair-list",NULL,0);assert(error(u,&a)==AP2_TLV_ERROR_AUTHENTICATION);
 a=dispatch(c,"POST","/pair-list",NULL,0);assert(!error(c,&a));size_t listed=0;const uint8_t *listed_id=ap2_tlv8_find(c->body,a.body_len,AP2_TLV_IDENTIFIER,&listed);assert(listed_id&&listed==5&&!memcmp(listed_id,"admin",5));
 assert(ap2_pairings_remove(id,5)==ESP_OK);assert(!ap2_hap_controller(c->hap,NULL,NULL,NULL));
 a=dispatch(c,"POST","/pair-list",NULL,0);assert(error(c,&a)==AP2_TLV_ERROR_AUTHENTICATION);
 c->encrypted=false;u->encrypted=false;rtsp_conn_end(c);rtsp_conn_end(u);
 puts("PASS encrypted first enrollment, no public-key-only admin retry, revocation and listing");
}
static void remote_setup(void){
 rtsp_conn_t*c=rtsp_conn_new(1,NULL);c->hap=ap2_hap_session_new();c->encrypted=true;c->paired=true;c->hap->session_established=true;
 assert(rtsp_core_idle());int ac=audio_calls,cc=clock_calls;
 reply_t a=dispatch(c,"SETUP","/",remote_fixture,sizeof(remote_fixture));assert(a.code==200);assert(c->event_open);assert(rtsp_core_idle());assert(audio_calls==ac&&clock_calls==cc);
 a=dispatch(c,"SETUP","/",stream_fixture,sizeof(stream_fixture));assert(a.code==455);assert(rtsp_core_idle());assert(audio_calls==ac&&clock_calls==cc);
 c->encrypted=false;rtsp_conn_end(c);assert(audio_calls==ac&&clock_calls==cc);
 rtsp_conn_t*owner=rtsp_conn_new(1,NULL);owner->hap=ap2_hap_session_new();owner->paired=true;owner->encrypted=true;owner->hap->session_established=true;
 a=dispatch(owner,"SETUP","/",normal_fixture,sizeof(normal_fixture));assert(a.code==200);assert(rtsp_core_holder()==owner);assert(!strcmp(group,"a-group"));
 a=dispatch(owner,"SETUP","/",empty_fixture,sizeof(empty_fixture));assert(a.code==200);assert(!strcmp(group,"a-group"));
 c=rtsp_conn_new(2,NULL);c->hap=ap2_hap_session_new();c->paired=true;c->encrypted=true;
 ac=audio_calls;cc=clock_calls;
 a=dispatch(c,"SETUP","/",remote_fixture,sizeof(remote_fixture));assert(a.code==200);assert(rtsp_core_holder()==owner);assert(audio_calls==ac&&clock_calls==cc);
 c->encrypted=false;rtsp_conn_end(c);assert(rtsp_core_holder()==owner);assert(audio_calls==ac&&clock_calls==cc);
 owner->encrypted=false;rtsp_conn_end(owner);assert(rtsp_core_idle());puts("PASS Home hub remote-only SETUP/stream/close cannot take or stop audio");
}
static rtsp_conn_t *paired_connection(uint32_t peer) {
 rtsp_conn_t *c=rtsp_conn_new(peer,NULL);
 assert(c);c->hap=ap2_hap_session_new();assert(c->hap);
 c->paired=true;c->encrypted=true;c->hap->session_established=true;
 return c;
}
static void takeover_waits_for_music(void) {
 rtsp_conn_t *phone=paired_connection(208),*tv=paired_connection(116);
 reply_t a=dispatch(phone,"SETUP","/",normal_fixture,sizeof(normal_fixture));
 assert(a.code==200&&rtsp_core_holder()==phone);
 a=dispatch(phone,"SETUP","/",audio_fixture,sizeof(audio_fixture));assert(a.code==200);
 a=dispatch(phone,"SETRATEANCHORTIME","/",play_fixture,sizeof(play_fixture));
 assert(a.code==200&&engine_playing);
 int starts=playing_starts,buffers=buffered_starts;
 a=dispatch(tv,"SETUP","/",normal_fixture,sizeof(normal_fixture));
 assert(a.code==200&&rtsp_core_holder()==tv);
 assert(phone->closing&&closed_conn==phone&&!phone->event_open&&tv->event_open);
 assert(!s.stream&&!engine_playing&&playing_starts==starts&&buffered_starts==buffers);
 assert(selected_peer==116);
 int ac=audio_calls,cc=clock_calls;
 a=dispatch(tv,"SETUP","/",normal_fixture,sizeof(normal_fixture));
 assert(a.code==200&&audio_calls==ac&&clock_calls==cc);
 a=dispatch(phone,"SETRATEANCHORTIME","/",play_fixture,sizeof(play_fixture));
 a=dispatch(phone,"TEARDOWN","/",end_audio_fixture,sizeof(end_audio_fixture));
 assert(rtsp_core_holder()==tv&&audio_calls==ac&&!engine_playing);
 a=dispatch(phone,"SETUP","/",normal_fixture,sizeof(normal_fixture));
 assert(a.code!=200&&rtsp_core_holder()==tv&&audio_calls==ac);
 a=dispatch(tv,"RECORD","/",NULL,0);assert(a.code==200&&!engine_playing);
 a=dispatch(tv,"SETUP","/",audio_fixture,sizeof(audio_fixture));
 assert(a.code==200&&s.stream&&!engine_playing&&buffered_starts==buffers+1&&playing_starts==starts);
 a=dispatch(tv,"RECORD","/",NULL,0);assert(a.code==200&&!engine_playing);
 a=dispatch(tv,"SETRATEANCHORTIME","/",play_fixture,sizeof(play_fixture));
 assert(a.code==200&&engine_playing&&playing_starts==starts+1);
 rtsp_conn_t *hub=paired_connection(116);ac=audio_calls;cc=clock_calls;
 a=dispatch(hub,"SETUP","/",remote_fixture,sizeof(remote_fixture));
 assert(a.code==200&&rtsp_core_holder()==tv&&engine_playing&&audio_calls==ac&&clock_calls==cc);
 hub->encrypted=false;rtsp_conn_end(hub);
 assert(rtsp_core_holder()==tv&&engine_playing&&audio_calls==ac&&clock_calls==cc);
 rtsp_conn_t *bad=paired_connection(99);fail_event=bad;ac=audio_calls;cc=clock_calls;
 a=dispatch(bad,"SETUP","/",normal_fixture,sizeof(normal_fixture));
 assert(a.code==500&&rtsp_core_holder()==tv&&engine_playing&&audio_calls==ac&&clock_calls==cc);
 assert(!tv->closing&&tv->event_open&&!bad->event_open);fail_event=NULL;
 bad->hap->session_established=false;
 a=dispatch(bad,"SETUP","/",normal_fixture,sizeof(normal_fixture));
 assert(a.code==500&&rtsp_core_holder()==tv&&engine_playing&&audio_calls==ac&&clock_calls==cc);
 a=dispatch(tv,"PAUSE","/",NULL,0);assert(a.code==200&&!engine_playing);
 a=dispatch(tv,"SETRATEANCHORTIME","/",play_fixture,sizeof(play_fixture));assert(engine_playing);
 cc=clock_calls;a=dispatch(tv,"TEARDOWN","/",end_audio_fixture,sizeof(end_audio_fixture));
 assert(a.code==200&&!engine_playing&&!s.stream&&rtsp_core_holder()==tv&&clock_calls==cc);
 a=dispatch(tv,"SETUP","/",audio_fixture,sizeof(audio_fixture));assert(a.code==200&&!engine_playing);
 a=dispatch(tv,"SETRATEANCHORTIME","/",play_fixture,sizeof(play_fixture));assert(engine_playing);
 phone->encrypted=false;rtsp_conn_end(phone);assert(rtsp_core_holder()==tv&&engine_playing);
 bad->encrypted=false;rtsp_conn_end(bad);assert(rtsp_core_holder()==tv&&engine_playing);
 tv->encrypted=false;rtsp_conn_end(tv);assert(rtsp_core_idle()&&!engine_playing);
 puts("PASS initial audio SETUP takes ownership; buffered playback waits for play anchor; stale/failed candidates cannot disturb owner");
}
static void configure(const char*dir){
 rtsp_conn_t*c=rtsp_conn_new(0,NULL);c->hap=ap2_hap_session_new();c->paired=true;
 reply_t a=dispatch(c,"POST","/configure",configure_fixture,sizeof(configure_fixture));assert(a.code==470);
 c->encrypted=true;a=dispatch(c,"POST","/configure",configure_fixture,sizeof(configure_fixture));assert(a.code==200&&a.body_len);
 char path[1024];snprintf(path,sizeof(path),"%s/configure.plist",dir);FILE*f=fopen(path,"wb");assert(f);fwrite(c->body,1,a.body_len,f);fclose(f);
 a=dispatch(c,"POST","/configure",restricted_fixture,sizeof(restricted_fixture));assert(a.code==200&&a.body_len);int64_t level=-1;assert(ap2_bplist_int(c->body,a.body_len,"Access_Control_Level",&level)&&level==0);
 a=dispatch(c,"GET","/info",NULL,0);assert(a.code==200&&a.body_len);snprintf(path,sizeof(path),"%s/info.plist",dir);f=fopen(path,"wb");assert(f);fwrite(c->body,1,a.body_len,f);fclose(f);
 c->encrypted=false;rtsp_conn_end(c);puts("PASS configure and info replies emitted for independent plistlib decoding");
}
static void exchange_m5(bool transient,bool owned){
 uint8_t ownerpk[32]={8};const uint8_t owner[]="owner",attacker[]="attacker";
 if(owned)assert(ap2_pairings_add(owner,5,ownerpk,1)==ESP_OK);
 ap2_hap_session_t *ss=ap2_hap_session_new();ss->pair_setup_state=4;ss->pair_setup_transient=transient;ss->srp=ap2_srp_new();
 uint8_t key[64]={0},signkey[32],enc[32],pk[32],sk[64],sig[64],info[128],plain[200],cipher[216],in[220],out[512];size_t n;
 crypto_sign_keypair(pk,sk);ap2_hap_kdf("Pair-Setup-Controller-Sign-Salt","Pair-Setup-Controller-Sign-Info",key,64,signkey,32);
 memcpy(info,signkey,32);memcpy(info+32,attacker,8);memcpy(info+40,pk,32);crypto_sign_detached(sig,NULL,info,72,sk);
 ap2_tlv8_writer_t w;ap2_tlv8_writer_init(&w,plain,sizeof(plain));ap2_tlv8_put(&w,AP2_TLV_IDENTIFIER,attacker,8);ap2_tlv8_put(&w,AP2_TLV_PUBLIC_KEY,pk,32);ap2_tlv8_put(&w,AP2_TLV_SIGNATURE,sig,64);
 ap2_hap_kdf("Pair-Setup-Encrypt-Salt","Pair-Setup-Encrypt-Info",key,64,enc,32);ap2_hap_seal(enc,"PS-Msg05",plain,w.size,cipher);size_t z=w.size+16;
 ap2_tlv8_writer_init(&w,in,sizeof(in));ap2_tlv8_put(&w,AP2_TLV_ENCRYPTED_DATA,cipher,z);
 assert(ap2_hap_setup_m5(ss,in,w.size,out,sizeof(out),&n)==ESP_OK);
 size_t e;
 if(owned || transient){assert(ap2_tlv8_find(out,n,AP2_TLV_ERROR,&e));assert(ap2_pairings_count()==(owned?1u:0u));assert(!ap2_pairings_find(attacker,8,NULL,NULL));}
 else {
  assert(!ap2_tlv8_find(out,n,AP2_TLV_ERROR,&e));assert(ap2_pairings_count()==1);assert(ap2_hap_controller(ss,NULL,NULL,NULL));
  assert(ap2_tlv8_get(out,n,AP2_TLV_ENCRYPTED_DATA,cipher,sizeof(cipher),&z));assert(ap2_hap_open(enc,"PS-Msg06",cipher,z,plain)==ESP_OK);
  uint8_t aid[64];size_t aid_len,sglen;assert(ap2_tlv8_get(plain,z-16,AP2_TLV_IDENTIFIER,aid,sizeof(aid),&aid_len));assert(aid_len==36);assert(ap2_tlv8_get(plain,z-16,AP2_TLV_SIGNATURE,sig,sizeof(sig),&sglen)&&sglen==64);
  ap2_hap_kdf("Pair-Setup-Accessory-Sign-Salt","Pair-Setup-Accessory-Sign-Info",key,64,signkey,32);memcpy(info,signkey,32);memcpy(info+32,aid,aid_len);memcpy(info+32+aid_len,ap2_hap_identity_public_key(),32);
  assert(crypto_sign_verify_detached(sig,info,64+aid_len,ap2_hap_identity_public_key())==0);assert(ap2_pairings_remove(attacker,8)==ESP_OK);
 }
 ap2_hap_session_delete(ss);if(owned)assert(ap2_pairings_remove(owner,5)==ESP_OK);
 puts(!owned&&!transient?"PASS signed full setup M5/M6 stores owner and presents verifiable pi":transient?"PASS transient M5 cannot enroll an administrator":"PASS interleaved full setup cannot add a second initial administrator");
}
static void verify(void){
 uint8_t pk[32],sk[64],cp[32],cs[32],in[1024],out[1024],secret[32],key[32],plain[256],cipher[272],msg[128],sig[64];size_t n,z,idn;const uint8_t id[]="verified-admin";
 crypto_sign_keypair(pk,sk);assert(ap2_pairings_add(id,sizeof(id)-1,pk,1)==ESP_OK);crypto_box_keypair(cp,cs);
 ap2_hap_session_t*s=ap2_hap_session_new();ap2_tlv8_writer_t w;ap2_tlv8_writer_init(&w,in,sizeof(in));ap2_tlv8_put(&w,AP2_TLV_PUBLIC_KEY,cp,32);
 assert(ap2_hap_verify_m1(s,in,w.size,out,sizeof(out),&n)==ESP_OK);
 assert(crypto_scalarmult(secret,cs,s->session_public_key)==0);assert(ap2_hap_kdf("Pair-Verify-Encrypt-Salt","Pair-Verify-Encrypt-Info",secret,32,key,32)==ESP_OK);
 assert(ap2_tlv8_get(out,n,AP2_TLV_ENCRYPTED_DATA,cipher,sizeof(cipher),&z));assert(ap2_hap_open(key,"PV-Msg02",cipher,z,plain)==ESP_OK);
 uint8_t aid[64];assert(ap2_tlv8_get(plain,z-16,AP2_TLV_IDENTIFIER,aid,sizeof(aid),&idn));assert(idn==36);assert(ap2_tlv8_get(plain,z-16,AP2_TLV_SIGNATURE,sig,64,&n));
 memcpy(msg,s->session_public_key,32);memcpy(msg+32,aid,idn);memcpy(msg+32+idn,cp,32);assert(crypto_sign_verify_detached(sig,msg,64+idn,ap2_hap_identity_public_key())==0);
 memcpy(msg,cp,32);memcpy(msg+32,id,sizeof(id)-1);memcpy(msg+32+sizeof(id)-1,s->session_public_key,32);crypto_sign_detached(sig,NULL,msg,64+sizeof(id)-1,sk);
 sig[0]^=1;ap2_tlv8_writer_init(&w,plain,sizeof(plain));ap2_tlv8_put(&w,AP2_TLV_IDENTIFIER,id,sizeof(id)-1);ap2_tlv8_put(&w,AP2_TLV_SIGNATURE,sig,64);ap2_hap_seal(key,"PV-Msg03",plain,w.size,cipher);z=w.size+16;
 ap2_tlv8_writer_init(&w,in,sizeof(in));ap2_tlv8_put(&w,AP2_TLV_ENCRYPTED_DATA,cipher,z);assert(ap2_hap_verify_m3(s,in,w.size,out,sizeof(out),&n)==ESP_OK);size_t en;assert(ap2_tlv8_find(out,n,AP2_TLV_ERROR,&en));assert(!ap2_hap_controller(s,NULL,NULL,NULL));assert(!ap2_hap_established(s));sig[0]^=1;
 ap2_tlv8_writer_init(&w,plain,sizeof(plain));ap2_tlv8_put(&w,AP2_TLV_IDENTIFIER,id,sizeof(id)-1);ap2_tlv8_put(&w,AP2_TLV_SIGNATURE,sig,64);ap2_hap_seal(key,"PV-Msg03",plain,w.size,cipher);z=w.size+16;
 ap2_tlv8_writer_init(&w,in,sizeof(in));ap2_tlv8_put(&w,AP2_TLV_ENCRYPTED_DATA,cipher,z);assert(ap2_hap_verify_m3(s,in,w.size,out,sizeof(out),&n)==ESP_OK);assert(ap2_hap_controller(s,NULL,NULL,NULL));
 /* Repeat M1 must revoke old authentication until the new M3 succeeds. */
 ap2_tlv8_writer_init(&w,in,sizeof(in));ap2_tlv8_put(&w,AP2_TLV_PUBLIC_KEY,cp,32);assert(ap2_hap_verify_m1(s,in,w.size,out,sizeof(out),&n)==ESP_OK);assert(!ap2_hap_controller(s,NULL,NULL,NULL));assert(!ap2_hap_established(s));
 ap2_hap_session_delete(s);assert(ap2_pairings_remove(id,sizeof(id)-1)==ESP_OK);puts("PASS pair-verify pi signature, real encrypted M3 and authentication reset");
}
typedef struct{rtsp_frame_reader_t frames;unsigned replies;} capture_t;
static int capture(void*ctx,const uint8_t*p,size_t n){capture_t*c=ctx;size_t used,pn;const uint8_t*plain;assert(rtsp_frame_feed(&c->frames,p,n,&used,&plain,&pn)==RTSP_FRAME_DONE);assert(used==n);assert(pn>=15&&!memcmp(plain,"RTSP/1.0 200 OK",15));c->replies++;return 0;}
static void encrypted_artwork(void){
 rtsp_conn_t*c=rtsp_conn_new(0,NULL);c->hap=ap2_hap_session_new();c->paired=true;c->encrypted=true;
 const uint8_t zero[32]={0};rtsp_frame_reader_init(&c->in_frames,zero);rtsp_cipher_init(&c->out_cipher,zero);rtsp_cipher_t client;rtsp_cipher_init(&client,zero);capture_t cap={0};rtsp_frame_reader_init(&cap.frames,zero);
 const char*h="SET_PARAMETER / RTSP/1.0\r\nContent-Type: image/jpeg\r\nContent-Length: 200000\r\nCSeq: 20\r\n\r\n";uint8_t wire[1042],body[1024]={0};size_t n=rtsp_frame_seal(&client,(const uint8_t*)h,strlen(h),wire);assert(rtsp_conn_receive(c,wire,n,capture,&cap)==0);assert(!cap.replies);
 size_t left=200000;while(left){size_t take=left<sizeof(body)?left:sizeof(body);n=rtsp_frame_seal(&client,body,take,wire);/* Split every encrypted frame across two socket reads. */assert(rtsp_conn_receive(c,wire,7,capture,&cap)==0);assert(rtsp_conn_receive(c,wire+7,n-7,capture,&cap)==0);left-=take;}
 assert(cap.replies==1);assert(largest_alloc<65536);h="OPTIONS * RTSP/1.0\r\nCSeq: 21\r\n\r\n";n=rtsp_frame_seal(&client,(const uint8_t*)h,strlen(h),wire);assert(rtsp_conn_receive(c,wire,n,capture,&cap)==0);assert(cap.replies==2);c->encrypted=false;rtsp_conn_end(c);puts("PASS 200KB encrypted fragmented artwork and subsequent request preserve frame counters without large allocation");
}
static void nested_stress(void){int64_t value;clock_t start=clock();assert(!ap2_bplist_int_deep(stress_fixture,sizeof(stress_fixture),"missing",&value));double seconds=(double)(clock()-start)/CLOCKS_PER_SEC;printf("Nested 62KB adversarial plist: %.3f CPU s\n",seconds);assert(seconds<1.0);puts("PASS depth-limit arrays cannot multiply plist traversal work");}
int main(int argc,char**argv){assert(argc==2);assert(ap2_hap_identity_init()==ESP_OK);nested_stress();management();remote_setup();takeover_waits_for_music();configure(argv[1]);verify();exchange_m5(true,true);exchange_m5(false,true);exchange_m5(false,false);encrypted_artwork();return 0;}
