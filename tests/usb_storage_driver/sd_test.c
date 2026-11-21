#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef unsigned char BYTE;
typedef int vfs_fat_x_ctx_flags_t;
typedef struct {int unused;} FATFS;
enum {ESP_OK,ESP_FAIL,ESP_ERR_NO_MEM,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE};
enum {FR_OK,FR_DISK_ERR};
#define FF_VOLUMES 2
#define FF_DRV_NOT_USED 0xff
#define SDMMC_HOST_FLAG_DEINIT_ARG 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define TAG "test"
#define CHECK_EXECUTE_RESULT(err,str) do{if((err)!=ESP_OK)goto cleanup;}while(0)
typedef struct {bool disk_status_check_enable;unsigned max_files;} esp_vfs_fat_mount_config_t;
typedef struct {const char *base_path,*fat_drive;unsigned max_files;} esp_vfs_fat_conf_t;
typedef struct {unsigned flags;int slot;esp_err_t (*deinit)(void);esp_err_t (*deinit_p)(int);} sdmmc_host_t;
typedef struct {sdmmc_host_t host;unsigned marker;} sdmmc_card_t;
typedef struct {BYTE pdrv;esp_vfs_fat_mount_config_t mount_config;FATFS *fs;sdmmc_card_t *card;char *base_path;vfs_fat_x_ctx_flags_t flags;bool vfs_unregistered;esp_err_t host_deinit_error;} vfs_fat_sd_ctx_t;
static vfs_fat_sd_ctx_t *s_ctx[FF_VOLUMES];
static sdmmc_card_t *s_cards[FF_VOLUMES];
static bool s_disk_status_check_en[FF_VOLUMES],drivers[FF_VOLUMES],usb_driver0,detached[FF_VOLUMES],vfs[FF_VOLUMES];
static unsigned unregister_failures,detach_failures,unregister_calls,deinit_calls,driver_calls,mount_calls;
static esp_err_t deinit_error;
static void *blocks[128];static unsigned allocated;static bool fail_calloc,fail_owner_allocation;
static void *mem_calloc(size_t count,size_t size){if(fail_calloc){fail_calloc=false;return NULL;}void*p=calloc(count,size);assert(p);unsigned i=0;while(i<128&&blocks[i])++i;assert(i<128);blocks[i]=p;++allocated;return p;}
static void mem_free(void*p){if(!p)return;unsigned i=0;while(i<128&&blocks[i]!=p)++i;assert(i<128);blocks[i]=NULL;--allocated;free(p);}
static char *mem_dup(const char*s){char*p=mem_calloc(strlen(s)+1,1);strcpy(p,s);return p;}
static unsigned path_index(const char *p){return !strcmp(p,"/sdcard2")?1:0;}
static unsigned s_get_unused_context_id(void){for(unsigned i=0;i<2;++i)if(!s_ctx[i])return i;return 2;}
static int f_mount(FATFS *fs,const char*drive,unsigned opt){assert(!fs&&!opt);unsigned i=(unsigned)(drive[0]-'0');assert(i<2&&drivers[i]);if(detach_failures){--detach_failures;return FR_DISK_ERR;}detached[i]=true;return FR_OK;}
static esp_err_t esp_vfs_fat_unregister_path(const char *p){++unregister_calls;if(unregister_failures){--unregister_failures;return ESP_FAIL;}unsigned i=path_index(p);assert(vfs[i]);vfs[i]=false;return ESP_OK;}
static void ff_diskio_unregister(BYTE drv){assert(drv<2&&drivers[drv]&&detached[drv]);assert(!(usb_driver0&&drv==0));drivers[drv]=false;++driver_calls;}
static esp_err_t host_deinit_p(int slot){assert(slot==43);++deinit_calls;return deinit_error;}
static esp_err_t host_deinit(void){++deinit_calls;return deinit_error;}
static esp_err_t mount_prepare_mem(const char *p,BYTE *drv,char **dup){*drv=1;*dup=mem_dup(p);if(fail_owner_allocation){fail_owner_allocation=false;fail_calloc=true;}return ESP_OK;}
static esp_err_t mount_internal_error;
static void ff_diskio_register_sdmmc(BYTE drv,sdmmc_card_t *c){assert(!drivers[drv]);drivers[drv]=true;s_cards[drv]=c;detached[drv]=false;}
static void ff_sdmmc_set_disk_status_check(BYTE drv,bool value){s_disk_status_check_en[drv]=value;}
static esp_err_t esp_vfs_fat_register(const esp_vfs_fat_conf_t *cfg,FATFS **fs){assert(allocated>=3);++mount_calls;vfs[path_index(cfg->base_path)]=true;*fs=(FATFS*)0x1234;return ESP_OK;}
static esp_err_t s_f_mount(sdmmc_card_t *card,FATFS *fs,BYTE drv,const esp_vfs_fat_mount_config_t *cfg,vfs_fat_x_ctx_flags_t *flags){return mount_internal_error;}
#define calloc mem_calloc
#define free mem_free
#include "actual_sd.inc"
#undef calloc
#undef free
static sdmmc_card_t *new_card(void){sdmmc_card_t*c=mem_calloc(1,sizeof(*c));c->host=(sdmmc_host_t){SDMMC_HOST_FLAG_DEINIT_ARG,43,host_deinit,host_deinit_p};c->marker=0xabcdef;return c;}
static void setup(sdmmc_card_t *c,unsigned drv,unsigned ctx,const char *path)
{s_ctx[ctx]=mem_calloc(1,sizeof(*s_ctx[ctx]));s_ctx[ctx]->pdrv=drv;s_ctx[ctx]->card=c;s_ctx[ctx]->base_path=mem_dup(path);s_ctx[ctx]->fs=(FATFS*)0x1234;s_cards[drv]=c;drivers[drv]=true;detached[drv]=false;vfs[path_index(path)]=true;}
static void reset(void){assert(!allocated);memset(s_ctx,0,sizeof s_ctx);memset(s_cards,0,sizeof s_cards);memset(drivers,0,sizeof drivers);memset(detached,0,sizeof detached);memset(vfs,0,sizeof vfs);usb_driver0=false;deinit_error=0;unregister_failures=detach_failures=0;}
int main(void)
{
    sdmmc_card_t*c=new_card();setup(c,1,0,"/sdcard");unsigned dc=driver_calls,hc=deinit_calls;
    unregister_failures=2;for(unsigned i=0;i<2;++i){assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_FAIL);assert(c->marker==0xabcdef&&s_ctx[0]&&s_cards[1]==c&&drivers[1]&&vfs[0]&&!s_ctx[0]->vfs_unregistered&&driver_calls==dc&&deinit_calls==hc);}
    assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_OK&&!s_ctx[0]&&!s_cards[1]&&!drivers[1]);assert(deinit_calls==hc+1);reset();
    puts("PASS actual SD unmount retains ctx/card/diskio on repeated VFS unregister errors and releases once only after successful retry");
    c=new_card();setup(c,1,0,"/sdcard");detach_failures=1;unsigned before=unregister_calls;
    assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_FAIL&&unregister_calls==before&&drivers[1]&&vfs[0]);
    assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_OK);reset();
    puts("PASS SD FatFs detach failure keeps every owner; retry does not skip VFS cleanup");
    c=new_card();s_cards[0]=c;s_disk_status_check_en[0]=true;ff_diskio_clear_pdrv_card(0);
    assert(ff_diskio_get_pdrv_card(c)==0xff&&!ff_diskio_get_pdrv_cnt_card(c)&&!s_disk_status_check_en[0]);
    usb_driver0=true;drivers[0]=true;setup(c,1,0,"/sdcard");assert(ff_diskio_get_pdrv_card(c)==1&&ff_diskio_get_pdrv_cnt_card(c)==1);
    assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_OK&&drivers[0]&&!s_cards[1]);drivers[0]=false;reset();
    puts("PASS cleared SD bindings prevent a reused card address from aliasing a USB-occupied old drive slot; TF unmount leaves USB intact");
    c=new_card();setup(c,1,0,"/sdcard");deinit_error=ESP_FAIL;hc=deinit_calls;
    assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_FAIL&&s_ctx[0]->vfs_unregistered&&!s_ctx[0]->fs&&!vfs[0]&&drivers[1]&&c->marker==0xabcdef);
    before=unregister_calls;for(unsigned i=0;i<100;++i)assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_FAIL);
    assert(deinit_calls==hc+1&&unregister_calls==before&&s_ctx[0]->host_deinit_error==ESP_FAIL);
    /* Simulated hard reset reclaims test memory; production preserves owner. */
    ff_diskio_clear_pdrv_card(1);mem_free(s_ctx[0]->base_path);mem_free(s_ctx[0]);s_ctx[0]=NULL;mem_free(c);reset();
    puts("PASS nontransactional SD host-deinit error latches the original failure without repeated deinit, double VFS unregister, card free or drive reuse");
    c=new_card();c->host.flags=0;setup(c,0,0,"/sdcard");setup(c,1,1,"/sdcard2");hc=deinit_calls;
    assert(esp_vfs_fat_sdcard_unmount("/sdcard2",c)==ESP_OK&&s_ctx[0]&&!s_ctx[1]&&deinit_calls==hc&&ff_diskio_get_pdrv_cnt_card(c)==1);
    assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_OK&&deinit_calls==hc+1);reset();
    puts("PASS SD multi-partition context lookup uses the requested path and only the final drive deinitializes/frees its card");
    c=new_card();unsigned mc=mount_calls;fail_owner_allocation=true;
    assert(esp_vfs_fat_mount_initialized(c,"/sdcard",&(esp_vfs_fat_mount_config_t){0})==ESP_ERR_NO_MEM&&mount_calls==mc&&!s_ctx[0]&&!drivers[1]&&allocated==1);
    mem_free(c);reset();
    c=new_card();mount_internal_error=ESP_FAIL;usb_driver0=true;drivers[0]=true;
    assert(esp_vfs_fat_mount_initialized(c,"/sdcard",&(esp_vfs_fat_mount_config_t){0})==ESP_FAIL&&!s_ctx[0]&&!drivers[1]&&!vfs[0]&&!s_cards[1]&&allocated==1&&drivers[0]);
    assert(ff_diskio_get_pdrv_card(c)==0xff&&!ff_diskio_get_pdrv_cnt_card(c));
    mount_internal_error=0;drivers[0]=false;mem_free(c);reset();
    puts("PASS actual failed SD mount rollback clears its drive/card binding without freeing the caller card or touching the USB-occupied slot");
    c=new_card();assert(esp_vfs_fat_mount_initialized(c,"/sdcard",&(esp_vfs_fat_mount_config_t){0})==ESP_OK&&mount_calls==mc+2);
    assert(esp_vfs_fat_sdcard_unmount("/sdcard",c)==ESP_OK);reset();
    puts("PASS SD cleanup-owner OOM is rejected before VFS/diskio publication; normal preallocated owner is reclaimed after successful unmount");
    return 0;
}
