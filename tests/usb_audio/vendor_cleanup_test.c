/* Executes extracted, unchanged vendor open/delete bodies and interface-add fail tail. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>
#include "esp_err.h"
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define UAC_RETURN_ON_INVALID_ARG(p) do { if (!(p)) return ESP_ERR_INVALID_ARG; } while (0)
#define UAC_RETURN_ON_FALSE(p,e,...) do { if (!(p)) return (e); } while (0)
#define UAC_RETURN_ON_ERROR(p,...) do { esp_err_t e_=(p);if(e_!=ESP_OK)return e_; } while (0)
#define UAC_GOTO_ON_ERROR(p,...) do { ret=(p);if(ret!=ESP_OK)goto fail; } while (0)
#define UAC_GOTO_ON_FALSE(p,e,...) do { if(!(p)){ret=(e);goto fail;} } while (0)
static unsigned critical;
#define UAC_ENTER_CRITICAL() (++critical)
#define UAC_EXIT_CRITICAL() do { assert(critical);--critical; } while(0)
#define RINGBUF_TYPE_BYTEBUF 1
#define UAC_INTERFACE_STATE_IDLE 1
#define UAC_INTERFACE_STATE_ACTIVE 2
#define UAC_INTERFACE_STATE_READY 3
#define FLAG_INTERFACE_WAIT_USER_DELETE 1
#define UAC_STREAM_RX 1
#define CONFIG_UAC_RINGBUF_SAFE_DELETE_WAITING_MS 1
#define pdMS_TO_TICKS(t) (t)
struct uac_host_device {
    STAILQ_ENTRY(uac_host_device) tailq_entry;
    void *ctrl_xfer,*ctrl_xfer_done,*device_busy,*cs_ac_desc;
    uint8_t addr;unsigned opened_cnt;void *dev_hdl;
};
typedef struct uac_host_device uac_device_t;
STAILQ_HEAD(device_queue,uac_host_device);
struct driver {void *client_handle;struct device_queue uac_devices_tailq;};
static struct driver driver;
static struct driver *s_uac_driver=&driver;
typedef void *usb_device_handle_t;
typedef struct {unsigned unused;} usb_config_desc_t;
struct alt {bool feature_unit;unsigned vol_ch_map;};
typedef struct {
    void *ringbuf,*ringbuf_mutex,*state_mutex;
    struct alt *iface_alt;
    unsigned cur_alt,ringbuf_size,ringbuf_threshold,state,flags;
    struct {unsigned type,addr,iface_num;} dev_info;
    uac_device_t *parent;
    void (*user_cb)(void);void *user_cb_arg;
    float vol_min_db,vol_max_db,vol_res_db;
} uac_iface_t;
typedef uac_iface_t *uac_host_device_handle_t;
typedef struct {
    uint8_t addr,iface_num;unsigned buffer_size,buffer_threshold;
    void (*callback)(void);void *callback_arg;
} uac_host_device_config_t;
static unsigned failure,device_closes,interface_deletes,semaphore_deletes,transfer_frees,ring_deletes;
static bool existing_parent;
static esp_err_t physical_close_error;
static unsigned interface_unlocks;
static uac_device_t *parent;
static usb_config_desc_t descriptor;
static esp_err_t _uac_host_device_delete(uac_device_t *);
static esp_err_t usb_host_transfer_free(void *p){++transfer_frees;free(p);return ESP_OK;}
static void vSemaphoreDelete(void *p){assert(p);++semaphore_deletes;free(p);}
static uac_iface_t *get_interface_by_addr(uint8_t addr,uint8_t iface){(void)addr;(void)iface;return NULL;}
static uac_device_t *get_uac_device_by_addr(uint8_t addr){(void)addr;return existing_parent?parent:NULL;}
static esp_err_t usb_host_device_open(void *client,uint8_t addr,usb_device_handle_t *out){(void)client;(void)addr;if(failure==1)return ESP_FAIL;*out=&descriptor;return ESP_OK;}
static esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t h,const usb_config_desc_t **out){assert(h);if(failure==2)return ESP_FAIL;*out=&descriptor;return ESP_OK;}
static esp_err_t _uac_host_device_add(uint8_t addr,usb_device_handle_t h,const usb_config_desc_t *desc,uac_device_t **out){(void)h;(void)desc;if(failure==3)return ESP_FAIL;parent=calloc(1,sizeof(*parent));parent->addr=addr;parent->dev_hdl=&descriptor;STAILQ_INSERT_TAIL(&driver.uac_devices_tailq,parent,tailq_entry);*out=parent;return ESP_OK;}
static esp_err_t uac_host_interface_add(uac_device_t *d,uint8_t iface,uac_iface_t **out){assert(d&&iface);if(failure==4)return ESP_FAIL;*out=calloc(1,sizeof(**out));(*out)->iface_alt=calloc(1,sizeof(struct alt));return ESP_OK;}
static void *xRingbufferCreate(unsigned bytes,unsigned kind){assert(bytes&&kind==1);return failure==5?NULL:malloc(bytes);}
static void vRingbufferDelete(void *p){assert(p);++ring_deletes;free(p);}
static esp_err_t uac_host_interface_delete(uac_iface_t *p){assert(p&&!p->ringbuf);++interface_deletes;free(p->iface_alt);free(p);return ESP_OK;}
static esp_err_t usb_host_device_close(void *client,usb_device_handle_t h){(void)client;assert(h);++device_closes;return physical_close_error;}
static esp_err_t uac_cs_request_get_volume_range(uac_iface_t *p,float *a,float *b,float *c){(void)p;(void)a;(void)b;(void)c;return ESP_OK;}
static uac_iface_t *get_iface_by_handle(uac_host_device_handle_t p){return p;}
static esp_err_t uac_host_interface_try_lock(uac_iface_t *p,uint32_t timeout){assert(p&&timeout);return ESP_OK;}
static void uac_host_interface_unlock(uac_iface_t *p){assert(p);++interface_unlocks;}
static esp_err_t uac_host_interface_suspend(uac_iface_t *p){p->state=UAC_INTERFACE_STATE_READY;return ESP_OK;}
static esp_err_t uac_host_interface_release_and_free_transfer(uac_iface_t *p){p->state=UAC_INTERFACE_STATE_IDLE;return ESP_OK;}
static void xRingbufferSend(void *p,void *data,unsigned bytes,unsigned timeout){assert(p&&data&&bytes==1&&!timeout);}
static void _ring_buffer_flush(void *p){assert(p);}
static void vTaskDelay(unsigned ticks){assert(ticks);}
#define DEFAULT_CTRL_XFER_TIMEOUT_MS 5000
#include "vendor_cleanup.inc"
static void reset(void){assert(STAILQ_EMPTY(&driver.uac_devices_tailq));failure=0;existing_parent=false;physical_close_error=ESP_OK;parent=NULL;device_closes=interface_deletes=0;}
static uac_device_t *partial_device(void){uac_device_t*d=calloc(1,sizeof(*d));d->ctrl_xfer=malloc(1);d->ctrl_xfer_done=malloc(1);d->device_busy=malloc(1);d->cs_ac_desc=malloc(1);return d;}
int main(void)
{
    STAILQ_INIT(&driver.uac_devices_tailq);
    const uac_host_device_config_t cfg={.addr=1,.iface_num=1,.buffer_size=8192,.buffer_threshold=1024};
    for(unsigned stage=1;stage<=5;++stage){reset();failure=stage;uac_host_device_handle_t out=(void*)1;
        assert(uac_host_device_open(&cfg,&out)!=ESP_OK&&!out&&STAILQ_EMPTY(&driver.uac_devices_tailq));
        assert(device_closes==(stage>1));assert(interface_deletes==(stage==5));}
    puts("PASS actual vendor open failure bodies survive USB-open/descriptor/device/interface/ring allocation errors without NULL dereference or use-after-free");
    reset();existing_parent=true;parent=calloc(1,sizeof(*parent));STAILQ_INSERT_TAIL(&driver.uac_devices_tailq,parent,tailq_entry);failure=4;
    uac_host_device_handle_t out=NULL;assert(uac_host_device_open(&cfg,&out)==ESP_FAIL&&!out&&!device_closes&&STAILQ_FIRST(&driver.uac_devices_tailq)==parent);
    assert(_uac_host_device_delete(parent)==ESP_OK);reset();
    assert(actual_interface_add_failure(NULL,ESP_ERR_NO_MEM)==ESP_ERR_NO_MEM);
    uac_iface_t *iface=calloc(1,sizeof(*iface));iface->iface_alt=malloc(16);iface->ringbuf_mutex=malloc(1);iface->state_mutex=malloc(1);
    const unsigned sem_before=semaphore_deletes;
    assert(actual_interface_add_failure(iface,ESP_FAIL)==ESP_FAIL&&semaphore_deletes==sem_before+2);
    puts("PASS exact interface-add fail tail is NULL-safe and releases partially allocated members; failed second interface preserves an existing physical device");
    const unsigned freed=transfer_frees;assert(_uac_host_device_delete(partial_device())==ESP_OK&&STAILQ_EMPTY(&driver.uac_devices_tailq)&&transfer_frees==freed+1);
    uac_device_t *a=partial_device(),*b=partial_device(),*unlinked=partial_device();
    STAILQ_INSERT_TAIL(&driver.uac_devices_tailq,a,tailq_entry);STAILQ_INSERT_TAIL(&driver.uac_devices_tailq,b,tailq_entry);
    assert(_uac_host_device_delete(unlinked)==ESP_OK&&STAILQ_FIRST(&driver.uac_devices_tailq)==a&&STAILQ_NEXT(a,tailq_entry)==b);
    assert(_uac_host_device_delete(b)==ESP_OK&&STAILQ_FIRST(&driver.uac_devices_tailq)==a&&!STAILQ_NEXT(a,tailq_entry));
    assert(_uac_host_device_delete(a)==ESP_OK&&STAILQ_EMPTY(&driver.uac_devices_tailq)&&!critical);
    puts("PASS actual vendor physical-device deletion safely handles empty/nonmember partial construction and exact-member removal while preserving other devices");
    reset();parent=calloc(1,sizeof(*parent));parent->dev_hdl=&descriptor;parent->opened_cnt=1;
    STAILQ_INSERT_TAIL(&driver.uac_devices_tailq,parent,tailq_entry);
    iface=calloc(1,sizeof(*iface));iface->parent=parent;iface->iface_alt=calloc(1,sizeof(struct alt));iface->state=UAC_INTERFACE_STATE_IDLE;
    physical_close_error=ESP_FAIL;const unsigned old_deletes=interface_deletes;
    assert(uac_host_device_close(iface)==ESP_FAIL&&parent->opened_cnt==1&&iface->parent==parent&&interface_deletes==old_deletes);
    assert(STAILQ_FIRST(&driver.uac_devices_tailq)==parent&&interface_unlocks==1);
    assert(uac_host_device_close(iface)==ESP_FAIL&&parent->opened_cnt==1&&interface_deletes==old_deletes);
    physical_close_error=ESP_OK;assert(uac_host_device_close(iface)==ESP_OK&&STAILQ_EMPTY(&driver.uac_devices_tailq)&&interface_deletes==old_deletes+1);
    reset();parent=calloc(1,sizeof(*parent));parent->dev_hdl=&descriptor;parent->opened_cnt=2;
    STAILQ_INSERT_TAIL(&driver.uac_devices_tailq,parent,tailq_entry);
    uac_iface_t *first=calloc(1,sizeof(*first)),*second=calloc(1,sizeof(*second));
    first->parent=second->parent=parent;first->iface_alt=calloc(1,sizeof(struct alt));second->iface_alt=calloc(1,sizeof(struct alt));
    assert(uac_host_device_close(first)==ESP_OK&&parent->opened_cnt==1&&!device_closes);
    physical_close_error=ESP_FAIL;assert(uac_host_device_close(second)==ESP_FAIL&&parent->opened_cnt==1);
    physical_close_error=ESP_OK;assert(uac_host_device_close(second)==ESP_OK&&STAILQ_EMPTY(&driver.uac_devices_tailq)&&!critical);
    puts("PASS actual vendor close restores parent reference after repeated USB-close failure; retry releases it once and cannot underflow or double-close another interface");
    return 0;
}
