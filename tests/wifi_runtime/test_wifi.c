#include "aether_wifi.h"
#include "test_sdk.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

struct test_task { pthread_t thread; void (*entry)(void *); void *context; };
struct test_queue { pthread_mutex_t lock; pthread_cond_t ready; unsigned capacity,size,count,head; unsigned char *data; };
static struct test_task task;
static event_handler_t wifi_handler,ip_handler;
static void *wifi_context,*ip_context;
esp_event_base_t WIFI_EVENT="wifi",IP_EVENT="ip";
static esp_netif_t netif;
static atomic_bool initialized,started,attempting,linked,flash_busy;
static atomic_bool hold_disconnect,hold_start_event;
static atomic_bool hold_scan,scan_entered,fail_ap_info,mismatch_ap;
static atomic_int fail_init,fail_scan,fail_commit,fail_task,fail_stop,fail_deinit,fail_disconnect;
static atomic_uint connect_calls,clear_calls,commit_calls,set_calls,erase_calls;
static atomic_uint scan_calls,scan_conflicts,disconnect_calls,scan_stop_calls;
static atomic_uint task_creates;
static atomic_uint wifi_init_calls,heap_log_queries,init_log_before,init_log_after;
static atomic_llong time_shift;
static wifi_config_t applied;
static unsigned char saved[128],staged[128];
static size_t saved_bytes,staged_bytes;
size_t heap_caps_get_free_size(unsigned caps)
{ assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT) || caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)); ++heap_log_queries; return initialized ? 40000 : 70000; }
size_t heap_caps_get_largest_free_block(unsigned caps)
{ assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT) || caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)); ++heap_log_queries; return initialized ? 20000 : 40000; }
void test_log(const char *tag,const char *format,...)
{
    assert(!strcmp(tag,"aether_wifi"));
    char line[256]; va_list args; va_start(args,format); vsnprintf(line,sizeof(line),format,args); va_end(args);
    if(strstr(line,"init before")) ++init_log_before;
    if(strstr(line,"init after")) ++init_log_after;
    /* Synthetic heap values are never printed as device measurements. */
}
static void delay(unsigned ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000+time_shift; }
static void advance(unsigned ms) { time_shift+=(int64_t)ms*1000; delay(130); }
QueueHandle_t xQueueCreate(unsigned capacity,size_t size)
{
    QueueHandle_t q=calloc(1,sizeof(*q)); assert(q); q->data=calloc(capacity,size); assert(q->data);
    q->capacity=capacity; q->size=(unsigned)size; pthread_mutex_init(&q->lock,NULL); pthread_cond_init(&q->ready,NULL); return q;
}
BaseType_t xQueueSend(QueueHandle_t q,const void *data,TickType_t ticks)
{
    (void)ticks; pthread_mutex_lock(&q->lock);
    if(q->count==q->capacity) { pthread_mutex_unlock(&q->lock); return pdFALSE; }
    memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,data,q->size); ++q->count;
    pthread_cond_signal(&q->ready); pthread_mutex_unlock(&q->lock); return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *data,TickType_t ticks)
{
    pthread_mutex_lock(&q->lock);
    if(!q->count && ticks) {
        struct timespec end; clock_gettime(CLOCK_REALTIME,&end); end.tv_nsec+=(long)ticks*1000000;
        end.tv_sec+=end.tv_nsec/1000000000; end.tv_nsec%=1000000000;
        pthread_cond_timedwait(&q->ready,&q->lock,&end);
    }
    if(!q->count) { pthread_mutex_unlock(&q->lock); return pdFALSE; }
    memcpy(data,q->data+q->head*q->size,q->size); q->head=(q->head+1)%q->capacity; --q->count;
    pthread_mutex_unlock(&q->lock); return pdTRUE;
}
void vQueueDelete(QueueHandle_t q) { pthread_cond_destroy(&q->ready); pthread_mutex_destroy(&q->lock); free(q->data); free(q); }
static void *task_entry(void *unused) { (void)unused; task.entry(task.context); return NULL; }
BaseType_t xTaskCreate(void (*entry)(void *),const char *name,uint32_t stack,void *context,int priority,TaskHandle_t *out)
{ (void)name; (void)priority; assert(stack>=8192); if(fail_task) return pdFALSE; ++task_creates; task.entry=entry; task.context=context; if(out) *out=&task; assert(!pthread_create(&task.thread,NULL,task_entry,NULL)); return pdPASS; }
void vTaskDelay(TickType_t ms) { delay(ms); }
void vTaskDelete(TaskHandle_t h) { (void)h; pthread_exit(NULL); }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &netif; }
void esp_netif_destroy_default_wifi(void *p) { assert(p==&netif && !initialized); }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base,int32_t id,event_handler_t cb,void *ctx,esp_event_handler_instance_t *out)
{ assert(id==-1); if(base==WIFI_EVENT) { wifi_handler=cb; wifi_context=ctx; } else { ip_handler=cb; ip_context=ctx; } *out=(void *)base; return ESP_OK; }
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base,int32_t id,esp_event_handler_instance_t instance)
{ assert(id==-1 && instance==(void *)base); if(base==WIFI_EVENT) wifi_handler=NULL; else ip_handler=NULL; return ESP_OK; }
static void emit_disconnect(unsigned reason)
{ linked=attempting=false; wifi_event_sta_disconnected_t e={reason}; wifi_handler(wifi_context,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&e); }
static void got_ip(void)
{ linked=true; attempting=false; ip_event_got_ip_t ip={.esp_netif=&netif,.ip_info.ip.addr=0x04030201}; ip_handler(ip_context,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip); }
esp_err_t esp_wifi_init(const wifi_init_config_t *c)
{
    assert(c && !initialized); ++wifi_init_calls;
    assert(c->static_rx_buf_num==4 && c->rx_ba_win==4 && c->tx_buf_type==0 && c->static_tx_buf_num==4);
    assert(c->dynamic_rx_buf_num==12 && c->cache_tx_buf_num==8 && c->rx_mgmt_buf_num==5 && c->mgmt_sbuf_num==6);
    if(fail_init) return ESP_ERR_NO_MEM;
    initialized=true; return ESP_OK;
}
esp_err_t esp_wifi_set_storage(int storage) { assert(initialized && storage==WIFI_STORAGE_RAM); return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { assert(initialized && mode==WIFI_MODE_STA); return ESP_OK; }
esp_err_t esp_wifi_start(void)
{ assert(initialized); started=true; if(!hold_start_event) wifi_handler(wifi_context,WIFI_EVENT,WIFI_EVENT_STA_START,NULL); return ESP_OK; }
esp_err_t esp_wifi_stop(void) { assert(initialized); if(fail_stop) return ESP_FAIL; started=attempting=linked=false; return ESP_OK; }
esp_err_t esp_wifi_deinit(void) { assert(initialized && !started); if(fail_deinit) return ESP_FAIL; initialized=false; return ESP_OK; }
esp_err_t esp_wifi_connect(void) { assert(initialized && started); attempting=true; ++connect_calls; return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { ++disconnect_calls; if(fail_disconnect) return ESP_FAIL; if(!attempting && !linked) return ESP_ERR_WIFI_NOT_CONNECT; if(!hold_disconnect) emit_disconnect(8); return ESP_OK; }
esp_err_t esp_wifi_set_config(int iface,wifi_config_t *config) { assert(iface==0 && started); applied=*config; return ESP_OK; }
esp_err_t esp_wifi_set_ps(int mode) { assert(mode==WIFI_PS_MIN_MODEM); return ESP_OK; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c,bool block)
{ assert(c && !block && started); ++scan_calls; scan_entered=true;while(hold_scan)delay(1);
  if(attempting) { ++scan_conflicts; return ESP_FAIL; } return fail_scan ? ESP_FAIL : ESP_OK; }
esp_err_t esp_wifi_scan_stop(void) { ++scan_stop_calls; return ESP_OK; }
esp_err_t esp_wifi_clear_ap_list(void) { ++clear_calls; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count,wifi_ap_record_t *records)
{
    assert(*count>=3); *count=3;
    for(unsigned i=0;i<3;++i) {
        records[i]=(wifi_ap_record_t){.primary=6,.rssi=(int8_t)(-30-i*10),.authmode=i==0 ? WIFI_AUTH_OPEN : i==1 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_ENTERPRISE};
        snprintf((char *)records[i].ssid,sizeof(records[i].ssid),"test-%u",i); records[i].bssid[5]=(uint8_t)i;
    }
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) { if(!linked || fail_ap_info) return ESP_ERR_WIFI_NOT_CONNECT; out->rssi=-42; memcpy(out->ssid,applied.sta.ssid,sizeof(applied.sta.ssid));if(mismatch_ap)out->ssid[0]^=1;memcpy(out->bssid,applied.sta.bssid,6);return ESP_OK; }
esp_err_t nvs_open(const char *space,int mode,nvs_handle_t *out) { assert(!strcmp(space,"aether_wifi")); if(mode==NVS_READONLY && !saved_bytes) return ESP_ERR_NVS_NOT_FOUND; *out=1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size)
{ assert(h==1 && !strcmp(key,"cred")); if(!saved_bytes) return ESP_ERR_NVS_NOT_FOUND; assert(*size>=saved_bytes); memcpy(out,saved,saved_bytes); *size=saved_bytes; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size)
{ assert(!flash_busy && h==1 && !strcmp(key,"cred") && size<=sizeof(staged)); memcpy(staged,data,size); staged_bytes=size; ++set_calls; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key)
{ assert(!flash_busy && h==1 && !strcmp(key,"cred")); staged_bytes=0; ++erase_calls; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h)
{ assert(!flash_busy && h==1); ++commit_calls; if(fail_commit) return ESP_FAIL; memcpy(saved,staged,staged_bytes); saved_bytes=staged_bytes; return ESP_OK; }
void nvs_close(nvs_handle_t h) { assert(h==1); }
static bool busy(void *ctx) { assert(ctx==(void *)7); return flash_busy; }
static aether_wifi_status_t snapshot(aether_wifi_handle_t h) { aether_wifi_status_t s; assert(aether_wifi_get_status(h,&s)==ESP_OK); return s; }
static aether_wifi_status_t wait_state(aether_wifi_handle_t h,aether_wifi_state_t expected)
{
    aether_wifi_status_t s; for(unsigned i=0;i<2000;++i) { s=snapshot(h); if(s.state==expected) return s; delay(1); }
    fprintf(stderr,"wifi expected state %u got %u error=%x\n",expected,s.state,s.last_error); abort();
}
static void wait_persist(aether_wifi_handle_t h)
{ for(unsigned i=0;i<2000;++i) { if(!snapshot(h).persistence_pending) return; delay(1); } abort(); }
static void finish_scan_event(void)
{ wifi_event_sta_scan_done_t complete={0}; wifi_handler(wifi_context,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&complete); }
static void saved_fixture(void)
{
    memset(saved,0,sizeof(saved)); saved[0]=1; saved[1]=WIFI_AUTH_WPA2_PSK;
    memcpy(saved+2,"remembered",10); memcpy(saved+35,"example-password",16); saved_bytes=99;
}
static void test_scan_priority(void)
{
    const aether_wifi_ap_t open={.ssid="explicit-open",.authmode=WIFI_AUTH_OPEN,.supported=true};
    const aether_wifi_ap_t secure={.ssid="explicit-secure",.authmode=WIFI_AUTH_WPA2_PSK,.supported=true};
    saved_fixture(); unsigned char remembered[99]; memcpy(remembered,saved,sizeof remembered);
    const unsigned commits=commit_calls,erases=erase_calls;
    aether_wifi_handle_t h=NULL; assert(aether_wifi_new(&h)==ESP_OK);
    assert(aether_wifi_set_enabled(h,true)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING);
    hold_disconnect=true;
    unsigned scans=scan_calls,calls=connect_calls,disconnects=disconnect_calls;
    assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_DISCONNECTING);
    for(unsigned i=0;i<8;++i) assert(aether_wifi_scan(h)==ESP_OK);
    delay(130); assert(scan_calls==scans && disconnect_calls==disconnects+1 && connect_calls==calls);
    emit_disconnect(8); wait_state(h,AETHER_WIFI_SCANNING);
    assert(scan_calls==scans+1 && !scan_conflicts && !snapshot(h).connected && !snapshot(h).retry_delay_ms && !snapshot(h).retry_count);
    const unsigned stops=scan_stop_calls;
    got_ip(); emit_disconnect(8); delay(130);
    assert(snapshot(h).state==AETHER_WIFI_SCANNING && !snapshot(h).connected && scan_stop_calls==stops);
    finish_scan_event(); wait_state(h,AETHER_WIFI_IDLE); advance(21000);
    assert(connect_calls==calls && snapshot(h).saved_network && saved_bytes==sizeof remembered && !memcmp(saved,remembered,sizeof remembered));
    assert(commit_calls==commits && erase_calls==erases);
    puts("PASS explicit scan cancels saved-network association, waits for disconnect, coalesces duplicates and rejects late IP/disconnect without erasing credentials or retrying");

    hold_disconnect=false;
    assert(aether_wifi_connect(h,&secure,"example-password",false)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING); got_ip(); wait_state(h,AETHER_WIFI_CONNECTED);
    scans=scan_calls; disconnects=disconnect_calls;
    assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_SCANNING);
    for(unsigned i=0;i<8;++i) assert(aether_wifi_scan(h)==ESP_OK);
    delay(130); assert(snapshot(h).connected && scan_calls==scans+1 && disconnect_calls==disconnects && snapshot(h).last_error==ESP_OK);
    finish_scan_event(); wait_state(h,AETHER_WIFI_CONNECTED);
    emit_disconnect(201); wait_state(h,AETHER_WIFI_CONNECTING); assert(snapshot(h).retry_delay_ms==1000);
    calls=connect_calls; assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_SCANNING); advance(1300);
    assert(connect_calls==calls && !snapshot(h).retry_delay_ms && !snapshot(h).retry_count);
    finish_scan_event(); wait_state(h,AETHER_WIFI_IDLE);
    puts("PASS connected background scans preserve the link; explicit offline scan cancels scheduled retry without racing SDK association");

    assert(aether_wifi_connect(h,&open,"",false)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING); got_ip(); wait_state(h,AETHER_WIFI_CONNECTED);
    hold_disconnect=true; assert(aether_wifi_connect(h,&secure,"example-password",false)==ESP_OK); wait_state(h,AETHER_WIFI_DISCONNECTING);
    scans=scan_calls; disconnects=disconnect_calls; calls=connect_calls;
    assert(aether_wifi_scan(h)==ESP_OK); delay(130); assert(disconnect_calls==disconnects && scan_calls==scans);
    emit_disconnect(8); wait_state(h,AETHER_WIFI_SCANNING);
    assert(!snapshot(h).connected && !snapshot(h).ip[0] && connect_calls==calls && scan_calls==scans+1);
    finish_scan_event(); wait_state(h,AETHER_WIFI_IDLE);
    hold_disconnect=false;
    assert(aether_wifi_connect(h,&open,"",false)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING); got_ip(); wait_state(h,AETHER_WIFI_CONNECTED);
    hold_disconnect=true; disconnects=disconnect_calls;
    ip_handler(ip_context,IP_EVENT,IP_EVENT_STA_LOST_IP,NULL); wait_state(h,AETHER_WIFI_CONNECTING);
    assert(disconnect_calls==disconnects+1); scans=scan_calls;
    assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_DISCONNECTING); delay(130);
    assert(disconnect_calls==disconnects+1 && scan_calls==scans);
    emit_disconnect(8); wait_state(h,AETHER_WIFI_SCANNING); finish_scan_event(); wait_state(h,AETHER_WIFI_IDLE);
    assert(saved_bytes==sizeof remembered && !memcmp(saved,remembered,sizeof remembered));
    puts("PASS scan supersedes a pending AP switch or joins LOST_IP cancellation exactly once, clears old connected/IP state and never overlaps a second disconnect");

    hold_disconnect=false;
    assert(aether_wifi_connect(h,&open,"",false)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING); got_ip(); wait_state(h,AETHER_WIFI_CONNECTED);
    assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_SCANNING);
    const unsigned background_stops=scan_stop_calls; calls=connect_calls;
    ip_handler(ip_context,IP_EVENT,IP_EVENT_STA_LOST_IP,NULL); wait_state(h,AETHER_WIFI_CONNECTING); delay(130);
    assert(!snapshot(h).scanning && scan_stop_calls==background_stops+1);
    advance(1300); assert(connect_calls==calls+1);
    finish_scan_event(); delay(130); assert(snapshot(h).state==AETHER_WIFI_CONNECTING);
    got_ip(); wait_state(h,AETHER_WIFI_CONNECTED);
    assert(aether_wifi_disconnect(h)==ESP_OK); wait_state(h,AETHER_WIFI_IDLE);
    puts("PASS LOST_IP during a connected background scan cancels its old deadline, preserves normal reconnect and ignores the canceled scan completion");

    for(unsigned action=0;action<4;++action) {
        hold_disconnect=false;
        assert(aether_wifi_connect(h,&open,"",false)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING);
        hold_disconnect=true; scans=scan_calls; calls=connect_calls;
        assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_DISCONNECTING);
        if(action==0) assert(aether_wifi_connect(h,&secure,"example-password",false)==ESP_OK);
        else if(action==1) assert(aether_wifi_disconnect(h)==ESP_OK);
        else if(action==2) assert(aether_wifi_set_enabled(h,false)==ESP_OK);
        else assert(aether_wifi_forget(h)==ESP_OK);
        delay(130);
        if(action==2) wait_state(h,AETHER_WIFI_OFF);
        emit_disconnect(8);
        if(action==0) {
            wait_state(h,AETHER_WIFI_CONNECTING); assert(connect_calls==calls+1 && !strcmp((char *)applied.sta.ssid,secure.ssid));
            got_ip(); wait_state(h,AETHER_WIFI_CONNECTED);
            hold_disconnect=false; assert(aether_wifi_disconnect(h)==ESP_OK); wait_state(h,AETHER_WIFI_IDLE);
        } else if(action==2) {
            delay(130); assert(snapshot(h).state==AETHER_WIFI_OFF);
            hold_disconnect=false; assert(aether_wifi_set_enabled(h,true)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING);
            assert(aether_wifi_disconnect(h)==ESP_OK); wait_state(h,AETHER_WIFI_IDLE);
        } else { wait_state(h,AETHER_WIFI_IDLE); if(action==3) wait_persist(h); }
        assert(scan_calls==scans && !snapshot(h).scanning);
    }
    assert(!saved_bytes); hold_disconnect=false;
    puts("PASS new connect, manual disconnect, OFF and forget revoke an awaiting scan; old cancellation events cannot launch it in the replaced session");

    assert(aether_wifi_connect(h,&open,"",false)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING);
    scans=scan_calls; hold_disconnect=true; assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_DISCONNECTING);
    for(unsigned i=0;i<8;++i) assert(aether_wifi_scan(h)==ESP_OK);
    advance(2200); wait_state(h,AETHER_WIFI_ERROR);
    assert(snapshot(h).last_error==ESP_ERR_TIMEOUT && !initialized && scan_calls==scans);
    emit_disconnect(8); delay(130); assert(!initialized && snapshot(h).state==AETHER_WIFI_ERROR);
    hold_disconnect=false; assert(aether_wifi_set_enabled(h,true)==ESP_OK); wait_state(h,AETHER_WIFI_IDLE);
    assert(aether_wifi_connect(h,&open,"",false)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING);
    fail_disconnect=1; assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_ERROR);
    assert(snapshot(h).last_error==ESP_FAIL && !initialized && scan_calls==scans); fail_disconnect=0;
    assert(aether_wifi_set_enabled(h,true)==ESP_OK); wait_state(h,AETHER_WIFI_IDLE);
    assert(aether_wifi_connect(h,&open,"",false)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING);
    fail_scan=1; assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_IDLE);
    assert(snapshot(h).last_error==ESP_FAIL && !snapshot(h).scanning); fail_scan=0;
    assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_SCANNING); finish_scan_event(); wait_state(h,AETHER_WIFI_IDLE);
    assert(!scan_conflicts && aether_wifi_del(h,2000)==ESP_OK); pthread_join(task.thread,NULL);
    puts("PASS missing/rejected cancellation and scan-start failure are bounded and retryable, duplicates cannot extend the cancel deadline, and scanning never enters SDK while connecting");

    saved_fixture(); hold_start_event=true; h=NULL; assert(aether_wifi_new(&h)==ESP_OK);
    assert(aether_wifi_set_enabled(h,true)==ESP_OK);
    for(unsigned i=0;i<2000&&!snapshot(h).enabled;++i) delay(1);
    assert(snapshot(h).enabled && snapshot(h).state==AETHER_WIFI_STARTING);
    calls=connect_calls; assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_SCANNING);
    wifi_handler(wifi_context,WIFI_EVENT,WIFI_EVENT_STA_START,NULL); delay(130);
    assert(connect_calls==calls && snapshot(h).state==AETHER_WIFI_SCANNING);
    finish_scan_event(); wait_state(h,AETHER_WIFI_IDLE); hold_start_event=false;
    assert(aether_wifi_del(h,2000)==ESP_OK); pthread_join(task.thread,NULL);
    puts("PASS explicit scan before delayed STA_START suppresses saved association and a late START cannot resurrect it; remembered credentials stay available");
}
static void test_peer_radio_lease(void)
{
    saved_fixture();
    const aether_wifi_ap_t unsaved={
        .ssid="temporary", .password_required=true, .supported=true,
        .authmode=WIFI_AUTH_WPA2_PSK, .channel=9, .bssid={2,1,0,1,2,3},
    };
    aether_wifi_handle_t h=NULL;
    assert(aether_wifi_new(&h)==ESP_OK);
    assert(aether_wifi_set_enabled(h,true)==ESP_OK);
    wait_state(h,AETHER_WIFI_CONNECTING);
    got_ip();
    wait_state(h,AETHER_WIFI_CONNECTED);
    assert(aether_wifi_connect(h,&unsaved,"temporary-password",false)==ESP_OK);
    wait_state(h,AETHER_WIFI_CONNECTING);
    got_ip();
    wait_state(h,AETHER_WIFI_CONNECTED);

    unsigned calls=connect_calls, commits=commit_calls;
    assert(aether_wifi_peer_acquire(h,2000)==ESP_OK);
    aether_wifi_status_t s=snapshot(h);
    assert(started && !linked && s.peer_radio && s.enabled_preference && !s.connected);
    assert(aether_wifi_scan(h)==ESP_ERR_INVALID_STATE && aether_wifi_peer_acquire(h,500)==ESP_ERR_INVALID_STATE);
    assert(aether_wifi_peer_release(h,2000)==ESP_OK);
    wait_state(h,AETHER_WIFI_CONNECTING);
    assert(!snapshot(h).peer_radio && connect_calls==calls+1 && commit_calls==commits);
    assert(!strcmp((char *)applied.sta.ssid,unsaved.ssid) &&
           !strcmp((char *)applied.sta.password,"temporary-password") &&
           applied.sta.channel==unsaved.channel && !memcmp(applied.sta.bssid,unsaved.bssid,6));
    got_ip();
    wait_state(h,AETHER_WIFI_CONNECTED);
    assert(aether_wifi_disconnect(h)==ESP_OK);
    wait_state(h,AETHER_WIFI_IDLE);
    calls=connect_calls;
    assert(aether_wifi_peer_acquire(h,2000)==ESP_OK);
    assert(aether_wifi_peer_release(h,2000)==ESP_OK);
    wait_state(h,AETHER_WIFI_IDLE);
    assert(connect_calls==calls && snapshot(h).saved_network);

    assert(aether_wifi_peer_acquire(h,2000)==ESP_OK);
    assert(aether_wifi_set_enabled(h,false)==ESP_OK);
    assert(!snapshot(h).enabled_preference);
    fail_stop=1;
    assert(aether_wifi_peer_release(h,0)==ESP_OK);
    wait_state(h,AETHER_WIFI_ERROR);
    assert(aether_wifi_peer_release(h,150)==ESP_ERR_TIMEOUT);
    assert(snapshot(h).peer_radio && started && initialized);
    fail_stop=0;
    assert(aether_wifi_peer_release(h,2000)==ESP_OK && !started && !initialized);
    assert(aether_wifi_peer_release(h,2000)==ESP_OK && !snapshot(h).peer_radio);
    assert(aether_wifi_del(h,2000)==ESP_OK);
    pthread_join(task.thread,NULL);
    puts("PASS peer radio serializes station access, restores unsaved association or manual disconnection, and retries failed teardown without undoing OFF");
}

int main(void)
{
    aether_wifi_handle_t h=NULL; assert(aether_wifi_new(&h)==ESP_OK && aether_wifi_get_default()==h);
    assert(!task_creates && snapshot(h).state==AETHER_WIFI_OFF);
    aether_wifi_ap_t idle_ap={.ssid="idle",.authmode=WIFI_AUTH_OPEN,.supported=true};
    assert(aether_wifi_scan(h)==ESP_ERR_INVALID_STATE && aether_wifi_disconnect(h)==ESP_ERR_INVALID_STATE);
    assert(aether_wifi_connect(h,&idle_ap,"",false)==ESP_ERR_INVALID_STATE);
    assert(aether_wifi_set_enabled(h,false)==ESP_OK && !task_creates);
    assert(aether_wifi_del(h,0)==ESP_OK && !aether_wifi_get_default() && !wifi_handler && !ip_handler);
    puts("PASS initial OFF needs no task/stack; unavailable commands fail explicitly and no-task delete releases resources");
    h=NULL; assert(aether_wifi_new(&h)==ESP_OK && aether_wifi_get_default()==h);
    assert(snapshot(h).state==AETHER_WIFI_OFF && !initialized);
    assert(aether_wifi_set_persistence_guard(h,busy,(void *)7)==ESP_OK);
    fail_task=1; assert(aether_wifi_set_enabled(h,true)==ESP_ERR_NO_MEM && !task_creates);
    assert(snapshot(h).state==AETHER_WIFI_ERROR && snapshot(h).last_error==ESP_ERR_NO_MEM && !initialized);
    fail_task=0;
    assert(aether_wifi_set_enabled(h,true)==ESP_OK); wait_state(h,AETHER_WIFI_IDLE);
    assert(task_creates==1);
    assert(aether_wifi_scan(h)==ESP_OK); wait_state(h,AETHER_WIFI_SCANNING);
    wifi_event_sta_scan_done_t scan={0}; wifi_handler(wifi_context,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&scan);
    wait_state(h,AETHER_WIFI_IDLE); aether_wifi_ap_t open,secure,enterprise;
    assert(aether_wifi_get_ap(h,0,&open)==ESP_OK && !open.password_required && open.supported);
    assert(aether_wifi_get_ap(h,1,&secure)==ESP_OK && secure.rssi==-40 && secure.password_required);
    assert(aether_wifi_get_ap(h,2,&enterprise)==ESP_OK && !enterprise.supported);
    assert(aether_wifi_connect(h,&secure,"short",true)==ESP_ERR_INVALID_ARG);
    assert(aether_wifi_connect(h,&enterprise,"example-password",true)==ESP_ERR_INVALID_ARG);
    puts("PASS first-enable task OOM retries safely; actual owner task scans/AP RSSI/security and password validation");
    flash_busy=true; assert(aether_wifi_connect(h,&secure,"example-password",true)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING);
    got_ip(); wait_state(h,AETHER_WIFI_CONNECTED); delay(20);
    assert(!strcmp(snapshot(h).ip,"1.2.3.4") && snapshot(h).persistence_pending && !commit_calls);
    assert(aether_wifi_set_enabled(h,false)==ESP_OK); wait_state(h,AETHER_WIFI_OFF);
    advance(1200); assert(!commit_calls && !initialized);
    flash_busy=false; advance(1200); wait_persist(h); assert(commit_calls==1 && snapshot(h).saved_network);
    assert(aether_wifi_set_enabled(h,true)==ESP_OK); wait_state(h,AETHER_WIFI_CONNECTING);
    assert(!strcmp((char *)applied.sta.password,"example-password")); got_ip(); wait_state(h,AETHER_WIFI_CONNECTED);
    puts("PASS connected IP and deferred credential commit: Flash busy guard, pending save survives WiFi OFF, saved-network reconnect");
    flash_busy=true; assert(aether_wifi_forget(h)==ESP_OK); wait_state(h,AETHER_WIFI_IDLE);
    assert(snapshot(h).persistence_pending && !snapshot(h).saved_network && erase_calls==0);
    unsigned calls=connect_calls;
    aether_wifi_set_enabled(h,false); wait_state(h,AETHER_WIFI_OFF);
    aether_wifi_set_enabled(h,true); wait_state(h,AETHER_WIFI_IDLE); assert(connect_calls==calls);
    flash_busy=false; fail_commit=1; advance(1200); unsigned commits=commit_calls; assert(commits==2 && snapshot(h).storage_error==ESP_FAIL);
    advance(400); assert(commit_calls==commits); fail_commit=0; advance(1200); wait_persist(h);
    assert(!saved_bytes && !snapshot(h).saved_network);
    puts("PASS forget immediately clears reconnect intent; deferred erase suppresses stale NVS reload, failed commit backs off then recovers");
    aether_wifi_connect(h,&open,"",false); wait_state(h,AETHER_WIFI_CONNECTING); got_ip(); wait_state(h,AETHER_WIFI_CONNECTED);
    for(unsigned i=0;i<5;++i) {
        calls=connect_calls; emit_disconnect(201); wait_state(h,AETHER_WIFI_CONNECTING);
        for(unsigned attempt=0;attempt<2000 && snapshot(h).retry_count!=i+1;++attempt) delay(1);
        assert(snapshot(h).retry_count==i+1); advance((1000U<<i)+150); assert(connect_calls==calls+1);
    }
    emit_disconnect(201); wait_state(h,AETHER_WIFI_ERROR); calls=connect_calls; advance(20000); assert(connect_calls==calls);
    puts("PASS reconnect uses bounded 1/2/4/8/16-second backoff and stops after five retries");
    aether_wifi_connect(h,&open,"",false); wait_state(h,AETHER_WIFI_CONNECTING);
    advance(20200); assert(snapshot(h).retry_count==1); delay(50); assert(snapshot(h).retry_count==1);
    aether_wifi_disconnect(h); wait_state(h,AETHER_WIFI_IDLE); calls=connect_calls; advance(2000); assert(connect_calls==calls);
    aether_wifi_scan(h); wait_state(h,AETHER_WIFI_SCANNING); advance(15200); wait_state(h,AETHER_WIFI_IDLE); assert(snapshot(h).last_error==ESP_ERR_TIMEOUT);
    aether_wifi_set_enabled(h,false); wait_state(h,AETHER_WIFI_OFF); fail_init=1;
    aether_wifi_set_enabled(h,true); wait_state(h,AETHER_WIFI_ERROR); assert(!initialized);
    fail_init=0; aether_wifi_set_enabled(h,true); wait_state(h,AETHER_WIFI_IDLE);
    /* A cancellation timeout/rejection cannot start a second association on
     * the still-owned link. Repeated selections only replace desired AP. */
    aether_wifi_connect(h,&open,"",false);wait_state(h,AETHER_WIFI_CONNECTING);got_ip();wait_state(h,AETHER_WIFI_CONNECTED);
    hold_disconnect=true;calls=connect_calls;
    aether_wifi_connect(h,&secure,"example-password",false);wait_state(h,AETHER_WIFI_DISCONNECTING);
    aether_wifi_connect(h,&open,"",false);delay(50);assert(snapshot(h).state==AETHER_WIFI_DISCONNECTING && connect_calls==calls);
    advance(2200);wait_state(h,AETHER_WIFI_ERROR);assert(snapshot(h).last_error==ESP_ERR_TIMEOUT && !initialized && connect_calls==calls);
    hold_disconnect=false;aether_wifi_set_enabled(h,true);wait_state(h,AETHER_WIFI_IDLE);
    aether_wifi_connect(h,&open,"",false);wait_state(h,AETHER_WIFI_CONNECTING);got_ip();wait_state(h,AETHER_WIFI_CONNECTED);
    fail_disconnect=1;calls=connect_calls;aether_wifi_connect(h,&secure,"example-password",false);wait_state(h,AETHER_WIFI_ERROR);
    assert(snapshot(h).last_error==ESP_FAIL && !initialized && connect_calls==calls);fail_disconnect=0;
    aether_wifi_set_enabled(h,true);wait_state(h,AETHER_WIFI_IDLE);
    aether_wifi_scan(h);wait_state(h,AETHER_WIFI_SCANNING);aether_wifi_forget(h);wait_state(h,AETHER_WIFI_IDLE);
    assert(!snapshot(h).scanning);wait_persist(h);
    puts("PASS cancel timeout/rejection closes owned link before explicit retry; fast AP replacements cannot overlap and forget cancels scan");
    /* Teardown failures retain ownership and cannot init a second driver. */
    unsigned inits=wifi_init_calls;fail_stop=1;
    aether_wifi_set_enabled(h,false);wait_state(h,AETHER_WIFI_ERROR);assert(initialized && started);
    aether_wifi_set_enabled(h,true);delay(130);assert(wifi_init_calls==inits && initialized && started);
    fail_stop=0;fail_deinit=1;aether_wifi_set_enabled(h,false);wait_state(h,AETHER_WIFI_ERROR);delay(130);assert(initialized && !started);
    aether_wifi_set_enabled(h,true);delay(130);assert(wifi_init_calls==inits && initialized && !started);
    fail_deinit=0;aether_wifi_set_enabled(h,true);wait_state(h,AETHER_WIFI_IDLE);assert(wifi_init_calls==inits+1);
    puts("PASS failed stop/deinit retain real SDK ownership; enable cannot re-init until cleanup succeeds");
    /* Old DHCP notifications cannot create phantom success or save the new
     * password without a matching real association. */
    aether_wifi_connect(h,&secure,"example-password",true);wait_state(h,AETHER_WIFI_CONNECTING);
    calls=connect_calls;ip_handler(ip_context,IP_EVENT,IP_EVENT_STA_LOST_IP,NULL);delay(130);
    assert(snapshot(h).retry_count==0 && connect_calls==calls);
    fail_ap_info=true;got_ip();delay(130);assert(snapshot(h).state==AETHER_WIFI_CONNECTING&&!snapshot(h).persistence_pending);
    fail_ap_info=false;mismatch_ap=true;got_ip();delay(130);assert(snapshot(h).state==AETHER_WIFI_CONNECTING);
    mismatch_ap=false;got_ip();wait_state(h,AETHER_WIFI_CONNECTED);wait_persist(h);
    /* Hold one actual SDK operation so OFF, ON, old IP and the old driver's
     * disconnect notification are deterministically queued across epochs. */
    hold_scan=true;scan_entered=false;aether_wifi_scan(h);
    for(unsigned i=0;i<2000&&!scan_entered;++i)delay(1);
    assert(scan_entered);calls=connect_calls;
    aether_wifi_set_enabled(h,false);aether_wifi_set_enabled(h,true);got_ip();hold_scan=false;
    wait_state(h,AETHER_WIFI_CONNECTING);
    assert(!snapshot(h).connected&&!snapshot(h).ip[0]&&snapshot(h).retry_count==0&&connect_calls==calls+1);
    got_ip();wait_state(h,AETHER_WIFI_CONNECTED);aether_wifi_forget(h);wait_state(h,AETHER_WIFI_IDLE);wait_persist(h);
    puts("PASS stale IP/AP mismatch and LOST_IP during association cannot publish success; OFF/ON drops queued old-epoch IP/disconnect events");
    assert(aether_wifi_del(h,2000)==ESP_OK); pthread_join(task.thread,NULL);
    assert(!initialized && !wifi_handler && !ip_handler && !aether_wifi_get_default());
    puts("PASS association/scan timeouts, manual disconnect, initialization failure/retry and clean driver/netif/queue teardown");
    test_scan_priority();
    /* A remembered credential can be forgotten before ever enabling RF. */
    memset(saved,0,sizeof(saved)); saved[0]=1; saved[1]=WIFI_AUTH_WPA2_PSK;
    memcpy(saved+2,"remembered",10); memcpy(saved+35,"example-password",16); saved_bytes=99;
    unsigned created=task_creates; h=NULL; assert(aether_wifi_new(&h)==ESP_OK);
    assert(snapshot(h).saved_network && !initialized && task_creates==created);
    aether_wifi_set_persistence_guard(h,busy,(void *)7); flash_busy=true;
    assert(aether_wifi_forget(h)==ESP_OK);
    for(unsigned i=0;i<2000 && !snapshot(h).persistence_pending;++i) delay(1);
    assert(snapshot(h).persistence_pending && !snapshot(h).saved_network && task_creates==created+1 && !initialized);
    assert(aether_wifi_del(h,0)==ESP_ERR_INVALID_STATE);
    flash_busy=false; advance(1200); wait_persist(h);
    assert(!saved_bytes && !initialized && aether_wifi_del(h,2000)==ESP_OK); pthread_join(task.thread,NULL);
    puts("PASS remembered network can be forgotten while RF remains OFF; lazy owner honors Flash guard and persists erase before delete");
    assert(wifi_init_calls && init_log_before==wifi_init_calls && init_log_after==wifi_init_calls && heap_log_queries==wifi_init_calls*8);
    puts("PASS actual driver-init config is RX4/BA4/TX4/dynamic12/cache8/mgmt5/short6; before/after diagnostics cover success and failed init recovery");
    test_peer_radio_lease();
    return 0;
}
