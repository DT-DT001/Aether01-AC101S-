#include "aether_media_fs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool tf_ready, usb_ready, usb_draining;
static unsigned tf_users, usb_users, usb_generation = 1;
static int token;
uint32_t aether_storage_generation(aether_storage_handle_t h){return h?7:0;}
bool aether_storage_is_mounted(aether_storage_handle_t h){return h && tf_ready;}
bool aether_storage_card_inserted(aether_storage_handle_t h){return h && tf_ready;}
esp_err_t aether_storage_acquire(aether_storage_handle_t h,uint32_t t){(void)t;if(!h||!tf_ready)return ESP_ERR_INVALID_STATE;++tf_users;return ESP_OK;}
void aether_storage_release(aether_storage_handle_t h){assert(h&&tf_users);--tf_users;}
aether_usb_storage_handle_t aether_usb_storage_default(void){return (void*)&token;}
esp_err_t aether_usb_storage_acquire(aether_usb_storage_handle_t h,aether_usb_storage_lease_t *l){assert(h);if(!usb_ready||usb_draining)return ESP_ERR_INVALID_STATE;l->owner=h;l->generation=usb_generation;l->held=true;++usb_users;return ESP_OK;}
void aether_usb_storage_release(aether_usb_storage_lease_t *l){assert(l->held&&usb_users);--usb_users;memset(l,0,sizeof(*l));}
bool aether_usb_storage_is_available(const aether_usb_storage_lease_t *l){return l->held&&usb_ready&&!usb_draining&&l->generation==usb_generation;}
esp_err_t aether_usb_storage_get_status(aether_usb_storage_handle_t h,aether_usb_storage_status_t *s){assert(h);memset(s,0,sizeof(*s));s->mounted=usb_ready;s->draining=usb_draining;s->generation=usb_generation;return ESP_OK;}
int main(void){
 const char *good[]={"/sdcard","/usb","/usb/Music/a.mp3","/sdcard/Books/Chinese.txt"};
 const char *bad[]={"/","/usbX","/sdcard2/a","/usb/../a","/usb/./a","/usb//a","/usb/","/usb/a\\b",NULL};
 for(unsigned i=0;i<sizeof(good)/sizeof(*good);++i)assert(aether_media_fs_safe_path(good[i]));
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)assert(!aether_media_fs_safe_path(bad[i]));
 aether_media_fs_lease_t l={0};void *tf=&token;
 assert(aether_media_fs_acquire(tf,"/usb/a",&l)==ESP_ERR_INVALID_STATE&&!l.held);
 usb_ready=true;assert(aether_media_fs_generation(tf,"/usb")==1);assert(aether_media_fs_generation(tf,"/sdcard")==7);assert(!strcmp(aether_media_fs_default_root(tf),"/usb"));
 assert(aether_media_fs_acquire(tf,"/usb/a",&l)==ESP_OK&&usb_users==1&&tf_users==0);
 assert(aether_media_fs_acquire(tf,"/usb/b",&l)==ESP_ERR_INVALID_ARG&&usb_users==1);
 tf_ready=true;assert(aether_media_fs_available(&l));
 usb_draining=true;assert(!aether_media_fs_available(&l)&&tf_ready&&usb_users==1);
 aether_media_fs_release(&l);aether_media_fs_release(&l);assert(usb_users==0);
 usb_draining=false;assert(aether_media_fs_acquire(tf,"/usb/b",&l)==ESP_OK);
 ++usb_generation;assert(!aether_media_fs_available(&l));aether_media_fs_release(&l);
 assert(!strcmp(aether_media_fs_default_root(tf),"/sdcard"));
 assert(aether_media_fs_acquire(tf,"/sdcard/a",&l)==ESP_OK&&tf_users==1&&usb_users==0);
 usb_ready=false;assert(aether_media_fs_available(&l));tf_ready=false;assert(!aether_media_fs_available(&l));
 aether_media_fs_release(&l);assert(!tf_users&&!usb_users&&!l.held);
 assert(aether_media_fs_acquire(tf,"/usb/../sdcard/a",&l)==ESP_ERR_INVALID_ARG);
 puts("PASS production media filesystem routing, unsafe paths, admission, independent unplug, stale generation and exact release");
}
