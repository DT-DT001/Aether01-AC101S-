#include "aether_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { CMD_ENABLE,CMD_SCAN,CMD_CONNECT,CMD_DISCONNECT,CMD_FORGET,CMD_EVENT,CMD_IP,CMD_LOST_IP } command_kind_t;
typedef enum {
    PEER_IDLE, PEER_REQUESTED, PEER_ACTIVE, PEER_FAILED, PEER_RELEASE_REQUESTED, PEER_RESTORING,
} peer_phase_t;
typedef struct {
    command_kind_t kind;
    uint32_t epoch;
    union {
        bool enabled;
        struct { aether_wifi_ap_t ap; char password[64]; bool save; } connect;
        struct { int32_t id; uint32_t scan_status; uint16_t reason; } event;
        uint32_t ip;
    } data;
} command_t;
typedef struct { uint8_t version,authmode; char ssid[33],password[64]; } credential_t;
typedef struct {
    bool enabled, known, pending, submitting;
    uint8_t failures;
    uint32_t revision;
    int64_t retry_at;
    esp_err_t error;
} enabled_preference_t;
struct aether_wifi {
    portMUX_TYPE lock;
    QueueHandle_t queue;
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_handler,ip_handler;
    bool wifi_registered,ip_registered;
    atomic_bool shutdown,done,event_overflow;
    atomic_uint event_epoch;
    bool owner_started,owner_starting;
    peer_phase_t peer_phase;
    esp_err_t peer_result;
    bool peer_restore, peer_restore_overridden;
    aether_wifi_status_t status;
    enabled_preference_t preference;
    aether_wifi_ap_t aps[AETHER_WIFI_MAX_APS];
    /* The fields below belong exclusively to the command-owner task. */
    bool initialized,started,desired,want_connect,switching,save_pending,ignore_disconnect,scan_pending;
    bool peer_saved_session, peer_saved_connect, peer_saved_pending;
    credential_t peer_credentials;
    wifi_config_t peer_config;
    credential_t credentials;
    credential_t pending_credentials;
    uint8_t persistence_kind,persistence_failures;
    bool forget_requested;
    bool (*persistence_busy)(void *);
    void *persistence_context;
    wifi_config_t config;
    int64_t retry_at,connect_deadline,scan_deadline;
    int64_t persistence_at;
};
static aether_wifi_handle_t default_handle;
static const char *TAG="aether_wifi";
static const char ENABLED_KEY[]="enabled_v1";
static void log_init_heap(const char *stage,esp_err_t result)
{
    ESP_LOGI(TAG,"init %s result=0x%x internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
        stage,(unsigned)result,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
}
static void erase_secret(void *data,size_t length)
{ volatile unsigned char *p=data; while(length--) *p++=0; }
static void copy_text(char *out,const char *text,size_t capacity)
{ size_t n=strnlen(text,capacity-1); memcpy(out,text,n); out[n]=0; }
static void publish_state(aether_wifi_handle_t h,aether_wifi_state_t state,esp_err_t error)
{ portENTER_CRITICAL(&h->lock); h->status.state=state; h->status.last_error=error; portEXIT_CRITICAL(&h->lock); }
static bool supported(uint8_t mode)
{ return mode==WIFI_AUTH_OPEN || mode==WIFI_AUTH_WPA_PSK || mode==WIFI_AUTH_WPA2_PSK || mode==WIFI_AUTH_WPA_WPA2_PSK || mode==WIFI_AUTH_WPA3_PSK || mode==WIFI_AUTH_WPA2_WPA3_PSK; }
const char *aether_wifi_security_name(uint8_t mode)
{
    switch(mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP (unsupported)";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_ENTERPRISE: return "Enterprise (unsupported)";
    default: return "Unsupported";
    }
}
static void event(void *context,esp_event_base_t base,int32_t id,void *data)
{
    aether_wifi_handle_t h=context; command_t message={.epoch=atomic_load(&h->event_epoch)};
    if(atomic_load(&h->shutdown)) return;
    if(base==WIFI_EVENT) {
        if(id!=WIFI_EVENT_STA_START && id!=WIFI_EVENT_STA_DISCONNECTED && id!=WIFI_EVENT_SCAN_DONE) return;
        message.kind=CMD_EVENT; message.data.event.id=id;
        if(id==WIFI_EVENT_STA_DISCONNECTED && data) message.data.event.reason=((wifi_event_sta_disconnected_t *)data)->reason;
        if(id==WIFI_EVENT_SCAN_DONE && data) message.data.event.scan_status=((wifi_event_sta_scan_done_t *)data)->status;
    } else if(id==IP_EVENT_STA_GOT_IP && data) {
        ip_event_got_ip_t *ip=data; if(ip->esp_netif!=h->netif) return;
        message.kind=CMD_IP; message.data.ip=ip->ip_info.ip.addr;
    } else if(id==IP_EVENT_STA_LOST_IP) message.kind=CMD_LOST_IP;
    else return;
    if(xQueueSend(h->queue,&message,0)!=pdTRUE) atomic_store(&h->event_overflow,true);
}
static bool load_credentials(aether_wifi_handle_t h)
{
    if(h->forget_requested) return false;
    if(h->persistence_kind==1) { h->credentials=h->pending_credentials; return true; }
    nvs_handle_t nvs; esp_err_t error=nvs_open("aether_wifi",NVS_READONLY,&nvs);
    credential_t saved={0}; size_t bytes=sizeof(saved);
    if(error==ESP_OK) { error=nvs_get_blob(nvs,"cred",&saved,&bytes); nvs_close(nvs); }
    bool valid=error==ESP_OK && bytes==sizeof(saved) && saved.version==1 &&
        memchr(saved.ssid,0,sizeof(saved.ssid)) && saved.ssid[0] && memchr(saved.password,0,sizeof(saved.password)) && supported(saved.authmode);
    portENTER_CRITICAL(&h->lock);
    h->status.saved_network=valid;
    h->status.storage_error=error==ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error!=ESP_OK ? error : valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    portEXIT_CRITICAL(&h->lock);
    if(valid) h->credentials=saved;
    erase_secret(&saved,sizeof(saved)); return valid;
}

static void load_enabled_preference(aether_wifi_handle_t h)
{
    nvs_handle_t nvs;
    esp_err_t error=nvs_open("aether_wifi",NVS_READONLY,&nvs);
    uint8_t value=0;
    if(error==ESP_OK) {
        error=nvs_get_u8(nvs,ENABLED_KEY,&value);
        nvs_close(nvs);
    }
    if(error==ESP_ERR_NVS_NOT_FOUND) { error=ESP_OK; value=0; }
    else if(error==ESP_OK && value>1) error=ESP_ERR_INVALID_RESPONSE;
    h->preference=(enabled_preference_t){
        .enabled=error==ESP_OK && value==1, .known=error==ESP_OK, .error=error,
    };
}

static bool flash_write_blocked(aether_wifi_handle_t h)
{
    portENTER_CRITICAL(&h->lock);
    bool (*busy)(void *)=h->persistence_busy;
    void *context=h->persistence_context;
    portEXIT_CRITICAL(&h->lock);
    return busy && busy(context);
}

static void persist_enabled_preference(aether_wifi_handle_t h,int64_t now)
{
    portENTER_CRITICAL(&h->lock);
    const enabled_preference_t request=h->preference;
    portEXIT_CRITICAL(&h->lock);
    if(!request.pending || now<request.retry_at) return;

    if(flash_write_blocked(h)) {
        portENTER_CRITICAL(&h->lock);
        if(request.revision==h->preference.revision) h->preference.retry_at=now+1000000;
        portEXIT_CRITICAL(&h->lock);
        return;
    }

    nvs_handle_t nvs;
    esp_err_t error=nvs_open("aether_wifi",NVS_READWRITE,&nvs);
    if(error==ESP_OK) {
        error=nvs_set_u8(nvs,ENABLED_KEY,request.enabled);
        if(error==ESP_OK) error=nvs_commit(nvs);
        nvs_close(nvs);
    }
    portENTER_CRITICAL(&h->lock);
    if(request.revision==h->preference.revision) {
        h->preference.error=error;
        if(error==ESP_OK) {
            h->preference.known=true;
            h->preference.pending=false;
        } else {
            if(h->preference.failures<5) ++h->preference.failures;
            h->preference.retry_at=now+(INT64_C(1000000)<<(h->preference.failures-1));
        }
    }
    portEXIT_CRITICAL(&h->lock);
}

static void defer_credentials(aether_wifi_handle_t h,bool erase)
{
    erase_secret(&h->pending_credentials,sizeof(h->pending_credentials));
    if(!erase) h->pending_credentials=h->credentials;
    h->persistence_kind=erase ? 2 : 1; h->persistence_at=0; h->persistence_failures=0;
    h->forget_requested=erase;
    portENTER_CRITICAL(&h->lock); h->status.persistence_pending=true;
    if(erase) h->status.saved_network=false;
    portEXIT_CRITICAL(&h->lock);
    h->save_pending=false;
}
static void persist_credentials(aether_wifi_handle_t h,int64_t now)
{
    if(!h->persistence_kind || now<h->persistence_at) return;
    if(flash_write_blocked(h)) { h->persistence_at=now+1000000; return; }
    nvs_handle_t nvs; esp_err_t error=nvs_open("aether_wifi",NVS_READWRITE,&nvs);
    if(error==ESP_OK) {
        error=h->persistence_kind==2 ? nvs_erase_key(nvs,"cred") : nvs_set_blob(nvs,"cred",&h->pending_credentials,sizeof(h->pending_credentials));
        if(error==ESP_ERR_NVS_NOT_FOUND && h->persistence_kind==2) error=ESP_OK;
        if(error==ESP_OK) error=nvs_commit(nvs);
        nvs_close(nvs);
    }
    portENTER_CRITICAL(&h->lock); h->status.storage_error=error;
    if(error==ESP_OK) { h->status.saved_network=h->persistence_kind==1; h->status.persistence_pending=false; }
    portEXIT_CRITICAL(&h->lock);
    if(error==ESP_OK) { h->persistence_kind=0; h->forget_requested=false; erase_secret(&h->pending_credentials,sizeof(h->pending_credentials)); }
    else {
        if(h->persistence_failures<5) ++h->persistence_failures;
        uint32_t seconds=1U<<(h->persistence_failures-1); if(seconds>30) seconds=30;
        h->persistence_at=now+(int64_t)seconds*1000000;
    }
}
static void configure(aether_wifi_handle_t h,const aether_wifi_ap_t *ap)
{
    erase_secret(&h->config,sizeof(h->config));
    memcpy(h->config.sta.ssid,h->credentials.ssid,strnlen(h->credentials.ssid,32));
    memcpy(h->config.sta.password,h->credentials.password,strnlen(h->credentials.password,63));
    uint8_t auth=h->credentials.authmode;
    h->config.sta.threshold.authmode=auth==WIFI_AUTH_WPA2_WPA3_PSK ? WIFI_AUTH_WPA2_PSK :
        auth==WIFI_AUTH_WPA_WPA2_PSK ? WIFI_AUTH_WPA2_PSK : (wifi_auth_mode_t)auth;
    h->config.sta.pmf_cfg.capable=true; h->config.sta.pmf_cfg.required=auth==WIFI_AUTH_WPA3_PSK;
    h->config.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;
    if(ap) { h->config.sta.bssid_set=true; memcpy(h->config.sta.bssid,ap->bssid,6); h->config.sta.channel=ap->channel; }
}
static void begin_connect(aether_wifi_handle_t h)
{
    if(!h->desired || !h->started || !h->want_connect) return;
    h->retry_at=0;
    esp_err_t error=esp_wifi_set_config(WIFI_IF_STA,&h->config);
    if(error==ESP_OK) error=esp_wifi_connect();
    h->connect_deadline=error==ESP_OK ? esp_timer_get_time()+20000000 : 0;
    if(error!=ESP_OK) h->want_connect=false;
    portENTER_CRITICAL(&h->lock);
    h->status.connected=false; h->status.ip[0]=0; h->status.retry_delay_ms=0;
    copy_text(h->status.ssid,h->credentials.ssid,sizeof(h->status.ssid));
    h->status.state=error==ESP_OK ? AETHER_WIFI_CONNECTING : AETHER_WIFI_ERROR; h->status.last_error=error;
    portEXIT_CRITICAL(&h->lock);
}
static esp_err_t stop_driver(aether_wifi_handle_t h)
{
    atomic_fetch_add(&h->event_epoch,1);
    h->desired=h->want_connect=h->switching=h->scan_pending=false;
    h->retry_at=h->connect_deadline=h->scan_deadline=0;
    h->ignore_disconnect=false;
    esp_err_t error=ESP_OK;
    if(h->initialized) {
        esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); esp_wifi_disconnect();
        if(h->started) { error=esp_wifi_stop(); if(error==ESP_OK) h->started=false; }
        /* A failed stop still owns the live driver. Do not pretend teardown
         * completed, or attempt another init over these resources. */
        if(!h->started) {
            esp_err_t deinit=esp_wifi_deinit(); if(deinit==ESP_OK) h->initialized=false;
            if(error==ESP_OK) error=deinit;
        }
    }
    erase_secret(&h->config,sizeof(h->config)); erase_secret(&h->credentials,sizeof(h->credentials));
    portENTER_CRITICAL(&h->lock);
    h->status.enabled=h->status.connected=h->status.scanning=false;
    h->status.ip[0]=h->status.ssid[0]=0; h->status.retry_count=0; h->status.retry_delay_ms=0;
    h->status.state=error==ESP_OK ? AETHER_WIFI_OFF : AETHER_WIFI_ERROR; h->status.last_error=error;
    portEXIT_CRITICAL(&h->lock);
    return error;
}
static esp_err_t start_station(aether_wifi_handle_t h)
{
    if(h->initialized) stop_driver(h);
    if(h->initialized) return ESP_ERR_INVALID_STATE;
    atomic_fetch_add(&h->event_epoch,1);
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    /* Keep the driver's PSRAM-preferred allocator and feature flags, but do
     * not reserve the default 16 static TX buffers in scarce internal DMA
     * RAM. Runtime fields are documented by this installed IDF 6.1 API. */
    init.static_rx_buf_num=4; init.rx_ba_win=4;
    init.tx_buf_type=0; init.static_tx_buf_num=4;
    init.dynamic_rx_buf_num=12; init.cache_tx_buf_num=8;
    init.rx_mgmt_buf_num=5; init.mgmt_sbuf_num=6;
    ESP_LOGI(TAG,"init config rx=%d ba=%d static_tx=%d dynamic_rx=%d cache_tx=%d mgmt_rx=%d mgmt_short=%d",
        init.static_rx_buf_num,init.rx_ba_win,init.static_tx_buf_num,init.dynamic_rx_buf_num,
        init.cache_tx_buf_num,init.rx_mgmt_buf_num,init.mgmt_sbuf_num);
    log_init_heap("before",ESP_OK);
    esp_err_t error=esp_wifi_init(&init);
    log_init_heap("after",error);
    if(error==ESP_OK) { h->initialized=true; error=esp_wifi_set_storage(WIFI_STORAGE_RAM); }
    if(error==ESP_OK) error=esp_wifi_set_mode(WIFI_MODE_STA);
    if(error==ESP_OK) error=esp_wifi_start();
    if(error!=ESP_OK) stop_driver(h);
    else h->started=true;
    return error;
}
static void start_driver_with_session(aether_wifi_handle_t h, bool restore_session)
{
    if(h->desired && h->initialized) return;
    esp_err_t error=start_station(h);
    if(error!=ESP_OK) { publish_state(h,AETHER_WIFI_ERROR,error); return; }
    h->desired=true; publish_state(h,AETHER_WIFI_STARTING,ESP_OK);
    if(restore_session) {
        h->want_connect=h->peer_saved_connect && !h->forget_requested;
        h->save_pending=h->want_connect && h->peer_saved_pending;
        if(h->want_connect) {
            h->credentials=h->peer_credentials;
            h->config=h->peer_config;
        }
    } else {
        h->want_connect=load_credentials(h); h->save_pending=false;
        if(h->want_connect) configure(h,NULL);
    }
    portENTER_CRITICAL(&h->lock); h->status.enabled=true; h->status.retry_count=0; portEXIT_CRITICAL(&h->lock);
    /* Association begins only on the driver's STA_START event. */
}
static void start_driver(aether_wifi_handle_t h)
{
    start_driver_with_session(h,false);
}
static peer_phase_t peer_phase(aether_wifi_handle_t h)
{
    portENTER_CRITICAL(&h->lock);
    peer_phase_t phase=h->peer_phase;
    portEXIT_CRITICAL(&h->lock);
    return phase;
}
static void peer_radio_tick(aether_wifi_handle_t h)
{
    peer_phase_t phase=peer_phase(h);
    if(phase==PEER_REQUESTED) {
        portENTER_CRITICAL(&h->lock);
        if(!h->peer_restore_overridden) h->peer_restore=h->desired;
        portEXIT_CRITICAL(&h->lock);
        h->peer_saved_session=h->desired && h->started;
        h->peer_saved_connect=h->want_connect;
        h->peer_saved_pending=h->save_pending;
        if(h->peer_saved_session && h->peer_saved_connect) {
            h->peer_credentials=h->credentials;
            h->peer_config=h->config;
        }
        esp_err_t error=stop_driver(h);
        if(error==ESP_OK) error=start_station(h);
        portENTER_CRITICAL(&h->lock);
        if(h->peer_phase==PEER_REQUESTED) {
            h->peer_result=error;
            h->peer_phase=error==ESP_OK ? PEER_ACTIVE : PEER_FAILED;
        }
        portEXIT_CRITICAL(&h->lock);
    } else if(phase==PEER_RELEASE_REQUESTED) {
        esp_err_t error=stop_driver(h);
        portENTER_CRITICAL(&h->lock);
        h->peer_result=error;
        if(error==ESP_OK) h->peer_phase=PEER_RESTORING;
        portEXIT_CRITICAL(&h->lock);
        if(error!=ESP_OK) return;
        phase=PEER_RESTORING;
    }
    if(phase==PEER_RESTORING) {
        portENTER_CRITICAL(&h->lock);
        bool restore=h->peer_restore;
        portEXIT_CRITICAL(&h->lock);
        if(restore && !h->desired) start_driver_with_session(h,h->peer_saved_session);
        if(!restore && h->desired) stop_driver(h);
        portENTER_CRITICAL(&h->lock);
        /* A preference change during start/stop stays pending for the next
         * owner iteration. Publish idle when callers can use the station. */
        if(restore==h->peer_restore) {
            h->peer_restore=false;
            h->peer_restore_overridden=false;
            h->peer_phase=PEER_IDLE;
        }
        bool restored=h->peer_phase==PEER_IDLE;
        portEXIT_CRITICAL(&h->lock);
        if(restored) {
            h->peer_saved_session=h->peer_saved_connect=h->peer_saved_pending=false;
            erase_secret(&h->peer_credentials,sizeof(h->peer_credentials));
            erase_secret(&h->peer_config,sizeof(h->peer_config));
        }
    }
}
static void retry(aether_wifi_handle_t h,uint16_t reason)
{
    h->connect_deadline=0;
    portENTER_CRITICAL(&h->lock);
    h->status.connected=false; h->status.ip[0]=0; h->status.disconnect_reason=reason;
    if(!h->desired || !h->want_connect) h->status.state=h->desired ? AETHER_WIFI_IDLE : AETHER_WIFI_OFF;
    else if(h->status.retry_count<5) {
        uint32_t delay_ms=1000U<<h->status.retry_count++;
        h->retry_at=esp_timer_get_time()+(int64_t)delay_ms*1000;
        h->status.state=AETHER_WIFI_CONNECTING; h->status.last_error=ESP_FAIL; h->status.retry_delay_ms=delay_ms;
    } else { h->want_connect=false; h->retry_at=0; h->status.state=AETHER_WIFI_ERROR; h->status.last_error=ESP_FAIL; }
    portEXIT_CRITICAL(&h->lock);
}
static void finish_scan(aether_wifi_handle_t h,uint32_t result)
{
    if(!h->started) return;
    aether_wifi_status_t status; aether_wifi_get_status(h,&status);
    if(!status.scanning) { esp_wifi_clear_ap_list(); return; }
    wifi_ap_record_t *records=calloc(AETHER_WIFI_MAX_APS,sizeof(*records));
    esp_err_t error=records ? ESP_OK : ESP_ERR_NO_MEM;
    uint16_t count=AETHER_WIFI_MAX_APS;
    if(error==ESP_OK && !result) error=esp_wifi_scan_get_ap_records(&count,records);
    else { esp_wifi_clear_ap_list(); if(error==ESP_OK) error=ESP_FAIL; }
    if(error!=ESP_OK) esp_wifi_clear_ap_list();
    if(count>AETHER_WIFI_MAX_APS) count=AETHER_WIFI_MAX_APS;
    h->scan_deadline=0;
    portENTER_CRITICAL(&h->lock);
    h->status.scanning=false; h->status.state=h->status.connected ? AETHER_WIFI_CONNECTED : AETHER_WIFI_IDLE; h->status.last_error=error;
    if(error==ESP_OK) {
        h->status.ap_count=(uint8_t)count; ++h->status.scan_generation;
        for(unsigned i=0;i<count;++i) {
            aether_wifi_ap_t *ap=&h->aps[i]; memset(ap,0,sizeof(*ap));
            memcpy(ap->ssid,records[i].ssid,32); ap->ssid[32]=0;
            memcpy(ap->bssid,records[i].bssid,6); ap->channel=records[i].primary; ap->rssi=records[i].rssi;
            ap->authmode=(uint8_t)records[i].authmode; ap->password_required=ap->authmode!=WIFI_AUTH_OPEN;
            ap->supported=supported(ap->authmode) && ap->ssid[0];
        }
    }
    portEXIT_CRITICAL(&h->lock); free(records);
}
static void cancel_scan(aether_wifi_handle_t h)
{
    h->scan_pending=false;
    if(h->scan_deadline) { esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); h->scan_deadline=0; }
    portENTER_CRITICAL(&h->lock); h->status.scanning=false; portEXIT_CRITICAL(&h->lock);
}
static void begin_scan(aether_wifi_handle_t h)
{
    const bool detached=h->scan_pending;
    h->scan_pending=false;
    if(!h->desired || !h->started) return;
    wifi_scan_config_t scan={.show_hidden=false,.scan_type=WIFI_SCAN_TYPE_ACTIVE};
    const esp_err_t error=esp_wifi_scan_start(&scan,false);
    h->scan_deadline=error==ESP_OK ? esp_timer_get_time()+15000000 : 0;
    portENTER_CRITICAL(&h->lock);
    if(detached) { h->status.connected=false; h->status.ip[0]=0; }
    h->status.scanning=error==ESP_OK; h->status.last_error=error;
    h->status.state=error==ESP_OK ? AETHER_WIFI_SCANNING :
        h->status.connected ? AETHER_WIFI_CONNECTED : AETHER_WIFI_IDLE;
    portEXIT_CRITICAL(&h->lock);
}
static void cancel_connection(aether_wifi_handle_t h)
{
    /* A second queued selection may replace the desired AP, but must not
     * overlap another SDK cancel/connect or extend its existing deadline. */
    if(h->switching) return;
    h->retry_at=0; h->switching=true; h->connect_deadline=esp_timer_get_time()+2000000;
    esp_err_t error=esp_wifi_disconnect();
    if(error==ESP_OK) { publish_state(h,AETHER_WIFI_DISCONNECTING,ESP_OK); return; }
    h->switching=false; h->connect_deadline=0;
    if(error==ESP_ERR_WIFI_NOT_CONNECT) {
        if(h->scan_pending) begin_scan(h);
        else if(h->want_connect) begin_connect(h); else publish_state(h,AETHER_WIFI_IDLE,ESP_OK);
    } else {
        /* Rejected cancellation does not establish ownership of a new link. */
        stop_driver(h); publish_state(h,AETHER_WIFI_ERROR,error);
    }
}
static void process(aether_wifi_handle_t h,const command_t *c)
{
    peer_phase_t phase=peer_phase(h);
    bool event=c->kind==CMD_EVENT || c->kind==CMD_IP || c->kind==CMD_LOST_IP;
    if(phase!=PEER_IDLE && !(phase==PEER_RESTORING && event)) {
        /* A station command can already be queued when the lease is requested. */
        if(c->kind==CMD_ENABLE) {
            portENTER_CRITICAL(&h->lock);
            if(!h->peer_restore_overridden) h->peer_restore=c->data.enabled;
            portEXIT_CRITICAL(&h->lock);
        } else if(c->kind==CMD_FORGET) {
            defer_credentials(h,true);
            h->peer_saved_connect=h->peer_saved_pending=false;
            erase_secret(&h->peer_credentials,sizeof(h->peer_credentials));
            erase_secret(&h->peer_config,sizeof(h->peer_config));
        }
        return;
    }
    if((c->kind==CMD_EVENT || c->kind==CMD_IP || c->kind==CMD_LOST_IP) &&
       c->epoch!=atomic_load(&h->event_epoch)) return;
    if(c->kind==CMD_ENABLE) { if(c->data.enabled) start_driver(h); else stop_driver(h); return; }
    if(c->kind==CMD_FORGET) {
        defer_credentials(h,true);
        h->want_connect=h->save_pending=false; h->retry_at=0;
        cancel_scan(h);
        if(h->started) cancel_connection(h);
        erase_secret(&h->config,sizeof(h->config)); erase_secret(&h->credentials,sizeof(h->credentials));
        portENTER_CRITICAL(&h->lock);
        h->status.connected=false; h->status.ip[0]=h->status.ssid[0]=0;
        h->status.retry_count=0; h->status.retry_delay_ms=0;
        if(!h->initialized && h->status.state!=AETHER_WIFI_ERROR) h->status.state=AETHER_WIFI_OFF;
        portEXIT_CRITICAL(&h->lock); return;
    }
    if(!h->started || !h->desired) return;
    if(c->kind==CMD_SCAN) {
        aether_wifi_status_t s; aether_wifi_get_status(h,&s);
        if(s.scanning || h->scan_pending) return; /* Coalesce one explicit scan. */
        if(h->switching || (!s.connected && (h->want_connect || h->connect_deadline || h->retry_at ||
                                           s.state==AETHER_WIFI_CONNECTING))) {
            /* Scanning has priority over an offline automatic association.
             * Keep remembered credentials, but suppress retry/late DHCP until
             * the user explicitly connects or starts a new radio session. */
            h->scan_pending=true; h->want_connect=h->save_pending=false; h->retry_at=0;
            portENTER_CRITICAL(&h->lock); h->status.retry_count=0; h->status.retry_delay_ms=0; portEXIT_CRITICAL(&h->lock);
            const bool canceled_link_pending=h->ignore_disconnect;
            h->ignore_disconnect=false;
            if(canceled_link_pending && !h->switching) {
                /* A timeout/LOST_IP cancellation is already in flight. Join
                 * its completion rather than issuing a second disconnect. */
                h->switching=true;
                h->connect_deadline=esp_timer_get_time()+2000000;
                publish_state(h,AETHER_WIFI_DISCONNECTING,ESP_OK);
            } else cancel_connection(h);
        } else begin_scan(h);
    } else if(c->kind==CMD_CONNECT) {
        aether_wifi_status_t s; aether_wifi_get_status(h,&s);
        cancel_scan(h);
        h->credentials=(credential_t){.version=1,.authmode=c->data.connect.ap.authmode};
        copy_text(h->credentials.ssid,c->data.connect.ap.ssid,sizeof(h->credentials.ssid));
        copy_text(h->credentials.password,c->data.connect.password,sizeof(h->credentials.password));
        configure(h,&c->data.connect.ap); h->want_connect=true; h->save_pending=c->data.connect.save; h->retry_at=0;
        portENTER_CRITICAL(&h->lock); h->status.retry_count=0; h->status.scanning=false; h->status.disconnect_reason=0; portEXIT_CRITICAL(&h->lock);
        if(s.connected || h->connect_deadline || h->switching) cancel_connection(h);
        else begin_connect(h);
    } else if(c->kind==CMD_DISCONNECT) {
        h->want_connect=h->save_pending=false; h->retry_at=0;
        cancel_scan(h); cancel_connection(h);
        portENTER_CRITICAL(&h->lock); h->status.connected=false; h->status.ip[0]=0; h->status.retry_count=0; h->status.retry_delay_ms=0; portEXIT_CRITICAL(&h->lock);
    } else if(c->kind==CMD_EVENT) {
        if(c->data.event.id==WIFI_EVENT_STA_START) {
            aether_wifi_status_t s; aether_wifi_get_status(h,&s);
            if(s.state!=AETHER_WIFI_STARTING) return;
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            if(h->want_connect) begin_connect(h); else publish_state(h,AETHER_WIFI_IDLE,ESP_OK);
        } else if(c->data.event.id==WIFI_EVENT_SCAN_DONE) finish_scan(h,c->data.event.scan_status);
        else if(c->data.event.id==WIFI_EVENT_STA_DISCONNECTED) {
            if(h->ignore_disconnect) { cancel_scan(h); h->ignore_disconnect=false; return; }
            else if(h->switching) { h->switching=false; h->connect_deadline=0;
                if(h->scan_pending) begin_scan(h);
                else if(h->want_connect) begin_connect(h); else retry(h,c->data.event.reason); }
            else {
                aether_wifi_status_t s; aether_wifi_get_status(h,&s);
                /* No association belongs to an offline explicit scan. A late
                 * canceled-link event cannot abort it or revive auto-retry. */
                if(h->scan_deadline && !h->want_connect && !s.connected) return;
                cancel_scan(h); retry(h,c->data.event.reason);
            }
        }
    } else if(c->kind==CMD_IP) {
        if(!h->want_connect || h->switching) return;
        wifi_ap_record_t ap={0}; esp_err_t info_error=esp_wifi_sta_get_ap_info(&ap);
        if(info_error!=ESP_OK || !c->data.ip ||
           memcmp(ap.ssid,h->config.sta.ssid,sizeof(h->config.sta.ssid)) ||
           (h->config.sta.bssid_set && memcmp(ap.bssid,h->config.sta.bssid,6))) return;
        h->retry_at=h->connect_deadline=0;
        esp_ip4_addr_t address={.addr=c->data.ip};
        portENTER_CRITICAL(&h->lock); h->status.connected=true; h->status.state=AETHER_WIFI_CONNECTED; h->status.last_error=ESP_OK;
        h->status.retry_count=0; h->status.retry_delay_ms=0; h->status.disconnect_reason=0;
        snprintf(h->status.ip,sizeof(h->status.ip),IPSTR,IP2STR(&address)); if(info_error==ESP_OK) h->status.rssi=ap.rssi;
        portEXIT_CRITICAL(&h->lock); if(h->save_pending) defer_credentials(h,false);
    } else if(c->kind==CMD_LOST_IP) {
        aether_wifi_status_t s; aether_wifi_get_status(h,&s);
        if(h->switching || !h->want_connect || !s.connected) return;
        h->ignore_disconnect=esp_wifi_disconnect()==ESP_OK; retry(h,0);
    }
}
static void worker(void *context)
{
    aether_wifi_handle_t h=context;
    while(!atomic_load(&h->shutdown)) {
        peer_radio_tick(h);
        command_t c={0};
        if(xQueueReceive(h->queue,&c,pdMS_TO_TICKS(100))==pdTRUE) process(h,&c);
        erase_secret(&c,sizeof(c));
        if(peer_phase(h)!=PEER_IDLE) {
            /* Late AP events do not own this station while the lease is active. */
            atomic_store(&h->event_overflow,false);
            continue;
        }
        if(atomic_exchange(&h->event_overflow,false)) { stop_driver(h); publish_state(h,AETHER_WIFI_ERROR,ESP_ERR_NO_MEM); }
        int64_t now=esp_timer_get_time();
        if(h->scan_deadline && now>=h->scan_deadline) { esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); h->scan_deadline=0;
            portENTER_CRITICAL(&h->lock); h->status.scanning=false; h->status.state=h->status.connected ? AETHER_WIFI_CONNECTED : AETHER_WIFI_IDLE; h->status.last_error=ESP_ERR_TIMEOUT; portEXIT_CRITICAL(&h->lock); }
        if(h->connect_deadline && now>=h->connect_deadline) {
            h->connect_deadline=0;
            if(h->switching) { stop_driver(h); publish_state(h,AETHER_WIFI_ERROR,ESP_ERR_TIMEOUT); }
            else { h->ignore_disconnect=esp_wifi_disconnect()==ESP_OK; retry(h,0); }
        }
        if(h->retry_at && now>=h->retry_at) begin_connect(h);
        persist_enabled_preference(h,now);
        persist_credentials(h,now);
    }
    stop_driver(h); command_t pending;
    while(xQueueReceive(h->queue,&pending,0)==pdTRUE) erase_secret(&pending,sizeof(pending));
    atomic_store(&h->done,true); vTaskDelete(NULL);
}
esp_err_t aether_wifi_new(aether_wifi_handle_t *out)
{
    if(!out) return ESP_ERR_INVALID_ARG;
    *out=NULL;
    if(default_handle) return ESP_ERR_INVALID_STATE;
    aether_wifi_handle_t h=calloc(1,sizeof(*h)); if(!h) return ESP_ERR_NO_MEM;
    h->lock=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    atomic_init(&h->shutdown,false); atomic_init(&h->done,false); atomic_init(&h->event_overflow,false);
    atomic_init(&h->event_epoch,0);
    esp_err_t error=esp_netif_init(); if(error!=ESP_OK && error!=ESP_ERR_INVALID_STATE) goto fail;
    error=esp_event_loop_create_default(); if(error!=ESP_OK && error!=ESP_ERR_INVALID_STATE) goto fail;
    h->netif=esp_netif_create_default_wifi_sta(); if(!h->netif) { error=ESP_ERR_NO_MEM; goto fail; }
    h->queue=xQueueCreate(32,sizeof(command_t)); if(!h->queue) { error=ESP_ERR_NO_MEM; goto fail; }
    error=esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,h,&h->wifi_handler); if(error!=ESP_OK) goto fail; h->wifi_registered=true;
    error=esp_event_handler_instance_register(IP_EVENT,ESP_EVENT_ANY_ID,event,h,&h->ip_handler); if(error!=ESP_OK) goto fail; h->ip_registered=true;
    /* Remembered-network availability is readable while the radio and its
     * 8KiB owner task are still OFF. No NVS write occurs here. */
    load_credentials(h); erase_secret(&h->credentials,sizeof(h->credentials));
    load_enabled_preference(h);
    default_handle=h; *out=h; return ESP_OK;
fail:
    if(h->ip_registered) esp_event_handler_instance_unregister(IP_EVENT,ESP_EVENT_ANY_ID,h->ip_handler);
    if(h->wifi_registered) esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,h->wifi_handler);
    if(h->queue) vQueueDelete(h->queue);
    if(h->netif) esp_netif_destroy_default_wifi(h->netif);
    free(h); return error;
}
aether_wifi_handle_t aether_wifi_get_default(void) { return default_handle; }
esp_err_t aether_wifi_set_persistence_guard(aether_wifi_handle_t h,bool (*busy)(void *),void *context)
{ if(!h) return ESP_ERR_INVALID_ARG; portENTER_CRITICAL(&h->lock); h->persistence_busy=busy; h->persistence_context=context; portEXIT_CRITICAL(&h->lock); return ESP_OK; }
static esp_err_t enqueue(aether_wifi_handle_t h,command_t *c)
{
    if(!h || atomic_load(&h->shutdown)) return ESP_ERR_INVALID_STATE;
    return xQueueSend(h->queue,c,0)==pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_err_t ensure_owner(aether_wifi_handle_t h)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    if(atomic_load(&h->shutdown)) { portEXIT_CRITICAL(&h->lock); return ESP_ERR_INVALID_STATE; }
    if(h->owner_started) { portEXIT_CRITICAL(&h->lock); return ESP_OK; }
    if(h->owner_starting) { portEXIT_CRITICAL(&h->lock); return ESP_ERR_INVALID_STATE; }
    h->owner_starting=true;
    portEXIT_CRITICAL(&h->lock);
    /* FreeRTOS allocation/scheduling must run outside the critical section.
     * Concurrent first requests can retry, but never create a second owner. */
    bool created=xTaskCreate(worker,"wifi_owner",8192,h,4,NULL)==pdPASS;
    portENTER_CRITICAL(&h->lock);
    h->owner_started=created; h->owner_starting=false;
    if(!created) { h->status.state=AETHER_WIFI_ERROR; h->status.last_error=ESP_ERR_NO_MEM; }
    portEXIT_CRITICAL(&h->lock);
    return created ? ESP_OK : ESP_ERR_NO_MEM;
}
static bool enabled(aether_wifi_handle_t h)
{
    if(!h || atomic_load(&h->shutdown)) return false;
    portENTER_CRITICAL(&h->lock);
    bool on=h->owner_started && h->status.enabled && h->peer_phase==PEER_IDLE;
    portEXIT_CRITICAL(&h->lock);
    return on;
}
esp_err_t aether_wifi_set_enabled(aether_wifi_handle_t h,bool enabled)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    if(h->peer_phase!=PEER_IDLE) {
        h->peer_restore=enabled;
        h->peer_restore_overridden=true;
        portEXIT_CRITICAL(&h->lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&h->lock);
    if(enabled) { esp_err_t error=ensure_owner(h); if(error!=ESP_OK) return error; }
    else {
        portENTER_CRITICAL(&h->lock);
        bool started=h->owner_started,starting=h->owner_starting;
        portEXIT_CRITICAL(&h->lock);
        if(starting || atomic_load(&h->shutdown)) return ESP_ERR_INVALID_STATE;
        if(!started) { publish_state(h,AETHER_WIFI_OFF,ESP_OK); return ESP_OK; }
    }
    command_t c={.kind=CMD_ENABLE,.data.enabled=enabled}; return enqueue(h,&c);
}

esp_err_t aether_wifi_restore_preference(aether_wifi_handle_t h)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    const bool enabled=h->preference.enabled;
    const bool known=h->preference.known;
    const esp_err_t error=h->preference.error;
    portEXIT_CRITICAL(&h->lock);
    if(!known) return error;
    return enabled ? aether_wifi_set_enabled(h,true) : ESP_OK;
}

esp_err_t aether_wifi_set_enabled_preference(aether_wifi_handle_t h,bool enabled)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    if(h->preference.submitting || atomic_load(&h->shutdown)) {
        portEXIT_CRITICAL(&h->lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool save=!h->preference.known || h->preference.pending || h->preference.enabled!=enabled;
    h->preference.submitting=true;
    portEXIT_CRITICAL(&h->lock);

    /* Reuse the session API so ESP-NOW owns the radio until lease release. */
    esp_err_t error=save && !enabled ? ensure_owner(h) : ESP_OK;
    if(error==ESP_OK) error=aether_wifi_set_enabled(h,enabled);

    portENTER_CRITICAL(&h->lock);
    if(error==ESP_OK && save) {
        h->preference.enabled=enabled;
        h->preference.pending=true;
        ++h->preference.revision;
        h->preference.retry_at=0;
        h->preference.failures=0;
        h->preference.error=ESP_OK;
    }
    h->preference.submitting=false;
    portEXIT_CRITICAL(&h->lock);
    return error;
}

esp_err_t aether_wifi_scan(aether_wifi_handle_t h)
{ if(!enabled(h)) return ESP_ERR_INVALID_STATE; command_t c={.kind=CMD_SCAN}; return enqueue(h,&c); }
esp_err_t aether_wifi_connect(aether_wifi_handle_t h,const aether_wifi_ap_t *ap,const char *password,bool save)
{
    if(!ap || !password || !ap->ssid[0] || !memchr(ap->ssid,0,sizeof(ap->ssid)) || !supported(ap->authmode)) return ESP_ERR_INVALID_ARG;
    if(!enabled(h)) return ESP_ERR_INVALID_STATE;
    size_t length=strnlen(password,64); unsigned minimum=ap->authmode==WIFI_AUTH_WPA3_PSK ? 1 : 8;
    if(length>63 || (ap->authmode==WIFI_AUTH_OPEN ? length!=0 : length<minimum)) return ESP_ERR_INVALID_ARG;
    command_t c={.kind=CMD_CONNECT}; c.data.connect.ap=*ap; c.data.connect.save=save;
    memcpy(c.data.connect.password,password,length); esp_err_t error=enqueue(h,&c); erase_secret(&c,sizeof(c)); return error;
}
esp_err_t aether_wifi_disconnect(aether_wifi_handle_t h)
{ if(!enabled(h)) return ESP_ERR_INVALID_STATE; command_t c={.kind=CMD_DISCONNECT}; return enqueue(h,&c); }
esp_err_t aether_wifi_forget(aether_wifi_handle_t h)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    if(peer_phase(h)!=PEER_IDLE) return ESP_ERR_INVALID_STATE;
    esp_err_t error=ensure_owner(h); if(error!=ESP_OK) return error;
    command_t c={.kind=CMD_FORGET}; return enqueue(h,&c);
}
esp_err_t aether_wifi_peer_acquire(aether_wifi_handle_t h, uint32_t timeout_ms)
{
    esp_err_t error=ensure_owner(h);
    if(error!=ESP_OK) return error;
    portENTER_CRITICAL(&h->lock);
    if(h->peer_phase!=PEER_IDLE || atomic_load(&h->shutdown)) {
        portEXIT_CRITICAL(&h->lock);
        return ESP_ERR_INVALID_STATE;
    }
    h->peer_restore=h->status.enabled;
    h->peer_result=ESP_OK;
    h->peer_phase=PEER_REQUESTED;
    portEXIT_CRITICAL(&h->lock);
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    do {
        portENTER_CRITICAL(&h->lock);
        peer_phase_t phase=h->peer_phase;
        error=h->peer_result;
        portEXIT_CRITICAL(&h->lock);
        if(phase==PEER_ACTIVE) return ESP_OK;
        if(phase==PEER_FAILED) break;
        if(phase!=PEER_REQUESTED) return ESP_ERR_INVALID_STATE;
        error=ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    } while(esp_timer_get_time()<deadline);
    aether_wifi_peer_release(h,0);
    return error;
}
esp_err_t aether_wifi_peer_release(aether_wifi_handle_t h, uint32_t timeout_ms)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    if(h->peer_phase==PEER_IDLE) {
        portEXIT_CRITICAL(&h->lock);
        return ESP_OK;
    }
    if(h->peer_phase!=PEER_RESTORING) {
        h->peer_phase=PEER_RELEASE_REQUESTED;
    }
    portEXIT_CRITICAL(&h->lock);
    if(!timeout_ms) return ESP_OK;
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    for(;;) {
        portENTER_CRITICAL(&h->lock);
        bool done=h->peer_phase==PEER_IDLE;
        esp_err_t error=h->peer_result;
        portEXIT_CRITICAL(&h->lock);
        if(done) return error;
        if(esp_timer_get_time()>=deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
esp_err_t aether_wifi_get_status(aether_wifi_handle_t h,aether_wifi_status_t *status)
{
    if(!h || !status) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    *status=h->status;
    status->peer_radio=h->peer_phase!=PEER_IDLE;
    status->enabled_preference=status->peer_radio ? h->peer_restore : h->status.enabled;
    status->preference_pending=h->preference.pending || h->preference.submitting;
    status->preference_error=h->preference.error;
    status->persistence_pending|=status->preference_pending;
    portEXIT_CRITICAL(&h->lock);
    return ESP_OK;
}
esp_err_t aether_wifi_get_ap(aether_wifi_handle_t h,size_t index,aether_wifi_ap_t *ap)
{
    if(!h || !ap) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    if(index>=h->status.ap_count) { portEXIT_CRITICAL(&h->lock); return ESP_ERR_NOT_FOUND; }
    *ap=h->aps[index]; portEXIT_CRITICAL(&h->lock); return ESP_OK;
}
esp_err_t aether_wifi_del(aether_wifi_handle_t h,uint32_t timeout_ms)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    bool pending=h->status.persistence_pending || h->preference.pending ||
                 h->preference.submitting || h->peer_phase!=PEER_IDLE;
    bool starting=h->owner_starting,started=h->owner_started;
    if(!pending && !starting) atomic_store(&h->shutdown,true);
    portEXIT_CRITICAL(&h->lock);
    if(pending || starting) return ESP_ERR_INVALID_STATE;
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    while(started && !atomic_load(&h->done)) { if(esp_timer_get_time()>=deadline) return ESP_ERR_TIMEOUT; vTaskDelay(1); }
    if(h->initialized) { stop_driver(h); if(h->initialized) return ESP_ERR_INVALID_STATE; }
    esp_event_handler_instance_unregister(IP_EVENT,ESP_EVENT_ANY_ID,h->ip_handler);
    esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,h->wifi_handler);
    esp_netif_destroy_default_wifi(h->netif); vQueueDelete(h->queue);
    if(default_handle==h) default_handle=NULL;
    erase_secret(&h->pending_credentials,sizeof(h->pending_credentials));
    free(h); return ESP_OK;
}
