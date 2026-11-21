#include "app_ui_internal.h"
#include "app_audio_system.h"
#include "app_telemetry.h"
#include "app_home.h"
#include "app_battery.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const lv_font_t font_16, font_24;
static uint16_t framebuffer[480 * 480], draw_buffer[480 * 40];
static unsigned opened;
static lv_timer_cb_t update_callback;
static app_audio_system_snapshot_t audio = {.ready = true, .master_volume = 80};
static app_telemetry_snapshot_t telemetry = {.rtc = {.hour=10,.minute=24,.month=11,.day=3,.weekday=2}};
static aether_storage_status_t storage = {.mounted=true,.card_inserted=true};
static app_tools_snapshot_t tools = {.ready=true,.battery_updated_ms=1000,
    .battery={.presence_valid=true,.battery_present=true,.percentage_valid=true,.percentage=80}};
static bool held;
static lv_point_t point;
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *s) {*s=audio;}
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
    ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui->screen,LV_OPA_COVER,0);return ui->screen;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t ms) {update_callback=cb;ui->timer=lv_timer_create(cb,ms,ui);}
#define APP(name,number) void app_open_##name(app_ui_t *ui) {assert(ui);opened=number;}
APP(music,1) APP(reader,2) APP(video,3) APP(files,4) APP(nes,5) APP(game,6)
APP(clock,7) APP(settings,8) APP(wifi,9) APP(bluetooth,10) APP(status,11)
APP(motion,12) APP(barometer,13) APP(recorder,14)
APP(pc_remote,15) APP(obs_switcher,16)
APP(displacement,17)
APP(intercom,18)
APP(gps,19)
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{const uint16_t *s=(const uint16_t*)p;for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x){assert(x>=0&&x<480&&y>=0&&y<480);framebuffer[y*480+x]=*s++;}lv_display_flush_ready(d);}
static void read_input(lv_indev_t *in,lv_indev_data_t *data)
{(void)in;data->point=point;data->state=held?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void advance(unsigned ms)
{for(unsigned i=0;i<ms;++i){lv_tick_inc(1);lv_timer_handler();}}
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
    lv_display_set_flush_cb(display,flush);app_ui_t ui={.storage=(void*)1};
    app_ui_show_home(&ui);lv_obj_update_layout(ui.screen);
    assert(app_home_app_count()==19&&lv_obj_get_child_count(ui.content)==19);
    assert(!find(ui.screen,"指南针"));
    for(unsigned i=0;i<app_home_app_count();++i){lv_obj_t *tile=lv_obj_get_child(ui.content,i);assert(lv_obj_get_width(tile)==100);lv_obj_t *icon=lv_obj_get_child(tile,0);if(i!=13&&i!=18){const lv_image_dsc_t *src=lv_image_get_src(icon);assert(src&&src->header.w==48&&src->header.h==48);}else{assert(lv_obj_get_width(icon)==48&&lv_obj_get_height(icon)==48);assert(lv_obj_get_child_count(icon)==(i==13?3:6));}lv_obj_send_event(tile,LV_EVENT_CLICKED,NULL);assert(opened==i+1);}
    assert(!strcmp(lv_label_get_text(ui.home_time),"10:24"));
    lv_obj_t *battery=find(ui.screen,"80%");assert(battery);
    lv_area_t badge,tf,audio_area;lv_obj_get_coords(lv_obj_get_parent(battery),&badge);
    lv_obj_get_coords(ui.home_sd,&tf);lv_obj_get_coords(ui.home_audio,&audio_area);
    assert(badge.x2==455&&badge.y1==18&&badge.y2<tf.y1&&tf.y2<audio_area.y1);
    capture(display,argv[1],"home-top");
    lv_obj_scroll_to_y(ui.content,1000,LV_ANIM_OFF);lv_obj_update_layout(ui.screen);
    assert(lv_obj_get_scroll_y(ui.content)>0);capture(display,argv[1],"home-bottom");
    lv_obj_t *microphone=lv_obj_get_child(lv_obj_get_child(ui.content,13),0);
    lv_area_t area;lv_obj_get_coords(microphone,&area);
    point=(lv_point_t){.x=(area.x1+area.x2)/2,.y=area.y1+12};opened=0;
    held=true;advance(80);held=false;advance(80);assert(opened==14);
    lv_obj_t *gps=lv_obj_get_child(lv_obj_get_child(ui.content,18),0);
    lv_obj_get_coords(gps,&area);
    point=(lv_point_t){.x=(area.x1+area.x2)/2,.y=(area.y1+area.y2)/2};opened=0;
    held=true;advance(80);held=false;advance(80);assert(opened==19);
    audio.master_volume=0;storage.mounted=false;storage.card_inserted=false;telemetry.rtc_error=ESP_FAIL;
    update_callback(ui.timer);
    assert(!strcmp(lv_label_get_text(ui.home_audio),"已静音"));
    assert(!strcmp(lv_label_get_text(ui.home_sd),"未插 TF 卡"));
    assert(!strcmp(lv_label_get_text(ui.home_time),"--:--"));
    tools.battery.percentage=12;advance(1100);assert(find(ui.screen,"12%"));
    tools.battery_error=ESP_FAIL;advance(1100);assert(find(ui.screen,"--%")&&!find(ui.screen,"12%"));
    lv_timer_delete(ui.timer);lv_obj_delete(ui.screen);lv_indev_delete(input);lv_display_delete(display);lv_deinit();
    puts("PASS production home: 19 routes including GPS, four columns, scrolling, RTC/TF/audio state and cleanup");
}
