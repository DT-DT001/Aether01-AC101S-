#include "app_ui_internal.h"
#include "aether_recorder.h"
#include "app_audio_system.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const lv_font_t font_16, font_24;
static uint16_t framebuffer[480*480], draw_buffer[480*20];
static bool mounted=true, held, gate_blocked;
static bool usb_blocked;
static lv_point_t point;
static unsigned start_calls, pause_calls, stop_calls, gate_invalidations, route;
static char opened[192];
static app_io_request_t request;
static aether_recorder_status_t status;
static esp_err_t start_error, pause_error, stop_error;
static aether_recorder_handle_t recorder=(void*)1;
static app_audio_system_snapshot_t audio={.ready=true};
static unsigned input_changes;
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *s){*s=audio;}
esp_err_t app_audio_system_set_input(app_audio_input_t input)
{++input_changes;if(status.active)return ESP_ERR_INVALID_STATE;audio.input=input;return ESP_OK;}

bool aether_storage_is_mounted(aether_storage_handle_t h) { (void)h; return mounted; }
bool app_io_usb_storage_blocked(app_io_handle_t io) { (void)io; return usb_blocked; }
aether_recorder_handle_t aether_recorder_get_default(void) { return recorder; }
esp_err_t aether_recorder_get_status(aether_recorder_handle_t h, aether_recorder_status_t *s)
{ assert(h==recorder&&s); *s=status; return ESP_OK; }
esp_err_t aether_recorder_start(aether_recorder_handle_t h)
{
    assert(h==recorder); ++start_calls;
    if(start_error)return start_error;
    status=(aether_recorder_status_t){.state=AETHER_RECORDER_STARTING,.active=true};
    snprintf(status.path,sizeof(status.path),"/sdcard/Recordings/REC0001.wav");return ESP_OK;
}
esp_err_t aether_recorder_set_paused(aether_recorder_handle_t h,bool paused)
{assert(h==recorder);++pause_calls;if(pause_error)return pause_error;status.state=paused?AETHER_RECORDER_PAUSED:AETHER_RECORDER_RECORDING;return ESP_OK;}
esp_err_t aether_recorder_stop(aether_recorder_handle_t h)
{assert(h==recorder);++stop_calls;if(stop_error)return stop_error;status.state=AETHER_RECORDER_SAVING;return ESP_OK;}
const char *esp_err_to_name(esp_err_t e)
{return e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":e==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":e?"ESP_ERR_INVALID_STATE":"ESP_OK";}
const lv_font_t *app_ui_chinese_font_size(unsigned px)
{static lv_font_t s,l;s=font_16;l=font_24;s.fallback=&lv_font_montserrat_16;l.fallback=&lv_font_montserrat_24;return px>=24?&l:&s;}
const lv_font_t *app_ui_chinese_font(void) {return app_ui_chinese_font_size(16);}
void app_ui_label_text(lv_obj_t *o,const char *s) {if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
void app_ui_label_fmt(lv_obj_t *o,const char *f,...)
{char s[512];va_list a;va_start(a,f);vsnprintf(s,sizeof(s),f,a);va_end(a);app_ui_label_text(o,s);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *s,lv_align_t a,int32_t x,int32_t y,uint32_t c,const lv_font_t *f)
{lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_style_text_font(o,f?f:app_ui_chinese_font(),0);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_align(o,a,x,y);return o;}
void app_ui_media_control_gate(app_ui_t *ui,bool blocked,bool invalidate)
{(void)ui;gate_blocked=blocked;if(invalidate)++gate_invalidations;}
static void header_back(lv_event_t *event)
{(void)app_ui_go_back(lv_event_get_user_data(event));}
lv_obj_t *app_ui_begin_page(app_ui_t *ui,const char *title)
{
    ++ui->page_generation;ui->io_complete=NULL;
    if(ui->timer){lv_timer_delete(ui->timer);ui->timer=NULL;}
    if(ui->page_cleanup){void(*cb)(app_ui_t*)=ui->page_cleanup;ui->page_cleanup=NULL;cb(ui);}
    ui->page_context=NULL;ui->page_back=NULL;
    lv_obj_t *old=ui->screen;
    ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen,lv_color_hex(0x070A12),0);
    lv_obj_set_style_text_font(ui->screen,app_ui_chinese_font(),0);
    lv_obj_t *back=lv_button_create(ui->screen);
    lv_obj_set_pos(back,14,12);lv_obj_set_size(back,48,44);
    lv_obj_add_event_cb(back,header_back,LV_EVENT_CLICKED,ui);
    app_ui_add_text(back,LV_SYMBOL_LEFT,LV_ALIGN_CENTER,0,0,0xBFD7FF,&lv_font_montserrat_16);
    app_ui_add_text(ui->screen,title,LV_ALIGN_TOP_MID,0,20,0xEAF2FF,app_ui_chinese_font_size(24));
    ui->content=lv_obj_create(ui->screen);lv_obj_set_size(ui->content,448,394);
    lv_obj_align(ui->content,LV_ALIGN_BOTTOM_MID,0,-14);lv_obj_set_style_pad_all(ui->content,20,0);
    lv_obj_set_style_border_width(ui->content,0,0);lv_obj_set_style_radius(ui->content,24,0);
    lv_obj_set_style_bg_color(ui->content,lv_color_hex(0x10192A),0);lv_screen_load(ui->screen);
    if(old)lv_obj_delete(old);
    return ui->content;
}
bool app_ui_go_back(app_ui_t *ui)
{
    if (ui->page_back) ui->page_back(ui);
    else app_ui_begin_page(ui,"应用抽屉");
    return true;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t ms)
{ui->timer=lv_timer_create(cb,ms,ui);}
void app_ui_request_io_ex(app_ui_t *ui,const app_io_request_t *r,void(*cb)(app_ui_t*,const app_io_result_t*))
{assert(r->operation==APP_IO_RECORDER);request=*r;request.generation=ui->page_generation;ui->io_complete=cb;}
static void deliver(app_ui_t *ui,esp_err_t error)
{assert(ui->io_complete);app_io_result_t r={.operation=request.operation,.generation=request.generation,.error=error};void(*cb)(app_ui_t*,const app_io_result_t*)=ui->io_complete;ui->io_complete=NULL;cb(ui,&r);}
void app_open_music_path(app_ui_t *ui,const char *path)
{route=1;snprintf(opened,sizeof(opened),"%s",path);app_ui_begin_page(ui,"回放");}
void app_open_files_path(app_ui_t *ui,const char *path)
{route=2;snprintf(opened,sizeof(opened),"%s",path);app_ui_begin_page(ui,"文件");}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{uint16_t *s=(uint16_t*)p;for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x){assert(x>=0&&x<480&&y>=0&&y<480);framebuffer[y*480+x]=*s++;}lv_display_flush_ready(d);}
static void read_input(lv_indev_t *in,lv_indev_data_t *data)
{(void)in;data->point=point;data->state=held?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void advance(unsigned ms)
{for(unsigned i=0;i<ms;++i){lv_tick_inc(1);lv_timer_handler();}}
static lv_obj_t *find(lv_obj_t *p,const char *text)
{if(lv_obj_check_type(p,&lv_label_class)&&strstr(lv_label_get_text(p),text))return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *r=find(lv_obj_get_child(p,i),text);if(r)return r;}return NULL;}
static lv_obj_t *control(app_ui_t *ui,const char *name)
{lv_obj_t *l=find(ui->screen,name);assert(l);return lv_obj_get_parent(l);}
static void tap(lv_obj_t *o)
{lv_area_t a;lv_obj_get_coords(o,&a);point=(lv_point_t){.x=(a.x1+a.x2)/2,.y=(a.y1+a.y2)/2};held=true;advance(70);held=false;advance(70);}
static void capture(lv_display_t *d,const char *directory,const char *name)
{
    lv_refr_now(d);char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");
    for(unsigned i=0;i<480*480;++i){uint16_t p=framebuffer[i];uint8_t rgb[]={(p>>11)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}fclose(f);
}

int main(int argc,char **argv)
{
    assert(argc==2);setbuf(stdout,NULL);lv_init();lv_display_t *d=lv_display_create(480,480);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);lv_indev_t *in=lv_indev_create();
    lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);lv_indev_set_display(in,d);lv_indev_set_read_cb(in,read_input);
    app_ui_t ui={.storage=(void*)1};app_open_recorder(&ui);advance(150);
    assert(find(ui.screen,"准备录音"));assert(!gate_blocked);
    lv_obj_t *start=control(&ui,"开始录音"),*pause=control(&ui,"暂停"),*stop=control(&ui,"停止保存");
    assert(lv_obj_get_height(start)==48);assert(lv_obj_has_state(pause,LV_STATE_DISABLED));
    lv_area_t content,last;lv_obj_get_content_coords(ui.content,&content);lv_obj_get_coords(control(&ui,"录音文件"),&last);
    assert(last.x1>=content.x1&&last.x2<=content.x2&&last.y2<=content.y2);
    capture(d,argv[1],"recorder-idle");
    lv_obj_t *input=NULL;
    for(unsigned i=0;i<lv_obj_get_child_count(ui.content);++i){lv_obj_t *o=lv_obj_get_child(ui.content,i);if(lv_obj_check_type(o,&lv_dropdown_class))input=o;}
    assert(input&&lv_dropdown_get_option_count(input)==1);
    audio.usb_capture_connected=true;advance(120);
    assert(lv_dropdown_get_option_count(input)==2);
    lv_dropdown_set_selected(input,1);lv_obj_send_event(input,LV_EVENT_VALUE_CHANGED,NULL);
    assert(input_changes==1&&audio.input==APP_AUDIO_INPUT_USB&&lv_obj_has_state(start,LV_STATE_DISABLED));
    audio.applied_input=APP_AUDIO_INPUT_USB;advance(120);
    assert(!lv_obj_has_state(start,LV_STATE_DISABLED));capture(d,argv[1],"recorder-usb-input");
    audio.usb_capture_connected=false;advance(120);
    assert(lv_dropdown_get_option_count(input)==2&&lv_dropdown_get_selected(input)==1);
    assert(lv_obj_has_state(start,LV_STATE_DISABLED)&&find(ui.screen,"USB 麦克风已断开"));
    lv_dropdown_set_selected(input,0);lv_obj_send_event(input,LV_EVENT_VALUE_CHANGED,NULL);
    assert(audio.input==APP_AUDIO_INPUT_BOARD&&lv_obj_has_state(start,LV_STATE_DISABLED));
    audio.applied_input=APP_AUDIO_INPUT_BOARD;advance(120);
    assert(lv_dropdown_get_option_count(input)==1&&lv_dropdown_get_selected(input)==0);

    tap(start);assert(ui.io_complete&&!start_calls&&gate_blocked);unsigned epochs=gate_invalidations;
    advance(400);assert(gate_invalidations==epochs);tap(start);assert(!start_calls);
    deliver(&ui,ESP_ERR_TIMEOUT);advance(110);assert(find(ui.screen,"ESP_ERR_TIMEOUT"));
    assert(!gate_blocked&&!lv_obj_has_state(start,LV_STATE_DISABLED));
    tap(start);deliver(&ui,ESP_OK);assert(start_calls==1&&status.state==AETHER_RECORDER_STARTING&&gate_blocked);
    status.state=AETHER_RECORDER_RECORDING;status.duration_ms=125000;status.data_bytes=12000000;status.peak=16384;
    advance(120);assert(find(ui.screen,"02:05")&&find(ui.screen,"50%"));
    assert(lv_obj_has_state(input,LV_STATE_DISABLED));
    assert(!lv_obj_has_state(pause,LV_STATE_DISABLED));capture(d,argv[1],"recorder-recording");
    tap(pause);assert(pause_calls==1&&status.state==AETHER_RECORDER_PAUSED);advance(200);
    assert(find(ui.screen,"继续")&&find(ui.screen,"0%")&&find(ui.screen,"02:05"));
    tap(control(&ui,"继续"));assert(pause_calls==2&&status.state==AETHER_RECORDER_RECORDING);
    tap(stop);assert(stop_calls==1&&status.state==AETHER_RECORDER_SAVING&&gate_blocked);
    assert(lv_obj_has_state(control(&ui,"回放录音"),LV_STATE_DISABLED));
    status.state=AETHER_RECORDER_FINISHED;status.active=false;advance(300);
    assert(!gate_blocked&&find(ui.screen,"录音已保存"));
    assert(!lv_obj_has_state(start,LV_STATE_DISABLED|LV_STATE_PRESSED));
    capture(d,argv[1],"recorder-saved");
    tap(control(&ui,"回放录音"));assert(route==1&&!strcmp(opened,"/sdcard/Recordings/REC0001.wav")&&stop_calls==1);
    assert(ui.page_back==app_open_recorder);
    tap(control(&ui,LV_SYMBOL_LEFT));
    assert(!ui.page_back&&find(ui.screen,"录音机")&&find(ui.screen,"录音已保存"));

    tap(control(&ui,"录音文件"));
    assert(route==2&&!strcmp(opened,"/sdcard/Recordings")&&ui.page_back==app_open_recorder);
    tap(control(&ui,LV_SYMBOL_LEFT));
    assert(!ui.page_back&&find(ui.screen,"录音机")&&find(ui.screen,"录音已保存"));
    status=(aether_recorder_status_t){0};app_open_recorder(&ui);advance(120);tap(control(&ui,"开始录音"));
    void(*stale)(app_ui_t*,const app_io_result_t*)=ui.io_complete;assert(stale);
    app_io_result_t stale_result={.generation=request.generation,.operation=APP_IO_RECORDER,.error=ESP_OK};
    app_ui_begin_page(&ui,"离开");stale(&ui,&stale_result);assert(start_calls==1&&!gate_blocked);
    status=(aether_recorder_status_t){.active=true,.state=AETHER_RECORDER_RECORDING};
    app_open_recorder(&ui);advance(120);assert(gate_blocked);tap(control(&ui,LV_SYMBOL_LEFT));
    assert(stop_calls==2&&status.state==AETHER_RECORDER_SAVING&&!gate_blocked&&!ui.page_context&&find(ui.screen,"应用抽屉"));
    puts("PASS playback and recordings folder Back returns to recorder; recorder Back stops active capture and opens drawer");

    status=(aether_recorder_status_t){0};mounted=false;app_open_recorder(&ui);advance(120);
    assert(find(ui.screen,"请插入 TF 卡")&&lv_obj_has_state(control(&ui,"开始录音"),LV_STATE_DISABLED));
    mounted=true;usb_blocked=true;advance(120);assert(lv_obj_has_state(control(&ui,"开始录音"),LV_STATE_DISABLED));
    usb_blocked=false;advance(120);assert(!lv_obj_has_state(control(&ui,"开始录音"),LV_STATE_DISABLED));
    puts("PASS recorder stays disabled after remount until the USB ownership gate releases");
    start_error=ESP_ERR_NO_MEM;tap(control(&ui,"开始录音"));deliver(&ui,ESP_OK);advance(120);
    assert(start_calls==2&&find(ui.screen,"ESP_ERR_NO_MEM")&&!gate_blocked);capture(d,argv[1],"recorder-error");
    status.state=AETHER_RECORDER_ERROR;status.last_error=ESP_FAIL;
    snprintf(status.detail,sizeof(status.detail),"TF 卡写入过慢，请换高速卡后重试");
    app_open_recorder(&ui);advance(120);assert(find(ui.screen,"TF 卡写入过慢"));capture(d,argv[1],"recorder-error");
    app_ui_begin_page(&ui,"离开");recorder=NULL;app_open_recorder(&ui);advance(120);assert(find(ui.screen,"录音不可用"));
    assert(lv_obj_has_state(control(&ui,"开始录音"),LV_STATE_DISABLED));
    app_ui_begin_page(&ui,"结束");lv_obj_delete(ui.screen);lv_indev_delete(in);
    assert(lv_mem_test()==LV_RESULT_OK);lv_display_delete(d);lv_deinit();
    puts("PASS recorder production UI: real pointer prepare/error/retry/start/pause/resume/save/replay/browse; 100ms level/time; epoch transition; stale callback and async leave; TF removal/reinsert; missing backend; geometry and LVGL heap");
}
