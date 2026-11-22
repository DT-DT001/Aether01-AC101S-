#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
/* Execute the production manager and HID profile with the IDF public APIs. */
#include "../../components/aether_bluetooth/aether_bluetooth.c"
#include "nvs.h"

struct test_queue { pthread_mutex_t lock; unsigned capacity,count,head;size_t size;unsigned char *data; };
static pthread_t manager_thread;
static unsigned controller_inits,controller_enables,controller_disables,host_inits,host_deinits;
static unsigned controller_init_attempts;
static atomic_size_t dma_available = 128U * 1024U, dma_largest = 64U * 1024U;
static atomic_uint dma_queries;
static atomic_llong timer_offset;
static bool controller_up,radio_up,host_up,host_active,fail_fifo;
static bool fail_manager_task;
static unsigned manager_tasks;
static atomic_bool hold_deinit, hold_controller_disable;
static bool hold_connect,fail_host_disable;
static bool hold_disconnect,fail_disconnect,pin_accepted,cfm_accepted;
static unsigned rejected_passkeys;
static atomic_uint connect_calls,disconnect_calls;
static atomic_uint scan_calls,media_start_calls;
static _Atomic(SemaphoreHandle_t) watched_producer;
static atomic_bool start_waiting;
static atomic_uint source_deinit_calls;
static int fail_at,stage;
static uint32_t negotiated_rate=48000;
static unsigned negotiated_channels=2;
static esp_bt_gap_cb_t gap;
static esp_a2d_cb_t a2dp;
static esp_a2d_source_data_cb_t pull;
static esp_avrc_ct_cb_t ct;
static esp_avrc_tg_cb_t tg;
static esp_hd_cb_t hid_cb;
static esp_bt_cod_t hid_cod;
static const esp_bd_addr_t approved_pc = {0xa0, 2, 3, 4, 5, 6};
static atomic_bool hid_bonded, hid_link, hid_passive_scan;
static atomic_uint hid_reconnects;
static esp_err_t step(void) { ++stage; if(fail_at==stage){fail_at=0;return ESP_FAIL;}return ESP_OK; }
int64_t esp_timer_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(int64_t)t.tv_sec*1000000+t.tv_nsec/1000+atomic_load(&timer_offset);}
void vTaskDelay(TickType_t ticks){struct timespec t={.tv_sec=ticks/1000,.tv_nsec=(ticks%1000)*1000000};nanosleep(&t,NULL);}
void vTaskDelete(TaskHandle_t task){assert(!task);pthread_exit(NULL);}
typedef struct {void(*fn)(void*);void*arg;} start_t;
static void *task_start(void *arg){start_t s=*(start_t*)arg;free(arg);s.fn(s.arg);return NULL;}
BaseType_t xTaskCreate(void(*fn)(void*),const char*name,uint32_t stack,void*arg,int priority,TaskHandle_t*out)
{(void)name;(void)stack;(void)priority;(void)out;if(fail_manager_task)return 0;++manager_tasks;start_t*s=malloc(sizeof(*s));*s=(start_t){fn,arg};return pthread_create(&manager_thread,NULL,task_start,s)==0?pdPASS:0;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){pthread_mutex_t*m=malloc(sizeof(*m));pthread_mutex_init(m,NULL);return m;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t ticks){int64_t end=esp_timer_get_time()+(int64_t)ticks*1000;do{if(!pthread_mutex_trylock(m))return pdTRUE;if(m==atomic_load(&watched_producer))atomic_store(&start_waiting,true);vTaskDelay(1);}while(esp_timer_get_time()<end);return pdFALSE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m){return pthread_mutex_unlock(m)==0?pdTRUE:pdFALSE;}
void vSemaphoreDelete(SemaphoreHandle_t m){pthread_mutex_destroy(m);free(m);}
QueueHandle_t xQueueCreate(unsigned count,size_t size){QueueHandle_t q=calloc(1,sizeof(*q));assert(q);q->capacity=count;q->size=size;q->data=calloc(count,size);assert(q->data);pthread_mutex_init(&q->lock,NULL);return q;}
BaseType_t xQueueSend(QueueHandle_t q,const void*value,TickType_t wait){assert(!wait);pthread_mutex_lock(&q->lock);bool ok=q->count<q->capacity;if(ok){memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,value,q->size);++q->count;}pthread_mutex_unlock(&q->lock);return ok;}
BaseType_t xQueueReceive(QueueHandle_t q,void*value,TickType_t wait){assert(!wait);pthread_mutex_lock(&q->lock);bool ok=q->count;if(ok){memcpy(value,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;--q->count;}pthread_mutex_unlock(&q->lock);return ok;}
BaseType_t xQueueReset(QueueHandle_t q){pthread_mutex_lock(&q->lock);q->head=q->count=0;pthread_mutex_unlock(&q->lock);return pdPASS;}
void vQueueDelete(QueueHandle_t q){pthread_mutex_destroy(&q->lock);free(q->data);free(q);}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps){assert(caps==3);return fail_fifo?NULL:calloc(count,size);}
size_t heap_caps_get_free_size(unsigned caps){assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));atomic_fetch_add(&dma_queries,1);return atomic_load(&dma_available);}
size_t heap_caps_get_largest_free_block(unsigned caps){assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));atomic_fetch_add(&dma_queries,1);return atomic_load(&dma_largest);}
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t*c){++controller_init_attempts;assert(c->mode==ESP_BT_MODE_BTDM&&!controller_up);esp_err_t e=step();if(e==ESP_OK){controller_up=true;++controller_inits;}return e;}
esp_err_t esp_bt_controller_enable(esp_bt_mode_t mode){assert(mode==ESP_BT_MODE_BTDM&&controller_up&&!radio_up);esp_err_t e=step();if(e==ESP_OK){radio_up=true;++controller_enables;}return e;}
esp_err_t esp_bt_controller_disable(void)
{
    assert(radio_up&&!host_up);
    while (atomic_load(&hold_controller_disable)) vTaskDelay(1);
    radio_up=false;
    ++controller_disables;
    return ESP_OK;
}
esp_err_t esp_bt_controller_deinit(void){assert(controller_up&&!radio_up);controller_up=false;return ESP_OK;}
esp_err_t esp_bluedroid_init(void){assert(radio_up&&!host_up);esp_err_t e=step();if(e==ESP_OK){host_up=true;++host_inits;}return e;}
esp_err_t esp_bluedroid_enable(void){assert(host_up&&!host_active);esp_err_t e=step();if(e==ESP_OK)host_active=true;return e;}
esp_err_t esp_bluedroid_disable(void){assert(host_active);if(fail_host_disable){fail_host_disable=false;return ESP_FAIL;}host_active=false;return ESP_OK;}
esp_err_t esp_bluedroid_deinit(void){assert(host_up&&!host_active);host_up=false;++host_deinits;return ESP_OK;}
esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t cb){gap=cb;return step();}
esp_err_t esp_bt_gap_set_device_name(const char*name){assert(name&&*name);return step();}
esp_err_t esp_bt_gap_set_security_param(esp_bt_sp_param_t p,void*v,uint8_t n){(void)p;assert(v&&n);return step();}
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c,esp_bt_discovery_mode_t d)
{
    assert(d==ESP_BT_NON_DISCOVERABLE||(c==ESP_BT_CONNECTABLE&&d==ESP_BT_GENERAL_DISCOVERABLE));
    const esp_err_t result=step();
    if (result==ESP_OK) atomic_store(&hid_passive_scan,c==ESP_BT_CONNECTABLE&&d==ESP_BT_NON_DISCOVERABLE);
    return result;
}
esp_err_t esp_bt_gap_get_cod(esp_bt_cod_t *cod) {*cod=hid_cod;return ESP_OK;}
esp_err_t esp_bt_gap_set_cod(esp_bt_cod_t cod,esp_bt_cod_mode_t mode)
{assert(mode==ESP_BT_SET_COD_MAJOR_MINOR);hid_cod=cod;return ESP_OK;}
esp_err_t esp_bt_gap_start_discovery(esp_bt_inq_mode_t m,uint8_t len,uint8_t rsp){++scan_calls;(void)m;(void)rsp;assert(len==10);esp_bt_gap_cb_param_t p={0};p.disc_st_chg.state=ESP_BT_GAP_DISCOVERY_STARTED;gap(ESP_BT_GAP_DISC_STATE_CHANGED_EVT,&p);return ESP_OK;}
esp_err_t esp_bt_gap_cancel_discovery(void){esp_bt_gap_cb_param_t p={0};p.disc_st_chg.state=ESP_BT_GAP_DISCOVERY_STOPPED;gap(ESP_BT_GAP_DISC_STATE_CHANGED_EVT,&p);return ESP_OK;}
esp_err_t esp_bt_gap_pin_reply(esp_bd_addr_t a,bool accept,uint8_t len,esp_bt_pin_code_t pin){(void)a;pin_accepted=accept;assert(len==4&&pin[0]=='0');return ESP_OK;}
esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t a,bool accept){(void)a;cfm_accepted=accept;return ESP_OK;}
esp_err_t esp_bt_gap_ssp_passkey_reply(esp_bd_addr_t peer, bool accept, uint32_t passkey)
{
    assert(peer[0] == 1 && !accept && passkey == 0);
    ++rejected_passkeys;
    return ESP_OK;
}
esp_err_t esp_bt_gap_get_bond_device_list(int *count, esp_bd_addr_t *devices)
{
    assert(count&&devices&&*count>=0);
    *count = *count > 0 && atomic_load(&hid_bonded) ? 1 : 0;
    if (*count) memcpy(devices[0],approved_pc,sizeof(approved_pc));
    return ESP_OK;
}
esp_err_t esp_avrc_ct_register_callback(esp_avrc_ct_cb_t cb){ct=cb;return step();}
esp_err_t esp_avrc_tg_register_callback(esp_avrc_tg_cb_t cb){tg=cb;return step();}
esp_err_t esp_avrc_ct_init(void){esp_err_t e=step();if(e==ESP_OK){esp_avrc_ct_cb_param_t p={0};p.avrc_ct_init_stat.state=ESP_AVRC_INIT_SUCCESS;ct(ESP_AVRC_CT_PROF_STATE_EVT,&p);}return e;}
esp_err_t esp_avrc_tg_init(void){esp_err_t e=step();if(e==ESP_OK){esp_avrc_tg_cb_param_t p={0};p.avrc_tg_init_stat.state=ESP_AVRC_INIT_SUCCESS;tg(ESP_AVRC_TG_PROF_STATE_EVT,&p);}return e;}
esp_err_t esp_avrc_ct_deinit(void){esp_avrc_ct_cb_param_t p={0};p.avrc_ct_init_stat.state=ESP_AVRC_DEINIT_SUCCESS;ct(ESP_AVRC_CT_PROF_STATE_EVT,&p);return ESP_OK;}
esp_err_t esp_avrc_tg_deinit(void){esp_avrc_tg_cb_param_t p={0};p.avrc_tg_init_stat.state=ESP_AVRC_DEINIT_SUCCESS;tg(ESP_AVRC_TG_PROF_STATE_EVT,&p);return ESP_OK;}
esp_err_t esp_avrc_tg_set_psth_cmd_filter(esp_avrc_psth_filter_t f,const esp_avrc_psth_bit_mask_t*m){(void)f;assert(m);return step();}
esp_err_t esp_avrc_tg_set_rn_evt_cap(const esp_avrc_rn_evt_cap_mask_t*m){assert(m);return step();}
esp_err_t esp_avrc_ct_send_get_rn_capabilities_cmd(uint8_t tl){(void)tl;return ESP_OK;}
esp_err_t esp_avrc_ct_send_register_notification_cmd(uint8_t tl,uint8_t e,uint32_t p){(void)tl;(void)e;(void)p;return ESP_OK;}
esp_err_t esp_avrc_tg_send_rn_rsp(esp_avrc_rn_event_ids_t e,esp_avrc_rn_rsp_t r,esp_avrc_rn_param_t*p){(void)e;(void)r;assert(p);return ESP_OK;}
esp_err_t esp_a2d_register_callback(esp_a2d_cb_t cb){a2dp=cb;return step();}
esp_err_t esp_a2d_source_register_data_callback(esp_a2d_source_data_cb_t cb){pull=cb;return step();}
esp_err_t esp_a2d_source_init(void){esp_err_t e=step();if(e==ESP_OK){esp_a2d_cb_param_t p={0};p.a2d_prof_stat.init_state=ESP_A2D_INIT_SUCCESS;a2dp(ESP_A2D_PROF_STATE_EVT,&p);}return e;}
esp_err_t esp_a2d_source_deinit(void){++source_deinit_calls;if(!hold_deinit){esp_a2d_cb_param_t p={0};p.a2d_prof_stat.init_state=ESP_A2D_DEINIT_SUCCESS;a2dp(ESP_A2D_PROF_STATE_EVT,&p);}return ESP_OK;}
esp_err_t esp_a2d_source_connect(esp_bd_addr_t address){++connect_calls;if(hold_connect)return ESP_OK;esp_a2d_cb_param_t p={0};p.audio_cfg.mcc.type=ESP_A2D_MCT_SBC;p.audio_cfg.mcc.cie.sbc_info.samp_freq=negotiated_rate==48000?ESP_A2D_SBC_CIE_SF_48K:ESP_A2D_SBC_CIE_SF_44K;p.audio_cfg.mcc.cie.sbc_info.ch_mode=negotiated_channels==1?ESP_A2D_SBC_CIE_CH_MODE_MONO:ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;memcpy(p.audio_cfg.remote_bda,address,6);a2dp(ESP_A2D_AUDIO_CFG_EVT,&p);memset(&p,0,sizeof(p));p.conn_stat.state=ESP_A2D_CONNECTION_STATE_CONNECTED;memcpy(p.conn_stat.remote_bda,address,6);a2dp(ESP_A2D_CONNECTION_STATE_EVT,&p);return ESP_OK;}
esp_err_t esp_a2d_source_disconnect(esp_bd_addr_t address){++disconnect_calls;if(fail_disconnect){fail_disconnect=false;return ESP_FAIL;}if(hold_disconnect)return ESP_OK;esp_a2d_cb_param_t p={0};p.conn_stat.state=ESP_A2D_CONNECTION_STATE_DISCONNECTED;memcpy(p.conn_stat.remote_bda,address,6);a2dp(ESP_A2D_CONNECTION_STATE_EVT,&p);return ESP_OK;}
esp_err_t esp_a2d_media_ctrl(esp_a2d_media_ctrl_t cmd){if(cmd==ESP_A2D_MEDIA_CTRL_START)++media_start_calls;esp_a2d_cb_param_t p={0};p.media_ctrl_stat.cmd=cmd;p.media_ctrl_stat.status=ESP_A2D_MEDIA_CTRL_ACK_SUCCESS;a2dp(ESP_A2D_MEDIA_CTRL_ACK_EVT,&p);return ESP_OK;}

esp_err_t esp_bt_hid_device_register_callback(esp_hd_cb_t cb)
{ assert(host_active); hid_cb=cb; return ESP_OK; }
esp_err_t esp_bt_hid_device_init(void)
{ esp_hidd_cb_param_t p={.init.status=ESP_HIDD_SUCCESS}; hid_cb(ESP_HIDD_INIT_EVT,&p); return ESP_OK; }
esp_err_t esp_bt_hid_device_register_app(esp_hidd_app_param_t *app,esp_hidd_qos_param_t *in,esp_hidd_qos_param_t *out)
{ assert(app&&in&&out); esp_hidd_cb_param_t p={.register_app.status=ESP_HIDD_SUCCESS}; hid_cb(ESP_HIDD_REGISTER_APP_EVT,&p); return ESP_OK; }
esp_err_t esp_bt_hid_device_unregister_app(void)
{
    if (atomic_load(&hid_link)) assert(esp_bt_hid_device_disconnect()==ESP_OK);
    esp_hidd_cb_param_t p={.unregister_app.status=ESP_HIDD_SUCCESS};
    hid_cb(ESP_HIDD_UNREGISTER_APP_EVT,&p);
    return ESP_OK;
}
esp_err_t esp_bt_hid_device_deinit(void)
{ esp_hidd_cb_param_t p={.deinit.status=ESP_HIDD_SUCCESS}; hid_cb(ESP_HIDD_DEINIT_EVT,&p); return ESP_OK; }
static void open_hid_link(const uint8_t *address)
{
    assert(!atomic_exchange(&hid_link,true));
    esp_hidd_cb_param_t p={.open={.status=ESP_HIDD_SUCCESS,.conn_status=ESP_HIDD_CONN_STATE_CONNECTED}};
    memcpy(p.open.bd_addr,address,6);
    hid_cb(ESP_HIDD_OPEN_EVT,&p);
}
esp_err_t esp_bt_hid_device_connect(esp_bd_addr_t address)
{
    assert(host_active&&atomic_load(&hid_bonded)&&!memcmp(address,approved_pc,6));
    ++hid_reconnects;
    open_hid_link(address);
    return ESP_OK;
}
esp_err_t esp_bt_hid_device_disconnect(void)
{
    assert(atomic_exchange(&hid_link,false));
    esp_hidd_cb_param_t p={.close={.status=ESP_HIDD_SUCCESS,.conn_status=ESP_HIDD_CONN_STATE_DISCONNECTED}};
    hid_cb(ESP_HIDD_CLOSE_EVT,&p);
    return ESP_OK;
}
esp_err_t esp_bt_hid_device_send_report(esp_hidd_report_type_t type,uint8_t id,uint16_t len,uint8_t *data)
{
    assert(atomic_load(&hid_link)&&type==ESP_HIDD_REPORT_TYPE_INTRDATA&&len==bt_hid_report_size(id)&&data);
    esp_hidd_cb_param_t p={.send_report={.status=ESP_HIDD_SUCCESS,.report_type=ESP_HIDD_REPORT_TYPE_INPUT,.report_id=id}};
    hid_cb(ESP_HIDD_SEND_REPORT_EVT,&p);
    return ESP_OK;
}
esp_err_t esp_bt_hid_device_report_error(esp_hidd_handshake_error_t error)
{ (void)error; assert(!"Unexpected HID report request in ownership test"); return ESP_FAIL; }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out)
{ assert(!strcmp(name,"pc_hid")&&mode==NVS_READONLY); *out=mode; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *value,size_t *size)
{
    assert(h==NVS_READONLY&&!strcmp(key,"approved")&&*size==sizeof(approved_pc));
    memcpy(value,approved_pc,sizeof(approved_pc));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size)
{ (void)h; (void)key; (void)value; (void)size; assert(!"Unexpected HID bond write in ownership test"); return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static aether_bluetooth_status_t wait_state(aether_bluetooth_handle_t h,aether_bluetooth_state_t state)
{aether_bluetooth_status_t s;int64_t end=esp_timer_get_time()+3000000;do{aether_bluetooth_get_status(h,&s);if(s.state==state)return s;vTaskDelay(1);}while(esp_timer_get_time()<end);fprintf(stderr,"state %d wanted %d error %x\n",s.state,state,s.last_error);assert(0);return s;}
static void discover_one(aether_bluetooth_handle_t h);

static void wait_discovery(aether_bluetooth_handle_t h,bool discoverable)
{
    aether_bluetooth_hid_status_t status;
    const int64_t until=esp_timer_get_time()+3000000;
    do {
        assert(aether_bluetooth_hid_get_status(h,&status)==ESP_OK);
        if(status.discoverable==discoverable&&status.requested==discoverable) return;
        vTaskDelay(1);
    } while(esp_timer_get_time()<until);
    assert(!"HID discovery request never reached the expected state");
}

static void wait_hid_connected(aether_bluetooth_handle_t h)
{
    aether_bluetooth_hid_status_t status;
    const int64_t until=esp_timer_get_time()+4000000;
    do {
        assert(aether_bluetooth_hid_get_status(h,&status)==ESP_OK);
        if(status.connected) {
            assert(status.requested&&!status.confirm_pending);
            return;
        }
        vTaskDelay(1);
    } while(esp_timer_get_time()<until);
    assert(!"Previously bonded HID peer did not reconnect after radio restart");
}

static void wait_hid_page_scan(void)
{
    const int64_t until=esp_timer_get_time()+3000000;
    while (!atomic_load(&hid_passive_scan) && esp_timer_get_time()<until) vTaskDelay(1);
    assert(atomic_load(&hid_passive_scan));
}

static void hid_ownership_checks(aether_bluetooth_handle_t h)
{
    assert(aether_bluetooth_hid_set_enabled(h,true)==ESP_OK);
    assert(!wait_state(h,AETHER_BT_IDLE).audio_requested);
    wait_discovery(h,true);
    assert(!atomic_load(&hid_reconnects));

    assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);
    discover_one(h);
    assert(aether_bluetooth_hid_set_enabled(h,false)==ESP_OK);
    wait_discovery(h,false);
    assert(wait_state(h,AETHER_BT_CONNECTED).audio_requested);
    assert(aether_bluetooth_set_enabled(h,false)==ESP_OK);
    wait_state(h,AETHER_BT_OFF);

    assert(aether_bluetooth_hid_set_enabled(h,true)==ESP_OK);
    wait_discovery(h,true);
    const unsigned enables=controller_enables, disables=controller_disables;
    hold_deinit=true;
    const unsigned before=atomic_load(&source_deinit_calls);
    assert(aether_bluetooth_hid_set_enabled(h,false)==ESP_OK);
    wait_state(h,AETHER_BT_STOPPING);
    atomic_store(&hid_bonded,true);
    assert(aether_bluetooth_hid_set_enabled(h,true)==ESP_OK);
    aether_bluetooth_hid_status_t status;
    assert(aether_bluetooth_hid_get_status(h,&status)==ESP_OK && status.requested && !status.connected);
    for(unsigned i=0;i<1000 && atomic_load(&source_deinit_calls)==before;++i) vTaskDelay(1);
    assert(atomic_load(&source_deinit_calls)==before+1&&!atomic_load(&hid_reconnects));
    hold_deinit=false;
    esp_a2d_cb_param_t finish={.a2d_prof_stat.init_state=ESP_A2D_DEINIT_SUCCESS};
    a2dp(ESP_A2D_PROF_STATE_EVT,&finish);
    wait_hid_connected(h);
    assert(atomic_load(&hid_reconnects)==1);
    assert(controller_enables==enables&&controller_disables==disables);
    assert(!wait_state(h,AETHER_BT_IDLE).audio_requested);
    assert(aether_bluetooth_hid_set_enabled(h,false)==ESP_OK);
    wait_state(h,AETHER_BT_OFF);
    assert(!atomic_load(&hid_passive_scan));
    assert(!h->fifo&&!host_up&&radio_up&&controller_up);

    assert(aether_bluetooth_hid_set_enabled(h,true)==ESP_OK);
    wait_hid_page_scan();
    assert(atomic_load(&hid_reconnects)==1);
    open_hid_link(approved_pc);
    wait_hid_connected(h);
    assert(atomic_load(&hid_reconnects)==1);
    assert(aether_bluetooth_hid_set_enabled(h,false)==ESP_OK);
    wait_state(h,AETHER_BT_OFF);
    assert(!h->fifo&&!host_up&&radio_up&&controller_up);
    assert(controller_enables==enables&&controller_disables==disables);

    atomic_store(&hold_controller_disable,true);
    assert(aether_bluetooth_set_enabled(h,false)==ESP_OK);
    assert(!wait_state(h,AETHER_BT_STOPPING).enabled&&radio_up);
    atomic_store(&hold_controller_disable,false);
    wait_state(h,AETHER_BT_OFF);
    assert(!radio_up&&controller_disables==disables+1);
    puts("HID ownership: headset survives HID exit, host releases while controller stays enabled, and explicit off disables the controller");
}
static void wait_budget_rejected(aether_bluetooth_handle_t h)
{
    int64_t end=esp_timer_get_time()+3000000;
    do {
        aether_bluetooth_status_t s; aether_bluetooth_get_status(h,&s);
        if (!atomic_load(&h->desired) && s.state==AETHER_BT_OFF && s.last_error==ESP_ERR_NO_MEM) {
            assert(!s.enabled && !h->fifo && !controller_init_attempts && !controller_up);
            unsigned queries=atomic_load(&dma_queries);
            vTaskDelay(60);
            assert(queries==atomic_load(&dma_queries) && !controller_init_attempts);
            return;
        }
        vTaskDelay(1);
    } while(esp_timer_get_time()<end);
    assert(!"controller DMA admission did not reject without retry");
}
static void discover_one(aether_bluetooth_handle_t h)
{
    assert(aether_bluetooth_scan(h)==ESP_OK);wait_state(h,AETHER_BT_SCANNING);
    uint8_t name[]={8,ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,'O','r','i','g','i','n','a',0};
    int8_t rssi=-42;uint32_t cod=(ESP_BT_COD_MAJOR_DEV_AV<<8);
    esp_bt_gap_dev_prop_t props[]={{.type=ESP_BT_GAP_DEV_PROP_EIR,.len=sizeof(name),.val=name},{.type=ESP_BT_GAP_DEV_PROP_RSSI,.len=1,.val=&rssi},{.type=ESP_BT_GAP_DEV_PROP_COD,.len=4,.val=&cod}};
    esp_bt_gap_cb_param_t p={0};p.disc_res.bda[0]=1;p.disc_res.num_prop=3;p.disc_res.prop=props;gap(ESP_BT_GAP_DISC_RES_EVT,&p);
    gap(ESP_BT_GAP_DISC_RES_EVT,&p);aether_bluetooth_status_t s;aether_bluetooth_get_status(h,&s);assert(s.device_count==1);
    aether_bluetooth_device_t device;assert(aether_bluetooth_get_device(h,0,&device)==ESP_OK);assert(!strcmp(device.name,"Origina")&&device.rssi==-42&&device.audio_device);
    assert(aether_bluetooth_connect(h,device.address)==ESP_OK);s=wait_state(h,AETHER_BT_CONNECTED);assert(s.sample_rate==negotiated_rate&&s.channels==negotiated_channels);
}
typedef struct{ac101s_audio_sink_t sink;esp_err_t result;int16_t *pcm;size_t frames;} writer_t;
static void *write_thread(void*arg){writer_t*w=arg;w->result=w->sink.write(w->sink.context,w->pcm,w->frames*4,2000);return NULL;}
static void *start_thread(void*arg){writer_t*w=arg;w->result=w->sink.start(w->sink.context);return NULL;}
static void audio_checks(aether_bluetooth_handle_t h)
{
    ac101s_audio_sink_t sink;assert(aether_bluetooth_get_sink(h,&sink)==ESP_OK&&sink.connected(h));assert(sink.start(h)==ESP_OK);
    int16_t pcm[1024*2];for(unsigned i=0;i<1024;++i){pcm[i*2]=(int16_t)i;pcm[i*2+1]=(int16_t)(-((int)i));}
    assert(sink.write(h,pcm,sizeof(pcm),100)==ESP_OK);
    uint8_t out[1024*4+3];memset(out,0xAB,sizeof(out));assert(pull(out,sizeof(out))==(int)sizeof(out));
    size_t frames=negotiated_rate==48000?1024:((1024U-1)*44100+47999)/48000;
    for(size_t i=0;i<frames;++i){int16_t l,r;if(negotiated_channels==1){memcpy(&l,out+i*2,2);assert(l==0);}else{memcpy(&l,out+i*4,2);memcpy(&r,out+i*4+2,2);int16_t expected=(int16_t)(i*48000U/negotiated_rate);assert(l==expected&&r==-expected);}}
    for(size_t i=frames*negotiated_channels*2;i<sizeof(out);++i)assert(out[i]==0);
    assert(sink.stop(h)==ESP_OK);assert(sink.start(h)==ESP_OK);
    writer_t writer={.sink=sink,.frames=20000,.pcm=calloc(40000,sizeof(int16_t))};assert(writer.pcm);pthread_t thread;assert(!pthread_create(&thread,NULL,write_thread,&writer));
    for(unsigned i=0;i<500;++i){aether_bluetooth_status_t s;aether_bluetooth_get_status(h,&s);if(s.queued_frames==FIFO_FRAMES)break;vTaskDelay(1);}
    int64_t before=esp_timer_get_time();assert(sink.stop(h)==ESP_OK);pthread_join(thread,NULL);assert(writer.result==ESP_ERR_INVALID_STATE&&esp_timer_get_time()-before<100000);free(writer.pcm);
    assert(sink.start(h)==ESP_OK);int16_t *large=calloc(20000*2,sizeof(int16_t));assert(large);assert(sink.write(h,large,20000*4,10)==ESP_ERR_TIMEOUT);free(large);
    aether_bluetooth_status_t s;aether_bluetooth_get_status(h,&s);assert(!s.queued_frames);assert(sink.stop(h)==ESP_OK);
    /* A start waiting behind the previous producer must not reopen after a
     * concurrent STOP invalidates that generation. */
    assert(xSemaphoreTake(h->producer,0)==pdTRUE);atomic_store(&watched_producer,h->producer);atomic_store(&start_waiting,false);
    writer=(writer_t){.sink=sink};assert(!pthread_create(&thread,NULL,start_thread,&writer));
    for(unsigned i=0;i<500&&!atomic_load(&start_waiting);++i) vTaskDelay(1);
    assert(atomic_load(&start_waiting));
    assert(sink.stop(h)==ESP_OK);xSemaphoreGive(h->producer);pthread_join(thread,NULL);atomic_store(&watched_producer,NULL);
    assert(writer.result==ESP_ERR_INVALID_STATE&&!atomic_load(&h->media_requested));
    portENTER_CRITICAL(&h->lock);assert(!h->active&&!h->count);portEXIT_CRITICAL(&h->lock);
}
static void remote_checks(aether_bluetooth_handle_t h)
{
    esp_avrc_tg_cb_param_t p={0};p.psth_cmd.key_code=ESP_AVRC_PT_CMD_PLAY;tg(ESP_AVRC_TG_PASSTHROUGH_CMD_EVT,&p);p.psth_cmd.key_state=1;tg(ESP_AVRC_TG_PASSTHROUGH_CMD_EVT,&p);
    aether_bluetooth_remote_event_t event;assert(aether_bluetooth_poll_remote(h,&event)&&event.action==AETHER_BT_REMOTE_PLAY);assert(!aether_bluetooth_poll_remote(h,&event));
    p.set_abs_vol.volume=64;tg(ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT,&p);assert(!aether_bluetooth_poll_remote(h,&event));aether_bluetooth_status_t s;aether_bluetooth_get_status(h,&s);assert(s.peer_volume==50&&s.volume==100);
    p.psth_cmd.key_code=ESP_AVRC_PT_CMD_VOL_UP;p.psth_cmd.key_state=0;for(unsigned i=0;i<20;++i)tg(ESP_AVRC_TG_PASSTHROUGH_CMD_EVT,&p);aether_bluetooth_get_status(h,&s);assert(s.remote_events_dropped>=4);
    while(aether_bluetooth_poll_remote(h,&event))assert(event.action==AETHER_BT_REMOTE_VOLUME_UP);
}
static void begin_pending_connect(aether_bluetooth_handle_t h)
{
    const uint8_t address[6]={1};
    const unsigned before=atomic_load(&connect_calls);
    hold_connect=true;
    assert(aether_bluetooth_connect(h,address)==ESP_OK);
    wait_state(h,AETHER_BT_CONNECTING);
    for(unsigned i=0;i<1000 && atomic_load(&connect_calls)==before;++i)vTaskDelay(1);
    assert(atomic_load(&connect_calls)==before+1);
}
static void disconnected(void)
{
    esp_a2d_cb_param_t p={0};p.conn_stat.state=ESP_A2D_CONNECTION_STATE_DISCONNECTED;
    p.conn_stat.remote_bda[0]=1;p.conn_stat.disc_rsn=ESP_A2D_DISC_RSN_NORMAL;
    a2dp(ESP_A2D_CONNECTION_STATE_EVT,&p);
}
static void connection_checks(aether_bluetooth_handle_t h)
{
    assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_state(h,AETHER_BT_IDLE);
    begin_pending_connect(h);
    aether_bluetooth_status_t s; aether_bluetooth_get_status(h,&s);
    assert(s.auth_status==-1 && s.acl_status==-1 && s.a2dp_disconnect_reason==-1);
    esp_bt_gap_cb_param_t gap_param={0};
    gap_param.pin_req.bda[0]=1;gap(ESP_BT_GAP_PIN_REQ_EVT,&gap_param);assert(pin_accepted);
    gap_param.pin_req.bda[0]=2;gap(ESP_BT_GAP_PIN_REQ_EVT,&gap_param);assert(!pin_accepted);
    memset(&gap_param,0,sizeof(gap_param));gap_param.cfm_req.bda[0]=1;
    gap(ESP_BT_GAP_CFM_REQ_EVT,&gap_param);assert(cfm_accepted);
    gap_param.cfm_req.bda[0]=2;gap(ESP_BT_GAP_CFM_REQ_EVT,&gap_param);assert(!cfm_accepted);
    gap_param.key_req.bda[0]=1;
    gap(ESP_BT_GAP_KEY_REQ_EVT, &gap_param);
    assert(rejected_passkeys == 1);
    memset(&gap_param,0,sizeof(gap_param));gap_param.acl_conn_cmpl_stat.bda[0]=1;
    gap_param.acl_conn_cmpl_stat.stat=(esp_bt_status_t)0x09;
    gap(ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT,&gap_param);
    disconnected();s=wait_state(h,AETHER_BT_IDLE);
    assert(s.acl_status==0x09 && s.last_error==ESP_ERR_INVALID_RESPONSE && !s.connected);
    const unsigned failed_calls=atomic_load(&connect_calls);vTaskDelay(50);
    assert(atomic_load(&connect_calls)==failed_calls); /* No automatic reconnect. */
    begin_pending_connect(h);
    memset(&gap_param,0,sizeof(gap_param));gap_param.auth_cmpl.bda[0]=1;
    gap_param.auth_cmpl.stat=ESP_BT_STATUS_AUTH_FAILURE;gap(ESP_BT_GAP_AUTH_CMPL_EVT,&gap_param);
    disconnected();s=wait_state(h,AETHER_BT_IDLE);
    assert(s.auth_status==ESP_BT_STATUS_AUTH_FAILURE && s.last_error==ESP_ERR_INVALID_RESPONSE);
    /* SDK OPEN failure may report NORMAL; still surface failed initiation. */
    begin_pending_connect(h);disconnected();s=wait_state(h,AETHER_BT_IDLE);
    assert(s.acl_status==-1 && s.auth_status==-1 && s.last_error==ESP_FAIL);

    /* A fast/synchronous completion must not be overwritten with DISCONNECTING. */
    begin_pending_connect(h);atomic_fetch_add(&timer_offset,21000000);
    s=wait_state(h,AETHER_BT_IDLE);assert(s.last_error==ESP_ERR_TIMEOUT);
    hold_connect=false;const uint8_t address[6]={1};
    assert(aether_bluetooth_connect(h,address)==ESP_OK);wait_state(h,AETHER_BT_CONNECTED);
    fail_disconnect=true;unsigned before=atomic_load(&disconnect_calls);
    assert(aether_bluetooth_disconnect(h)==ESP_OK);
    for(unsigned i=0;i<1000 && atomic_load(&disconnect_calls)==before;++i)vTaskDelay(1);
    vTaskDelay(10);s=wait_state(h,AETHER_BT_CONNECTED);assert(s.last_error==ESP_FAIL);
    hold_disconnect=true;assert(aether_bluetooth_disconnect(h)==ESP_OK);wait_state(h,AETHER_BT_DISCONNECTING);
    disconnected();s=wait_state(h,AETHER_BT_IDLE);assert(s.last_error==ESP_FAIL);

    /* Accepted cancel with no completion closes profiles/host before retry,
     * retaining the once-initialized controller; no false IDLE with a live link. */
    begin_pending_connect(h);atomic_fetch_add(&timer_offset,21000000);wait_state(h,AETHER_BT_DISCONNECTING);
    esp_a2d_cb_param_t late={0};late.conn_stat.state=ESP_A2D_CONNECTION_STATE_CONNECTED;
    late.conn_stat.remote_bda[0]=1;a2dp(ESP_A2D_CONNECTION_STATE_EVT,&late);
    aether_bluetooth_get_status(h,&s);assert(s.state==AETHER_BT_DISCONNECTING && s.last_error==ESP_ERR_TIMEOUT);
    atomic_fetch_add(&timer_offset,4000000);s=wait_state(h,AETHER_BT_OFF);
    assert(!atomic_load(&h->desired) && s.last_error==ESP_ERR_TIMEOUT && controller_up && !host_up && !h->fifo);
    hold_disconnect=hold_connect=false;
    assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_state(h,AETHER_BT_IDLE);
    assert(aether_bluetooth_connect(h,address)==ESP_OK);wait_state(h,AETHER_BT_CONNECTED);
    memset(&gap_param,0,sizeof(gap_param));gap_param.acl_disconn_cmpl_stat.bda[0]=1;
    gap_param.acl_disconn_cmpl_stat.handle=0x42;gap_param.acl_disconn_cmpl_stat.reason=(esp_bt_status_t)0x08;
    gap(ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT,&gap_param);disconnected();s=wait_state(h,AETHER_BT_IDLE);
    assert(s.acl_disconnect_reason==0x08 && s.last_error==ESP_ERR_INVALID_STATE);
    assert(aether_bluetooth_set_enabled(h,false)==ESP_OK);wait_state(h,AETHER_BT_OFF);
    puts("Bluetooth asynchronous connect failures: selected PIN/SSP, raw ACL09/auth/disconnect status, failed OPEN, cancel/timeout recovery and manual retry passed");
}
static void complete_profile_deinit(aether_bluetooth_handle_t h)
{
    assert(h->fifo && host_active && radio_up && controller_up);
    hold_deinit=false;
    esp_a2d_cb_param_t finished={0};finished.a2d_prof_stat.init_state=ESP_A2D_DEINIT_SUCCESS;
    a2dp(ESP_A2D_PROF_STATE_EVT,&finished);
    wait_state(h,AETHER_BT_OFF);
    assert(!h->fifo && !host_up && !radio_up && controller_up && !h->connecting);
}
static void cancellation_ownership_checks(aether_bluetooth_handle_t h)
{
    assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_state(h,AETHER_BT_IDLE);
    begin_pending_connect(h);
    fail_disconnect=true;const unsigned before_cancel=atomic_load(&disconnect_calls);
    assert(aether_bluetooth_disconnect(h)==ESP_OK);wait_state(h,AETHER_BT_DISCONNECTING);
    for(unsigned i=0;i<1000 && atomic_load(&disconnect_calls)==before_cancel;++i)vTaskDelay(1);
    vTaskDelay(10);assert(h->connecting);
    const unsigned old_connects=atomic_load(&connect_calls),old_scans=atomic_load(&scan_calls),old_starts=atomic_load(&media_start_calls);
    const uint8_t other_peer[6]={2};
    assert(aether_bluetooth_scan(h)==ESP_OK && aether_bluetooth_connect(h,other_peer)==ESP_OK);
    vTaskDelay(50);aether_bluetooth_status_t status;
    aether_bluetooth_get_status(h,&status);
    assert(status.state==AETHER_BT_DISCONNECTING && !status.scanning && status.peer.address[0]==1);
    assert(old_connects==atomic_load(&connect_calls) && old_scans==atomic_load(&scan_calls));
    /* The old pending connect succeeds after cancellation was rejected. It
     * still owns the link and cannot become available as an audio route. */
    esp_a2d_cb_param_t late={0};late.audio_cfg.remote_bda[0]=1;late.audio_cfg.mcc.type=ESP_A2D_MCT_SBC;
    late.audio_cfg.mcc.cie.sbc_info.samp_freq=ESP_A2D_SBC_CIE_SF_48K;
    late.audio_cfg.mcc.cie.sbc_info.ch_mode=ESP_A2D_SBC_CIE_CH_MODE_STEREO;
    a2dp(ESP_A2D_AUDIO_CFG_EVT,&late);memset(&late,0,sizeof(late));
    late.conn_stat.remote_bda[0]=1;late.conn_stat.state=ESP_A2D_CONNECTION_STATE_CONNECTED;
    a2dp(ESP_A2D_CONNECTION_STATE_EVT,&late);
    ac101s_audio_sink_t sink;assert(aether_bluetooth_get_sink(h,&sink)==ESP_OK);
    assert(!sink.connected(h) && sink.start(h)==ESP_ERR_INVALID_STATE);
    assert(aether_bluetooth_scan(h)==ESP_OK && aether_bluetooth_connect(h,other_peer)==ESP_OK);
    /* Exercise media-owner gating independently of the sink's front door: a
     * stale desired media flag/ACK cannot restart a canceling connection. */
    atomic_store(&h->media_requested,true);
    vTaskDelay(50);aether_bluetooth_get_status(h,&status);
    assert(status.state==AETHER_BT_DISCONNECTING && status.peer.address[0]==1);
    assert(old_connects==atomic_load(&connect_calls) && old_scans==atomic_load(&scan_calls) && old_starts==atomic_load(&media_start_calls));
    atomic_fetch_add(&timer_offset,4000000);wait_state(h,AETHER_BT_OFF);
    assert(controller_up && !host_up && !h->fifo);

    /* Direct OFF used to restart a fresh 3s cancel wait on every cleanup
     * retry. Hold profile teardown and prove no repeat cancel or early free. */
    for(unsigned rejected=0;rejected<2;++rejected) {
        hold_connect=hold_disconnect=true;fail_disconnect=rejected!=0;
        assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_state(h,AETHER_BT_IDLE);
        begin_pending_connect(h);hold_deinit=true;
        const unsigned old_disconnects=atomic_load(&disconnect_calls),old_deinits=source_deinit_calls;
        assert(aether_bluetooth_set_enabled(h,false)==ESP_OK);wait_state(h,AETHER_BT_STOPPING);
        for(unsigned i=0;i<1000 && atomic_load(&disconnect_calls)==old_disconnects;++i)vTaskDelay(1);
        vTaskDelay(20);if(!rejected)atomic_fetch_add(&timer_offset,4000000);
        for(unsigned i=0;i<1000 && source_deinit_calls==old_deinits;++i)vTaskDelay(1);
        assert(source_deinit_calls==old_deinits+1 && h->fifo && host_active && radio_up);
        vTaskDelay(50);assert(atomic_load(&disconnect_calls)==old_disconnects+1);
        complete_profile_deinit(h);
    }
    hold_connect=hold_disconnect=hold_deinit=false;
    assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_state(h,AETHER_BT_IDLE);
    assert(aether_bluetooth_set_enabled(h,false)==ESP_OK);wait_state(h,AETHER_BT_OFF);
    puts("Bluetooth cancel ownership: rejected pending cancel + late CONNECTED/CFG blocks connect/scan/media; direct OFF missing/rejected disconnect waits once, retains FIFO until profile completion, then permits explicit retry passed");
}
int main(void)
{
    aether_bluetooth_handle_t h;
    for(unsigned cold=0;cold<100;++cold) {
        assert(aether_bluetooth_new(NULL,&h)==ESP_OK);
        assert(!manager_tasks&&!controller_inits&&!atomic_load(&h->task_created));
        assert(aether_bluetooth_set_enabled(h,false)==ESP_OK);
        assert(aether_bluetooth_del(h)==ESP_OK);
    }
    assert(aether_bluetooth_new(NULL,&h)==ESP_OK);assert(!controller_inits&&!manager_tasks);
    fail_manager_task=true;
    assert(aether_bluetooth_set_enabled(h,true)==ESP_ERR_NO_MEM);
    assert(!atomic_load(&h->task_created)&&!atomic_load(&h->desired)&&!manager_tasks);
    assert(aether_bluetooth_del(h)==ESP_OK);
    assert(aether_bluetooth_new(NULL,&h)==ESP_OK);
    assert(aether_bluetooth_set_enabled(h,true)==ESP_ERR_NO_MEM);
    fail_manager_task=false;
    atomic_store(&dma_available,64U*1024U-1);
    atomic_store(&dma_largest,32U*1024U);
    assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_budget_rejected(h);
    atomic_store(&dma_available,128U*1024U);
    atomic_store(&dma_largest,24U*1024U-1);
    assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_budget_rejected(h);
    /* Recovery at the exact admission boundary. Subsequent re-enables retain
     * the controller and must not require the first-init budget again. */
    atomic_store(&dma_available,64U*1024U);
    atomic_store(&dma_largest,24U*1024U);
    for(unsigned pass=0;pass<20;++pass){negotiated_rate=pass&1?44100:48000;negotiated_channels=pass%3==0?1:2;assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_state(h,AETHER_BT_IDLE);discover_one(h);audio_checks(h);remote_checks(h);assert(aether_bluetooth_set_enabled(h,false)==ESP_OK);wait_state(h,AETHER_BT_OFF);assert(!h->fifo&&!host_up&&!radio_up&&controller_up);}
    assert(controller_inits==1&&controller_enables==20&&controller_disables==20&&host_inits==host_deinits);
    assert(manager_tasks==1);
    const unsigned first_init_queries=atomic_load(&dma_queries);
    atomic_store(&dma_available,0);atomic_store(&dma_largest,0);
    assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);wait_state(h,AETHER_BT_IDLE);
    assert(aether_bluetooth_set_enabled(h,false)==ESP_OK);wait_state(h,AETHER_BT_OFF);
    assert(first_init_queries==atomic_load(&dma_queries) && controller_init_attempts==1);
    puts("Bluetooth first-init DMA admission: low total/fragmented reject without SDK calls or busy retry; recovered retry and retained-controller re-enable passed");
    hid_ownership_checks(h);
    connection_checks(h);
    cancellation_ownership_checks(h);
    fail_fifo=true;assert(aether_bluetooth_set_enabled(h,true)==ESP_OK);vTaskDelay(50);aether_bluetooth_status_t s;aether_bluetooth_get_status(h,&s);assert(!s.enabled&&s.last_error==ESP_ERR_NO_MEM);fail_fifo=false;
    /* Every startup stage fails once, then a complete retry must succeed. */
    for(int failure=1;failure<=16;++failure){stage=0;fail_at=failure;aether_bluetooth_set_enabled(h,true);vTaskDelay(80);assert(!atomic_load(&h->desired));assert(!h->fifo&&!host_up&&!radio_up);fail_at=0;aether_bluetooth_set_enabled(h,true);wait_state(h,AETHER_BT_IDLE);aether_bluetooth_set_enabled(h,false);wait_state(h,AETHER_BT_OFF);}
    aether_bluetooth_set_enabled(h,true);wait_state(h,AETHER_BT_IDLE);
    hold_deinit=true;unsigned before=source_deinit_calls;aether_bluetooth_set_enabled(h,false);wait_state(h,AETHER_BT_STOPPING);vTaskDelay(40);
    assert(h->fifo&&host_active&&radio_up&&source_deinit_calls==before+1);
    uint8_t late[127];memset(late,0xFE,sizeof(late));assert(pull(late,sizeof(late))==(int)sizeof(late));for(unsigned i=0;i<sizeof(late);++i)assert(!late[i]);
    hold_deinit=false;esp_a2d_cb_param_t finish={0};finish.a2d_prof_stat.init_state=ESP_A2D_DEINIT_SUCCESS;a2dp(ESP_A2D_PROF_STATE_EVT,&finish);wait_state(h,AETHER_BT_OFF);assert(source_deinit_calls==before+1);
    aether_bluetooth_set_enabled(h,true);wait_state(h,AETHER_BT_IDLE);hold_connect=true;uint8_t address[6]={1};aether_bluetooth_connect(h,address);wait_state(h,AETHER_BT_CONNECTING);aether_bluetooth_set_enabled(h,false);wait_state(h,AETHER_BT_OFF);assert(!h->connecting);hold_connect=false;
    aether_bluetooth_set_enabled(h,true);wait_state(h,AETHER_BT_IDLE);fail_host_disable=true;aether_bluetooth_set_enabled(h,false);wait_state(h,AETHER_BT_STOPPING);vTaskDelay(40);aether_bluetooth_set_enabled(h,true);wait_state(h,AETHER_BT_IDLE);assert(host_active&&radio_up);aether_bluetooth_set_enabled(h,false);wait_state(h,AETHER_BT_OFF);
    assert(aether_bluetooth_del(h)==ESP_OK);pthread_join(manager_thread,NULL);assert(!controller_up);assert(aether_bluetooth_new(NULL,&h)==ESP_ERR_INVALID_STATE);
    puts("Bluetooth production manager: 100 cold create/delete without task stack, task OOM/retry, 20 radio cycles, official API types, 44.1/48 mono/stereo FIFO, zero fill, bounded stop/backpressure, 16 init rollbacks and delayed teardown passed");
    return 0;
}
