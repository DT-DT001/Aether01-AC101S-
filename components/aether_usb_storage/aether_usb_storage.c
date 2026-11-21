#include "aether_usb_storage.h"
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "esp_vfs_fat_lifecycle.h"

#define LEASE_LIMIT 32
struct aether_usb_storage {
    portMUX_TYPE lock;
    atomic_bool stop_requested;
    TaskHandle_t task;
    aether_usb_storage_status_t status;
    uint32_t cookies[LEASE_LIMIT], lease_generations[LEASE_LIMIT], next_cookie;
    uint8_t pending_address;
    bool mounting, working, stopped, cleanup_fatal;
    msc_host_device_handle_t published_device; /* Protected callback identity. */
    /* Device and VFS handles belong only to the storage worker. */
    msc_host_device_handle_t device;
    msc_host_vfs_handle_t vfs;
};
static struct aether_usb_storage s_storage = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .stop_requested = ATOMIC_VAR_INIT(true), .stopped = true,
};
static uint32_t next_generation(uint32_t generation) { return generation + 1U ? generation + 1U : 1U; }
static void notify_worker(struct aether_usb_storage *s) { if(s->task)xTaskNotifyGive(s->task); }
static bool known(aether_usb_storage_handle_t s) { return s==&s_storage; }

aether_usb_storage_handle_t aether_usb_storage_default(void) { return &s_storage; }

esp_err_t aether_usb_storage_acquire(aether_usb_storage_handle_t s,aether_usb_storage_lease_t *lease)
{
    if(!known(s)||!lease)return ESP_ERR_INVALID_ARG;
    if(lease->held)return ESP_ERR_INVALID_STATE;
    memset(lease,0,sizeof(*lease));
    portENTER_CRITICAL(&s->lock);
    if(!s->status.mounted||!s->status.connected||s->status.draining||atomic_load(&s->stop_requested)){
        portEXIT_CRITICAL(&s->lock);return ESP_ERR_INVALID_STATE;
    }
    unsigned slot=0;for(;slot<LEASE_LIMIT;++slot)if(!s->cookies[slot])break;
    if(slot==LEASE_LIMIT){portEXIT_CRITICAL(&s->lock);return ESP_ERR_NO_MEM;}
    s->next_cookie=next_generation(s->next_cookie);
    s->cookies[slot]=s->next_cookie;s->lease_generations[slot]=s->status.generation;
    ++s->status.users;
    *lease=(aether_usb_storage_lease_t){.owner=s,.generation=s->status.generation,
        .cookie=s->next_cookie,.slot=(uint16_t)slot,.held=true};
    portEXIT_CRITICAL(&s->lock);
    return ESP_OK;
}
void aether_usb_storage_release(aether_usb_storage_lease_t *lease)
{
    if(!lease||!lease->held||lease->owner!=&s_storage)return;
    struct aether_usb_storage *s=lease->owner;
    portENTER_CRITICAL(&s->lock);
    if(lease->slot<LEASE_LIMIT&&s->cookies[lease->slot]==lease->cookie&&
       s->lease_generations[lease->slot]==lease->generation){
        s->cookies[lease->slot]=0;s->lease_generations[lease->slot]=0;
        if(s->status.users)--s->status.users;
    }
    portEXIT_CRITICAL(&s->lock);
    memset(lease,0,sizeof(*lease));notify_worker(s);
}
bool aether_usb_storage_is_available(const aether_usb_storage_lease_t *lease)
{
    if(!lease||!lease->held||lease->owner!=&s_storage||lease->slot>=LEASE_LIMIT)return false;
    struct aether_usb_storage *s=lease->owner;
    portENTER_CRITICAL(&s->lock);
    const bool available=s->cookies[lease->slot]==lease->cookie&&
        s->lease_generations[lease->slot]==lease->generation&&s->status.generation==lease->generation&&
        s->status.mounted&&s->status.connected&&!s->status.draining&&!atomic_load(&s->stop_requested);
    portEXIT_CRITICAL(&s->lock);return available;
}
esp_err_t aether_usb_storage_get_status(aether_usb_storage_handle_t s,aether_usb_storage_status_t *status)
{
    if(!known(s)||!status)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s->lock);*status=s->status;portEXIT_CRITICAL(&s->lock);return ESP_OK;
}

static void storage_event(const msc_host_event_t *event,void *context)
{
    struct aether_usb_storage *s=context;
    portENTER_CRITICAL(&s->lock);
    if(event->event==MSC_DEVICE_CONNECTED){
        if(!atomic_load(&s->stop_requested)&&!s->status.device_address&&!s->mounting&&!s->status.draining){
            s->status.device_address=event->device.address;s->pending_address=event->device.address;
            s->status.connected=true;s->status.generation=next_generation(s->status.generation);
        }else if(s->status.ignored_devices<UINT8_MAX)++s->status.ignored_devices;
    }else if(event->event==MSC_DEVICE_DISCONNECTED&&
             (event->device.handle==s->published_device||(s->mounting&&!s->published_device))){
        /* Gate leases in the bus callback, before a delayed filesystem worker
         * can run. Existing FILE/DIR owners retain the VFS until release. */
        s->status.connected=false;s->status.draining=true;s->pending_address=0;
        s->status.generation=next_generation(s->status.generation);
    }
    portEXIT_CRITICAL(&s->lock);notify_worker(s);
}

static void record_cleanup_failure(struct aether_usb_storage *s,esp_err_t error)
{
    portENTER_CRITICAL(&s->lock);
    s->cleanup_fatal=true;s->working=false;s->status.last_error=error;
    portEXIT_CRITICAL(&s->lock);
}

/* One owner iteration is also exercised by the hardware-independent tests. */
static void process_storage(struct aether_usb_storage *s)
{
    portENTER_CRITICAL(&s->lock);
    const bool installed=s->status.driver_running;
    const bool fatal=s->cleanup_fatal;
    const bool draining=s->status.draining||atomic_load(&s->stop_requested);
    const uint32_t users=s->status.users;
    const uint8_t address=!draining?s->pending_address:0;
    const uint32_t generation=s->status.generation;
    if(address){s->pending_address=0;s->mounting=true;s->working=true;}
    portEXIT_CRITICAL(&s->lock);
    if(!installed||fatal)return;

    if(address){
        /* A contended registration gate retries this same installed device,
         * rather than installing it twice or losing a still-connected disk. */
        msc_host_device_handle_t device=s->device;
        esp_err_t error=device?ESP_OK:msc_host_install_device(address,&device);
        bool retry_mount=false;
        if(error==ESP_OK){
            s->device=device;
            portENTER_CRITICAL(&s->lock);
            s->published_device=device;
            const bool current=s->status.connected&&s->status.generation==generation&&
                !s->status.draining&&!atomic_load(&s->stop_requested);
            portEXIT_CRITICAL(&s->lock);
            if(current){
                msc_host_device_info_t info;
                error=msc_host_get_device_info(device,&info);
                if(error==ESP_OK){
                    portENTER_CRITICAL(&s->lock);
                    s->status.sector_size=info.sector_size;
                    s->status.capacity_bytes=(uint64_t)info.sector_count*info.sector_size;
                    portEXIT_CRITICAL(&s->lock);
                }
                const esp_vfs_fat_mount_config_t mount={
                    .format_if_mount_failed=false,.max_files=16,.allocation_unit_size=16384,
                };
                if(error==ESP_OK){
                    error=esp_vfs_fat_lifecycle_acquire(200);
                    if(error==ESP_OK){
                        portENTER_CRITICAL(&s->lock);
                        const bool still_current=s->status.connected&&s->status.generation==generation&&
                            !s->status.draining&&!atomic_load(&s->stop_requested);
                        portEXIT_CRITICAL(&s->lock);
                        if(still_current)
                            error=msc_host_vfs_register(device,AETHER_USB_STORAGE_MOUNT_POINT,&mount,&s->vfs);
                        else error=ESP_ERR_INVALID_STATE;
                        esp_vfs_fat_lifecycle_release();
                    }else retry_mount=error==ESP_ERR_TIMEOUT;
                }
            }
        }
        portENTER_CRITICAL(&s->lock);
        if(error!=ESP_OK)s->status.last_error=error;
        if(!s->device){
            s->status.connected=false;s->status.device_address=0;
        }
        if(retry_mount&&s->status.connected&&s->status.generation==generation&&
           !s->status.draining&&!atomic_load(&s->stop_requested))s->pending_address=address;
        if(s->vfs&&s->status.connected&&s->status.generation==generation&&
           !s->status.draining&&!atomic_load(&s->stop_requested)){
            s->status.mounted=true;s->status.last_error=ESP_OK;
        }else if(s->vfs)s->status.draining=true;
        s->mounting=false;s->working=false;
        portEXIT_CRITICAL(&s->lock);
        return;
    }
    if(!draining||users)return;
    portENTER_CRITICAL(&s->lock);
    s->working=true;s->status.draining=true;s->status.mounted=false;
    portEXIT_CRITICAL(&s->lock);
    if(s->vfs){
        esp_err_t error=esp_vfs_fat_lifecycle_acquire(20);
        if(error==ESP_OK){
            error=msc_host_vfs_unregister(s->vfs);
            esp_vfs_fat_lifecycle_release();
        }
        if(error!=ESP_OK){
            /* The owned MSC/BDL VFS path preserves every remaining handle
             * on failure. Retry there; only nontransactional device teardown
             * below requires a fatal class reservation. */
            portENTER_CRITICAL(&s->lock);
            s->working=false;s->status.last_error=error;
            portEXIT_CRITICAL(&s->lock);
            return;
        }
        s->vfs=NULL;
    }
    if(s->device){
        /* Official MSC teardown is not transactional on failure. Never retry
         * a partially deinitialized object or free its pending transfer on a
         * timer: retain class/core ownership and report an explicit error. */
        const esp_err_t error=msc_host_uninstall_device(s->device);
        if(error!=ESP_OK){record_cleanup_failure(s,error);return;}
        s->device=NULL;
        portENTER_CRITICAL(&s->lock);s->published_device=NULL;portEXIT_CRITICAL(&s->lock);
    }
    if(atomic_load(&s->stop_requested)){
        /* May join the driver's own client task; only this owner worker can
         * block here. Host poll_stopped never joins, sleeps, or touches USB. */
        const esp_err_t error=msc_host_uninstall();
        if(error!=ESP_OK){record_cleanup_failure(s,error);return;}
        portENTER_CRITICAL(&s->lock);
        s->status.driver_running=false;s->stopped=true;
        portEXIT_CRITICAL(&s->lock);
    }
    portENTER_CRITICAL(&s->lock);
    s->status.connected=false;s->status.mounted=false;s->status.draining=false;
    s->status.device_address=0;s->status.capacity_bytes=0;s->status.sector_size=0;
    s->working=false;
    portEXIT_CRITICAL(&s->lock);
}
static void storage_worker(void *context)
{
    struct aether_usb_storage *s=context;
    unsigned retry_ms=20;
    for(;;){
        process_storage(s);
        portENTER_CRITICAL(&s->lock);
        /* Lease release and new bus events notify us immediately. Do not poll
         * while another owner still holds files, or spin on a failed close. */
        const bool pending=!s->cleanup_fatal&&(s->pending_address||
            (s->status.draining&&!s->status.users));
        portEXIT_CRITICAL(&s->lock);
        ulTaskNotifyTake(pdTRUE,pending?pdMS_TO_TICKS(retry_ms):portMAX_DELAY);
        retry_ms=pending?(retry_ms<100?retry_ms*2:200):20;
    }
}
esp_err_t aether_usb_storage_class_start(void *context)
{
    struct aether_usb_storage *s=context?context:&s_storage;
    if(!known(s))return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s->lock);
    const bool busy=s->status.driver_running||s->working||s->status.users||s->cleanup_fatal;
    portEXIT_CRITICAL(&s->lock);
    if(busy)return ESP_ERR_INVALID_STATE;
    if(!s->task&&xTaskCreate(storage_worker,"usb_disk",6144,s,3,&s->task)!=pdPASS)return ESP_ERR_NO_MEM;
    const msc_host_driver_config_t config={
        .create_backround_task=true,.task_priority=4,.stack_size=4096,.core_id=tskNO_AFFINITY,
        .callback=storage_event,.callback_arg=s,
    };
    atomic_store(&s->stop_requested,false);
    const esp_err_t error=msc_host_install(&config);
    portENTER_CRITICAL(&s->lock);
    s->status.driver_running=error==ESP_OK;s->stopped=error!=ESP_OK;
    s->status.last_error=error;s->status.ignored_devices=0;
    s->status.draining=false;s->status.connected=false;s->status.mounted=false;
    s->status.generation=next_generation(s->status.generation);
    s->status.device_address=0;s->pending_address=0;
    portEXIT_CRITICAL(&s->lock);
    if(error!=ESP_OK)atomic_store(&s->stop_requested,true);
    notify_worker(s);return error;
}
void aether_usb_storage_class_request_stop(void *context)
{
    struct aether_usb_storage *s=context?context:&s_storage;
    if(!known(s))return;
    const bool was_stopping=atomic_exchange(&s->stop_requested,true);
    portENTER_CRITICAL(&s->lock);
    if(!was_stopping)s->status.generation=next_generation(s->status.generation);
    if(s->status.driver_running)s->status.draining=true;
    s->status.connected=false;s->pending_address=0;
    portEXIT_CRITICAL(&s->lock);notify_worker(s);
}
esp_err_t aether_usb_storage_class_poll_stopped(void *context)
{
    struct aether_usb_storage *s=context?context:&s_storage;
    if(!known(s))return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s->lock);
    const esp_err_t error=s->stopped&&!s->status.driver_running&&!s->working?ESP_OK:
        s->cleanup_fatal?s->status.last_error:ESP_ERR_NOT_FINISHED;
    portEXIT_CRITICAL(&s->lock);return error;
}
