#include "app_ui_internal.h"
#include "aether_recorder.h"
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct test_queue {pthread_mutex_t lock;size_t bytes,count,head;unsigned char data[16*32];};
static pthread_mutex_t pause_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_signal=PTHREAD_COND_INITIALIZER;
static bool pause_send,send_entered,resume_send;
static pthread_t gui;
static unsigned dispatched,completed,owner,dispatch_owner,last_action;
static esp_err_t submit_error;
static bool has_result,chain;
static app_io_result_t result;
static app_io_request_t submitted;
static bool recorder_present, recorder_active;
static esp_err_t recorder_snapshot_error;
static atomic_bool usb_blocked;
static unsigned usb_notices;
static bool release_pending;
static bool intercom_busy;
static esp_err_t release_error,power_error;
static app_power_status_t power_status;
static unsigned labels_created,labels_deleted,homes;
const lv_font_t lv_font_montserrat_20=20;
int64_t esp_timer_get_time(void){return 1;}
esp_err_t app_io_media_release_status(app_io_handle_t io,bool *pending,esp_err_t *error)
{assert(io==(void *)4&&pthread_equal(pthread_self(),gui));*pending=release_pending;*error=release_error;return ESP_OK;}
esp_err_t app_power_get_status(const app_power_t *power,app_power_status_t *status)
{assert(power==(void *)7&&pthread_equal(pthread_self(),gui));*status=power_status;return power_error;}
bool app_ui_input_blocked(const app_ui_t *ui){return ui->test_input_blocked;}
bool app_intercom_busy(void){return intercom_busy;}
void app_ui_show_home(app_ui_t *ui)
{assert(ui&&pthread_equal(pthread_self(),gui));++homes;++ui->page_generation;}
const char *esp_err_to_name(esp_err_t error)
{return error==ESP_OK?"ESP_OK":error==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":"ESP_FAIL";}
lv_obj_t *app_ui_add_text(lv_obj_t *parent,const char *text,lv_align_t align,int32_t x,int32_t y,uint32_t color,const lv_font_t *font)
{
    assert(pthread_equal(pthread_self(),gui)&&parent==(void *)6&&align==LV_ALIGN_BOTTOM_RIGHT);
    assert(x==-12&&y==-8&&color==0xFFD180&&font==&lv_font_montserrat_20);
    lv_obj_t *obj=calloc(1,sizeof(*obj));assert(obj);snprintf(obj->text,sizeof(obj->text),"%s",text);++labels_created;return obj;
}
void app_ui_label_text(lv_obj_t *obj,const char *text)
{assert(obj&&pthread_equal(pthread_self(),gui));snprintf(obj->text,sizeof(obj->text),"%s",text);}
void app_ui_label_fmt(lv_obj_t *obj,const char *format,...)
{assert(obj&&pthread_equal(pthread_self(),gui));va_list ap;va_start(ap,format);vsnprintf(obj->text,sizeof(obj->text),format,ap);va_end(ap);}
void lv_obj_set_size(lv_obj_t *obj,int32_t w,int32_t h){assert(obj&&w==32&&h==26&&pthread_equal(pthread_self(),gui));}
void lv_obj_set_style_text_align(lv_obj_t *obj,int align,int selector){assert(obj&&align==LV_TEXT_ALIGN_CENTER&&selector==0&&pthread_equal(pthread_self(),gui));}
void lv_obj_set_style_bg_color(lv_obj_t *obj,uint32_t color,int selector){assert(obj&&color==0x10192A&&selector==0&&pthread_equal(pthread_self(),gui));}
void lv_obj_set_style_bg_opa(lv_obj_t *obj,int opa,int selector){assert(obj&&opa==LV_OPA_90&&selector==0&&pthread_equal(pthread_self(),gui));}
uint32_t lv_color_hex(uint32_t color){return color;}
void lv_obj_delete(lv_obj_t *obj){assert(obj&&pthread_equal(pthread_self(),gui));++labels_deleted;free(obj);}
bool app_io_usb_storage_blocked(app_io_handle_t h) {(void)h;return atomic_load(&usb_blocked);}
void app_usb_storage_notice(app_ui_t *ui) {assert(ui&&pthread_equal(pthread_self(),gui));++usb_notices;}
aether_recorder_handle_t aether_recorder_get_default(void) { return recorder_present ? (void *)5 : NULL; }
esp_err_t aether_recorder_get_status(aether_recorder_handle_t h,aether_recorder_status_t *s)
{ assert(h==(void *)5); *s=(aether_recorder_status_t){.active=recorder_active}; return recorder_snapshot_error; }
QueueHandle_t xQueueCreate(unsigned n,size_t bytes)
{assert(n==16&&bytes<=32);struct test_queue *q=calloc(1,sizeof(*q));assert(q&&!pthread_mutex_init(&q->lock,NULL));q->bytes=bytes;return q;}
BaseType_t xQueueSend(QueueHandle_t q,const void *p,TickType_t ticks)
{
    assert(ticks==0);pthread_mutex_lock(&pause_lock);
    if(pause_send){send_entered=true;pthread_cond_broadcast(&pause_signal);while(!resume_send)pthread_cond_wait(&pause_signal,&pause_lock);}
    pthread_mutex_unlock(&pause_lock);pthread_mutex_lock(&q->lock);bool ok=q->count<16;
    if(ok){memcpy(q->data+((q->head+q->count)%16)*q->bytes,p,q->bytes);++q->count;}
    pthread_mutex_unlock(&q->lock);return ok;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *p,TickType_t ticks)
{assert(ticks==0);pthread_mutex_lock(&q->lock);bool ok=q->count!=0;if(ok){memcpy(p,q->data+q->head*q->bytes,q->bytes);q->head=(q->head+1)%16;--q->count;}pthread_mutex_unlock(&q->lock);return ok;}
void vQueueDelete(QueueHandle_t q){assert(!q->count);assert(!pthread_mutex_destroy(&q->lock));free(q);}
esp_err_t app_media_control_dispatch(aether_audio_player_handle_t a,aether_video_player_handle_t v,aether_nes_handle_t n,aether_bluetooth_remote_action_t action)
{assert(pthread_equal(pthread_self(),gui)&&a==(void*)1&&v==(void*)2&&n==(void*)3);++dispatched;dispatch_owner=owner;last_action=action;return ESP_OK;}
void lv_timer_set_period(lv_timer_t *t,uint32_t p){assert(t&&p==16&&pthread_equal(pthread_self(),gui));}
void lv_timer_ready(lv_timer_t *t){assert(t&&pthread_equal(pthread_self(),gui));}
void *lv_timer_get_user_data(lv_timer_t *t){assert(pthread_equal(pthread_self(),gui));return t;}
esp_err_t app_io_submit_request(app_io_handle_t h,const app_io_request_t *q){assert(h);submitted=*q;return submit_error;}
bool app_io_poll(app_io_handle_t h,app_io_result_t *r){assert(h);if(!has_result)return false;*r=result;has_result=false;return true;}
void app_io_result_free(app_io_result_t *r){(void)r;}
static void complete(app_ui_t *ui,const app_io_result_t *r)
{
    ++completed;assert(ui->media_control_blocked);
    assert(app_ui_queue_media_control(ui,AETHER_BT_REMOTE_PLAY)==ESP_ERR_INVALID_STATE);
    if(chain){chain=false;app_io_request_t next={.operation=APP_IO_PREPARE,.filter=APP_IO_NES};app_ui_request_io_ex(ui,&next,complete);}
    else if(r->error==ESP_OK)owner=2;
}
static void deliver(app_ui_t *ui)
{result=(app_io_result_t){.generation=ui->page_generation,.operation=submitted.operation,.error=ESP_OK};has_result=true;test_service_timer(ui);}
static app_ui_t create(void)
{app_ui_t ui={.audio_player=(void*)1,.video_player=(void*)2,.nes=(void*)3,.io=(void*)4,.page_generation=1,.screen=(void *)6,.power=(void *)7};return ui;}
static void test_integration(void)
{
    app_ui_t ui=create();ui.service_timer=&ui;assert(app_ui_media_control_init(&ui)==ESP_OK);owner=1;
    const app_io_request_t requests[]={
        {.operation=APP_IO_MUSIC},{.operation=APP_IO_VIDEO},{.operation=APP_IO_NES},
        {.operation=APP_IO_PREPARE,.filter=APP_IO_VIDEO},{.operation=APP_IO_CATALOG,.filter=APP_IO_MUSIC},
        {.operation=APP_IO_CATALOG,.filter=APP_IO_NES},
        {.operation=APP_IO_RECORDER},
    };
    for(unsigned i=0;i<sizeof(requests)/sizeof(*requests);++i){
        unsigned before=dispatched;
        assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_OK);
        app_ui_request_io_ex(&ui,&requests[i],complete);assert(ui.io_complete&&ui.io_media_pending&&ui.media_control_blocked);
        assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_NEXT)==ESP_ERR_INVALID_STATE);
        deliver(&ui);assert(!ui.media_control_blocked&&dispatched==before);
        assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PAUSE)==ESP_OK);test_service_timer(&ui);
        assert(dispatched==before+1&&dispatch_owner==2&&last_action==AETHER_BT_REMOTE_PAUSE);
    }
    chain=true;app_ui_request_io_ex(&ui,&requests[0],complete);deliver(&ui);
    assert(ui.media_control_blocked&&ui.io_complete&&ui.io_media_pending);deliver(&ui);assert(!ui.media_control_blocked);
    submit_error=ESP_ERR_INVALID_STATE;unsigned before=completed;app_ui_request_io_ex(&ui,&requests[0],complete);
    assert(completed==before+1&&!ui.io_complete&&!ui.media_control_blocked);submit_error=ESP_OK;
    app_io_request_t text={.operation=APP_IO_READ_TEXT};app_ui_request_io_ex(&ui,&text,NULL);
    assert(!ui.media_control_blocked);assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PAUSE)==ESP_OK);test_service_timer(&ui);
    app_ui_media_control_delete(&ui);
    puts("PASS real UI request/completion core: media IO gates, stale drops, callback launch protection, chained IO and submit failure");
}
static void *producer(void *p)
{assert(app_ui_queue_media_control(p,AETHER_BT_REMOTE_NEXT)==ESP_OK);return NULL;}
static void test_races_and_bounds(void)
{
    app_ui_t ui=create();ui.service_timer=&ui;assert(app_ui_media_control_init(&ui)==ESP_OK);
    for(unsigned i=0;i<16;++i)assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_NEXT)==ESP_OK);
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_NEXT)==ESP_ERR_TIMEOUT);unsigned before=dispatched;
    app_ui_media_control_process(&ui);assert(dispatched==before+16);
    pause_send=true;send_entered=resume_send=false;pthread_t thread;assert(!pthread_create(&thread,NULL,producer,&ui));
    pthread_mutex_lock(&pause_lock);while(!send_entered)pthread_cond_wait(&pause_signal,&pause_lock);pthread_mutex_unlock(&pause_lock);
    before=dispatched;app_ui_media_control_gate(&ui,true,true);app_ui_media_control_process(&ui);app_ui_media_control_gate(&ui,false,false);
    pthread_mutex_lock(&pause_lock);resume_send=true;pthread_cond_broadcast(&pause_signal);pthread_mutex_unlock(&pause_lock);
    assert(!pthread_join(thread,NULL));pause_send=false;app_ui_media_control_process(&ui);assert(dispatched==before);
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_OK);app_ui_media_control_gate(&ui,false,true);
    app_ui_media_control_process(&ui);assert(dispatched==before);
    assert(app_ui_queue_media_control(&ui,(aether_bluetooth_remote_action_t)99)==ESP_ERR_INVALID_ARG);
    app_ui_media_control_delete(&ui);assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_ERR_INVALID_ARG);
    app_ui_media_control_process(&ui);
    puts("PASS 16-slot nonblocking queue, producer late-send epoch race, page invalidation and no worker-thread LVGL calls");
}
static void test_recorder_ownership(void)
{
    app_ui_t ui=create();assert(app_ui_media_control_init(&ui)==ESP_OK);
    unsigned before=dispatched;
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_OK);
    recorder_present=recorder_active=true;
    app_ui_media_control_process(&ui);assert(dispatched==before);
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_NEXT)==ESP_ERR_INVALID_STATE);
    recorder_active=false;recorder_snapshot_error=ESP_ERR_TIMEOUT;
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_ERR_INVALID_STATE);
    recorder_snapshot_error=ESP_OK;
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_OK);
    app_ui_media_control_process(&ui);assert(dispatched==before+1);
    recorder_present=false;app_ui_media_control_delete(&ui);
    puts("PASS recorder ownership blocks queued/new remote playback and unknown snapshots, then restores commands after save");
}
static void usb_rejected(app_ui_t *ui,const app_io_result_t *r)
{assert(ui&&r->error==ESP_ERR_INVALID_STATE);++completed;}
static void test_usb_ownership(void)
{
    app_ui_t ui=create();ui.service_timer=&ui;assert(app_ui_media_control_init(&ui)==ESP_OK);
    const unsigned before=dispatched,notices=usb_notices;
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_OK);
    atomic_store(&usb_blocked,true);
    app_ui_media_control_process(&ui);assert(dispatched==before);
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_NEXT)==ESP_ERR_INVALID_STATE);
    app_io_request_t q={.operation=APP_IO_FILES};submit_error=ESP_ERR_INVALID_STATE;
    app_ui_request_io_ex(&ui,&q,usb_rejected);assert(usb_notices==notices+1);
    q.operation=APP_IO_READER_LOAD;
    app_ui_request_io_ex(&ui,&q,usb_rejected);assert(usb_notices==notices+1);
    submit_error=ESP_OK;q.operation=APP_IO_MUSIC;
    app_ui_request_io_ex(&ui,&q,usb_rejected);
    result=(app_io_result_t){.generation=ui.page_generation,.operation=q.operation,.error=ESP_OK};has_result=true;
    test_service_timer(&ui);assert(usb_notices==notices+2&&dispatched==before);
    atomic_store(&usb_blocked,false);
    test_service_timer(&ui);assert(!ui.media_control_blocked);
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_OK);
    app_ui_media_control_process(&ui);assert(dispatched==before+1);
    app_ui_media_control_delete(&ui);
    puts("PASS USB/quiesce ownership blocks new and queued remote playback; synchronous/asynchronous TF failures show notices while NVS loads remain separate");
}
static void test_release_feedback(void)
{
    app_ui_t ui=create();ui.service_timer=&ui;assert(app_ui_media_control_init(&ui)==ESP_OK);
    const unsigned before=dispatched,created=labels_created,deleted=labels_deleted;
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_OK);
    release_pending=true;test_service_timer(&ui);
    assert(ui.media_control_blocked&&dispatched==before&&ui.media_release_notice);
    assert(labels_created==created+1&&!strcmp(ui.media_release_notice->text,LV_SYMBOL_REFRESH));
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_ERR_INVALID_STATE);
    lv_obj_t *notice=ui.media_release_notice;
    release_error=ESP_ERR_TIMEOUT;test_service_timer(&ui);
    assert(ui.media_release_notice==notice&&labels_created==created+1);
    assert(!strcmp(notice->text,LV_SYMBOL_WARNING));
    app_io_request_t q={.operation=APP_IO_MUSIC};app_ui_request_io_ex(&ui,&q,complete);
    deliver(&ui);assert(ui.media_control_blocked&&dispatched==before&&ui.media_release_notice==notice);
    release_pending=false;release_error=ESP_OK;ui.io_media_pending=true;test_service_timer(&ui);
    assert(ui.media_control_blocked&&!ui.media_release_notice&&labels_deleted==deleted+1);
    ui.io_media_pending=false;test_service_timer(&ui);assert(!ui.media_control_blocked);
    assert(app_ui_queue_media_control(&ui,AETHER_BT_REMOTE_PLAY)==ESP_OK);test_service_timer(&ui);
    assert(dispatched==before+1&&labels_created==created+1&&labels_deleted==deleted+1);
    app_ui_media_control_delete(&ui);
    puts("PASS real service timer: release pending/error blocks remote dispatch through completion, retries reuse caption and drain deletes it");
}
static void test_power_and_user_key_service(void)
{
    app_ui_t ui=create();ui.service_timer=&ui;assert(app_ui_media_control_init(&ui)==ESP_OK);
    const unsigned before=dispatched,home_before=homes,deleted=labels_deleted;
    release_pending=true;test_service_timer(&ui);assert(ui.media_release_notice);
    power_status.shutdown_state=APP_POWER_SHUTDOWN_PREPARING;test_service_timer(&ui);
    assert(ui.media_control_blocked&&!ui.media_release_notice&&labels_deleted==deleted+1);
    release_pending=false;
    power_status.shutdown_state=APP_POWER_SHUTDOWN_REQUESTED;test_service_timer(&ui);assert(ui.media_control_blocked);
    power_status.shutdown_state=APP_POWER_SHUTDOWN_IDLE;power_status.shutdown_recovering=true;
    test_service_timer(&ui);assert(ui.media_control_blocked);
    power_status.shutdown_recovering=false;power_error=ESP_ERR_TIMEOUT;
    test_service_timer(&ui);assert(ui.media_control_blocked);
    power_error=ESP_OK;test_service_timer(&ui);assert(!ui.media_control_blocked);
    ui.user_key_exit_pending=true;ui.user_key_requested_generation=ui.page_generation-1;
    test_service_timer(&ui);assert(homes==home_before&&!ui.user_key_exit_pending&&!ui.user_key_exit_armed);
    ui.user_key_exit_pending=true;ui.user_key_requested_generation=ui.page_generation;ui.test_input_blocked=true;
    test_service_timer(&ui);assert(homes==home_before&&!ui.user_key_exit_pending&&ui.user_key_exit_armed);
    ui.test_input_blocked=false;ui.user_key_exit_pending=true;ui.user_key_requested_generation=ui.page_generation;
    test_service_timer(&ui);assert(homes==home_before+1&&!ui.user_key_exit_pending&&dispatched==before);
    test_service_timer(&ui);assert(homes==home_before+1);
    app_ui_media_control_delete(&ui);
    assert(labels_created==labels_deleted);
    puts("PASS real service timer: shutdown/recovery/unknown power gates commands; matching user-key exits once and stale/blocked requests expire");
}
static void test_intercom_ownership(void)
{
    app_ui_t ui = create();
    assert(app_ui_media_control_init(&ui) == ESP_OK);
    unsigned before = dispatched;
    assert(app_ui_queue_media_control(&ui, AETHER_BT_REMOTE_PLAY) == ESP_OK);
    intercom_busy = true;
    test_service_timer(&ui);
    assert(ui.media_control_blocked && dispatched == before);
    assert(app_ui_queue_media_control(&ui, AETHER_BT_REMOTE_PLAY) == ESP_ERR_INVALID_STATE);
    intercom_busy = false;
    test_service_timer(&ui);
    assert(app_ui_queue_media_control(&ui, AETHER_BT_REMOTE_PLAY) == ESP_OK);
    test_service_timer(&ui);
    assert(dispatched == before + 1);
    app_ui_media_control_delete(&ui);
}
int main(void){gui=pthread_self();test_integration();test_races_and_bounds();test_recorder_ownership();test_usb_ownership();test_release_feedback();test_power_and_user_key_service();test_intercom_ownership();return 0;}
