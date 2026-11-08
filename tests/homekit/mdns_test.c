#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mdns.h"
#include "mdns_airplay.h"
#include "airplay_identity.h"
#include "ap2_hap.h"
static unsigned flags=4;static int adds,changes,fail_sf,fail_gcgl;
static char group[48],metadata[32],announced_flags[12];
esp_err_t mdns_init(void){return ESP_OK;}
esp_err_t mdns_hostname_set(const char*h){assert(*h);return ESP_OK;}
esp_err_t mdns_service_add(const char*n,const char*t,const char*p,uint16_t port,const mdns_txt_item_t*items,size_t count){
 assert(port==7000);assert(!strcmp(p,"_tcp"));assert(n[0]);adds++;
 if(!strcmp(t,"_airplay"))assert(count==18);else assert(count==12);
 for(size_t i=0;i<count;i++){assert(items[i].key&&items[i].value);if(!strcmp(items[i].key,"md"))snprintf(metadata,sizeof(metadata),"%s",items[i].value);}
 return ESP_OK;
}
esp_err_t mdns_service_txt_item_set(const char*t,const char*p,const char*k,const char*v){changes++;if((fail_sf&&!strcmp(k,"sf"))||(fail_gcgl&&!strcmp(k,"gcgl")))return ESP_FAIL;if(!strcmp(k,"gid"))snprintf(group,sizeof(group),"%s",v);if(!strcmp(k,"flags"))snprintf(announced_flags,sizeof(announced_flags),"%s",v);return ESP_OK;}
esp_err_t settings_get_device_name(char*p,size_t n){snprintf(p,n,"Speaker");return ESP_OK;}
void wifi_sanitize_hostname(const char*i,char*p,size_t n){snprintf(p,n,"speaker");}
void wifi_get_mac_str(char*p,size_t n){snprintf(p,n,"01:02:03:04:05:06");}
const uint8_t *ap2_hap_identity_public_key(void){static uint8_t p[32];return p;}
uint32_t airplay_status_flags(void){return flags;}
void airplay_get_pairing_id(char*p,size_t n){snprintf(p,n,"00000000-1111-4222-8333-444444444444");}
void airplay_get_serial_number(char*p,size_t n){snprintf(p,n,"010203040506");}
void airplay_get_firmware_revision(char*p,size_t n){snprintf(p,n,"4.5.7");}
int main(void){mdns_airplay_init();assert(adds==2);assert(!strcmp(metadata,"0,1,2"));flags=0x404;mdns_airplay_update_flags();assert(!strcmp(announced_flags,"0x404"));int prev=changes;mdns_airplay_update_flags();assert(changes==prev);mdns_airplay_set_group("sender-group",true);assert(!strcmp(group,"sender-group"));mdns_airplay_set_group(NULL,false);assert(!strcmp(group,"00000000-1111-4222-8333-444444444444"));fail_gcgl=1;mdns_airplay_set_group("partial-group",true);assert(!strcmp(group,"partial-group"));fail_gcgl=0;mdns_airplay_set_group(NULL,false);assert(!strcmp(group,"00000000-1111-4222-8333-444444444444"));fail_sf=1;flags=0xC04;mdns_airplay_update_flags();assert(!strcmp(announced_flags,"0xC04"));fail_sf=0;flags=0x404;mdns_airplay_update_flags();assert(!strcmp(announced_flags,"0x404"));puts("PASS actual mDNS registration bounds, artwork advertising, flags/group updates");return 0;}
