/* Production control/MSC callbacks, with mocked USB transport and SD endpoint. */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include "../../components/aether_usb_msc/aether_usb_msc.c"

struct test_mutex { pthread_mutex_t lock; };
static _Atomic uint32_t tick;
static aether_storage_status_t card = {.card_inserted=true,.mounted=true,.capacity_bytes=1024*512};
static bool owned, pad=true, installed, vbus;
static int install_error, uninstall_error, begin_error, end_error, io_error;
static unsigned installs,uninstalls,ends,gpio_calls,reads,writes,syncs;
static int route;
static int query_action;
static bool mac_read_error;
static uint8_t sense,asc;
static uint64_t last_offset;
static size_t last_bytes;
static tinyusb_config_t saved_config;
static tusb_speed_t negotiated_speed = TUSB_SPEED_HIGH;
static unsigned speed_queries;
static bool block_write, write_entered;
static pthread_mutex_t write_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t write_cond=PTHREAD_COND_INITIALIZER;

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    struct test_mutex *m=malloc(sizeof(*m));assert(m);pthread_mutex_init(&m->lock,NULL);return m;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t timeout) {
    (void)timeout;return pthread_mutex_trylock(&m->lock)==0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {return pthread_mutex_unlock(&m->lock)==0 ? pdTRUE : pdFALSE;}
void vSemaphoreDelete(SemaphoreHandle_t m) {pthread_mutex_destroy(&m->lock);free(m);}
TickType_t xTaskGetTickCount(void) {return atomic_load(&tick);}
void vTaskDelay(TickType_t delay) {atomic_fetch_add(&tick,delay);struct timespec t={0,1000000};nanosleep(&t,NULL);}
void test_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
const char *esp_err_to_name(int error) {(void)error;return "test error";}
/* Match S31's default API: this deliberately writes eight bytes so ASan
 * catches regressions to passing the production six-byte factory buffer. */
esp_err_t esp_efuse_mac_get_default(uint8_t *mac) {
    const uint8_t eui64[8]={0x30,0xed,0xa0,0xff,0xfe,0xf4,0x68,0x8e};
    memcpy(mac,eui64,sizeof(eui64));return ESP_OK;
}
esp_err_t esp_read_mac(uint8_t *mac,esp_mac_type_t type) {
    assert(type==ESP_MAC_EFUSE_FACTORY);
    if(mac_read_error)return ESP_FAIL;
    const uint8_t factory[6]={0x30,0xed,0xa0,0xf4,0x68,0x8e};
    memcpy(mac,factory,sizeof(factory));return ESP_OK;
}
esp_err_t gpio_set_level(int pin,int level) {assert(pin==51 && (level==0 || level==1) && !installed);route=level;++gpio_calls;return ESP_OK;}
bool usb_serial_jtag_ll_phy_is_pad_enabled(void) {assert(!"USJ pad must not be modified by MSC component");return pad;}
void usb_serial_jtag_ll_phy_enable_pad(bool value) {(void)value;assert(!"USJ pad must not be modified by MSC component");}
esp_err_t aether_storage_get_status(aether_storage_handle_t h,aether_storage_status_t *out) {assert(h==(void*)1);*out=card;return ESP_OK;}
esp_err_t aether_storage_usb_begin(aether_storage_handle_t h,uint32_t timeout) {
    (void)timeout;assert(h==(void*)1);if(begin_error)return begin_error;owned=true;card.mounted=false;card.usb_exported=true;return ESP_OK;
}
esp_err_t aether_storage_usb_end(aether_storage_handle_t h,uint32_t timeout) {
    (void)timeout;assert(h==(void*)1 && !installed && pad && route==0);++ends;if(end_error)return end_error;
    owned=false;card.usb_exported=false;card.mounted=card.card_inserted;return ESP_OK;
}
bool aether_storage_usb_owned(aether_storage_handle_t h) {assert(h==(void*)1);return owned;}
static esp_err_t check_io(uint64_t offset,size_t bytes) {
    assert(owned);last_offset=offset;last_bytes=bytes;
    if(!card.card_inserted || !card.usb_exported)return ESP_ERR_INVALID_STATE;
    if(offset>card.capacity_bytes || bytes>card.capacity_bytes-offset)return ESP_ERR_INVALID_SIZE;
    return io_error;
}
esp_err_t aether_storage_usb_read(aether_storage_handle_t h,uint64_t offset,void *out,size_t bytes) {
    assert(h==(void*)1);++reads;esp_err_t result=check_io(offset,bytes);if(result==ESP_OK)memset(out,0x5a,bytes);return result;
}
esp_err_t aether_storage_usb_write(aether_storage_handle_t h,uint64_t offset,const void *data,size_t bytes) {
    (void)data;assert(h==(void*)1);++writes;
    pthread_mutex_lock(&write_lock);write_entered=true;pthread_cond_broadcast(&write_cond);
    while(block_write)pthread_cond_wait(&write_cond,&write_lock);
    pthread_mutex_unlock(&write_lock);
    return check_io(offset,bytes);
}
esp_err_t aether_storage_usb_sync(aether_storage_handle_t h) {assert(h==(void*)1 && owned);++syncs;return io_error;}
esp_err_t tinyusb_driver_install(const tinyusb_config_t *config) {
    assert(!installed && pad && owned && !card.mounted && route==1);
    assert(config->descriptor.device->idVendor==0x303a && config->descriptor.device->bNumConfigurations==1);
    assert(config->descriptor.full_speed_config && config->descriptor.high_speed_config && config->descriptor.qualifier);
    assert(!strcmp(config->descriptor.string[3],"30eda0f4688e"));
    ++installs;if(install_error)return install_error;saved_config=*config;installed=true;return ESP_OK;
}
esp_err_t tinyusb_driver_uninstall(void) {
    assert(installed && !s_callbacks && !s_accepting);++uninstalls;
    if(uninstall_error)return uninstall_error;
    installed=false;return ESP_OK;
}
tusb_speed_t tud_speed_get(void) {
    assert(installed);
    /* Reading the stack must never happen while holding our status lock. */
    assert(!pthread_mutex_trylock(&s_lock));pthread_mutex_unlock(&s_lock);
    ++speed_queries;return negotiated_speed;
}
bool tud_disconnect(void) {assert(installed);return true;}
bool tud_msc_set_sense(uint8_t lun,uint8_t key,uint8_t code,uint8_t qual) {(void)lun;(void)qual;sense=key;asc=code;return true;}
static esp_err_t query_vbus(bool *present,void *context) {
    (void)context;*present=vbus;
    const int action=query_action;query_action=0;
    if(action==1) {
        tinyusb_event_t attached={.id=TINYUSB_EVENT_ATTACHED};
        saved_config.event_cb(&attached,NULL);
    } else if(action==2) assert(tud_msc_start_stop_cb(0,0,true,true));
    return ESP_OK;
}
static void connected(bool yes) {vbus=yes;tinyusb_event_t e={.id=yes?TINYUSB_EVENT_ATTACHED:TINYUSB_EVENT_DETACHED};saved_config.event_cb(&e,NULL);}
static void expect_link_rate(tusb_speed_t speed, uint16_t expected) {
    aether_usb_msc_status_t status;
    negotiated_speed=speed;
    const unsigned before=speed_queries;
    connected(true);
    assert(speed_queries==before+1);
    for(unsigned i=0;i<3;++i) {
        assert(aether_usb_msc_get_status(&status)==ESP_OK && status.host_connected &&
               status.usb_speed_mbps==expected && speed_queries==before+1);
    }
    connected(false);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && !status.host_connected &&
           status.usb_speed_mbps==0 && speed_queries==before+1);
}
static void *writer(void *context) {(void)context;unsigned char data[512]={0};assert(tud_msc_write10_cb(0,0,0,data,512)==512);return NULL;}

int main(int argc,char **argv) {
    mac_read_error=argc==2 && !strcmp(argv[1],"--mac-read-error");
    aether_usb_msc_status_t status;
    assert(aether_usb_msc_get_status(&status)==ESP_ERR_INVALID_STATE && !aether_usb_msc_is_active() &&
           status.usb_speed_mbps==0 && !speed_queries);
    assert(aether_usb_msc_init((void*)1)==ESP_OK && !installs && !gpio_calls && pad);
    assert(!strcmp(s_serial,mac_read_error ? "000000000000" : "30eda0f4688e"));
    if(mac_read_error) {
        puts("PASS MAC read failure retains valid 12-digit fallback without starting USB");
        vSemaphoreDelete(s_control);s_control=NULL;s_storage=NULL;
        return 0;
    }
    assert(aether_usb_bus_claim(AETHER_USB_BUS_HOST)==ESP_OK);
    assert(aether_usb_msc_enter(100)==ESP_ERR_INVALID_STATE && !installs && !owned);
    assert(aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST);
    assert(aether_usb_bus_release(AETHER_USB_BUS_HOST)==ESP_OK);
    puts("PASS reserved OTG Host rejects MSC before storage/PHY/mux changes");
    aether_usb_msc_set_vbus_query(query_vbus,NULL);
    assert(aether_usb_msc_init((void*)1)==ESP_OK && aether_usb_msc_init((void*)2)==ESP_ERR_INVALID_STATE);
    assert(aether_usb_msc_enter(1000)==ESP_OK && installed && owned && pad && aether_usb_msc_is_active());
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.diagnostic_stage==AETHER_USB_MSC_STAGE_WAIT_HOST &&
           status.selected_route==1 && status.usb_speed_mbps==0 && !speed_queries);
    expect_link_rate(TUSB_SPEED_HIGH,480);
    expect_link_rate(TUSB_SPEED_FULL,12);
    expect_link_rate(TUSB_SPEED_INVALID,0);
    expect_link_rate(TUSB_SPEED_AUTO,0);
    expect_link_rate(TUSB_SPEED_LOW,0);
    negotiated_speed=TUSB_SPEED_HIGH;
    const unsigned cached_queries=speed_queries;
    card.usb_clock_khz=40000;card.usb_high_speed_fallback=false;
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_clock_khz==40000 &&
           !status.usb_high_speed_fallback && speed_queries==cached_queries);
    card.usb_clock_khz=20000;card.usb_high_speed_fallback=true;
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_clock_khz==20000 &&
           status.usb_high_speed_fallback && speed_queries==cached_queries);
    card.usb_clock_khz=0;card.usb_high_speed_fallback=false;
    puts("PASS negotiated HS/FS/unknown link rate queried once outside lock on attach, cached for status and cleared on detach");
    puts("PASS raw SD actual clock/fallback copied from storage snapshot without querying USB or card hardware");
    connected(true);assert(aether_usb_msc_exit(100)==ESP_ERR_INVALID_STATE && installed && owned);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_speed_mbps==480);
    tinyusb_event_t reset={.id=TINYUSB_EVENT_DETACHED};saved_config.event_cb(&reset,NULL);
    assert(aether_usb_msc_exit(100)==ESP_ERR_INVALID_STATE && installed && owned);
    connected(true);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.host_connected && !status.host_ejected && status.active && status.diagnostic_stage==AETHER_USB_MSC_STAGE_HOST);
    uint32_t blocks;uint16_t size;tud_msc_capacity_cb(0,&blocks,&size);assert(blocks==1024 && size==512 && tud_msc_get_maxlun_cb()==1);
    uint8_t vendor[8],product[16],revision[4];tud_msc_inquiry_cb(0,vendor,product,revision);assert(!memcmp(vendor,"AETHER01",8));
    unsigned char data[1024]={0};
    assert(tud_msc_read10_cb(0,1,17,data,sizeof(data))==1024 && data[0]==0x5a && last_offset==529 && last_bytes==1024);
    assert(tud_msc_write10_cb(0,2,9,data,512)==512 && last_offset==1033 && writes==1);
    assert(tud_msc_read10_cb(0,UINT32_MAX,UINT32_MAX,data,1)==-1 && sense==SCSI_SENSE_ILLEGAL_REQUEST && asc==0x21);
    io_error=ESP_FAIL;assert(tud_msc_write10_cb(0,0,0,data,512)==-1 && sense==SCSI_SENSE_MEDIUM_ERROR && asc==0x0c);io_error=0;
    uint8_t command[16]={0x35};assert(tud_msc_scsi_cb(0,command,NULL,0)==0 && syncs);
    assert(tud_msc_start_stop_cb(0,0,false,false));
    assert(aether_usb_msc_exit(100)==ESP_ERR_INVALID_STATE && installed && owned);
    assert(tud_msc_prevent_allow_medium_removal_cb(0,1,0));
    assert(!tud_msc_start_stop_cb(0,0,false,true) && aether_usb_msc_exit(100)==ESP_ERR_INVALID_STATE);
    assert(tud_msc_prevent_allow_medium_removal_cb(0,0,0));
    assert(aether_usb_msc_get_status(&status)==ESP_OK && !status.host_ejected);
    assert(aether_usb_msc_exit(100)==ESP_ERR_INVALID_STATE && tud_msc_test_unit_ready_cb(0));
    assert(!tud_msc_prevent_allow_medium_removal_cb(1,0,0));
    assert(tud_msc_start_stop_cb(0,0,false,true));
    assert(tud_msc_read10_cb(0,0,0,data,512)==-1 && !tud_msc_test_unit_ready_cb(0));
    assert(aether_usb_msc_exit(1000)==ESP_OK && !owned && !installed && pad && card.mounted);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_speed_mbps==0);
    puts("PASS lazy OFF/download route0; explicit MSC route1 only before driver install, FS/HS descriptors, synchronous IO, bounds, sense and safe-eject exit to route0");

    assert(aether_usb_msc_enter(1000)==ESP_OK);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_speed_mbps==0);
    vbus=true;query_action=1;
    assert(aether_usb_msc_exit(1000)==ESP_ERR_INVALID_STATE && installed && owned);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.host_connected && status.active);
    assert(tud_msc_start_stop_cb(0,0,false,true));
    query_action=2;
    assert(aether_usb_msc_exit(1000)==ESP_ERR_INVALID_STATE && installed && owned);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && !status.host_ejected && status.active);
    assert(tud_msc_start_stop_cb(0,0,false,true));
    assert(aether_usb_msc_exit(1000)==ESP_OK && !owned && !installed && route==0);
    puts("PASS host configuration or media reload during VBUS query cannot bypass current ownership/safe-eject checks");

    unsigned before=installs;begin_error=ESP_FAIL;
    assert(aether_usb_msc_enter(1000)==ESP_FAIL && !aether_usb_msc_is_active() && installs==before && pad);begin_error=0;
    install_error=ESP_FAIL;
    assert(aether_usb_msc_enter(1000)==ESP_FAIL && !aether_usb_msc_is_active() && !owned && pad && card.mounted);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.diagnostic_stage==AETHER_USB_MSC_STAGE_STACK_START);install_error=0;
    assert(aether_usb_msc_enter(1000)==ESP_OK);connected(true);
    assert(tud_msc_start_stop_cb(0,0,false,true));uninstall_error=ESP_FAIL;
    assert(aether_usb_msc_exit(1000)==ESP_FAIL && aether_usb_msc_is_active() && owned && installed && pad && !card.mounted);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_speed_mbps==0);
    uninstall_error=0;assert(aether_usb_msc_exit(1000)==ESP_OK && pad && !owned && card.mounted);
    puts("PASS startup rollback and uninstall failure never dual-mount; explicit retry restores the local card/download path");

    assert(aether_usb_msc_enter(1000)==ESP_OK);connected(true);
    pthread_mutex_lock(&write_lock);block_write=true;write_entered=false;pthread_mutex_unlock(&write_lock);
    pthread_t thread;assert(!pthread_create(&thread,NULL,writer,NULL));
    pthread_mutex_lock(&write_lock);while(!write_entered)pthread_cond_wait(&write_cond,&write_lock);pthread_mutex_unlock(&write_lock);
    connected(false);before=uninstalls;
    assert(aether_usb_msc_exit(20)==ESP_ERR_TIMEOUT && uninstalls==before && installed && owned && aether_usb_msc_is_active());
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_speed_mbps==0);
    pthread_mutex_lock(&write_lock);block_write=false;pthread_cond_broadcast(&write_cond);pthread_mutex_unlock(&write_lock);
    pthread_join(thread,NULL);
    assert(aether_usb_msc_exit(1000)==ESP_OK && !owned && pad);
    puts("PASS in-flight SD writes drain before detach; timeout keeps ownership blocked and retry completes safely");

    assert(aether_usb_msc_enter(1000)==ESP_OK);connected(true);
    assert(tud_msc_prevent_allow_medium_removal_cb(0,1,0));
    assert(aether_usb_msc_exit(100)==ESP_ERR_INVALID_STATE && s_prevent_removal && s_accepting);
    uninstall_error=ESP_FAIL;
    assert(aether_usb_msc_exit_confirmed(1000)==ESP_FAIL && installed && owned && !card.mounted);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.active && !status.host_ejected);
    assert(!s_accepting && tud_msc_read10_cb(0,0,0,data,512)==-1);
    uninstall_error=0;
    assert(aether_usb_msc_exit(1000)==ESP_OK && !owned && card.mounted);
    assert(aether_usb_msc_enter(1000)==ESP_OK);connected(true);
    assert(aether_usb_msc_exit(100)==ESP_ERR_INVALID_STATE && installed && owned && s_accepting);
    pthread_mutex_lock(&write_lock);block_write=true;write_entered=false;pthread_mutex_unlock(&write_lock);
    assert(!pthread_create(&thread,NULL,writer,NULL));
    pthread_mutex_lock(&write_lock);while(!write_entered)pthread_cond_wait(&write_cond,&write_lock);pthread_mutex_unlock(&write_lock);
    before=uninstalls;
    assert(aether_usb_msc_exit_confirmed(20)==ESP_ERR_TIMEOUT && uninstalls==before && installed && owned && !card.mounted);
    assert(aether_usb_msc_get_status(&status)==ESP_OK && status.active && !status.host_ejected);
    assert(!s_accepting && tud_msc_read10_cb(0,0,0,data,512)==-1);
    pthread_mutex_lock(&write_lock);block_write=false;pthread_cond_broadcast(&write_cond);pthread_mutex_unlock(&write_lock);
    pthread_join(thread,NULL);
    assert(aether_usb_msc_exit(1000)==ESP_OK && !owned && card.mounted);
    assert(aether_usb_msc_enter(1000)==ESP_OK);connected(true);
    assert(aether_usb_msc_exit(100)==ESP_ERR_INVALID_STATE && installed && owned);
    assert(aether_usb_msc_exit_confirmed(1000)==ESP_OK && !owned && !installed && card.mounted && route==0);
    puts("PASS one-call confirmed exit drains writes and preserves failure ownership; no fake eject or confirmation leaks to later sessions; STOP0/ALLOW never release a mounted host");

    for(unsigned i=0;i<100;++i) {
        assert(aether_usb_msc_enter(1000)==ESP_OK);
        assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_speed_mbps==0);
        connected(true);
        assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_speed_mbps==480);
        card.card_inserted=false;card.usb_exported=false;
        assert(!tud_msc_test_unit_ready_cb(0) && tud_msc_write10_cb(0,0,0,data,512)==-1);
        assert(aether_usb_msc_exit(1000)==ESP_OK && !aether_usb_msc_is_active() && pad);
        assert(aether_usb_msc_get_status(&status)==ESP_OK && status.usb_speed_mbps==0);
        card.card_inserted=true;card.mounted=true;
    }
    puts("PASS 100 export/removal/exit cycles preserve explicit OFF default, callback lifetime, local gate and USJ route");
    assert(aether_usb_bus_get_owner()==AETHER_USB_BUS_NONE);
    vSemaphoreDelete(s_control);s_control=NULL;
    return 0;
}

