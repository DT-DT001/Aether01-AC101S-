#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef uint8_t BYTE;
enum {ESP_OK,ESP_FAIL,ESP_ERR_NO_MEM,ESP_ERR_INVALID_STATE,ESP_ERR_MSC_MOUNT_FAILED,ESP_ERR_INVALID_ARG};
enum {FR_OK,FR_DISK_ERR};
#define FF_VOLUMES 2
#define FF_FS_TINY 0
#define FF_USE_DYN_BUFFER 1
#define TAG "test"
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_FALSE(cond,err,...) do {if(!(cond))return (err);}while(0)
#define MSC_RETURN_ON_FALSE(cond,err) ESP_RETURN_ON_FALSE(cond,err)
#define MSC_RETURN_ON_ERROR(call) do {esp_err_t _e=(call);if(_e!=ESP_OK)return _e;}while(0)
#define MSC_RETURN_ON_INVALID_ARG(arg) MSC_RETURN_ON_FALSE(arg,ESP_ERR_INVALID_ARG)
typedef struct {unsigned marker;} FATFS;
typedef void *esp_blockdev_handle_t;
typedef struct {void *buf;} FIL;
typedef struct {char base_path[16];int lock;FATFS fs;unsigned max_files;uint32_t *flags;FIL files[2];} vfs_fat_ctx_t;
typedef struct {esp_blockdev_handle_t bdl_handle;BYTE pdrv;FATFS *fs;} vfs_fat_bdl_ctx_t;
typedef struct {esp_blockdev_handle_t bdl;bool bdl_vfs_registered;} msc_device_t;
typedef msc_device_t *msc_host_device_handle_t;
typedef struct msc_host_vfs {char *base_path;esp_blockdev_handle_t bdl;msc_device_t *dev;} msc_host_vfs_t;
typedef msc_host_vfs_t *msc_host_vfs_handle_t;
typedef struct {bool format_if_mount_failed;} esp_vfs_fat_mount_config_t;
static vfs_fat_ctx_t *s_fat_ctxs[FF_VOLUMES];
static vfs_fat_bdl_ctx_t *s_bdl_ctx[FF_VOLUMES];
static bool vfs_registered,diskio_registered,fatfs_mounted;
static esp_blockdev_handle_t registered_bdl;
static FATFS *registered_fs;
static unsigned unregister_failures,mount_failures,detach_failures;
static unsigned mounts,detaches,unregisters,disks_released,bdls_cleared,locks_closed;
static bool calloc_fail,strdup_fail;
static void *blocks[128];
static unsigned allocated;
static void *tracked_calloc(size_t n,size_t size)
{
    if(calloc_fail){calloc_fail=false;return NULL;}
    void *p=calloc(n,size);assert(p);
    unsigned i=0;while(i<128&&blocks[i])++i;assert(i<128);blocks[i]=p;++allocated;return p;
}
static char *tracked_strdup(const char *s)
{
    if(strdup_fail){strdup_fail=false;return NULL;}
    char *p=tracked_calloc(strlen(s)+1,1);strcpy(p,s);return p;
}
static void tracked_free(void *p)
{
    if(!p)return;
    unsigned i=0;while(i<128&&blocks[i]!=p)++i;assert(i<128);
    blocks[i]=NULL;assert(allocated);--allocated;free(p);
}
static size_t find_context_index_by_path(const char *path)
{for(size_t i=0;i<FF_VOLUMES;++i)if(s_fat_ctxs[i]&&!strcmp(path,s_fat_ctxs[i]->base_path))return i;return FF_VOLUMES;}
static bool get_ctx_id_by_bdl(esp_blockdev_handle_t h,uint32_t *id)
{for(unsigned i=0;i<FF_VOLUMES;++i)if(s_bdl_ctx[i]&&s_bdl_ctx[i]->bdl_handle==h){*id=i;return true;}return false;}
static BYTE ff_diskio_get_pdrv_bdl(esp_blockdev_handle_t h)
{return diskio_registered&&registered_bdl==h?0:0xff;}
static void ff_diskio_unregister(BYTE drv)
{assert(!drv&&diskio_registered&&!vfs_registered&&!fatfs_mounted);++disks_released;diskio_registered=false;}
static void ff_diskio_clear_pdrv_bdl(esp_blockdev_handle_t h)
{assert(h==registered_bdl&&!diskio_registered);++bdls_cleared;registered_bdl=NULL;}
static void _lock_close(int *lock){assert(!vfs_registered);++locks_closed;*lock=0;}
static void ff_memfree(void *p){tracked_free(p);}
static int f_mount(FATFS *fs,const char *drive,unsigned opt)
{
    assert(!fs&&!strcmp(drive,"0:")&&!opt);++detaches;
    if(detach_failures){--detach_failures;return FR_DISK_ERR;}
    /* Touching a detached/freed embedded FS would be caught by ASan. */
    if(registered_fs)assert(registered_fs->marker==0x12345678);
    registered_fs=NULL;fatfs_mounted=false;return FR_OK;
}
static esp_err_t esp_vfs_unregister(const char *base)
{
    assert(!strcmp(base,"/usb")&&vfs_registered&&!fatfs_mounted);++unregisters;
    if(unregister_failures){--unregister_failures;return ESP_FAIL;}
    vfs_registered=false;return ESP_OK;
}
static esp_err_t esp_vfs_fat_bdl_mount(const char *base,esp_blockdev_handle_t h,const esp_vfs_fat_mount_config_t *cfg)
{
    assert(!strcmp(base,"/usb")&&h&&!vfs_registered&&!diskio_registered&&!cfg->format_if_mount_failed);++mounts;
    if(mount_failures){--mount_failures;return ESP_FAIL;}
    vfs_fat_ctx_t *c=tracked_calloc(1,sizeof(*c));strcpy(c->base_path,base);c->max_files=2;c->fs.marker=0x12345678;
    c->flags=tracked_calloc(2,sizeof(uint32_t));c->files[0].buf=tracked_calloc(512,1);c->files[1].buf=tracked_calloc(512,1);
    s_fat_ctxs[0]=c;registered_fs=&c->fs;vfs_registered=true;fatfs_mounted=true;diskio_registered=true;registered_bdl=h;
    s_bdl_ctx[0]=tracked_calloc(1,sizeof(*s_bdl_ctx[0]));s_bdl_ctx[0]->bdl_handle=h;s_bdl_ctx[0]->fs=&c->fs;return ESP_OK;
}
#define calloc tracked_calloc
#define strdup tracked_strdup
#define free tracked_free
#include "actual_vfs.inc"
#undef calloc
#undef strdup
#undef free
static msc_device_t device={.bdl=(void*)0x1234};
static msc_host_vfs_handle_t mount_disk(void)
{
    msc_host_vfs_handle_t v=NULL;const esp_vfs_fat_mount_config_t cfg={0};
    assert(msc_host_vfs_register(&device,"/usb",&cfg,&v)==ESP_OK&&v&&device.bdl_vfs_registered);return v;
}
static void assert_clean(void)
{assert(!allocated&&!vfs_registered&&!diskio_registered&&!fatfs_mounted&&!s_bdl_ctx[0]&&!s_fat_ctxs[0]&&!device.bdl_vfs_registered&&!registered_bdl);}
int main(void)
{
    msc_host_vfs_handle_t v=mount_disk();void *fat=s_fat_ctxs[0],*bdl=s_bdl_ctx[0];const unsigned live=allocated;
    unregister_failures=3;
    for(unsigned n=0;n<3;++n){
        assert(msc_host_vfs_unregister(v)==ESP_FAIL);
        assert(device.bdl_vfs_registered&&v->dev==&device&&vfs_registered&&!fatfs_mounted&&diskio_registered);
        assert(s_fat_ctxs[0]==fat&&s_bdl_ctx[0]==bdl&&allocated==live&&!locks_closed&&!disks_released&&!bdls_cleared);
    }
    assert(msc_host_vfs_unregister(v)==ESP_OK);assert_clean();
    assert(detaches==4&&unregisters==4&&disks_released==1&&bdls_cleared==1&&locks_closed==1);
    puts("PASS actual MSC/BDL/VFS unregister chain retains every callback/context/buffer/device guard on repeated failure, then drains once without stale FATFS use or double free");
    v=mount_disk();detach_failures=1;unsigned count=unregisters;
    assert(msc_host_vfs_unregister(v)==ESP_FAIL&&unregisters==count&&fatfs_mounted&&vfs_registered&&diskio_registered&&device.bdl_vfs_registered);
    assert(msc_host_vfs_unregister(v)==ESP_OK);assert_clean();
    puts("PASS FatFs detach failure cannot free VFS/diskio/BDL ownership; retry remains safe");
    const esp_vfs_fat_mount_config_t cfg={0};v=NULL;count=mounts;calloc_fail=true;
    assert(msc_host_vfs_register(&device,"/usb",&cfg,&v)==ESP_ERR_NO_MEM&&!v&&mounts==count);assert_clean();
    strdup_fail=true;assert(msc_host_vfs_register(&device,"/usb",&cfg,&v)==ESP_ERR_NO_MEM&&!v&&mounts==count);assert_clean();
    mount_failures=1;assert(msc_host_vfs_register(&device,"/usb",&cfg,&v)==ESP_ERR_MSC_MOUNT_FAILED&&!v&&mounts==count+1);assert_clean();
    puts("PASS wrapper/path OOM precedes mount publication; mount failure frees wrapper without unchecked rollback or leaked allocation");
    for(unsigned n=0;n<100;++n){v=mount_disk();assert(msc_host_vfs_unregister(v)==ESP_OK);assert_clean();}
    puts("PASS100 actual driver VFS register/unregister cycles reclaim every wrapper/context/FILE cache with no live owner or allocation growth");
    return 0;
}
