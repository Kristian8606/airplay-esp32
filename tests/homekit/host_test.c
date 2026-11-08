/* Behavioral host tests: real RTSP reader, TLV8 and pairing store.
 * Only FreeRTOS allocation/scheduling and NVS hardware are substituted. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ap2_pairings.h"
#include "ap2_tlv8.h"
#include "rtsp_msg.h"
#include "nvs.h"
static int allocs, fail_commit, read_error;
static unsigned char saved[4096], pending[4096];
static size_t saved_len,pending_len;
void *rtsp_port_malloc(size_t n){allocs++;return malloc(n);}
void rtsp_port_free(void *p){free(p);}
esp_err_t nvs_open(const char*n,int m,nvs_handle_t*h){assert(!strcmp(n,"airplay"));(void)m;*h=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*p,size_t*n){(void)h;assert(!strcmp(k,"hk_pairs"));if(read_error)return read_error;if(!saved_len)return ESP_ERR_NVS_NOT_FOUND;if(*n<saved_len)return ESP_ERR_INVALID_SIZE;memcpy(p,saved,saved_len);*n=saved_len;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*p,size_t n){(void)h;assert(!strcmp(k,"hk_pairs"));assert(n<=sizeof(pending));memcpy(pending,p,n);pending_len=n;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;if(fail_commit)return ESP_FAIL;memcpy(saved,pending,pending_len);saved_len=pending_len;return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}
static void artwork(void){
 rtsp_reader_t r;rtsp_reader_init(&r,65536);const char *h="SET_PARAMETER / RTSP/1.0\r\nCSeq: 1\r\nContent-Type: image/jpeg\r\nContent-Length: 200000\r\n\r\n";
 size_t used;assert(rtsp_reader_feed(&r,(const uint8_t*)h,strlen(h),&used)==RTSP_READ_MORE);assert(used==strlen(h));assert(allocs==0);
 uint8_t chunk[997]={0};size_t left=200000;
 while(left){size_t n=left<sizeof(chunk)?left:sizeof(chunk);rtsp_read_result_t got=rtsp_reader_feed(&r,chunk,n,&used);assert(used==n);left-=n;assert(got==(left?RTSP_READ_MORE:RTSP_READ_MESSAGE));}
 assert(r.msg.body_state==RTSP_BODY_OK);assert(!r.msg.body);assert(r.msg.body_len==200000);assert(allocs==0);
 rtsp_reader_next(&r);h="OPTIONS * RTSP/1.0\r\nCSeq: 2\r\n\r\n";assert(rtsp_reader_feed(&r,(const uint8_t*)h,strlen(h),&used)==RTSP_READ_MESSAGE);assert(!strcmp(r.msg.start[0],"OPTIONS"));rtsp_reader_next(&r);
 puts("PASS artwork is streamed/discarded with constant memory; next request intact");
}
static void oversized_control(void){
 rtsp_reader_t r;rtsp_reader_init(&r,8);size_t used;const char*h="POST /configure RTSP/1.0\r\nContent-Length: 9\r\n\r\n123456789";assert(rtsp_reader_feed(&r,(const uint8_t*)h,strlen(h),&used)==RTSP_READ_MESSAGE);assert(r.msg.body_state==RTSP_BODY_TOO_LARGE);assert(!r.msg.body);rtsp_reader_next(&r);puts("PASS oversized control request remains rejected");
}
static void pairings(void){
 assert(ap2_pairings_init()==ESP_OK);uint8_t pk[32]={42},other[32]={43};const uint8_t id[]="admin";uint8_t perm;
 assert(ap2_pairings_add(id,5,pk,AP2_PERM_ADMIN)==ESP_OK);assert(ap2_pairings_count()==1);assert(ap2_pairings_has_admin());assert(saved_len==1584); /* 16 legacy 99-byte records */
 assert(ap2_pairings_add(id,5,other,1)==ESP_ERR_INVALID_STATE);assert(ap2_pairings_count()==1);
 fail_commit=1;assert(ap2_pairings_remove(id,5)==ESP_FAIL);assert(ap2_pairings_find(id,5,NULL,&perm));assert(perm==1);
 fail_commit=0;assert(ap2_pairings_add((uint8_t*)"user",4,pk,0)==ESP_OK);
 uint8_t out[4096];assert(ap2_pairings_list_tlv(out,sizeof(out))>0);
 assert(ap2_pairings_remove(id,5)==ESP_OK);assert(ap2_pairings_count()==0);assert(!ap2_pairings_has_admin());
 fail_commit=1;assert(ap2_pairings_add(id,5,pk,1)==ESP_FAIL);assert(ap2_pairings_count()==0);
 puts("PASS pairing persistence rollback, public-key conflict and removal of last admin");
}
static void demote(void){assert(ap2_pairings_init()==ESP_OK);uint8_t pk[32]={1};assert(ap2_pairings_add((uint8_t*)"admin",5,pk,1)==ESP_OK);assert(ap2_pairings_add((uint8_t*)"user",4,pk,0)==ESP_OK);assert(ap2_pairings_add((uint8_t*)"admin",5,pk,0)==ESP_OK);assert(ap2_pairings_count()==0);puts("PASS demoting last admin clears all pairings");}
static void load_fail(void){read_error=ESP_FAIL;assert(ap2_pairings_init()==ESP_FAIL);puts("PASS NVS read failure does not reopen enrollment");}
static void corrupt(void){saved_len=1584;saved[0]=1;saved[1]=1;saved[2]=65;assert(ap2_pairings_init()==ESP_ERR_INVALID_STATE);puts("PASS corrupt legacy record rejected");}
static void short_table(void){saved_len=1;saved[0]=1;assert(ap2_pairings_init()==ESP_ERR_INVALID_STATE);puts("PASS truncated pairing table rejected");}
int main(int argc,char**argv){assert(argc==2);if(!strcmp(argv[1],"artwork"))artwork();else if(!strcmp(argv[1],"pairings"))pairings();else if(!strcmp(argv[1],"demote"))demote();else if(!strcmp(argv[1],"load-fail"))load_fail();else if(!strcmp(argv[1],"corrupt"))corrupt();else if(!strcmp(argv[1],"short-table"))short_table();else oversized_control();return 0;}
