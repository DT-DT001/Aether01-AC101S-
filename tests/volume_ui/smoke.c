#include "app_ui_modal.h"
#include "app_screen.h"
#include "aether_usb_audio.h"
#include "aether_usb_storage.h"
#include "app_ui_internal.h"
#include "app_audio_system.h"
#include "app_otg_control.h"
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
#include "aether_bluetooth.h"
#include "aether_wifi.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const lv_font_t font_16,font_24;
static uint16_t buffer[480*40],framebuffer[480*480];
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
static const char *output_directory;
#endif
static lv_point_t pointer;
static bool held;
static int64_t now_us;
static unsigned master_updates,master_persist,music_updates;
static unsigned music_seek_requests;
static uint32_t music_seek_position;
static uint8_t requested_master,saved_master;
static app_audio_system_snapshot_t master={.ready=true,.master_volume=17,.applied_volume=17};
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
static aether_wifi_status_t station={.enabled=true,.connected=true};
static aether_bluetooth_hid_status_t bluetooth_input={.requested=true};
aether_wifi_handle_t aether_wifi_get_default(void) { return (void *)1; }
esp_err_t aether_wifi_get_status(aether_wifi_handle_t handle, aether_wifi_status_t *status)
{ assert(handle); *status=station; return ESP_OK; }
aether_bluetooth_handle_t app_audio_system_bluetooth(void) { return (void *)1; }
esp_err_t aether_bluetooth_get_status(aether_bluetooth_handle_t handle, aether_bluetooth_status_t *status)
{ assert(handle); *status=(aether_bluetooth_status_t){0}; return ESP_OK; }
esp_err_t aether_bluetooth_hid_get_status(aether_bluetooth_handle_t handle, aether_bluetooth_hid_status_t *status)
{ assert(handle); *status=bluetooth_input; return ESP_OK; }
#endif
static aether_audio_player_status_t music={.active=true,.state=AETHER_AUDIO_PLAYING,.volume=100,.sample_rate=44100,.channels=2,.duration_ms=120000,.path="/sdcard/Music/test.mp3"};
static app_io_usb_status_t usb={.media_present=true,.capacity_bytes=8ULL*1024*1024*1024};
static unsigned usb_commands,usb_confirmed_commands;
static aether_usb_host_status_t otg;
static app_otg_power_status_t otg_power = {.vbus_adc_trusted=true,.age_ms=UINT32_MAX};
static unsigned otg_commands, manual_otg_commands;
static esp_err_t manual_otg_submit_error;
esp_err_t app_otg_get_status(aether_usb_host_status_t *s){*s=otg;return ESP_OK;}
esp_err_t app_otg_get_power_status(app_otg_power_status_t *s){*s=otg_power;return ESP_OK;}
esp_err_t app_otg_set_enabled(bool enabled){++otg_commands;otg.enabled=enabled;otg.state=enabled?AETHER_OTG_WAIT_SINK:AETHER_OTG_OFF;return ESP_OK;}
esp_err_t app_otg_set_manual_enabled(bool enabled)
{
    ++manual_otg_commands;
    if (manual_otg_submit_error != ESP_OK) return manual_otg_submit_error;
    otg_power.manual_source_requested = enabled;
    otg_power.manual_source_failed = false;
    otg_power.last_error = ESP_OK;
    otg.enabled = enabled;
    otg.cleanup_pending = !enabled;
    otg.state = enabled ? AETHER_OTG_WAIT_SINK : AETHER_OTG_STOPPING;
    return ESP_OK;
}

static esp_err_t usb_submit_error;
static app_power_status_t power_status;
static bool quiescing,lvgl_locked,lvgl_lock_error,reader_saved,resume_accepted;
static esp_err_t quiesce_wait_error;
bool lvgl_port_lock(uint32_t ms){assert(ms==50&&!lvgl_locked);if(lvgl_lock_error)return false;lvgl_locked=true;return true;}
void lvgl_port_unlock(void){assert(lvgl_locked);lvgl_locked=false;}
esp_err_t app_io_quiesce_begin(app_io_handle_t h){assert(h&&!lvgl_locked);if(usb.blocked)return ESP_ERR_INVALID_STATE;quiescing=true;return ESP_OK;}
esp_err_t app_io_quiesce_wait(app_io_handle_t h,uint32_t ms){assert(h&&ms==1234&&!lvgl_locked&&reader_saved);return quiesce_wait_error;}
esp_err_t app_io_resume(app_io_handle_t h){assert(h&&!lvgl_locked);resume_accepted=true;return ESP_OK;}
esp_err_t app_io_usb_get_status(app_io_handle_t h,app_io_usb_status_t *s){(void)h;*s=usb;return ESP_OK;}
bool app_io_is_quiescing(app_io_handle_t h){(void)h;return quiescing;}
bool app_io_usb_storage_blocked(app_io_handle_t h){(void)h;return quiescing||usb.blocked;}
void app_ui_media_control_gate(app_ui_t *ui,bool b,bool i){assert(ui);(void)b;(void)i;}
esp_err_t app_io_usb_set_enabled(app_io_handle_t h,bool b){assert(h);++usb_commands;++usb.request_id;if(usb_submit_error)return usb_submit_error;usb.pending=usb.blocked=true;usb.requested_enabled=b;usb.last_error=ESP_OK;return ESP_OK;}
esp_err_t app_io_usb_exit_confirmed(app_io_handle_t h,uint32_t id){assert(h);if(usb.pending||quiescing||!usb.active||usb.request_id!=id)return ESP_ERR_INVALID_STATE;++usb_confirmed_commands;return app_io_usb_set_enabled(h,false);}
uint32_t app_power_get_timeout(const app_power_t *p){return p->timeout;}
uint8_t app_power_get_brightness(const app_power_t *p){return p->brightness;}
esp_err_t app_power_set_timeout(app_power_t *p,uint32_t t,bool b){(void)b;p->timeout=t;return ESP_OK;}
esp_err_t app_power_set_brightness(app_power_t *p,uint8_t v,bool b){(void)b;p->brightness=v;return ESP_OK;}
esp_err_t app_power_sleep_now(app_power_t *p){(void)p;return ESP_OK;}
esp_err_t app_power_get_status(const app_power_t *p,app_power_status_t *s){(void)p;*s=power_status;return ESP_OK;}
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *s){*s=master;}
esp_err_t app_audio_system_set_volume(uint8_t v,bool persist){assert(v<=100);master.master_volume=v;requested_master=v;++master_updates;if(persist){saved_master=v;++master_persist;}return ESP_OK;}
esp_err_t app_audio_system_toggle_mute(bool b){return app_audio_system_set_volume(master.master_volume?0:70,b);}
esp_err_t app_audio_system_set_output(app_audio_output_t o){master.output=o;return ESP_OK;}
bool aether_storage_is_mounted(aether_storage_handle_t h){(void)h;return true;}
const char *esp_err_to_name(esp_err_t e)
{
    if (e == ESP_ERR_INVALID_STATE) return "ESP_ERR_INVALID_STATE";
    if (e == ESP_ERR_TIMEOUT) return "ESP_ERR_TIMEOUT";
    return e ? "ERROR" : "OK";
}
int64_t esp_timer_get_time(void){return now_us;}
size_t strlcpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}
const lv_font_t *app_ui_chinese_font_size(unsigned s){return s>=24?&font_24:&font_16;}
const lv_font_t *app_ui_chinese_font(void){return &font_16;}
void app_ui_label_text(lv_obj_t *o,const char *s){if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
void app_ui_label_fmt(lv_obj_t *o,const char *f,...){char s[512];va_list a;va_start(a,f);vsnprintf(s,sizeof(s),f,a);va_end(a);app_ui_label_text(o,s);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *s,lv_align_t a,int32_t x,int32_t y,uint32_t c,const lv_font_t *f){lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_style_text_font(o,f,0);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_align(o,a,x,y);return o;}
void app_ui_show_drawer(app_ui_t *ui) { app_ui_begin_page(ui, "应用列表"); }
#ifdef AETHER_VOLUME_REAL_CHROME
#include "app_tools.h"
void app_tools_get_snapshot(app_tools_snapshot_t *snapshot)
{
    *snapshot = (app_tools_snapshot_t){.ready = true, .battery_updated_ms = (uint32_t)(now_us / 1000),
        .battery = {.presence_valid = true, .battery_present = true, .percentage_valid = true, .percentage = 80}};
}
void app_ui_set_user_key_exit(app_ui_t *ui, bool enabled) {(void)ui; (void)enabled;}
lv_obj_t *app_ui_new_screen(app_ui_t *ui)
#else
static void test_header_back(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (lv_obj_get_parent(lv_event_get_current_target_obj(event)) != ui->screen) return;
    if (ui->page_back) ui->page_back(ui);
    else app_ui_show_drawer(ui);
}
lv_obj_t *app_ui_begin_page(app_ui_t *ui,const char *title)
#endif
{
    if(ui->timer){lv_timer_delete(ui->timer);ui->timer=NULL;}if(ui->page_cleanup){ui->page_cleanup(ui);ui->page_cleanup=NULL;}
    ui->page_can_exit=NULL;
    ui->page_back=NULL;
    ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen,lv_color_hex(0x101321),0);lv_obj_set_style_bg_opa(ui->screen,LV_OPA_COVER,0);
    lv_obj_set_style_text_font(ui->screen,&font_16,0);
#ifdef AETHER_VOLUME_REAL_CHROME
    return ui->screen;
#else
    (void)title;
    lv_obj_t *back = lv_button_create(ui->screen);
    lv_obj_set_size(back, 48, 44);
    lv_obj_set_pos(back, 14, 12);
    lv_obj_add_event_cb(back, test_header_back, LV_EVENT_CLICKED, ui);
    app_ui_add_text(back, LV_SYMBOL_LEFT, LV_ALIGN_CENTER, 0, 0, 0xBFD7FF, &lv_font_montserrat_16);
    ui->content=lv_obj_create(ui->screen);lv_obj_set_size(ui->content,448,394);lv_obj_align(ui->content,LV_ALIGN_BOTTOM_MID,0,-14);
    lv_obj_set_style_pad_all(ui->content,20,0);lv_obj_set_style_border_width(ui->content,0,0);
    lv_obj_set_style_bg_color(ui->content,lv_color_hex(0x10192A),0);
    app_ui_load_screen(ui->screen);return ui->content;
#endif
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t ms){ui->timer=lv_timer_create(cb,ms,ui);}
void app_ui_show_home(app_ui_t *ui){assert(lvgl_locked);app_ui_begin_page(ui,"主页");}
void app_open_nes_controls(app_ui_t *ui){(void)ui;}
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
void app_open_wifi(app_ui_t *ui) { app_ui_begin_page(ui, "Wi-Fi 网络"); }
void app_open_bluetooth(app_ui_t *ui) { app_ui_begin_page(ui, "蓝牙"); }
void app_open_status(app_ui_t *ui) { app_ui_begin_page(ui, "设备状态"); }
#endif
static unsigned music_page_releases;
esp_err_t app_io_release_media(app_io_handle_t io){assert(io);++music_page_releases;music.active=false;music.state=AETHER_AUDIO_STOPPED;music.track_count=0;music.path[0]=0;music.position_ms=music.duration_ms=0;return ESP_OK;}
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
void app_ui_request_io_ex(app_ui_t *ui,const app_io_request_t *q,void (*cb)(app_ui_t *,const app_io_result_t *))
{app_io_result_t r={.operation=q->operation,.error=ESP_OK};cb(ui,&r);}
#endif
esp_err_t aether_audio_player_get_status(aether_audio_player_handle_t h,aether_audio_player_status_t *s){assert(h);*s=music;return ESP_OK;}
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
esp_err_t aether_audio_player_get_resume_path(aether_audio_player_handle_t h,char *path,size_t size)
{ assert(h && path && size); return ESP_ERR_NOT_FOUND; }
esp_err_t aether_audio_player_restore(aether_audio_player_handle_t h)
{ assert(h); return ESP_ERR_NOT_FOUND; }
esp_err_t aether_audio_player_play(aether_audio_player_handle_t h,const char *path,int volume)
{
    assert(h && path && volume >= 0 && volume <= 100);
    strlcpy(music.path,path,sizeof(music.path));
    music.volume=(uint8_t)volume;
    music.active=true;
    music.state=AETHER_AUDIO_PLAYING;
    return ESP_OK;
}
#endif
esp_err_t aether_audio_player_set_volume(aether_audio_player_handle_t h,int v){assert(h&&v>=0&&v<=100);music.volume=(uint8_t)v;++music_updates;return ESP_OK;}
static unsigned ui_playlist_copies,ui_playlist_selects;
static size_t ui_playlist_selected_index;
esp_err_t aether_audio_player_set_playlist(aether_audio_player_handle_t h,const char *const *p,size_t n){(void)h;(void)p;(void)n;++ui_playlist_copies;return ESP_OK;}
esp_err_t aether_audio_player_select(aether_audio_player_handle_t h,size_t n)
{(void)h;ui_playlist_selected_index=n;++ui_playlist_selects;return ESP_OK;}
esp_err_t aether_audio_player_set_paused(aether_audio_player_handle_t h,bool p){(void)h;music.state=p?AETHER_AUDIO_PAUSED:AETHER_AUDIO_PLAYING;return ESP_OK;}
esp_err_t aether_audio_player_stop(aether_audio_player_handle_t h){(void)h;return ESP_OK;}
esp_err_t aether_audio_player_next(aether_audio_player_handle_t h,int d){(void)h;(void)d;return ESP_OK;}
esp_err_t aether_audio_player_set_repeat(aether_audio_player_handle_t h,aether_audio_repeat_t r){(void)h;(void)r;return ESP_OK;}
static esp_err_t eq_submit_error;
esp_err_t aether_audio_player_set_eq(aether_audio_player_handle_t h, const aether_audio_eq_t *eq)
{ assert(h && eq); if (eq_submit_error) return eq_submit_error; music.eq = *eq; return ESP_OK; }
esp_err_t aether_audio_player_seek(aether_audio_player_handle_t h, uint32_t position_ms)
{
    assert(h);
    ++music_seek_requests;
    music_seek_position = position_ms;
    return ESP_OK;
}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){const uint16_t *s=(const uint16_t*)p;for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x){assert(x>=0&&x<480&&y>=0&&y<480);framebuffer[y*480+x]=*s++;}lv_display_flush_ready(d);}
static void read_pointer(lv_indev_t *i,lv_indev_data_t *d){(void)i;d->point=pointer;d->state=held?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void advance(unsigned ms){for(unsigned i=0;i<ms;++i){lv_tick_inc(1);now_us+=1000;lv_timer_handler();}}
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
static void capture(const char *name)
{
    if(!output_directory)return;
    lv_refr_now(NULL);char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",output_directory,name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");
    for(unsigned i=0;i<480*480;++i){uint16_t p=framebuffer[i];uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}assert(!fclose(f));
}
#endif
static lv_obj_t *find(lv_obj_t *p,const char *text)
{if(lv_obj_check_type(p,&lv_label_class)&&!strcmp(lv_label_get_text(p),text))return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *o=find(lv_obj_get_child(p,i),text);if(o)return o;}return NULL;}
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
static lv_obj_t *switch_control(lv_obj_t *p)
{if(lv_obj_check_type(p,&lv_switch_class))return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *o=switch_control(lv_obj_get_child(p,i));if(o)return o;}return NULL;}
static lv_obj_t *settings_switch_for(lv_obj_t *screen, const char *title)
{
    lv_obj_t *label = find(screen, title);
    assert(label);
    lv_obj_t *control = lv_obj_get_sibling(label, 1);
    assert(lv_obj_check_type(control, &lv_switch_class));
    return control;
}
#endif
static aether_usb_audio_status_t usb_audio_status;
static aether_usb_storage_status_t usb_storage_status;
esp_err_t aether_usb_audio_get_status(aether_usb_audio_status_t *s){*s=usb_audio_status;return ESP_OK;}
bool aether_usb_audio_playback_connected(void){return usb_audio_status.playback_connected;}
bool aether_usb_audio_capture_connected(void){return usb_audio_status.capture_connected;}
aether_usb_storage_handle_t aether_usb_storage_default(void){return (void*)1;}
esp_err_t aether_usb_storage_get_status(aether_usb_storage_handle_t h,aether_usb_storage_status_t *s){(void)h;*s=usb_storage_status;return ESP_OK;}
#ifndef APP_VOLUME_UI_DOUBLES_ONLY
static void test_otg(app_ui_t *ui)
{
    usb=(app_io_usb_status_t){.media_present=true};otg=(aether_usb_host_status_t){0};
    app_open_settings(ui);advance(350);
    lv_obj_t *s=settings_switch_for(ui->content, "自动 OTG（CC 检测）");assert(s&&!lv_obj_has_state(s,LV_STATE_CHECKED));
    lv_obj_add_state(s,LV_STATE_CHECKED);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);advance(350);
    assert(otg_commands==1&&otg.enabled&&lv_obj_has_state(s,LV_STATE_CHECKED));
    assert(find(ui->screen,"等待 CC 确认受电设备…"));
    lv_obj_scroll_to_y(ui->content,890,LV_ANIM_OFF);advance(100);capture("settings-otg-wait");
    assert(lv_obj_has_state(settings_switch_for(ui->content, "USB 虚拟U盘"),LV_STATE_DISABLED));
    otg.cleanup_pending=true;otg.state=AETHER_OTG_STOPPING;advance(350);
    assert(!lv_obj_has_state(s,LV_STATE_DISABLED)); /* User can always turn an enabled host OFF. */
    otg.cleanup_pending=false;
    otg.state=AETHER_OTG_STARTING;advance(350);
    assert(find(ui->screen,"正在检查升压输出…"));
    otg.active=otg.vbus_enabled=true;otg.hid_interfaces=0;otg.state=AETHER_OTG_ACTIVE;advance(350);
    assert(find(ui->screen,"Host 已启动；供电状态待确认"));
    assert(find(ui->screen,"电源诊断：尚未采样"));
    otg_power=(app_otg_power_status_t){.valid=true,.vbus_adc_trusted=true,.diagnostics_valid=true,.boost_enabled=true,.rbfet_enabled=true,.rbfet_control_raw=1,.output_verified=true,
        .vbus_valid=true,.vbus_mv=5126,.vbus_raw=5126,.adc_enable_raw=7,.ts_valid=true,.ts_mv=500,.cc_raw=0x16,.control_raw=0x16,.ts_config=0x1e};
    advance(350);
    assert(find(ui->screen,"供电已验证；等待 USB 设备枚举…"));
    assert(find(ui->screen,"采样 VBUS:5.126 V TS:0.500 V\nCC:16 R19:16 R13:01 R50:1E 温度:00\nC0:07 ADC原码:1406\nERR:0 OFF:0 DIAG:0"));
    lv_obj_scroll_to_y(ui->content,1079,LV_ANIM_OFF);advance(100);capture("settings-otg-power");
    otg_power.valid=otg_power.vbus_valid=otg_power.ts_valid=otg_power.output_verified=false;otg_power.age_ms=2500;
    advance(350);assert(find(ui->screen,"Host 已启动；供电状态待确认"));
    assert(find(ui->screen,"快照已过期 VBUS:未测或过期 TS:未测或过期\nCC:-- R19:-- R13:-- R50:-- 温度:--\nC0:-- ADC原码:----\nERR:0 OFF:0 DIAG:0"));
    otg_power.valid=otg_power.failure_snapshot=otg_power.diagnostics_valid=otg_power.ts_valid=true;
    otg_power.age_ms=5000;otg_power.vbus_raw=0;otg_power.verify_error=ESP_ERR_TIMEOUT;
    advance(350);
    assert(find(ui->screen,"失败时采样 VBUS:未测或过期 TS:0.500 V\nCC:16 R19:16 R13:01 R50:1E 温度:00\nC0:07 ADC原码:0000\nERR:107 OFF:0 DIAG:0"));
    otg_power.diagnostics_valid=false;advance(350);
    assert(find(ui->screen,"失败时采样 VBUS:未测或过期 TS:未测或过期\nCC:16 R19:16 R13:01 R50:-- 温度:--\nC0:07 ADC原码:0000\nERR:107 OFF:0 DIAG:0"));
    otg_power.valid=false;advance(350);
    assert(find(ui->screen,"失败诊断 VBUS:未测或过期 TS:未测或过期\nCC:-- R19:-- R13:-- R50:-- 温度:--\nC0:-- ADC原码:----\nERR:107 OFF:0 DIAG:0"));
    otg_power.valid=true;otg_power.failure_snapshot=false;otg_power.vbus_adc_trusted=false;
    otg_power.vbus_valid=true;otg_power.vbus_mv=16380;otg_power.vbus_raw=0x3ffc;
    otg_power.output_verified=false;advance(350);
    assert(find(ui->screen,"供电控制已开启；等待 USB 设备枚举…"));
    assert(find(ui->screen,"采样 VBUS:ADC不可用 TS:未测或过期\nCC:16 R19:16 R13:01 R50:-- 温度:--\nC0:07 ADC原码:3FFC\nERR:107 OFF:0 DIAG:0"));
    otg.hid_interfaces=2;advance(350);
    assert(find(ui->screen,"USB HID 已连接：2 个接口"));
    otg.vid=0x046d;otg.pid=0xc52b;otg.hid_interfaces=1;otg.ignored_interfaces=2;
    otg.hid_stage=AETHER_USB_HID_STAGE_READY;otg.last_interface=0;
    otg.last_subclass=otg.last_protocol=1;otg.report_length=otg.report_received=63;
    otg.transfer_status=0;advance(350);
    assert(find(ui->screen,"USB HID 已连接：1 个接口"));
    assert(find(ui->screen,"USB 046D:C52B 输入:1 跳过:2\n阶段:可用 接口:0 类型:1/1\n描述符:63/63 字节 传输:0\n解析:0 HID错误:0\nUSB 音频 播放:未接入 录音:未接入\n音频错误:0 不支持:0\nUSB U盘:未接入 0 MiB\nU盘错误:0"));
    lv_obj_scroll_to_y(ui->content,1230,LV_ANIM_OFF);advance(100);capture("settings-otg-receiver");
    otg.hid_stage=AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR;otg.last_interface=2;
    otg.report_received=0;otg.transfer_status=4;otg.hid_error=otg.last_error=(esp_err_t)0x108; /* INVALID_RESPONSE fixture. */
    advance(350);
    assert(find(ui->screen,"USB 046D:C52B 输入:1 跳过:2\n阶段:读取 HID 描述符 接口:2 类型:1/1\n描述符:0/63 字节 传输:4\n解析:0 HID错误:108\nUSB 音频 播放:未接入 录音:未接入\n音频错误:0 不支持:0\nUSB U盘:未接入 0 MiB\nU盘错误:0"));
    otg.hid_interfaces=2;advance(350);
    assert(find(ui->screen,"USB 046D:C52B 输入:2 跳过:2\n阶段:读取 HID 描述符 接口:2 类型:1/1\n描述符:0/63 字节 传输:4\n解析:0 HID错误:108\nUSB 音频 播放:未接入 录音:未接入\n音频错误:0 不支持:0\nUSB U盘:未接入 0 MiB\nU盘错误:0"));
    otg.last_error=otg.hid_error=ESP_OK;otg.hid_interfaces=0;
    usb_audio_status.playback_connected=usb_audio_status.capture_connected=true;advance(350);
    assert(find(ui->screen,"USB 音频已接入；可选择输出或录音输入"));
    usb_audio_status=(aether_usb_audio_status_t){0};

    lv_obj_remove_state(s,LV_STATE_CHECKED);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
    otg.active=otg.vbus_enabled=false;otg.cleanup_pending=true;otg.state=AETHER_OTG_STOPPING;advance(350);
    assert(otg_commands==2&&!otg.enabled&&!lv_obj_has_state(s,LV_STATE_CHECKED));
    assert(lv_obj_has_state(settings_switch_for(ui->content, "USB 虚拟U盘"),LV_STATE_DISABLED));
    otg.cleanup_pending=false;otg.state=AETHER_OTG_OFF;advance(350);
    assert(!lv_obj_has_state(settings_switch_for(ui->content, "USB 虚拟U盘"),LV_STATE_DISABLED));
    usb.blocked=usb.active=true;advance(350);
    assert(lv_obj_has_state(s,LV_STATE_DISABLED));
    lv_obj_add_state(s,LV_STATE_CHECKED);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);advance(350);
    assert(otg_commands==2&&!otg.enabled&&!lv_obj_has_state(s,LV_STATE_CHECKED));
    usb=(app_io_usb_status_t){.media_present=true};otg=(aether_usb_host_status_t){0};
    otg_power=(app_otg_power_status_t){.vbus_adc_trusted=true,.age_ms=UINT32_MAX};
    puts("PASS real settings OTG OFF/wait/active/cleanup state, USB mutual exclusion and rejection without publishing an enable command");
}

static void tap_switch(lv_obj_t *control)
{
    lv_obj_scroll_to_view_recursive(control, LV_ANIM_OFF);
    advance(100);
    lv_area_t area;
    lv_obj_get_coords(control, &area);
    pointer = (lv_point_t){.x=(area.x1+area.x2)/2, .y=(area.y1+area.y2)/2};
    held = true;
    advance(80);
    held = false;
    advance(80);
}

static void launch_settings(lv_event_t *event)
{
    app_open_settings(lv_event_get_user_data(event));
}

static void assert_settings_at_top(app_ui_t *ui)
{
    assert(lv_obj_get_scroll_y(ui->content) == 0);
    advance(1000);
    const int32_t offset = lv_obj_get_scroll_y(ui->content);
    if (offset) fprintf(stderr, "Settings opened at scroll offset %d\n", (int)offset);
    assert(offset == 0);
}

static void test_settings_entry(app_ui_t *ui)
{
    lv_group_t *previous_group = lv_group_get_default();
    lv_group_t *group = lv_group_create();
    assert(group);
    lv_group_set_default(group);
    lv_obj_t *drawer = app_ui_begin_page(ui, "应用列表");
    lv_obj_t *launcher = lv_button_create(drawer);
    lv_obj_add_event_cb(launcher, launch_settings, LV_EVENT_CLICKED, ui);
    lv_group_focus_obj(launcher);
    assert(lv_group_get_focused(group) == launcher);
    tap_switch(launcher);
    assert_settings_at_top(ui);

    lv_area_t viewport;
    lv_obj_get_coords(ui->content, &viewport);
    pointer = (lv_point_t){viewport.x1 + 8, viewport.y2 - 48};
    held = true;
    advance(60);
    for (unsigned i = 0; i < 16; ++i) {
        pointer.y -= 8;
        advance(20);
    }
    held = false;
    advance(1000);
    assert(lv_obj_get_scroll_y(ui->content) > 0);
    lv_obj_scroll_to_y(ui->content, 0, LV_ANIM_OFF);

    lv_obj_t *lower = settings_switch_for(ui->content, "自动 OTG（CC 检测）");
    assert(lower);
    lv_group_focus_obj(lower);
    advance(1000);
    const int32_t scrolled = lv_obj_get_scroll_y(ui->content);
    assert(scrolled > 0);
    advance(600);
    assert(lv_obj_get_scroll_y(ui->content) == scrolled);

    lv_obj_t *details = find(ui->screen, "传输详情");
    assert(details);
    tap_switch(lv_obj_get_parent(details));
    assert(ui->page_back == app_open_settings);
    ui->page_back(ui);
    assert_settings_at_top(ui);
    lv_group_set_default(previous_group);
    lv_group_delete(group);
    puts("PASS Settings opens at top, touch and keyboard scroll, and return from transfer details starts at top");
}

static void test_settings_connections(app_ui_t *ui)
{
    app_open_settings(ui);
    advance(350);
    lv_obj_t *connections = lv_obj_get_child(ui->content, 0);
    lv_obj_t *wifi = find(connections, "Wi-Fi");
    lv_obj_t *bluetooth = find(connections, "蓝牙");
    assert(find(connections, "连接") && wifi && bluetooth);
    assert(find(lv_obj_get_parent(wifi), "已连接"));
    assert(find(lv_obj_get_parent(bluetooth), "已开启"));
    station.connected = false;
    bluetooth_input.connected = true;
    advance(350);
    assert(find(lv_obj_get_parent(wifi), "已开启"));
    assert(find(lv_obj_get_parent(bluetooth), "已连接"));
    lv_obj_t *device = lv_obj_get_child(ui->content, -1);
    assert(find(device, "设备") && find(device, "设备状态"));
    const char *const rows[] = {"Wi-Fi", "蓝牙", "设备状态"};
    const char *const titles[] = {"Wi-Fi 网络", "蓝牙", "设备状态"};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        lv_obj_t *row = find(ui->content, rows[i]);
        assert(row);
        tap_switch(lv_obj_get_parent(row));
        assert(ui->page_back == app_open_settings && find(ui->screen, titles[i]));
        lv_obj_t *icon = find(ui->screen, LV_SYMBOL_LEFT);
        assert(icon && lv_obj_get_parent(lv_obj_get_parent(icon)) == ui->screen);
        assert(!find(ui->content, "返回"));
        tap_switch(lv_obj_get_parent(icon));
        assert(find(ui->screen, "设置") && !ui->page_back);
        assert_settings_at_top(ui);
    }
    puts("PASS Settings places Wi-Fi and Bluetooth first, Device status last, and uses one header Back for all three pages");
}

/* Force a stale event past LVGL's disabled state to check the handler guard. */
static void send_stale_enable(lv_obj_t *control)
{
    lv_obj_add_state(control, LV_STATE_CHECKED);
    lv_obj_send_event(control, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!lv_obj_has_state(control, LV_STATE_CHECKED));
}

static void test_manual_otg(app_ui_t *ui)
{
    app_open_settings(ui);
    advance(350);
    lv_obj_t *automatic = settings_switch_for(ui->content, "自动 OTG（CC 检测）");
    lv_obj_t *manual = settings_switch_for(ui->content, "手动 OTG（无 CC 外设）");
    lv_obj_t *msc = settings_switch_for(ui->content, "USB 虚拟U盘");
    assert(automatic && manual && msc);
    assert(find(ui->screen, "手动 OTG（无 CC 外设）"));
    assert(find(ui->screen, "接充电器或电脑前，请关闭手动 OTG 并等供电停止。"));
    assert(!lv_obj_has_state(manual, LV_STATE_CHECKED | LV_STATE_DISABLED));
    const unsigned auto_before = otg_commands;
    tap_switch(automatic);
    assert(otg_commands == auto_before + 1 && lv_obj_has_state(manual, LV_STATE_DISABLED));
    send_stale_enable(manual);
    assert(manual_otg_commands == 0);
    tap_switch(automatic);
    assert(otg_commands == auto_before + 2 && !otg.enabled);

    manual_otg_submit_error = ESP_ERR_INVALID_STATE;
    tap_switch(manual);
    assert(manual_otg_commands == 1 && !lv_obj_has_state(manual, LV_STATE_CHECKED));
    assert(find(ui->screen, "OTG 操作失败：ESP_ERR_INVALID_STATE"));
    manual_otg_submit_error = ESP_OK;
    tap_switch(manual);
    assert(manual_otg_commands == 2 && otg_power.manual_source_requested);
    assert(lv_obj_has_state(manual, LV_STATE_CHECKED) && !lv_obj_has_state(manual, LV_STATE_DISABLED));
    assert(lv_obj_has_state(automatic, LV_STATE_DISABLED) && lv_obj_has_state(msc, LV_STATE_DISABLED));
    send_stale_enable(automatic);
    assert(otg_commands == auto_before + 2);

    otg.state = AETHER_OTG_STARTING;
    quiescing = true;
    advance(350);
    assert(!lv_obj_has_state(manual, LV_STATE_DISABLED));
    tap_switch(manual);
    assert(manual_otg_commands == 3 && !otg_power.manual_source_requested);
    assert(find(ui->screen, "正在关闭 OTG 并释放接口…"));
    quiescing = false;
    assert(lv_obj_has_state(automatic, LV_STATE_DISABLED) && lv_obj_has_state(manual, LV_STATE_DISABLED));
    send_stale_enable(manual);
    send_stale_enable(automatic);
    assert(manual_otg_commands == 3 && otg_commands == auto_before + 2);

    otg.cleanup_pending = false;
    otg.state = AETHER_OTG_WAIT_SINK;
    advance(350);
    assert(find(ui->screen, "正在关闭 OTG 并释放接口…"));
    assert(lv_obj_has_state(automatic, LV_STATE_DISABLED) && lv_obj_has_state(manual, LV_STATE_DISABLED));
    assert(lv_obj_has_state(msc, LV_STATE_DISABLED));
    send_stale_enable(manual);
    assert(manual_otg_commands == 3);
    otg.state = AETHER_OTG_OFF;
    for (unsigned blocked = 0; blocked < 3; ++blocked) {
        usb.active = blocked == 0;
        usb.pending = blocked == 1;
        quiescing = blocked == 2;
        advance(350);
        assert(lv_obj_has_state(automatic, LV_STATE_DISABLED) && lv_obj_has_state(manual, LV_STATE_DISABLED));
        send_stale_enable(manual);
        send_stale_enable(automatic);
        assert(manual_otg_commands == 3 && otg_commands == auto_before + 2);
    }
    usb.active = usb.pending = quiescing = false;
    advance(350);
    tap_switch(manual);
    assert(manual_otg_commands == 4 && find(ui->screen, "正在准备手动供电…"));
    otg.state = AETHER_OTG_STARTING;
    advance(350);
    assert(find(ui->screen, "手动 OTG：正在检查升压输出…"));
    assert(!find(ui->screen, "等待 CC 确认受电设备…"));
    lv_obj_scroll_to_y(ui->content, 1000, LV_ANIM_OFF);
    advance(100);
    capture("settings-otg-manual");

    otg_power.manual_source_requested = false;
    otg_power.manual_source_failed = true;
    otg_power.last_error = ESP_ERR_TIMEOUT;
    advance(350);
    assert(find(ui->screen, "正在关闭 OTG 并释放接口…"));
    assert(!lv_obj_has_state(automatic, LV_STATE_CHECKED));
    assert(lv_obj_has_state(automatic, LV_STATE_DISABLED) && lv_obj_has_state(manual, LV_STATE_DISABLED));
    otg = (aether_usb_host_status_t){.cleanup_pending=true,.state=AETHER_OTG_STOPPING};
    advance(350);
    assert(find(ui->screen, "正在关闭 OTG 并释放接口…"));
    otg = (aether_usb_host_status_t){0};
    advance(350);
    assert(find(ui->screen, "手动 OTG 已停止：ESP_ERR_TIMEOUT"));
    assert(!lv_obj_has_state(manual, LV_STATE_CHECKED | LV_STATE_DISABLED));
    assert(!lv_obj_has_state(msc, LV_STATE_DISABLED));
    usb = (app_io_usb_status_t){.media_present=true};
    otg_power = (app_otg_power_status_t){.vbus_adc_trusted=true,.age_ms=UINT32_MAX};
    puts("PASS real manual OTG switches: exclusive modes, startup OFF, stale ON rejection, MSC/quiesce guard and retained power error after cleanup");
}

static void click(app_ui_t *ui,const char *text)
{lv_obj_t *o=find(ui->screen,text);assert(o);lv_obj_send_event(lv_obj_get_parent(o),LV_EVENT_CLICKED,NULL);advance(350);}
static void test_usb(app_ui_t *ui)
{
    otg.enabled=true;otg.state=AETHER_OTG_WAIT_SINK;app_open_usb(ui);advance(350);
    assert(lv_obj_has_state(lv_obj_get_parent(find(ui->screen,"开启传输")),LV_STATE_DISABLED));
    assert(find(ui->screen,"请先关闭 OTG 并等待外设释放，再开启电脑传输"));
    otg=(aether_usb_host_status_t){0};
    app_open_settings(ui);advance(350);lv_obj_t *s=switch_control(ui->content);assert(s);
    assert(!lv_obj_has_state(s,LV_STATE_CHECKED)&&!lv_obj_has_state(s,LV_STATE_DISABLED));
    lv_obj_add_state(s,LV_STATE_CHECKED);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
    assert(usb.pending&&usb_commands==1&&!lv_obj_has_state(s,LV_STATE_CHECKED)&&lv_obj_has_state(s,LV_STATE_DISABLED));
    usb.pending=false;usb.active=usb.host_connected=true;advance(350);
    assert(lv_obj_has_state(s,LV_STATE_CHECKED)&&!lv_obj_has_state(s,LV_STATE_DISABLED));
    unsigned commands=usb_commands;
    lv_obj_remove_state(s,LV_STATE_CHECKED);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);advance(100);
    assert(!usb.pending&&usb_commands==commands&&find(ui->screen,"电脑已安全弹出？"));
    assert(lv_obj_has_state(s,LV_STATE_CHECKED));
    capture("usb-eject-confirmation");click(ui,"返回");
    assert(!find(ui->screen,"电脑已安全弹出？")&&usb_commands==commands&&usb.active);
    lv_obj_remove_state(s,LV_STATE_CHECKED);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
    ++usb.request_id; /* A newer ownership operation invalidates this dialog. */
    click(ui,"已安全弹出，退出");
    assert(!usb.pending&&usb_commands==commands&&usb_confirmed_commands==0);
    assert(find(ui->screen,"USB 状态已改变，请返回后重新操作。\n确认前仍需在电脑安全弹出。"));
    click(ui,"返回");
    lv_obj_remove_state(s,LV_STATE_CHECKED);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
    click(ui,"已安全弹出，退出");
    assert(usb.pending&&usb_confirmed_commands==1&&usb_commands==commands+1);
    assert(lv_obj_has_state(s,LV_STATE_CHECKED)&&lv_obj_has_state(s,LV_STATE_DISABLED));
    usb.pending=usb.active=usb.blocked=false;usb.last_error=ESP_OK;advance(350);
    assert(!lv_obj_has_state(s,LV_STATE_CHECKED));
    commands=usb_commands;
    app_open_settings(ui);advance(350);assert(usb_commands==commands);
    app_open_usb(ui);advance(350);capture("usb-local");
    assert(find(ui->screen,"退出前请在电脑安全弹出。"));
    assert(lv_obj_get_height(lv_obj_get_parent(find(ui->screen,"开启传输")))>=44);
    click(ui,"开启传输");assert(usb.pending&&usb_commands==commands+1);
    app_open_settings(ui);advance(350);s=switch_control(ui->content);
    assert(lv_obj_has_state(s,LV_STATE_DISABLED)&&!lv_obj_has_state(s,LV_STATE_CHECKED));
    usb.pending=false;usb.active=usb.blocked=usb.host_connected=true;usb.host_ejected=false;advance(350);
    usb.usb_speed_mbps=480;usb.usb_clock_khz=40000;usb.capacity_bytes=8ULL*1024*1024*1024;
    app_open_usb(ui);advance(350);
    assert(ui->page_can_exit && !ui->page_can_exit(ui, app_open_settings));
    assert(find(ui->screen,"电脑已安全弹出？"));
    click(ui,"返回");
    assert(!find(ui->screen,"电脑已安全弹出？")&&usb.active&&!usb.pending);
    assert(find(ui->screen,"USB：电脑已连接 / 高速480Mb/s\nTF：已就绪 / 40.0MHz\n容量：8192 MiB"));
    capture("usb-active");
    usb.usb_speed_mbps=12;usb.usb_clock_khz=32000;usb.usb_high_speed_fallback=true;advance(350);
    assert(find(ui->screen,"USB：电脑已连接 / 全速12Mb/s\nTF：已就绪 / 32.0MHz 已降频\n容量：8192 MiB"));
    capture("usb-fallback");
    usb.usb_speed_mbps=0;usb.usb_clock_khz=0;advance(350);
    assert(find(ui->screen,"USB：电脑已连接 / 速率待确认\nTF：已就绪 / 时钟待确认\n容量：8192 MiB"));
    usb.usb_speed_mbps=480;usb.usb_clock_khz=40000;usb.usb_high_speed_fallback=false;advance(350);
    puts("PASS USB UI displays cached HS/FS/unknown link and actual/fallback SD clock without assuming target clock");
    assert(lv_obj_has_state(lv_obj_get_parent(find(ui->screen,"开启传输")),LV_STATE_DISABLED));
    assert(!lv_obj_has_state(lv_obj_get_parent(find(ui->screen,"退出并恢复")),LV_STATE_DISABLED));
    app_usb_storage_notice(ui);advance(100);assert(find(ui->screen,"TF 卡由电脑占用"));capture("usb-storage-notice");
    click(ui,"知道了");assert(!find(ui->screen,"TF 卡由电脑占用"));
    for(unsigned i=0;i<100;++i){
        click(ui,"退出并恢复");assert(find(ui->screen,"电脑已安全弹出？"));
        click(ui,"返回");assert(!find(ui->screen,"电脑已安全弹出？")&&usb.active&&!usb.pending);
    }
    assert(!ui->page_can_exit(ui, app_open_settings));
    click(ui,"已安全弹出，退出");
    assert(usb.pending && switch_control(ui->content));
    assert(!find(ui->screen,"电脑已安全弹出？") && !ui->page_can_exit);
    usb.pending=false;
    app_open_usb(ui);advance(350);
    usb.host_ejected=true;commands=usb_commands;
    assert(ui->page_can_exit(ui, app_open_settings));
    assert(usb.pending&&usb_commands==commands+1&&!find(ui->screen,"电脑已安全弹出？"));
    usb=(app_io_usb_status_t){.media_present=true};
    app_open_settings(ui);advance(350);s=switch_control(ui->content);
    assert(!lv_obj_has_state(s,LV_STATE_CHECKED));
    puts("PASS settings/default OFF/real pending state; explicit safe-eject cancel/confirm, stale tokens, protocol-eject fast path and100 dialog lifecycles");
}
static void reader_snapshot(app_ui_t *ui){assert(quiescing&&lvgl_locked);reader_saved=true;ui->page_context=NULL;}
extern bool app_ui_input_blocked(const app_ui_t *ui);
static void test_shutdown(app_ui_t *ui)
{
    app_ui_begin_page(ui,"阅读");ui->page_cleanup=reader_snapshot;reader_saved=false;
    power_status.shutdown_state=APP_POWER_SHUTDOWN_PREPARING;
    assert(app_ui_quiesce_storage_begin(ui)==ESP_OK&&quiescing&&reader_saved&&!lvgl_locked&&ui->shutdown_overlay);
    assert(app_ui_input_blocked(ui));
    test_ui_shutdown_service(ui);capture("shutdown-preparing");
    assert(app_ui_quiesce_storage_wait(ui,1234)==ESP_OK);
    quiesce_wait_error=ESP_ERR_TIMEOUT;assert(app_ui_quiesce_storage_wait(ui,1234)==ESP_ERR_TIMEOUT);
    assert(app_ui_quiesce_storage_resume(ui)==ESP_OK&&resume_accepted);
    assert(app_ui_input_blocked(ui));
    test_ui_shutdown_service(ui);assert(ui->shutdown_overlay); /* No worker ACK yet. */
    quiescing=false;power_status.shutdown_state=APP_POWER_SHUTDOWN_FAILED;
    power_status.shutdown_stage=APP_POWER_SHUTDOWN_FINALIZE;power_status.shutdown_error=ESP_ERR_TIMEOUT;
    power_status.shutdown_recovering=true;power_status.shutdown_cancel_error=ESP_ERR_TIMEOUT;
    test_ui_shutdown_service(ui);assert(ui->shutdown_overlay&&!atomic_load(&ui->storage_resume_pending));
    assert(!app_ui_input_blocked(ui)); /* Pure IO/GUI ACK; CANCEL can complete. */
    power_status.shutdown_recovering=false;power_status.shutdown_cancel_error=ESP_OK;
    test_ui_shutdown_service(ui);assert(!ui->shutdown_overlay&&!atomic_load(&ui->storage_resume_pending));
    assert(!app_ui_input_blocked(ui));
    assert(find(ui->screen,"关机未完成，保持开机"));capture("shutdown-failed");
    click(ui,"知道了");test_ui_shutdown_service(ui);assert(!find(ui->screen,"关机未完成，保持开机"));
    usb.active=usb.blocked=true;assert(app_ui_quiesce_storage_begin(ui)==ESP_ERR_INVALID_STATE&&!ui->shutdown_overlay);
    usb.active=usb.blocked=false;lvgl_lock_error=true;resume_accepted=false;
    assert(app_ui_quiesce_storage_begin(ui)==ESP_ERR_TIMEOUT&&quiescing&&!resume_accepted&&!ui->shutdown_overlay);
    assert(!atomic_load(&ui->storage_resume_pending));
    assert(app_ui_quiesce_storage_resume(ui)==ESP_OK&&resume_accepted); /* Explicit coordinator CANCEL only. */
    lvgl_lock_error=false;quiescing=false;power_status=(app_power_status_t){0};quiesce_wait_error=ESP_OK;
    test_ui_shutdown_service(ui);
    puts("PASS real shutdown UI snapshots reader after gating, waits outside LVGL, holds overlay until worker resume ACK, reports failure once and rejects USB ownership");
}
static lv_obj_t *slider(lv_obj_t *p,unsigned wanted,unsigned *seen)
{if(lv_obj_check_type(p,&lv_slider_class)&&(*seen)++==wanted)return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *f=slider(lv_obj_get_child(p,i),wanted,seen);if(f)return f;}return NULL;}
static void move_to(int x,int y,unsigned ms){pointer=(lv_point_t){.x=x,.y=y};held=true;advance(ms);}
static void drag(lv_obj_t *s,unsigned target,int y_offset)
{
    lv_obj_update_layout(lv_screen_active());lv_area_t area;lv_obj_get_coords(s,&area);
    const int left=area.x1+lv_obj_get_style_pad_left(s,LV_PART_MAIN),width=lv_obj_get_width(s)-lv_obj_get_style_pad_left(s,LV_PART_MAIN)-lv_obj_get_style_pad_right(s,LV_PART_MAIN);
    const int start=left+(width*lv_slider_get_value(s)+50)/100,end=left+(width*(int)target+50)/100,y=(area.y1+area.y2)/2+y_offset;
    move_to(start,y,60);
    for(int i=1;i<=20;++i)move_to(start+(end-start)*i/20,y,30);
    assert(lv_obj_has_state(s,LV_STATE_PRESSED));
    advance(700);assert(abs(lv_slider_get_value(s)-(int)target)<=1);
    held=false;advance(100);assert(abs(lv_slider_get_value(s)-(int)target)<=1);
}
static void test_music_eq(app_ui_t *ui)
{
    click(ui,"均衡器");
    lv_obj_t *band_label=find(ui->screen,"100 Hz");assert(band_label);
    lv_obj_t *panel=lv_obj_get_parent(band_label);
    assert(!lv_obj_has_flag(panel,LV_OBJ_FLAG_HIDDEN));
    unsigned seen=0;
    lv_obj_t *band=slider(panel,0,&seen);assert(band);
    lv_slider_set_value(band,5,LV_ANIM_OFF);
    lv_obj_send_event(band,LV_EVENT_VALUE_CHANGED,NULL);
    assert(music.eq.enabled&&music.eq.bands_db[0]==5&&find(panel,"+5 dB"));
    click(ui,"对比原声");
    assert(!music.eq.enabled&&music.eq.bands_db[0]==5);
    click(ui,"启用 EQ");
    assert(music.eq.enabled);
    eq_submit_error=ESP_ERR_TIMEOUT;
    lv_slider_set_value(band,-3,LV_ANIM_OFF);
    lv_obj_send_event(band,LV_EVENT_VALUE_CHANGED,NULL);
    assert(lv_slider_get_value(band)==5&&music.eq.bands_db[0]==5&&find(panel,"调整失败：ESP_ERR_TIMEOUT"));
    eq_submit_error=ESP_OK;
    click(ui,"恢复默认");
    assert(!music.eq.enabled&&!music.eq.bands_db[0]&&find(panel,"已恢复默认值"));
    assert(!find(panel,"返回"));
    lv_obj_t *screen = ui->screen;
    click(ui,LV_SYMBOL_LEFT);
    assert(ui->screen == screen && lv_obj_has_flag(panel,LV_OBJ_FLAG_HIDDEN));
    puts("PASS AC101S Music EQ actual LVGL: open, band edit, bypass/re-enable, backend rollback, defaults and close");
}
static void modal_underlying(lv_event_t *event)
{ ++*(unsigned*)lv_event_get_user_data(event); }
static void modal_pointer_tap(void)
{
    pointer=(lv_point_t){.x=24,.y=24};held=true;advance(80);held=false;advance(80);
}
static void test_modal_backdrop(app_ui_t *ui)
{
    app_ui_begin_page(ui,"Modal");
    unsigned clicks=0;
    lv_obj_t *under=lv_button_create(ui->screen);
    lv_obj_set_pos(under,0,0);lv_obj_set_size(under,48,48);
    lv_obj_add_event_cb(under,modal_underlying,LV_EVENT_CLICKED,&clicks);
    advance(100);modal_pointer_tap();assert(clicks==1);
    lv_obj_t *box=app_ui_modal_msgbox(ui->screen);assert(box);
    lv_obj_set_size(box,300,180);lv_obj_center(box);
    lv_msgbox_add_text(box,"Modal");lv_msgbox_add_footer_button(box,"Close");
    advance(100);assert(app_ui_modal_present(ui->screen));
    modal_pointer_tap();assert(clicks==1); /* Outside clicks cannot reach the page. */
    lv_msgbox_close(box);advance(100);assert(!app_ui_modal_present(ui->screen));
    modal_pointer_tap();assert(clicks==2); /* Close removes backdrop and restores pointer. */
    box=app_ui_modal_msgbox(ui->screen);assert(box);
    app_ui_begin_page(ui,"After modal");advance(100);
    assert(!app_ui_modal_present(ui->screen)&&lv_obj_get_child_count(lv_layer_top())==0);
    puts("PASS actual modal backdrop blocks outside pointer, close restores hit testing, and navigation leaves no top-layer orphan");
}
int main(int argc,char **argv)
{
    const int offset=argc>1?atoi(argv[1]):20;
    output_directory=argc>2?argv[2]:NULL;
    setbuf(stdout,NULL);lv_init();lv_display_t *d=lv_display_create(480,480);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    lv_indev_t *in=lv_indev_create();lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);lv_indev_set_display(in,d);lv_indev_set_read_cb(in,read_pointer);
    app_power_t power={.brightness=75,.timeout=60};app_ui_t ui={.power=&power,.storage=(void*)1,.audio_player=(void*)2,.io=(void*)4};
    test_settings_entry(&ui);
    test_settings_connections(&ui);
    app_open_settings(&ui);advance(200);unsigned seen=0;lv_obj_t *s=slider(ui.content,0,&seen);assert(s);
    drag(s,70,offset);assert(requested_master==70&&master.master_volume==70&&saved_master==70&&master_persist==1&&master_updates>=10);
    advance(1500);assert(lv_slider_get_value(s)==70&&master.master_volume==70);
    printf("PASS actual settings pointer y%+d 17->70: slider=%d request=%u persist-request=%u commits_requested=%u; hold+timer stable\n",offset,lv_slider_get_value(s),requested_master,saved_master,master_persist);
    app_open_music(&ui);advance(200);seen=0;s=slider(ui.content,1,&seen);assert(s);
    assert(lv_slider_get_value(s)==70); /* Settings -> music uses the same value. */
    app_audio_system_set_volume(10,false);advance(300);assert(lv_slider_get_value(s)==10);
    const unsigned before_persist=master_persist;
    drag(s,70,offset);assert(requested_master==70&&master.master_volume==70&&saved_master==70);
    assert(master_persist==before_persist+1&&music_updates==0&&music.volume==100);
    advance(1500);assert(lv_slider_get_value(s)==70&&master.master_volume==70);
    printf("PASS actual music pointer y%+d 10->70: global-request=%u persist-request=%u; stream-gain=%u unchanged; hold+timer stable\n",offset,requested_master,saved_master,music.volume);
    test_music_eq(&ui);
    app_open_settings(&ui);advance(200);seen=0;s=slider(ui.content,0,&seen);assert(s&&lv_slider_get_value(s)==70);
    capture("settings-main");
    assert(master_persist==before_persist+1); /* Reopening pages never writes preferences. */
    puts("PASS settings -> music -> external volume update -> settings share master and reopening does not persist");
    test_usb(&ui);
    test_otg(&ui);
    test_manual_otg(&ui);
    test_shutdown(&ui);
    test_modal_backdrop(&ui);
    if(ui.timer)lv_timer_delete(ui.timer);
    if(ui.page_cleanup)ui.page_cleanup(&ui);
    lv_obj_delete(ui.screen);lv_indev_delete(in);lv_display_delete(d);lv_deinit();
}
#endif
