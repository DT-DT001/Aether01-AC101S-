#include "aether_usb_host.h"
#include "aether_usb_bus.h"
#include "aether_board_pins.h"
#include "hid_report.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#define HID_SLOTS 8
#define DEVICE_SLOTS 4
#define REPORT_LIMIT 4096
#define POLL_US 20000
#define CLASS_LIMIT 3
struct aether_usb_host;
typedef struct {
    uint16_t vid,pid,report_length;
    uint8_t number,subclass,protocol;
} hid_identity_t;
typedef struct {
    struct aether_usb_host *owner;
    usb_device_handle_t dev;
    usb_transfer_t *in,*control;
    aether_hid_report_t parser;
    aether_hid_input_t generic;
    hid_identity_t identity;
    uint8_t endpoint,control_stage;
    uint16_t packet,buttons,control_bytes,report_received;
    bool used,claimed,in_pending,control_pending,ready,closing;
} hid_interface_t;
struct aether_usb_host {
    aether_usb_host_config_t config;
    portMUX_TYPE lock;
    TaskHandle_t task;
    aether_usb_host_status_t status;
    atomic_bool requested,reserved;
    bool requested_camera_profile; /* Protected by lock. */
    bool session_camera_profile; /* Host worker only. */
    bool installed,closing,policy_dirty,bus_fault;
    usb_host_client_handle_t client;
    usb_device_handle_t devices[DEVICE_SLOTS];
    hid_interface_t *hid;
    int64_t role_due,retry_after,exit_combo_since;
    unsigned sink_samples;
    uint8_t started_classes;
    uint8_t generic_keys[32],generic_mouse_buttons;
};
/* Class hooks/input snapshots remain bounded small control metadata; large
 * report models and class payloads stay outside this INTERNAL allocation. */
_Static_assert(sizeof(struct aether_usb_host)<=512,"Keep Host lock/atomics/control in a small internal allocation");
static void snapshot_error(struct aether_usb_host *h,esp_err_t e) {
    portENTER_CRITICAL(&h->lock);h->status.last_error=e;portEXIT_CRITICAL(&h->lock);
}
static void hid_diagnostic(struct aether_usb_host *h,const hid_identity_t *id,
                           aether_usb_hid_stage_t stage,esp_err_t error,
                           int32_t transfer_status,uint16_t received,esp_err_t parse_error) {
    portENTER_CRITICAL(&h->lock);
    /* Keep the identity, stage and transfer status of the actual failure
     * together, even if another composite interface becomes ready later. */
    if(error!=ESP_OK||h->status.hid_error==ESP_OK){
        h->status.hid_stage=stage;h->status.hid_error=error;
        h->status.transfer_status=transfer_status;h->status.report_received=received;
        h->status.report_parse_error=parse_error;
        h->status.vid=id?id->vid:0;h->status.pid=id?id->pid:0;
        h->status.last_interface=id?id->number:UINT8_MAX;
        h->status.last_subclass=id?id->subclass:0;h->status.last_protocol=id?id->protocol:0;
        h->status.report_length=id?id->report_length:0;
    }
    if(error!=ESP_OK)h->status.last_error=error;
    portEXIT_CRITICAL(&h->lock);
}
static void reset_hid_diagnostics(struct aether_usb_host *h) {
    portENTER_CRITICAL(&h->lock);
    h->status.hid_stage=AETHER_USB_HID_STAGE_NONE;h->status.hid_error=ESP_OK;
    h->status.report_parse_error=ESP_OK;h->status.vid=h->status.pid=0;
    h->status.report_length=h->status.report_received=0;h->status.last_interface=UINT8_MAX;
    h->status.last_subclass=h->status.last_protocol=h->status.ignored_interfaces=0;
    h->status.transfer_status=-1;
    portEXIT_CRITICAL(&h->lock);
}
static esp_err_t transfer_error(int status) {
    switch(status){
    case USB_TRANSFER_STATUS_STALL:return ESP_ERR_NOT_SUPPORTED;
    case USB_TRANSFER_STATUS_TIMED_OUT:return ESP_ERR_TIMEOUT;
    case USB_TRANSFER_STATUS_NO_DEVICE:return ESP_ERR_NOT_FOUND;
    case USB_TRANSFER_STATUS_CANCELED:return ESP_ERR_INVALID_STATE;
    case USB_TRANSFER_STATUS_OVERFLOW:return ESP_ERR_INVALID_RESPONSE;
    default:return ESP_FAIL;
    }
}
static void aggregate_generic(struct aether_usb_host *h,const aether_hid_input_t *motion) {
    uint8_t keys[32]={0},buttons=0;
    for(unsigned n=0;n<HID_SLOTS;++n)if(h->hid[n].used&&h->hid[n].ready&&!h->hid[n].closing){
        for(unsigned k=0;k<32;++k)keys[k]|=h->hid[n].generic.keys[k];
        buttons|=h->hid[n].generic.buttons;
    }
    if(memcmp(keys,h->generic_keys,sizeof keys)){
        memcpy(h->generic_keys,keys,sizeof keys);
        if(h->config.hid_input){
            aether_usb_input_event_t event={.type=AETHER_USB_INPUT_KEYBOARD};
            memcpy(event.keys,keys,sizeof keys);h->config.hid_input(h->config.context,&event);
        }
    }
    if(buttons!=h->generic_mouse_buttons||(motion&&motion->mouse&&(motion->dx||motion->dy||motion->wheel))){
        h->generic_mouse_buttons=buttons;
        if(h->config.hid_input){
            const aether_usb_input_event_t event={.type=AETHER_USB_INPUT_MOUSE,.buttons=buttons,
                .dx=motion?motion->dx:0,.dy=motion?motion->dy:0,.wheel=motion?motion->wheel:0};
            h->config.hid_input(h->config.context,&event);
        }
    }
}
static uint16_t aggregate_motion(struct aether_usb_host *h,const aether_hid_input_t *motion) {
    uint16_t b=0;unsigned count=0,keyboards=0;uint8_t keys[32]={0};
    for(unsigned i=0;i<HID_SLOTS;++i)if(h->hid[i].used&&h->hid[i].ready&&!h->hid[i].closing){
        b|=h->config.keyboard_map ? h->hid[i].generic.non_keyboard_buttons : h->hid[i].buttons;
        if(h->config.keyboard_map)for(unsigned k=0;k<32;++k)keys[k]|=h->hid[i].generic.keys[k];
        ++count;
        keyboards+=h->hid[i].parser.has_keyboard;
    }
    if(h->config.keyboard_map)b|=h->config.keyboard_map(h->config.context,keys);
    const uint16_t exit_chord=AETHER_HID_SELECT|AETHER_HID_START|AETHER_HID_UP;
    const int64_t now=esp_timer_get_time();
    if((b&exit_chord)==exit_chord){
        if(!h->exit_combo_since)h->exit_combo_since=now?now:1;
        if(now-h->exit_combo_since>=1000000)b|=AETHER_HID_EXIT;
    }else h->exit_combo_since=0;
    portENTER_CRITICAL(&h->lock);h->status.buttons=b;h->status.hid_interfaces=(uint8_t)count;
    h->status.keyboard_interfaces=(uint8_t)keyboards;portEXIT_CRITICAL(&h->lock);
    if(h->config.input)h->config.input(h->config.context,b);
    aggregate_generic(h,motion);
    return b;
}
static uint16_t aggregate(struct aether_usb_host *h) { return aggregate_motion(h,NULL); }
static void pump(struct aether_usb_host *h) {
    if(h->installed){uint32_t events=0;esp_err_t e=usb_host_lib_handle_events(0,&events);
        if(e!=ESP_OK&&e!=ESP_ERR_TIMEOUT){snapshot_error(h,e);h->bus_fault=true;}}
    if(h->client){esp_err_t e=usb_host_client_handle_events(h->client,0);
        if(e!=ESP_OK&&e!=ESP_ERR_TIMEOUT){snapshot_error(h,e);h->bus_fault=true;}}
}
static void input_done(usb_transfer_t *t) {
    hid_interface_t *i=t->context;struct aether_usb_host *h=i->owner;i->in_pending=false;
    if(i->closing||h->closing||!atomic_load(&h->requested)){i->buttons=0;aggregate(h);return;}
    if(t->status==USB_TRANSFER_STATUS_COMPLETED){uint16_t buttons;aether_hid_input_t generic;
        if(t->actual_num_bytes<0||t->actual_num_bytes>(int)t->data_buffer_size){
            i->closing=true;i->buttons=0;
            hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_INPUT,ESP_ERR_INVALID_RESPONSE,t->status,i->report_received,ESP_OK);
            aggregate(h);return;
        }
        if(aether_hid_decode_input(&i->parser,t->data_buffer,(size_t)t->actual_num_bytes,&buttons,&generic)){
            i->buttons=buttons;i->generic=generic;aggregate_motion(h,&generic);
        }
        esp_err_t e=usb_host_transfer_submit(t);if(e==ESP_OK)i->in_pending=true;
        else{i->closing=true;i->buttons=0;
            hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_INPUT,e,-1,i->report_received,ESP_OK);aggregate(h);}
    } else {i->closing=true;i->buttons=0;
        hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_INPUT,transfer_error(t->status),t->status,i->report_received,ESP_OK);aggregate(h);}
}
static esp_err_t request_report_descriptor(hid_interface_t *i) {
    const uint16_t length=i->identity.report_length;
    const uint8_t setup[8]={0x81,6,0,0x22,i->identity.number,0,(uint8_t)length,(uint8_t)(length>>8)};
    memcpy(i->control->data_buffer,setup,8);i->control->num_bytes=i->control_bytes;i->control_stage=1;
    hid_diagnostic(i->owner,&i->identity,AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR,ESP_OK,-1,0,ESP_OK);
    const esp_err_t e=usb_host_transfer_submit_control(i->owner->client,i->control);
    if(e==ESP_OK)i->control_pending=true;
    else{i->closing=true;hid_diagnostic(i->owner,&i->identity,AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR,e,-1,0,ESP_OK);}
    return e;
}
static void report_done(usb_transfer_t *t) {
    hid_interface_t *i=t->context;struct aether_usb_host *h=i->owner;i->control_pending=false;
    if(i->closing||h->closing||!atomic_load(&h->requested)){i->buttons=0;aggregate(h);return;}
    if(i->control_stage==0) {
        /* Some real receivers stall SET_PROTOCOL despite report protocol being
         * their power-on default. Only STALL is a compatibility fallback:
         * cancellation, disconnect and transport errors remain real failures.
         * USBH owns EP0 halt/flush/clear before this client completion. */
        if(t->status!=USB_TRANSFER_STATUS_COMPLETED&&t->status!=USB_TRANSFER_STATUS_STALL){
            i->closing=true;
            hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_SET_PROTOCOL,transfer_error(t->status),t->status,0,ESP_OK);return;
        }
        (void)request_report_descriptor(i);return;
    }
    if(t->status!=USB_TRANSFER_STATUS_COMPLETED){
        i->closing=true;
        hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR,transfer_error(t->status),t->status,0,ESP_OK);return;
    }
    /* Actual control length includes setup. Never parse transport padding, a
     * truncated report descriptor, or bytes beyond the allocated transfer. */
    const int actual=t->actual_num_bytes;
    if(actual<8||actual>(int)t->data_buffer_size||actual>i->control_bytes||
       actual-8<i->identity.report_length){
        i->closing=true;
        const uint16_t got=actual>=8&&actual-8<=UINT16_MAX?(uint16_t)(actual-8):0;
        hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR,ESP_ERR_INVALID_RESPONSE,t->status,got,ESP_OK);return;
    }
    i->report_received=(uint16_t)(actual-8);
    const aether_hid_parse_result_t parsed=aether_hid_parse_ex(&i->parser,t->data_buffer+8,i->identity.report_length);
    if(parsed==AETHER_HID_PARSE_UNMAPPED){
        /* Consumer/media/vendor/mouse-only interfaces are normal on wireless
         * keyboard receivers. They do not invalidate a usable sibling. */
        portENTER_CRITICAL(&h->lock);
        if(h->status.ignored_interfaces<UINT8_MAX)++h->status.ignored_interfaces;
        portEXIT_CRITICAL(&h->lock);
        i->closing=true;i->buttons=0;
        hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_IGNORED,ESP_OK,t->status,i->report_received,ESP_OK);
        aggregate(h);return;
    }
    if(parsed!=AETHER_HID_PARSE_MAPPED){
        i->closing=true;
        const esp_err_t e=parsed==AETHER_HID_PARSE_LIMIT?ESP_ERR_NOT_SUPPORTED:ESP_ERR_INVALID_RESPONSE;
        hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_REPORT_PARSE,e,t->status,i->report_received,e);return;
    }
    i->ready=true;
    esp_err_t e=usb_host_transfer_submit(i->in);
    if(e==ESP_OK){i->in_pending=true;
        hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_READY,ESP_OK,t->status,i->report_received,ESP_OK);
    }else{i->closing=true;i->ready=false;
        hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_INPUT,e,-1,i->report_received,ESP_OK);}
    aggregate(h);
}
static esp_err_t open_interface(struct aether_usb_host *h,usb_device_handle_t dev,const usb_device_desc_t *descriptor,
                                uint8_t number,uint8_t subclass,uint8_t protocol,uint8_t ep,uint16_t packet,uint16_t report_len) {
    const hid_identity_t identity={.vid=descriptor->idVendor,.pid=descriptor->idProduct,
        .number=number,.subclass=subclass,.protocol=protocol,.report_length=report_len};
    hid_diagnostic(h,&identity,AETHER_USB_HID_STAGE_INTERFACE,ESP_OK,-1,0,ESP_OK);
    if(!report_len||report_len>REPORT_LIMIT||!packet||packet>1024){
        hid_diagnostic(h,&identity,AETHER_USB_HID_STAGE_INTERFACE,ESP_ERR_NOT_SUPPORTED,-1,0,ESP_OK);return ESP_ERR_NOT_SUPPORTED;}
    hid_interface_t *i=NULL;for(unsigned n=0;n<HID_SLOTS;++n)if(!h->hid[n].used){i=&h->hid[n];break;}
    if(!i){hid_diagnostic(h,&identity,AETHER_USB_HID_STAGE_INTERFACE,ESP_ERR_NO_MEM,-1,0,ESP_OK);return ESP_ERR_NO_MEM;}
    memset(i,0,sizeof(*i));i->used=true;i->owner=h;i->dev=dev;i->identity=identity;
    i->endpoint=ep;i->packet=packet;
    esp_err_t e=usb_host_interface_claim(h->client,dev,number,0);
    if(e!=ESP_OK)goto fail;
    i->claimed=true;
    e=usb_host_transfer_alloc(packet,0,&i->in);if(e!=ESP_OK)goto fail;
    i->in->device_handle=dev;i->in->bEndpointAddress=ep;i->in->callback=input_done;i->in->context=i;
    i->in->num_bytes=packet;i->in->timeout_ms=0;
    const unsigned ep0=descriptor->bMaxPacketSize0;
    if(!ep0||ep0>64||(ep0&(ep0-1))){e=ESP_ERR_INVALID_RESPONSE;goto fail;}
    const unsigned control_bytes=8+((report_len+ep0-1)/ep0)*ep0;
    i->control_bytes=(uint16_t)control_bytes;
    e=usb_host_transfer_alloc(control_bytes,0,&i->control);if(e!=ESP_OK)goto fail;
    i->control->device_handle=dev;i->control->bEndpointAddress=0;i->control->callback=report_done;i->control->context=i;
    i->control->timeout_ms=1000;
    /* Protocol is defined only for the Boot subclass. Decode all interfaces
     * from their real report descriptor, including composite mouse protocols. */
    if(subclass==1&&(protocol==1||protocol==2)){
        const uint8_t set_report_protocol[8]={0x21,0x0b,1,0,number,0,0,0};
        memcpy(i->control->data_buffer,set_report_protocol,8);i->control->num_bytes=8;i->control_stage=0;
        hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_SET_PROTOCOL,ESP_OK,-1,0,ESP_OK);
        e=usb_host_transfer_submit_control(h->client,i->control);
        if(e==ESP_OK)i->control_pending=true;
        else{i->closing=true;hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_SET_PROTOCOL,e,-1,0,ESP_OK);}
        return e;
    }
    return request_report_descriptor(i);
fail:
    i->closing=true;hid_diagnostic(h,&i->identity,AETHER_USB_HID_STAGE_INTERFACE,e,-1,0,ESP_OK);return e;
}
static void inspect_device(struct aether_usb_host *h,uint8_t addr) {
    unsigned slot=0;for(;slot<DEVICE_SLOTS;++slot)if(!h->devices[slot])break;
    if(slot==DEVICE_SLOTS){hid_diagnostic(h,NULL,AETHER_USB_HID_STAGE_DEVICE,ESP_ERR_NO_MEM,-1,0,ESP_OK);return;}
    usb_device_handle_t dev=NULL;esp_err_t e=usb_host_device_open(h->client,addr,&dev);
    if(e!=ESP_OK){hid_diagnostic(h,NULL,AETHER_USB_HID_STAGE_DEVICE,e,-1,0,ESP_OK);return;}h->devices[slot]=dev;
    const usb_device_desc_t *descriptor=NULL;e=usb_host_get_device_descriptor(dev,&descriptor);
    if(e!=ESP_OK||!descriptor){hid_diagnostic(h,NULL,AETHER_USB_HID_STAGE_DEVICE,e!=ESP_OK?e:ESP_ERR_INVALID_RESPONSE,-1,0,ESP_OK);return;}
    const hid_identity_t device_id={.vid=descriptor->idVendor,.pid=descriptor->idProduct,.number=UINT8_MAX};
    hid_diagnostic(h,&device_id,AETHER_USB_HID_STAGE_DEVICE,ESP_OK,-1,0,ESP_OK);
    const usb_config_desc_t *cfg=NULL;e=usb_host_get_active_config_descriptor(dev,&cfg);
    if(e!=ESP_OK){hid_diagnostic(h,&device_id,AETHER_USB_HID_STAGE_DEVICE,e,-1,0,ESP_OK);return;}
    const uint8_t *p=(const uint8_t*)cfg;size_t total=cfg->wTotalLength,at=0;
    bool hid=false;uint8_t number=0,subclass=0,protocol=0,ep=0;uint16_t packet=0,report=0;
    while(at<total){if(total-at<2||p[at]<2||p[at]>total-at){
            hid_diagnostic(h,&device_id,AETHER_USB_HID_STAGE_DEVICE,ESP_ERR_INVALID_RESPONSE,-1,0,ESP_OK);break;}
        uint8_t n=p[at],type=p[at+1];
        if(type==4){
            if(hid&&ep)(void)open_interface(h,dev,descriptor,number,subclass,protocol,ep,packet,report);
            hid=n>=9&&p[at+3]==0&&p[at+5]==3;ep=0;packet=report=0;
            if(n>=9){number=p[at+2];subclass=p[at+6];protocol=p[at+7];}
        } else if(hid&&type==0x21&&n>=6){
            unsigned count=p[at+5];if(6U+3U*count>n){
                const hid_identity_t bad={.vid=descriptor->idVendor,.pid=descriptor->idProduct,
                    .number=number,.subclass=subclass,.protocol=protocol};
                hid_diagnostic(h,&bad,AETHER_USB_HID_STAGE_INTERFACE,ESP_ERR_INVALID_RESPONSE,-1,0,ESP_OK);hid=false;
            }else for(unsigned j=0;j<count;++j)if(p[at+6+j*3]==0x22)
                report=(uint16_t)(p[at+7+j*3]|p[at+8+j*3]<<8);
        } else if(hid&&!ep&&type==5&&n>=7&&(p[at+2]&0x80)&&(p[at+3]&3)==3){
            ep=p[at+2];packet=(uint16_t)(p[at+4]|p[at+5]<<8);
        }
        at+=n;
    }
    if(hid&&ep)(void)open_interface(h,dev,descriptor,number,subclass,protocol,ep,packet,report);
}
static void client_event(const usb_host_client_event_msg_t *event,void *ctx) {
    struct aether_usb_host *h=ctx;
    if(event->event==USB_HOST_CLIENT_EVENT_NEW_DEV){if(!h->closing&&atomic_load(&h->requested))inspect_device(h,event->new_dev.address);}
    else if(event->event==USB_HOST_CLIENT_EVENT_DEV_GONE){
        for(unsigned n=0;n<HID_SLOTS;++n)if(h->hid[n].used&&h->hid[n].dev==event->dev_gone.dev_hdl){h->hid[n].closing=true;h->hid[n].buttons=0;}
        aggregate(h);
    }
}
/* All calls are on the single owner task. A closing transfer stays allocated
 * until the Host library has returned its callback; never free on a timer. */
static bool reap(struct aether_usb_host *h) {
    bool busy=false;
    for(unsigned n=0;n<HID_SLOTS;++n){hid_interface_t *i=&h->hid[n];if(!i->used||!i->closing)continue;
        if(i->in_pending){usb_host_endpoint_halt(i->dev,i->endpoint);usb_host_endpoint_flush(i->dev,i->endpoint);}
        if(i->in_pending||i->control_pending){busy=true;continue;}
        if(i->claimed){esp_err_t e=usb_host_interface_release(h->client,i->dev,i->identity.number);if(e!=ESP_OK){snapshot_error(h,e);busy=true;continue;}i->claimed=false;}
        if(i->in){esp_err_t e=usb_host_transfer_free(i->in);if(e!=ESP_OK){snapshot_error(h,e);busy=true;continue;}i->in=NULL;}
        if(i->control){esp_err_t e=usb_host_transfer_free(i->control);if(e!=ESP_OK){snapshot_error(h,e);busy=true;continue;}i->control=NULL;}
        memset(i,0,sizeof(*i));
    }
    for(unsigned d=0;d<DEVICE_SLOTS;++d)if(h->devices[d]){
        bool used=false;for(unsigned n=0;n<HID_SLOTS;++n)if(h->hid[n].used&&h->hid[n].dev==h->devices[d])used=true;
        if(!used){esp_err_t e=usb_host_device_close(h->client,h->devices[d]);if(e==ESP_OK)h->devices[d]=NULL;else{snapshot_error(h,e);busy=true;}}
    }
    return !busy;
}
static esp_err_t backend_start(struct aether_usb_host *h,bool camera_profile) {
    reset_hid_diagnostics(h);
    esp_err_t e=gpio_set_level(AETHER_GPIO_USB_SELECT,AETHER_USB_MSC_SELECT_LEVEL);if(e!=ESP_OK)return e;
    /* Same measured OTG PHY path as MSC; Host role is selected by the official
     * S31 UTMI driver. Do not manipulate native-USJ pad/clock registers. */
    usb_host_config_t cfg={.skip_phy_setup=false,.root_port_unpowered=true,.intr_flags=ESP_INTR_FLAG_LOWMED,.peripheral_map=0};
    if(camera_profile){
        /* ESP32-S31 USB DMA: 896 words total. RX covers 3 x 1024 bytes per
         * microframe, with room for status. NPTX is for EP0 in this profile. */
        cfg.fifo_settings_custom.rx_fifo_lines=800;
        cfg.fifo_settings_custom.nptx_fifo_lines=64;
        cfg.fifo_settings_custom.ptx_fifo_lines=32;
    }
    e=usb_host_install(&cfg);if(e!=ESP_OK)return e;
    h->installed=true;h->session_camera_profile=camera_profile;
    const usb_host_client_config_t client={.is_synchronous=false,.max_num_event_msg=8,
        .async={.client_event_callback=client_event,.callback_arg=h}};
    e=usb_host_client_register(&client,&h->client);
    if(e!=ESP_OK)return e;
    for(unsigned n=0;n<h->config.class_count;++n){
        if(camera_profile&&!(h->config.camera_class_mask&(1U<<n)))continue;
        const aether_usb_host_class_ops_t *ops=&h->config.classes[n];
        e=ops->start(ops->context);
        if(e!=ESP_OK)return e; /* Failed start rolls back its own partial work. */
        h->started_classes|=(uint8_t)(1U<<n);
    }
    return ESP_OK;
}
static esp_err_t backend_stop(struct aether_usb_host *h) {
    if(h->installed&&!h->closing){h->closing=true;
        for(unsigned n=0;n<h->config.class_count;++n)if(h->started_classes&(1U<<n)){
            const aether_usb_host_class_ops_t *ops=&h->config.classes[n];
            ops->request_stop(ops->context);
        }
        usb_host_lib_set_root_port_power(false);
        for(unsigned n=0;n<HID_SLOTS;++n)if(h->hid[n].used){h->hid[n].closing=true;h->hid[n].buttons=0;}
        aggregate(h);
        if(h->config.hid_input){
            const aether_usb_input_event_t reset={.type=AETHER_USB_INPUT_RESET};
            h->config.hid_input(h->config.context,&reset);
        }
    }
    pump(h);
    bool drained=reap(h);esp_err_t class_error=ESP_OK;
    for(unsigned n=0;n<h->config.class_count;++n)if(h->started_classes&(1U<<n)){
        const aether_usb_host_class_ops_t *ops=&h->config.classes[n];
        const esp_err_t e=ops->poll_stopped(ops->context);
        if(e==ESP_OK)h->started_classes&=(uint8_t)~(1U<<n);
        else{drained=false;if(e!=ESP_ERR_NOT_FINISHED)class_error=e;}
    }
    if(!drained)return class_error!=ESP_OK?class_error:ESP_ERR_NOT_FINISHED;
    if(h->client){esp_err_t e=usb_host_client_deregister(h->client);if(e!=ESP_OK)return e;h->client=NULL;}
    if(h->installed){esp_err_t e=usb_host_device_free_all();if(e!=ESP_OK)return e;
        pump(h);e=usb_host_uninstall();if(e!=ESP_OK)return e;h->installed=false;}
    h->closing=false;h->bus_fault=false;return ESP_OK;
}
static esp_err_t power_off(struct aether_usb_host *h) {
    if(!h->policy_dirty)return ESP_OK;
    esp_err_t e=h->config.set_vbus(h->config.context,false);
    if(e==ESP_OK){h->policy_dirty=false;portENTER_CRITICAL(&h->lock);h->status.vbus_enabled=false;portEXIT_CRITICAL(&h->lock);}return e;
}
static bool stop_session(struct aether_usb_host *h) {
    const esp_err_t stop=backend_stop(h);
    /* Even a stalled cancel/uninstall must not leave boost powered. The USB
     * root port was disabled first; retain driver buffers/reservation to retry. */
    const esp_err_t off=power_off(h);
    const bool done=stop==ESP_OK&&off==ESP_OK;
    portENTER_CRITICAL(&h->lock);h->status.active=false;h->status.camera_profile=false;h->status.cleanup_pending=!done;
    h->status.state=done?AETHER_OTG_WAIT_SINK:AETHER_OTG_STOPPING;
    if(off!=ESP_OK)h->status.last_error=off;
    else if(stop!=ESP_OK&&stop!=ESP_ERR_NOT_FINISHED)h->status.last_error=stop;
    portEXIT_CRITICAL(&h->lock);
    if(done){esp_err_t e=gpio_set_level(AETHER_GPIO_USB_SELECT,AETHER_USB_DOWNLOAD_SELECT_LEVEL);
        if(e!=ESP_OK){snapshot_error(h,e);portENTER_CRITICAL(&h->lock);h->status.cleanup_pending=true;h->status.state=AETHER_OTG_STOPPING;portEXIT_CRITICAL(&h->lock);return false;}}
    return done;
}
static void service(struct aether_usb_host *h,int64_t now) {
    pump(h);reap(h);
    const bool wanted=atomic_load(&h->requested);
    portENTER_CRITICAL(&h->lock);
    const bool cleanup_pending=h->status.cleanup_pending;
    const bool camera_profile=h->requested_camera_profile;
    portEXIT_CRITICAL(&h->lock);
    const bool change_profile=h->installed&&camera_profile!=h->session_camera_profile;
    /* An idle default-OFF host must not touch the mux owned by MSC/USJ. */
    if(!wanted&&!h->reserved&&!h->installed&&!h->policy_dirty&&!cleanup_pending)return;
    if(!wanted||h->closing||h->bus_fault||cleanup_pending||change_profile){
        bool done=stop_session(h);
        if(done){h->sink_samples=0;if(wanted)h->retry_after=now+(change_profile?100000:2000000);}
        portENTER_CRITICAL(&h->lock);
        if(done&&atomic_load(&h->requested)){
            h->status.state=AETHER_OTG_WAIT_SINK;h->status.cleanup_pending=false;
        }else if(done){
            if(h->reserved&&aether_usb_bus_release(AETHER_USB_BUS_HOST)==ESP_OK)h->reserved=false;
            h->status.state=AETHER_OTG_OFF;h->status.cleanup_pending=h->reserved;
            if(!h->reserved)h->status.last_error=ESP_OK;
        }
        portEXIT_CRITICAL(&h->lock);return;
    }
    if(now<h->role_due)return;
    h->role_due=now+POLL_US;
    bool sink=false;h->policy_dirty=true;esp_err_t e=h->config.query_sink(h->config.context,&sink);
    if(e!=ESP_OK||!sink){h->sink_samples=0;
        if(h->installed||h->status.vbus_enabled){stop_session(h);h->retry_after=now+100000;}
        if(e!=ESP_OK)snapshot_error(h,e);
        return;
    }
    if(h->status.active){aggregate(h);return;}
    if(now<h->retry_after||++h->sink_samples<3)return;
    if(aether_usb_bus_get_owner()!=AETHER_USB_BUS_HOST){snapshot_error(h,ESP_ERR_INVALID_STATE);return;}
    portENTER_CRITICAL(&h->lock);h->status.state=AETHER_OTG_STARTING;portEXIT_CRITICAL(&h->lock);
    e=backend_start(h,camera_profile);
    if(e==ESP_OK){sink=false;e=h->config.query_sink(h->config.context,&sink);if(!sink&&e==ESP_OK)e=ESP_ERR_INVALID_STATE;}
    if(e==ESP_OK&&!atomic_load(&h->requested))e=ESP_ERR_INVALID_STATE;
    if(e==ESP_OK){h->policy_dirty=true;e=h->config.set_vbus(h->config.context,true);
        if(e==ESP_OK){portENTER_CRITICAL(&h->lock);h->status.vbus_enabled=true;portEXIT_CRITICAL(&h->lock);}}
    if(e==ESP_OK&&!atomic_load(&h->requested))e=ESP_ERR_INVALID_STATE;
    if(e==ESP_OK)e=usb_host_lib_set_root_port_power(true);
    if(e!=ESP_OK){snapshot_error(h,e);stop_session(h);h->retry_after=now+2000000;h->sink_samples=0;return;}
    portENTER_CRITICAL(&h->lock);h->status.active=true;h->status.state=AETHER_OTG_ACTIVE;
    h->status.camera_profile=camera_profile;
    h->status.last_error=ESP_OK;h->status.cleanup_pending=false;portEXIT_CRITICAL(&h->lock);
}
static void worker(void *ctx) {
    struct aether_usb_host *h=ctx;
    for(;;){
        service(h,esp_timer_get_time());
        TickType_t wait=h->installed?pdMS_TO_TICKS(2):pdMS_TO_TICKS(20);
        if(!atomic_load(&h->requested)&&!h->reserved&&!h->policy_dirty)wait=portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE,wait);
    }
}
esp_err_t aether_usb_host_new(const aether_usb_host_config_t *cfg,aether_usb_host_handle_t *out) {
    if(!cfg||!cfg->query_sink||!cfg->set_vbus||!out||cfg->class_count>CLASS_LIMIT||
       (cfg->class_count&&!cfg->classes)||(cfg->camera_class_mask>>cfg->class_count))return ESP_ERR_INVALID_ARG;
    for(unsigned n=0;n<cfg->class_count;++n)
        if(!cfg->classes[n].start||!cfg->classes[n].request_stop||!cfg->classes[n].poll_stopped)
            return ESP_ERR_INVALID_ARG;
    *out=NULL;
    struct aether_usb_host *h=heap_caps_calloc(1,sizeof(*h),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!h)return ESP_ERR_NO_MEM;
    /* Report models are owner-task-only. Keep their large bounded arrays in
     * PSRAM; the spinlock/atomics, state and callbacks stay in internal RAM. */
    h->hid=heap_caps_calloc(HID_SLOTS,sizeof(*h->hid),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!h->hid){heap_caps_free(h);return ESP_ERR_NO_MEM;}
    h->config=*cfg;h->lock=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;atomic_init(&h->requested,false);atomic_init(&h->reserved,false);
    h->status.state=AETHER_OTG_OFF;h->status.transfer_status=-1;h->status.last_interface=UINT8_MAX;
    if(xTaskCreate(worker,"usb_hid",6144,h,4,&h->task)!=pdPASS){heap_caps_free(h->hid);heap_caps_free(h);return ESP_ERR_NO_MEM;}
    *out=h;return ESP_OK;
}
esp_err_t aether_usb_host_set_enabled(aether_usb_host_handle_t h,bool enabled) {
    if(!h)return ESP_ERR_INVALID_ARG;
    esp_err_t e=ESP_OK;
    portENTER_CRITICAL(&h->lock);
    if(enabled&&!h->reserved){e=aether_usb_bus_claim(AETHER_USB_BUS_HOST);if(e==ESP_OK)h->reserved=true;}
    if(e==ESP_OK){atomic_store(&h->requested,enabled);h->status.enabled=enabled;
        if(enabled&&h->status.state==AETHER_OTG_OFF){h->status.state=AETHER_OTG_WAIT_SINK;h->status.last_error=ESP_OK;}}
    portEXIT_CRITICAL(&h->lock);if(e==ESP_OK)xTaskNotifyGive(h->task);return e;
}
esp_err_t aether_usb_host_set_camera_profile(aether_usb_host_handle_t h,bool enabled) {
    if(!h||(enabled&&!h->config.camera_class_mask))return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    h->requested_camera_profile=enabled;
    portEXIT_CRITICAL(&h->lock);
    xTaskNotifyGive(h->task);
    return ESP_OK;
}
esp_err_t aether_usb_host_get_status(aether_usb_host_handle_t h,aether_usb_host_status_t *s) {
    if(!h||!s)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);*s=h->status;portEXIT_CRITICAL(&h->lock);return ESP_OK;
}
esp_err_t aether_usb_host_disable_wait(aether_usb_host_handle_t h,uint32_t timeout_ms) {
    if(!h)return ESP_ERR_INVALID_ARG;
    aether_usb_host_set_enabled(h,false);TickType_t begin=xTaskGetTickCount();
    for(;;){aether_usb_host_status_t s;aether_usb_host_get_status(h,&s);
        if(s.state==AETHER_OTG_OFF&&!s.cleanup_pending&&!s.vbus_enabled)return ESP_OK;
        if(xTaskGetTickCount()-begin>=pdMS_TO_TICKS(timeout_ms))return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(2));}
}
