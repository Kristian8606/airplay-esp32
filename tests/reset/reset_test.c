/* Platform boundaries are fakes; the HTTP validation and erase sequence are production code. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device_reset.h"
#include "web_server_internal.h"
#include "esp_partition.h"
#include "freertos/task.h"
#include "nvs.h"

static esp_err_t (*handler)(httpd_req_t *);
static bool authorized=true, paused, pause_fails, open_fails, key_fails, commit_fails;
static bool storage_fails, nvs_fails, no_storage;
static int restarts, open_handles, key_calls, factory_calls, storage_calls;
static unsigned priority=3;
static const char *keys[]={"hk_pairs","ed25519_pub","ed25519_sec","wifi_list","device_name","volume_mdb","eq_config","loud_en","out_lat_us"};
static bool present[9]={true,true,true,true,true,true,true,true,true};
static unsigned char storage[0x21000];
static const esp_partition_t partition={sizeof storage};
static httpd_req_t *active_req;

esp_err_t httpd_register_uri_handler(httpd_handle_t s,const httpd_uri_t *u){assert(!strcmp(u->uri,"/api/system/reset")&&u->method==HTTP_POST);handler=u->handler;return ESP_OK;}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char*k,char*b,size_t n){const char*v=!strcmp(k,"Content-Type")?r->content_type:!strcmp(k,"Sec-Fetch-Site")?r->site:NULL;if(!v)return ESP_ERR_NOT_FOUND;if(strlen(v)>=n)return ESP_ERR_INVALID_SIZE;strcpy(b,v);return ESP_OK;}
esp_err_t httpd_resp_set_status(httpd_req_t*r,const char*s){r->status=atoi(s);return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t*r,const char*t){return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){return ESP_OK;}
esp_err_t httpd_resp_sendstr(httpd_req_t*r,const char*s){snprintf(r->response,sizeof r->response,"%s",s);return ESP_OK;}
esp_err_t httpd_resp_send_err(httpd_req_t*r,int status,const char*s){r->status=status;return httpd_resp_sendstr(r,s);}
bool web_admin_ok(httpd_req_t*r){if(!authorized)httpd_resp_send_err(r,401,"Authentication required");return authorized;}
esp_err_t web_recv_json(httpd_req_t*r,char*b,size_t n){if(!r->content_len||r->content_len>=n)return ESP_ERR_INVALID_SIZE;memcpy(b,r->body,r->content_len);b[r->content_len]=0;return ESP_OK;}
esp_err_t web_audio_pause(httpd_req_t*r,const char*what,bool*p){if(pause_fails){httpd_resp_send_err(r,503,"Source still stopping");return ESP_FAIL;}paused=true;*p=true;return ESP_OK;}
UBaseType_t uxTaskPriorityGet(void*t){return priority;}
void vTaskPrioritySet(void*t,UBaseType_t p){priority=p;}
void vTaskDelay(unsigned n){}
void esp_restart(void){assert(paused&&active_req&&active_req->response[0]);assert(open_handles==0);++restarts;}
esp_err_t nvs_open(const char*ns,int mode,nvs_handle_t*h){assert(paused&&priority==1&&!strcmp(ns,"airplay")&&mode==NVS_READWRITE);if(open_fails)return ESP_FAIL;*h=1;++open_handles;return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char*k){assert(open_handles==1);++key_calls;if(key_fails)return ESP_FAIL;for(unsigned i=0;i<9;i++)if(!strcmp(k,keys[i])){if(!present[i])return ESP_ERR_NVS_NOT_FOUND;present[i]=false;return ESP_OK;}assert(0);return ESP_FAIL;}
esp_err_t nvs_commit(nvs_handle_t h){assert(open_handles==1);return commit_fails?ESP_FAIL:ESP_OK;}
void nvs_close(nvs_handle_t h){--open_handles;}
const esp_partition_t *esp_partition_find_first(int type,int sub,const char*name){assert(paused&&priority==1&&type==1&&sub==0x82&&!strcmp(name,"storage"));return no_storage?NULL:&partition;}
esp_err_t esp_partition_erase_range(const esp_partition_t*p,size_t off,size_t len){assert(p==&partition&&paused&&priority==1&&off+len<=sizeof storage&&off%4096==0&&len%4096==0&&len<=65536);++storage_calls;if(storage_fails)return ESP_FAIL;memset(storage+off,255,len);return ESP_OK;}
esp_err_t nvs_flash_erase(void){assert(paused&&priority==1);if(!no_storage)for(unsigned i=0;i<sizeof storage;i++)assert(storage[i]==255);++factory_calls;if(nvs_fails)return ESP_FAIL;memset(present,0,sizeof present);return ESP_OK;}

static httpd_req_t request(const char *body,const char *type,const char *site){httpd_req_t r={.body=body,.content_len=strlen(body),.content_type=type,.site=site,.status=200};active_req=&r;/* handler is synchronous, including the reboot boundary */handler(&r);active_req=NULL;return r;}
static void success(httpd_req_t*r){assert(r->status==200&&restarts==1);cJSON*j=cJSON_Parse(r->response);assert(cJSON_IsTrue(cJSON_GetObjectItem(j,"success"))&&cJSON_IsTrue(cJSON_GetObjectItem(j,"restarting")));cJSON_Delete(j);}
int main(int argc,char**argv){assert(argc==2);memset(storage,0x5a,sizeof storage);device_reset_register(NULL);assert(handler);const char*test=argv[1];
 const char*hk="{\"mode\":\"homekit\",\"confirm\":\"RESET HOMEKIT\"}";
 const char*factory="{\"mode\":\"factory\",\"confirm\":\"ERASE ALL\"}";
 if(!strcmp(test,"boot")){httpd_req_t r=request(factory,"application/json",NULL);assert(r.status==503&&!paused&&!restarts&&!factory_calls);puts("PASS reset rejects boot race before identity/audio initialization finishes");return 0;}
 device_reset_mark_ready();
 if(!strcmp(test,"auth")){authorized=false;httpd_req_t r=request(factory,"application/json",NULL);assert(r.status==401&&!paused&&!restarts&&!factory_calls);puts("PASS unauthorized reset has no erase or stop effects");return 0;}
 if(!strcmp(test,"bad")){
  const char*bad[]={"{}","{\"mode\":\"other\",\"confirm\":\"ERASE ALL\"}","{\"mode\":\"factory\"}","{\"mode\":\"factory\",\"confirm\":true}","{\"mode\":\"factory\",\"confirm\":\"RESET HOMEKIT\"}","{\"mode\":\"factory\",\"confirm\":\"ERASE ALL\"}junk","{\"mode\":\"factory\",\"confirm\":\"ERASE ALL\",\"extra\":1}","not json",""};
  for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){httpd_req_t r=request(bad[i],"application/json",NULL);assert(r.status==400);}
  char oversized[512];memset(oversized,'x',sizeof oversized-1);oversized[511]=0;httpd_req_t r=request(oversized,"application/json",NULL);assert(r.status==400);
  r=request(factory,"text/plain",NULL);assert(r.status==415);r=request(factory,NULL,NULL);assert(r.status==415);r=request(factory,"application/json","cross-site");assert(r.status==403);
  assert(!paused&&!restarts&&!key_calls&&!factory_calls);puts("PASS bounded strict JSON, explicit matching confirmation and browser request checks");return 0;
 }
 if(!strcmp(test,"stop")){pause_fails=true;httpd_req_t r=request(hk,"application/json",NULL);assert(r.status==503&&!restarts&&!key_calls&&!factory_calls&&priority==3);puts("PASS no deletion when quiescing fails; HTTP priority restored");return 0;}
 if(!strcmp(test,"missing"))present[0]=present[1]=present[2]=false;
 open_fails=!strcmp(test,"open-fail");key_fails=!strcmp(test,"key-fail");commit_fails=!strcmp(test,"commit-fail");storage_fails=!strcmp(test,"storage-fail");nvs_fails=!strcmp(test,"nvs-fail");no_storage=!strcmp(test,"no-storage");
 bool fail=open_fails||key_fails||commit_fails||storage_fails||nvs_fails;
 bool full=!strcmp(test,"factory")||storage_fails||nvs_fails||no_storage;
 httpd_req_t r=request(full?factory:hk,"application/json; charset=utf-8","same-origin");
 if(fail){assert(r.status==500&&restarts==1&&paused&&strstr(r.response,"false"));if(storage_fails)assert(!factory_calls);puts("PASS erase failure is reported and restarts instead of resuming stale RAM settings");return 0;}
 success(&r);if(full){assert(factory_calls==1&&!key_calls);for(unsigned i=0;i<9;i++)assert(!present[i]);assert(no_storage?storage_calls==0:storage_calls==3);puts("PASS full reset clears all NVS and only storage partition, with bounded yielding erases");}
 else{assert(!factory_calls&&!storage_calls);for(unsigned i=0;i<3;i++)assert(!present[i]);for(unsigned i=3;i<9;i++)assert(present[i]);for(unsigned i=0;i<sizeof storage;i++)assert(storage[i]==0x5a);puts("PASS HomeKit reset forgets all controllers and identity, preserving Wi-Fi, name, EQ, volume, loudness and latency");}return 0;
}
