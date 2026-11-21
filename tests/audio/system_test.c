#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/app_audio_system.c"

static TickType_t ticks;
static bool fail_task,have_preferences,media_busy,peer_connected;
static aether_bluetooth_state_t peer_state=AETHER_BT_CONNECTED;
static uint8_t stored_master=100,stored_unmute=100,staged_master,staged_unmute;
static int fail_set_volume,fail_get_volume,fail_get_route,fail_commit;
static uint8_t actual_volume=100;
static ac101s_audio_output_t actual_output;
static ac101s_audio_input_t actual_input;
static bool usb_out, usb_in, capture_busy;
static ac101s_audio_sink_t usb_sink;
static ac101s_audio_source_t usb_source;
static unsigned volume_calls,route_calls,commits,control_calls,polls;
static unsigned change_during_volume,change_during_commit;
static aether_bluetooth_remote_event_t remote[40];
static size_t remote_count,remote_head;
static bool run_service_during_poll, quiesce_during_volume;

TickType_t xTaskGetTickCount(void){return ticks;}
BaseType_t xTaskCreate(void(*fn)(void*),const char*n,uint32_t stack,void*arg,int priority,TaskHandle_t*out)
{assert(fn==service&&stack==4096);(void)n;(void)arg;(void)priority;if(fail_task)return 0;*out=(void*)2;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t t){assert(t==(void*)2);}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t delay){(void)clear;(void)delay;return 0;}
esp_err_t nvs_open(const char*space,int mode,nvs_handle_t*out)
{assert(!strcmp(space,"sys_audio"));if(mode==NVS_READONLY&&!have_preferences)return ESP_ERR_NVS_NOT_FOUND;*out=1;staged_master=stored_master;staged_unmute=stored_unmute;return ESP_OK;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char*key,uint8_t*out){assert(h==1);*out=!strcmp(key,"master")?stored_master:stored_unmute;return ESP_OK;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char*key,uint8_t value){assert(h==1);if(!strcmp(key,"master"))staged_master=value;else{assert(!strcmp(key,"unmute"));staged_unmute=value;}return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);++commits;if(change_during_commit){unsigned v=change_during_commit;change_during_commit=0;assert(app_audio_system_set_volume((uint8_t)v,true)==ESP_OK);}if(fail_commit){--fail_commit;return ESP_FAIL;}stored_master=staged_master;stored_unmute=staged_unmute;have_preferences=true;return ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1);}
esp_err_t ac101s_audio_set_master_volume(ac101s_audio_handle_t h,uint8_t value)
{assert(h==(void*)1);++volume_calls;if(quiesce_during_volume){quiesce_during_volume=false;app_audio_system_quiesce_controls(true);}if(change_during_volume){unsigned v=change_during_volume;change_during_volume=0;assert(app_audio_system_set_volume((uint8_t)v,false)==ESP_OK);}if(fail_set_volume){--fail_set_volume;return ESP_ERR_TIMEOUT;}actual_volume=value;return ESP_OK;}
esp_err_t ac101s_audio_set_output(ac101s_audio_handle_t h,ac101s_audio_output_t output)
{assert(h==(void*)1);++route_calls;if(output==AC101S_OUTPUT_EXTERNAL&&!peer_connected&&actual_output!=output)return ESP_ERR_INVALID_STATE;actual_output=output;return ESP_OK;}
esp_err_t ac101s_audio_get_master_volume(ac101s_audio_handle_t h,uint8_t*out)
{assert(h==(void*)1);if(fail_get_volume){--fail_get_volume;return ESP_ERR_TIMEOUT;}*out=actual_volume;return ESP_OK;}
esp_err_t ac101s_audio_get_output(ac101s_audio_handle_t h,ac101s_audio_output_t*out)
{assert(h==(void*)1);if(fail_get_route){--fail_get_route;return ESP_ERR_TIMEOUT;}*out=actual_output;return ESP_OK;}
esp_err_t ac101s_audio_set_external_sink(ac101s_audio_handle_t h,const ac101s_audio_sink_t*sink)
{assert(h==(void*)1);(void)sink;return ESP_OK;}
esp_err_t ac101s_audio_set_usb_sink(ac101s_audio_handle_t h,const ac101s_audio_sink_t*sink)
{assert(h==(void*)1);usb_sink=sink?*sink:(ac101s_audio_sink_t){0};return ESP_OK;}
esp_err_t ac101s_audio_set_usb_source(ac101s_audio_handle_t h,const ac101s_audio_source_t*source)
{assert(h==(void*)1);usb_source=source?*source:(ac101s_audio_source_t){0};return ESP_OK;}
esp_err_t ac101s_audio_set_input(ac101s_audio_handle_t h,ac101s_audio_input_t input)
{assert(h==(void*)1);if(capture_busy&&input!=actual_input)return ESP_ERR_INVALID_STATE;actual_input=input;return ESP_OK;}
esp_err_t ac101s_audio_get_input(ac101s_audio_handle_t h,ac101s_audio_input_t*out)
{assert(h==(void*)1);*out=actual_input;return ESP_OK;}
bool aether_usb_audio_playback_connected(void){return usb_out;}
bool aether_usb_audio_capture_connected(void){return usb_in;}
esp_err_t aether_usb_audio_playback_start(uint32_t timeout){assert(timeout==1000);return ESP_OK;}
esp_err_t aether_usb_audio_playback_stop(uint32_t timeout){assert(timeout==500);return ESP_OK;}
esp_err_t aether_usb_audio_write(const void*pcm,size_t bytes,uint32_t timeout)
{assert(pcm&&bytes==4&&timeout==123);return ESP_OK;}
esp_err_t aether_usb_audio_capture_start(uint32_t timeout){assert(timeout==1000);return ESP_OK;}
esp_err_t aether_usb_audio_capture_stop(uint32_t timeout){assert(timeout==500);return ESP_OK;}
esp_err_t aether_usb_audio_capture_read(void*pcm,size_t bytes,uint32_t timeout)
{assert(pcm&&bytes==4&&timeout==123);memset(pcm,0x33,bytes);return ESP_OK;}
esp_err_t aether_bluetooth_get_sink(aether_bluetooth_handle_t h,ac101s_audio_sink_t*out){assert(h==(void*)3);memset(out,0,sizeof(*out));return ESP_OK;}
esp_err_t aether_bluetooth_get_status(aether_bluetooth_handle_t h,aether_bluetooth_status_t*out)
{assert(h==(void*)3);*out=(aether_bluetooth_status_t){.enabled=true,.connected=peer_connected,.state=peer_state,.sample_rate=peer_connected?48000:0,.channels=2};return ESP_OK;}
esp_err_t aether_bluetooth_set_volume(aether_bluetooth_handle_t h,uint8_t volume){assert(h==(void*)3&&volume<=100);return ESP_OK;}
bool aether_bluetooth_poll_remote(aether_bluetooth_handle_t h,aether_bluetooth_remote_event_t*out)
{assert(h==(void*)3);++polls;if(run_service_during_poll){run_service_during_poll=false;service_once();}if(remote_head==remote_count)return false;*out=remote[remote_head++];return true;}
static bool busy(void*context){assert(context==&media_busy);return media_busy;}
static void control(aether_bluetooth_remote_action_t action,void*context)
{assert(context==&control_calls);assert(action==AETHER_BT_REMOTE_PLAY||action==AETHER_BT_REMOTE_NEXT);++control_calls;}
static app_audio_system_snapshot_t view(void){app_audio_system_snapshot_t v;app_audio_system_get_snapshot(&v);return v;}

static void test_shutdown_controls(void)
{
    /* A request made while codec apply is in flight is not yet an owner ACK. */
    assert(app_audio_system_set_volume(46,false)==ESP_OK);
    quiesce_during_volume=true;service_once();
    assert(!view().controls_quiesced);
    assert(app_audio_system_set_volume(80,true)==ESP_ERR_INVALID_STATE);
    assert(app_audio_system_set_output(APP_AUDIO_OUTPUT_BLUETOOTH)==ESP_ERR_INVALID_STATE);
    service_once();assert(view().controls_quiesced);
    app_audio_system_quiesce_controls(false);assert(!view().controls_quiesced);
    /* A disconnected BT intention cannot complete, but it must not prevent
     * the owner's control barrier or persistence from completing. */
    peer_connected=false;
    assert(app_audio_system_set_output(APP_AUDIO_OUTPUT_BLUETOOTH)==ESP_OK);
    assert(app_audio_system_set_volume(44,true)==ESP_OK);
    app_audio_system_quiesce_controls(true);
    assert(!view().controls_quiesced);
    const unsigned routes=route_calls, volumes=volume_calls, controls=control_calls, saves=commits;
    remote_count=40;remote_head=0;
    for(unsigned i=0;i<remote_count;++i)remote[i]=(aether_bluetooth_remote_event_t){
        .action=i%2?AETHER_BT_REMOTE_VOLUME_UP:AETHER_BT_REMOTE_PLAY};
    ticks+=1000;fail_commit=1;service_once();
    assert(view().controls_quiesced&&view().pending&&view().master_volume==44&&view().output==APP_AUDIO_OUTPUT_BLUETOOTH);
    assert(route_calls==routes&&volume_calls==volumes&&control_calls==controls&&remote_head==16);
    assert(commits==saves+1&&view().persistence_pending&&view().persistence_error==ESP_FAIL);
    ticks+=1000;service_once();
    assert(commits==saves+2&&!view().persistence_pending&&stored_master==44);
    assert(route_calls==routes&&volume_calls==volumes&&control_calls==controls&&remote_head==32);
    /* Schedule the owner exactly when the resume caller begins draining its
     * queue. The caller must still hold the control gate at this boundary. */
    run_service_during_poll=true;app_audio_system_quiesce_controls(false);
    assert(!view().controls_quiesced&&remote_head==remote_count&&control_calls==controls);
    assert(view().master_volume==44&&view().output==APP_AUDIO_OUTPUT_BLUETOOTH&&view().pending);
    assert(route_calls==routes&&volume_calls==volumes);
    service_once();
    assert(control_calls==controls&&view().master_volume==44&&actual_output==AC101S_OUTPUT_SPEAKER);
    peer_connected=true;ticks+=250;service_once();
    assert(!view().pending&&actual_output==AC101S_OUTPUT_EXTERNAL&&view().master_volume==44);
    puts("PASS shutdown controls: owner ACK after in-flight apply, no hardware/remote control while gated, NVS retry still drains, resume preserves user intent and drops old PLAY");
}

int main(void)
{
    fail_task=true;assert(app_audio_system_init((void*)1)==ESP_ERR_NO_MEM);assert(!view().ready&&view().master_volume==70);
    fail_task=false;have_preferences=true;stored_master=0;stored_unmute=65;
    assert(app_audio_system_init((void*)1)==ESP_OK);assert(app_audio_system_init((void*)1)==ESP_ERR_INVALID_STATE);
    service_once();assert(actual_volume==0&&!view().pending&&view().applied_volume==0);
    assert(app_audio_system_toggle_mute(false)==ESP_OK);service_once();assert(actual_volume==65&&view().master_volume==65);
    /* Timeout must retain pending and retry after 250ms, including tick wrap. */
    ticks=UINT32_MAX-100;fail_set_volume=1;assert(app_audio_system_set_volume(40,false)==ESP_OK);service_once();
    assert(view().pending&&view().error==ESP_ERR_TIMEOUT&&view().applied_volume==65);unsigned calls=volume_calls;
    ticks+=249;service_once();assert(volume_calls==calls);ticks+=1;service_once();assert(actual_volume==40&&!view().pending);
    /* A failed getter must preserve the previous confirmed snapshot. */
    fail_get_volume=1;fail_get_route=1;assert(app_audio_system_set_volume(35,false)==ESP_OK);service_once();
    assert(actual_volume==35&&view().applied_volume==40&&view().pending&&view().applied_output==APP_AUDIO_OUTPUT_SPEAKER);
    ticks+=250;service_once();assert(view().applied_volume==35&&!view().pending);
    change_during_volume=80;assert(app_audio_system_set_volume(20,false)==ESP_OK);service_once();assert(actual_volume==20&&view().master_volume==80&&view().pending);service_once();assert(actual_volume==80&&!view().pending);
    /* Route stays external when the peer leaves: never unexpectedly externalize audio. */
    app_audio_system_set_bluetooth((void*)3);assert(app_audio_system_bluetooth()==(void*)3);
    assert(app_audio_system_set_output(APP_AUDIO_OUTPUT_BLUETOOTH)==ESP_OK);service_once();assert(actual_output==AC101S_OUTPUT_SPEAKER&&view().pending);
    calls=volume_calls;ticks+=1000;service_once();assert(volume_calls==calls); /* No 4Hz PA/gain writes while route is unavailable. */
    peer_connected=true;peer_state=AETHER_BT_DISCONNECTING;ticks+=250;service_once();assert(actual_output==AC101S_OUTPUT_SPEAKER&&view().pending);
    peer_state=AETHER_BT_CONNECTED;
    peer_connected=true;ticks+=250;service_once();assert(actual_output==AC101S_OUTPUT_EXTERNAL&&!view().pending);
    calls=route_calls;peer_connected=false;service_once();assert(route_calls==calls&&actual_output==AC101S_OUTPUT_EXTERNAL&&view().error==ESP_ERR_INVALID_STATE);
    peer_connected=true;service_once();assert(actual_output==AC101S_OUTPUT_EXTERNAL&&view().error==ESP_OK&&route_calls==calls);
    peer_state=AETHER_BT_STOPPING;service_once();assert(view().error==ESP_ERR_INVALID_STATE&&actual_output==AC101S_OUTPUT_EXTERNAL);
    peer_state=AETHER_BT_CONNECTED;service_once();assert(view().error==ESP_OK);
    assert(app_audio_system_set_output(APP_AUDIO_OUTPUT_SPEAKER)==ESP_OK);service_once();assert(actual_output==AC101S_OUTPUT_SPEAKER);
    /* Persistence debounce/media guard, commit failure backoff and newer writes. */
    app_audio_system_set_persistence_guard(busy,&media_busy);media_busy=true;
    assert(app_audio_system_set_volume(50,true)==ESP_OK);ticks+=2000;service_once();assert(!commits&&view().persistence_pending&&view().persistence_deferred);
    media_busy=false;fail_commit=1;service_once();assert(commits==1&&view().persistence_pending&&view().persistence_error==ESP_FAIL);
    ticks+=999;service_once();assert(commits==1);ticks+=1;service_once();assert(commits==2&&!view().persistence_pending&&stored_master==50);
    app_audio_system_set_volume(70,true);ticks+=1000;change_during_commit=85;service_once();assert(view().persistence_pending&&stored_master==70&&view().master_volume==85);
    ticks+=1000;service_once();assert(!view().persistence_pending&&stored_master==85&&stored_unmute==85);
    /* Event flood is bounded per service pass and absolute peer volume is ignored. */
    app_audio_system_set_media_control(control,&control_calls);
    remote[0]=(aether_bluetooth_remote_event_t){.action=AETHER_BT_REMOTE_VOLUME,.volume=1};
    remote[1]=(aether_bluetooth_remote_event_t){.action=AETHER_BT_REMOTE_VOLUME_UP};
    for(unsigned i=2;i<32;++i)remote[i]=(aether_bluetooth_remote_event_t){.action=AETHER_BT_REMOTE_PLAY};
    remote_count=32;
    service_once();assert(remote_head==16&&view().master_volume==90&&control_calls==14);service_once();assert(remote_head==32&&control_calls==30);
    assert(app_audio_system_set_volume(101,false)==ESP_ERR_INVALID_ARG);
    assert(app_audio_system_set_usb_audio(true)==ESP_OK);
    service_once();
    assert(!view().usb_playback_connected&&!view().usb_capture_connected);
    assert(app_audio_system_set_output(APP_AUDIO_OUTPUT_USB)==ESP_ERR_INVALID_STATE);
    assert(app_audio_system_set_input(APP_AUDIO_INPUT_USB)==ESP_ERR_INVALID_STATE);
    usb_out=usb_in=true;service_once();
    assert(view().usb_playback_connected&&view().usb_capture_connected);
    assert(app_audio_system_set_output(APP_AUDIO_OUTPUT_USB)==ESP_OK);
    assert(app_audio_system_set_input(APP_AUDIO_INPUT_USB)==ESP_OK);service_once();
    assert(actual_output==AC101S_OUTPUT_USB&&actual_input==AC101S_INPUT_USB&&!view().pending);
    uint32_t sample=0;
    assert(usb_sink.start(NULL)==ESP_OK&&usb_sink.write(NULL,&sample,4,123)==ESP_OK&&usb_sink.stop(NULL)==ESP_OK);
    assert(usb_source.start(NULL)==ESP_OK&&usb_source.read(NULL,&sample,4,123)==ESP_OK&&sample==0x33333333&&usb_source.stop(NULL)==ESP_OK);
    /* Disconnect never substitutes the board microphone, including while an
     * asynchronous recording is preparing but has not acquired capture yet. */
    capture_busy=true;usb_out=usb_in=false;service_once();
    assert(view().output==APP_AUDIO_OUTPUT_SPEAKER&&view().input==APP_AUDIO_INPUT_USB);
    assert(actual_input==AC101S_INPUT_USB);
    assert(app_audio_system_set_input(APP_AUDIO_INPUT_BOARD)==ESP_OK);service_once();
    assert(view().input==APP_AUDIO_INPUT_BOARD&&view().pending&&actual_input==AC101S_INPUT_USB);
    capture_busy=false;ticks+=250;service_once();
    assert(actual_input==AC101S_INPUT_BOARD&&actual_output==AC101S_OUTPUT_SPEAKER&&!view().pending);
    app_audio_system_quiesce_controls(true);usb_out=usb_in=true;service_once();
    assert(app_audio_system_set_input(APP_AUDIO_INPUT_USB)==ESP_ERR_INVALID_STATE);
    assert(app_audio_system_set_output(APP_AUDIO_OUTPUT_USB)==ESP_ERR_INVALID_STATE);
    app_audio_system_quiesce_controls(false);
    puts("PASS USB normalized sink/source registration, bounded wrappers, cached availability, disconnected-route rejection, active-capture ownership, unplug fallback and shutdown controls");
    test_shutdown_controls();
    puts("PASS production audio service: NVS/init, mute, timeout/getter retry+tick wrap, stale updates, retained BT route, commit/media ordering and bounded remote queue");
    return 0;
}
