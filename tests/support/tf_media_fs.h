#pragma once
/* Existing media/UI harnesses provide a TF fake. USB admission/generation is
 * tested with the production adapter in tests/media_fs, not emulated here. */
#include "aether_storage.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#ifndef AETHER_STORAGE_MOUNT_POINT
#define AETHER_STORAGE_MOUNT_POINT "/sdcard"
#endif
bool aether_storage_is_mounted(aether_storage_handle_t);
bool aether_storage_card_inserted(aether_storage_handle_t);
esp_err_t aether_storage_acquire(aether_storage_handle_t,uint32_t);
void aether_storage_release(aether_storage_handle_t);
typedef struct {aether_storage_handle_t tf;bool held;} aether_media_fs_lease_t;
static inline bool aether_media_fs_safe_path(const char *path) {
 const char *root=path && !strncmp(path,"/sdcard",7) && (!path[7]||path[7]=='/') ? "/sdcard" :
  path && !strncmp(path,"/usb",4) && (!path[4]||path[4]=='/') ? "/usb" : NULL;
 if(!root)return false;
 for(const char *p=path+strlen(root);*p;){if(*p++!='/')return false;const char *e=strchr(p,'/');size_t n=e?(size_t)(e-p):strlen(p);
  if(!n||(n==1&&*p=='.')||(n==2&&p[0]=='.'&&p[1]=='.')||memchr(p,'\\',n))return false;
  p+=n;
 }
 return true;
}
#ifndef AETHER_TEST_MEDIA_READY
#define AETHER_TEST_MEDIA_READY aether_storage_is_mounted
#endif
static inline bool aether_media_fs_path_ready(aether_storage_handle_t tf,const char *p){return p && !strncmp(p,"/sdcard",7) && AETHER_TEST_MEDIA_READY(tf);}
static inline uint32_t aether_media_fs_generation(aether_storage_handle_t tf,const char *p){(void)tf;(void)p;return 0;}
static inline const char *aether_media_fs_default_root(aether_storage_handle_t tf){(void)tf;return "/sdcard";}
static inline esp_err_t aether_media_fs_acquire(aether_storage_handle_t tf,const char *p,aether_media_fs_lease_t *l){
 (void)p;esp_err_t e=aether_storage_acquire(tf,0);if(e==ESP_OK){l->tf=tf;l->held=true;}return e;}
static inline bool aether_media_fs_available(const aether_media_fs_lease_t *l){return l&&l->held&&aether_storage_card_inserted(l->tf);}
static inline void aether_media_fs_release(aether_media_fs_lease_t *l){if(l->held)aether_storage_release(l->tf);memset(l,0,sizeof(*l));}
