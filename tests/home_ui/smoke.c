#include "app_ui_internal.h"
#include "app_audio_system.h"
#include "app_telemetry.h"
#include "app_home.h"
#include "app_battery.h"
#include "app_screen.h"
#include "app_utilities.h"
#include "aether_wifi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const lv_font_t font_16, font_24;
static uint16_t framebuffer[480 * 480], draw_buffer[480 * 40];
static unsigned opened;
static lv_timer_cb_t update_callback;
static aether_wifi_status_t wifi = {.enabled = true, .connected = true};
static aether_bluetooth_status_t bluetooth = {.enabled = true};
static app_weather_snapshot_t weather = {.valid = true, .temperature_c = 22, .weather_code = 1};
static app_telemetry_snapshot_t telemetry = {.rtc = {.hour=10,.minute=24,.month=11,.day=3,.weekday=2}};
static aether_storage_status_t storage = {.mounted=true,.card_inserted=true};
static app_tools_snapshot_t tools = {.ready=true,.battery_updated_ms=1000,
    .battery={.presence_valid=true,.battery_present=true,.percentage_valid=true,.percentage=80}};
static bool held;
static lv_point_t point;
aether_wifi_handle_t aether_wifi_get_default(void) {return (void*)1;}
esp_err_t aether_wifi_get_status(aether_wifi_handle_t handle,aether_wifi_status_t *s) {assert(handle);*s=wifi;return ESP_OK;}
aether_bluetooth_handle_t app_audio_system_bluetooth(void) {return (void*)1;}
esp_err_t aether_bluetooth_get_status(aether_bluetooth_handle_t handle,aether_bluetooth_status_t *s) {assert(handle);*s=bluetooth;return ESP_OK;}
esp_err_t aether_bluetooth_hid_get_status(aether_bluetooth_handle_t handle,aether_bluetooth_hid_status_t *s) {assert(handle);*s=(aether_bluetooth_hid_status_t){0};return ESP_OK;}
void app_weather_get(app_weather_snapshot_t *s) {*s=weather;}
void app_weather_refresh(bool manual) {assert(!manual);}
const char *app_weather_city(unsigned city) {assert(city==0);return "上海";}
const char *app_weather_condition(unsigned code) {assert(code==1);return "少云";}
void app_tools_get_snapshot(app_tools_snapshot_t *s) {*s=tools;}
int64_t esp_timer_get_time(void) {return 1000000LL+(int64_t)lv_tick_get()*1000;}
const char *esp_err_to_name(esp_err_t error) {return error?"ESP_ERR_INVALID_STATE":"ESP_OK";}
void app_telemetry_get(app_telemetry_snapshot_t *s) {*s=telemetry;}
esp_err_t aether_storage_get_status(aether_storage_handle_t h, aether_storage_status_t *s) {(void)h;*s=storage;return ESP_OK;}
const lv_font_t *app_ui_chinese_font_size(unsigned px)
{
    static lv_font_t small, large;
    small=font_16; large=font_24;
    small.fallback=&lv_font_montserrat_16; large.fallback=&lv_font_montserrat_24;
    return px>=24?&large:&small;
}
const lv_font_t *app_ui_chinese_font(void) {return app_ui_chinese_font_size(16);}
void app_ui_label_text(lv_obj_t *o,const char *s) {if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
void app_ui_label_fmt(lv_obj_t *o,const char *fmt,...)
{char s[256];va_list a;va_start(a,fmt);vsnprintf(s,sizeof(s),fmt,a);va_end(a);app_ui_label_text(o,s);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *s,lv_align_t a,int32_t x,int32_t y,uint32_t c,const lv_font_t *f)
{lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_set_style_text_font(o,f,0);lv_obj_align(o,a,x,y);return o;}
lv_obj_t *app_ui_new_screen(app_ui_t *ui)
{
    if(ui->timer){lv_timer_delete(ui->timer);ui->timer=NULL;}
    ui->home_time=NULL;ui->content=NULL;ui->user_key_exit_armed=false;
    ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui->screen,LV_OPA_COVER,0);return ui->screen;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t ms) {update_callback=cb;ui->timer=lv_timer_create(cb,ms,ui);}
void app_ui_set_user_key_exit(app_ui_t *ui,bool armed) {ui->user_key_exit_armed=armed;}
#define APP(name,number) void app_open_##name(app_ui_t *ui) {assert(ui);opened=number;}
APP(clock,1) APP(weather,2) APP(calendar,3) APP(notes,4)
APP(music,5) APP(reader,6) APP(video,7) APP(files,8)
APP(game,9) APP(nes,10) APP(recorder,11) APP(settings,12)
APP(motion,13) APP(barometer,14) APP(displacement,15) APP(pc_remote,16)
APP(obs_switcher,17) APP(intercom,18) APP(gps,19) APP(camera,20) APP(gallery,21)
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{const uint16_t *s=(const uint16_t*)p;for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x){assert(x>=0&&x<480&&y>=0&&y<480);framebuffer[y*480+x]=*s++;}lv_display_flush_ready(d);}
static void read_input(lv_indev_t *in,lv_indev_data_t *data)
{(void)in;data->point=point;data->state=held?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void advance(unsigned ms)
{for(unsigned i=0;i<ms;++i){lv_tick_inc(1);lv_timer_handler();}}
static void pointer_down(int32_t x, int32_t y)
{
    point=(lv_point_t){x,y};
    held=true;
    advance(50);
}
static void pointer_move(int32_t x, int32_t y)
{
    point=(lv_point_t){x,y};
    advance(34);
}
static void pointer_up(void)
{
    held=false;
    advance(50);
}
static void tap(int32_t x,int32_t y)
{
    pointer_down(x,y);
    pointer_up();
}
static void swipe(int32_t x1,int32_t y1,int32_t x2,int32_t y2)
{
    pointer_down(x1,y1);
    for(int32_t step=1;step<=5;++step) {
        pointer_move(x1+(x2-x1)*step/5,y1+(y2-y1)*step/5);
    }
    pointer_up();
}
static void screen_deleted(lv_event_t *event)
{
    unsigned *count=lv_event_get_user_data(event);
    ++*count;
}
static void replace_screen_in_event(lv_event_t *event)
{
    app_ui_t *ui=lv_event_get_user_data(event);
    app_ui_load_screen(app_ui_new_screen(ui));
}
static void return_home(app_ui_t *ui)
{
    app_ui_show_home(ui);
    advance(50);
    assert(ui->home_time&&!ui->content&&!ui->user_key_exit_armed);
}
static void return_from_focused_app(app_ui_t *ui, lv_group_t *group,
                                    unsigned selected, int32_t scroll)
{
    assert(ui->drawer_scroll_y==scroll);
    lv_obj_t *app_screen=app_ui_new_screen(ui);
    lv_obj_t *back=lv_button_create(app_screen);
    app_ui_load_screen(app_screen);
    advance(50);
    assert(lv_screen_active()==app_screen&&!ui->content&&!ui->home_time);
    lv_group_focus_obj(back);
    assert(lv_group_get_focused(group)==back);
    unsigned deleted=0;
    lv_obj_add_event_cb(app_screen,screen_deleted,LV_EVENT_DELETE,&deleted);

    app_ui_show_drawer(ui);
    assert(ui->content&&ui->user_key_exit_armed&&!ui->home_time&&deleted==0);
    lv_obj_update_layout(ui->screen);
    lv_obj_t *tile=lv_obj_get_child(ui->content,selected);
    assert(lv_group_get_focused(group)==tile);
    assert(lv_obj_has_flag(tile,LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    assert(lv_obj_get_scroll_y(ui->content)==scroll);

    advance(400);
    assert(deleted==1&&lv_group_get_focused(group)==tile);
    assert(lv_obj_get_scroll_y(ui->content)==scroll);
}
static lv_obj_t *find(lv_obj_t *p,const char *text)
{if(lv_obj_check_type(p,&lv_label_class)&&!strcmp(lv_label_get_text(p),text))return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *o=find(lv_obj_get_child(p,i),text);if(o)return o;}return NULL;}
static void capture(lv_display_t *display,const char *directory,const char *name)
{
    lv_refr_now(display);char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");
    for(unsigned i=0;i<480*480;++i){uint16_t p=framebuffer[i];uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}fclose(f);
}
int main(int argc,char **argv)
{
    assert(argc==2);lv_init();lv_display_t *display=lv_display_create(480,480);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input,display);lv_indev_set_read_cb(input,read_input);
    lv_group_t *keyboard_group=lv_group_create();
    lv_group_set_default(keyboard_group);
    lv_display_set_flush_cb(display,flush);app_ui_t ui={.storage=(void*)1};
    app_ui_show_home(&ui);lv_obj_update_layout(ui.screen);
    for(unsigned run=0;run<60;++run) {
        unsigned deleted=0;
        lv_obj_t *old=ui.screen;
        lv_obj_add_event_cb(old,screen_deleted,LV_EVENT_DELETE,&deleted);
        lv_obj_add_event_cb(old,replace_screen_in_event,LV_EVENT_CLICKED,&ui);
        assert(lv_obj_send_event(old,LV_EVENT_CLICKED,NULL)==LV_RESULT_OK);
        assert(ui.screen!=old&&lv_screen_active()==ui.screen&&deleted==0);
        advance(30);assert(deleted==1&&lv_mem_test()==LV_RESULT_OK);
        app_ui_show_home(&ui);advance(30);
    }
    puts("PASS event source screens survive callback dispatch and are retired by LVGL timers in all 60 cycles");
    assert(!ui.content && !ui.user_key_exit_armed && !find(ui.screen,"音乐") &&
           !find(ui.screen,LV_SYMBOL_HOME));
    assert(!strcmp(lv_label_get_text(ui.home_time),"10:24"));
    assert(find(ui.screen,"22°")&&find(ui.screen,"上海")&&find(ui.screen,"少云"));
    assert(find(ui.screen,"80")&&!find(ui.screen,"80%"));
    lv_obj_t *wifi_icon=find(ui.screen,LV_SYMBOL_WIFI);
    assert(wifi_icon&&lv_color_eq(lv_obj_get_style_text_color(wifi_icon,0),lv_color_hex(0xA4EBD4)));
    assert(find(ui.screen,LV_SYMBOL_BLUETOOTH)&&find(ui.screen,LV_SYMBOL_SD_CARD));
    capture(display,argv[1],"home-desktop");
    lv_obj_t *desktop=ui.screen;
    tap(240,315);
    tap(240,315);
    assert(ui.screen==desktop&&!ui.content&&opened==0);
    swipe(240,345,240,333);
    assert(ui.screen==desktop&&!ui.content&&opened==0);
    swipe(105,335,325,335);
    assert(ui.screen==desktop&&!ui.content&&opened==0);
    swipe(105,350,325,275);
    assert(ui.screen==desktop&&!ui.content&&opened==0);
    swipe(240,170,240,390);
    assert(ui.screen==desktop&&!ui.content&&opened==0);

    /* A continuous press ends over Clock and cannot also activate its tile. */
    pointer_down(70,400);
    for(int32_t y=350;y>=150;y-=50) pointer_move(70,y);
    pointer_move(70,104);
    advance(250);
    assert(opened==0);
    pointer_up();
    assert(ui.content&&ui.user_key_exit_armed&&!ui.home_time&&opened==0);
    lv_obj_update_layout(ui.screen);
    tap(70,104);
    assert(opened==1);
    return_from_focused_app(&ui,keyboard_group,0,0);
    assert(lv_obj_get_scroll_y(ui.content)==0);
    for(unsigned i=1;i<app_home_app_count();++i) lv_group_focus_next(keyboard_group);
    advance(400);
    assert(lv_group_get_focused(keyboard_group)==lv_obj_get_child(ui.content,app_home_app_count()-1));
    const int32_t keyboard_scroll=lv_obj_get_scroll_y(ui.content);
    assert(keyboard_scroll>0);
    lv_group_focus_next(keyboard_group);
    lv_obj_t *home_label=find(ui.screen,LV_SYMBOL_HOME);
    assert(home_label&&lv_group_get_focused(keyboard_group)==lv_obj_get_parent(home_label));
    lv_group_focus_next(keyboard_group);
    advance(400);
    lv_obj_t *first=lv_obj_get_child(ui.content,0);
    assert(lv_group_get_focused(keyboard_group)==first);
    lv_area_t viewport,first_tile;
    lv_obj_get_coords(ui.content,&viewport);
    lv_obj_get_coords(first,&first_tile);
    assert(first_tile.y1>=viewport.y1&&first_tile.y2<=viewport.y2);
    assert(lv_obj_get_scroll_y(ui.content)<keyboard_scroll);
    lv_obj_scroll_to_y(ui.content,0,LV_ANIM_OFF);
    puts("PASS Clock Back picks the Clock tile and keeps offset zero across async retirement; keyboard focus still scrolls the drawer");
    puts("PASS pointer double tap, short, sideways, sideways-dominant diagonal and down strokes stay on the desktop; one up stroke opens the drawer without also opening Clock");
    assert(app_home_app_count()==21&&lv_obj_get_child_count(ui.content)==21);
    for(unsigned i=0;i<app_home_app_count();++i){
        lv_obj_t *tile=lv_obj_get_child(ui.content,i),*icon=lv_obj_get_child(tile,0);
        assert(lv_obj_get_width(tile)==102&&lv_obj_get_width(icon)==48&&lv_obj_get_height(icon)==48);
        lv_obj_send_event(tile,LV_EVENT_CLICKED,NULL);assert(opened==i+1);
    }
    assert(!find(ui.content,"指南针"));
    assert(!find(ui.content,"无线网络")&&!find(ui.content,"蓝牙耳机")&&!find(ui.content,"设备状态"));
    assert(find(ui.content,"无线对讲")&&find(ui.content,"定位"));
    assert(find(ui.content,"USB 摄像头")&&find(ui.content,"图库"));
    capture(display,argv[1],"drawer-top");
    lv_obj_scroll_to_y(ui.content,1000,LV_ANIM_OFF);lv_obj_update_layout(ui.screen);
    assert(lv_obj_get_scroll_y(ui.content)>0);capture(display,argv[1],"drawer-bottom");
    lv_obj_t *microphone=lv_obj_get_child(lv_obj_get_child(ui.content,10),0);
    lv_area_t area;lv_obj_get_coords(microphone,&area);
    point=(lv_point_t){.x=(area.x1+area.x2)/2,.y=area.y1+12};opened=0;
    held=true;advance(80);held=false;advance(80);assert(opened==11);
    for(unsigned i=17;i<app_home_app_count();++i) {
        lv_obj_t *tile=lv_obj_get_child(ui.content,i),*icon=lv_obj_get_child(tile,0);
        if(i==18) assert(lv_obj_get_child_count(icon)==6);
        lv_obj_get_coords(icon,&area);opened=0;
        tap((area.x1+area.x2)/2,(area.y1+area.y2)/2);
        assert(opened==i+1);
    }
    const int32_t scroll=lv_obj_get_scroll_y(ui.content);
    assert(scroll>0);
    return_from_focused_app(&ui,keyboard_group,20,scroll);
    home_label=find(ui.screen,LV_SYMBOL_HOME);
    assert(home_label);
    lv_obj_t *home_button=lv_obj_get_parent(home_label);
    assert(lv_obj_get_parent(home_button)==ui.screen&&lv_obj_has_flag(home_button,LV_OBJ_FLAG_CLICKABLE));
    lv_obj_get_coords(home_button,&area);
    assert(area.x1>=0&&area.y1>=0&&area.x2<100&&area.y2<70);
    lv_obj_t *drawer=ui.screen;
    tap((area.x1+area.x2)/2,(area.y1+area.y2)/2);
    assert(ui.screen!=drawer&&ui.home_time&&!ui.content&&!ui.user_key_exit_armed);
    assert(opened==21&&!find(ui.screen,LV_SYMBOL_HOME)&&!lv_group_get_focused(keyboard_group));
    swipe(240,380,240,141);
    assert(ui.content&&ui.user_key_exit_armed&&!ui.home_time&&opened==21);
    assert(lv_obj_get_scroll_y(ui.content)==0&&
           lv_group_get_focused(keyboard_group)==lv_obj_get_child(ui.content,0));
    return_home(&ui);
    puts("PASS Gallery Back preserves scroll and focus; drawer Home taps to desktop; next upward swipe opens a fresh drawer at Clock without activating it");
    opened=0;
    for(unsigned run=0;run<20;++run) {
        int32_t x=70+(int32_t)(run%4)*110;
        swipe(x,380,x+8,141);
        assert(ui.content&&ui.user_key_exit_armed&&!ui.home_time&&opened==0);
        return_home(&ui);
        assert(lv_mem_test()==LV_RESULT_OK);
    }
    puts("PASS 20 repeated native upward swipes and drawer-to-desktop returns across four starting columns");
    pointer_down(240,380);
    for(int32_t y=377;y>=320;y-=3) pointer_move(240,y);
    pointer_up();
    assert(ui.content&&ui.user_key_exit_armed&&!ui.home_time&&opened==0);
    return_home(&ui);
    puts("PASS slow 60 px upward swipe sampled in 3 px steps opens the drawer once");
    wifi.connected=false;storage.mounted=false;storage.card_inserted=false;telemetry.rtc.oscillator_stopped=true;
    weather.valid=false;
    update_callback(ui.timer);
    assert(lv_color_eq(lv_obj_get_style_text_color(find(ui.screen,LV_SYMBOL_WIFI),0),lv_color_hex(0xE9BB78)));
    assert(lv_color_eq(lv_obj_get_style_text_color(find(ui.screen,LV_SYMBOL_SD_CARD),0),lv_color_hex(0x7984A2)));
    assert(!strcmp(lv_label_get_text(ui.home_time),"--:--"));
    assert(find(ui.screen,"--°"));
    assert(find(ui.screen,"--")&&!find(ui.screen,"80"));
    tools.battery.percentage=12;
    tools.battery_updated_ms=(uint32_t)(esp_timer_get_time()/1000);
    advance(1100);assert(find(ui.screen,"12"));
    tools.battery_error=ESP_FAIL;advance(1100);assert(find(ui.screen,"--")&&!find(ui.screen,"12"));
    lv_timer_delete(ui.timer);lv_obj_delete(ui.screen);lv_group_delete(keyboard_group);
    lv_indev_delete(input);lv_display_delete(display);lv_deinit();
    puts("PASS production desktop colors and 21 scrollable drawer routes; Wi-Fi, Bluetooth and device status live in Settings");
}
