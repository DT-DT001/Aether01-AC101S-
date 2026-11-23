#include "app_ui_internal.h"
#include "app_audio_system.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const lv_font_t font_16,font_24;
static uint16_t buffer[480*40];
static lv_point_t pointer;
static bool held;
static unsigned connections;
static uint8_t connected_address[6];
static aether_bluetooth_device_t devices[AETHER_BLUETOOTH_MAX_DEVICES];
static aether_bluetooth_status_t status={.state=AETHER_BT_SCANNING,.enabled=true,.audio_requested=true,.scanning=true,.device_count=16,
    .acl_status=-1,.auth_status=-1,.acl_disconnect_reason=-1,.a2dp_disconnect_reason=-1};
static app_audio_system_snapshot_t audio={.ready=true,.master_volume=70};
aether_bluetooth_handle_t app_audio_system_bluetooth(void){return (void*)1;}
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *s){*s=audio;}
esp_err_t app_audio_system_set_output(app_audio_output_t o){audio.output=o;return ESP_OK;}
esp_err_t app_audio_system_set_volume(uint8_t v,bool p){(void)p;audio.master_volume=v;return ESP_OK;}
esp_err_t aether_bluetooth_get_status(aether_bluetooth_handle_t h,aether_bluetooth_status_t *s){assert(h);*s=status;return ESP_OK;}
esp_err_t aether_bluetooth_get_device(aether_bluetooth_handle_t h,size_t i,aether_bluetooth_device_t *d){assert(h&&i<16);*d=devices[i];return ESP_OK;}
esp_err_t aether_bluetooth_connect(aether_bluetooth_handle_t h,const uint8_t a[6]){assert(h);++connections;memcpy(connected_address,a,6);return ESP_OK;}
esp_err_t aether_bluetooth_scan(aether_bluetooth_handle_t h){assert(h);return ESP_OK;}
esp_err_t aether_bluetooth_disconnect(aether_bluetooth_handle_t h){assert(h);return ESP_OK;}
esp_err_t aether_bluetooth_set_enabled(aether_bluetooth_handle_t h,bool on){assert(h);(void)on;return ESP_OK;}
const char *esp_err_to_name(esp_err_t e){return e?"ESP_FAIL":"ESP_OK";}
const lv_font_t *app_ui_chinese_font_size(unsigned s){return s>=24?&font_24:&font_16;}
const lv_font_t *app_ui_chinese_font(void){return &font_16;}
void app_ui_label_text(lv_obj_t *o,const char *s){if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
void app_ui_label_fmt(lv_obj_t *o,const char *f,...){char s[512];va_list a;va_start(a,f);vsnprintf(s,sizeof(s),f,a);va_end(a);app_ui_label_text(o,s);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *s,lv_align_t a,int32_t x,int32_t y,uint32_t c,const lv_font_t *f){lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_style_text_font(o,f,0);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_align(o,a,x,y);return o;}
lv_obj_t *app_ui_begin_page(app_ui_t *ui,const char *title)
{
    (void)title;lv_obj_t *old=ui->screen;if(ui->timer){lv_timer_delete(ui->timer);ui->timer=NULL;}if(ui->page_cleanup){ui->page_cleanup(ui);ui->page_cleanup=NULL;}
    ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_text_font(ui->screen,&font_16,0);
    ui->content=lv_obj_create(ui->screen);lv_obj_set_size(ui->content,448,394);lv_obj_align(ui->content,LV_ALIGN_BOTTOM_MID,0,-14);
    lv_obj_set_style_pad_all(ui->content,20,0);lv_obj_set_style_border_width(ui->content,0,0);lv_screen_load(ui->screen);if(old)lv_obj_delete(old);return ui->content;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t ms){ui->timer=lv_timer_create(cb,ms,ui);}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){(void)a;(void)p;lv_display_flush_ready(d);}
static void read_pointer(lv_indev_t *i,lv_indev_data_t *d){(void)i;d->point=pointer;d->state=held?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void advance(unsigned ms){for(unsigned i=0;i<ms;++i){lv_tick_inc(1);lv_timer_handler();}}
static lv_obj_t *find(lv_obj_t *p,const char *text)
{if(lv_obj_check_type(p,&lv_label_class)&&strstr(lv_label_get_text(p),text))return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *f=find(lv_obj_get_child(p,i),text);if(f)return f;}return NULL;}
int main(void)
{
    setbuf(stdout,NULL);for(unsigned i=0;i<16;++i){devices[i].address[5]=i+1;snprintf(devices[i].name,sizeof(devices[i].name),"Nearby %u",i);}
    strcpy(devices[0].name,"Old target");lv_init();lv_display_t *d=lv_display_create(480,480);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    lv_indev_t *in=lv_indev_create();lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);lv_indev_set_display(in,d);lv_indev_set_read_cb(in,read_pointer);
    app_ui_t ui={0};app_open_bluetooth(&ui);advance(100);lv_obj_t *name=find(ui.screen,"Old target");assert(name);
    lv_obj_t *row=lv_obj_get_parent(name);lv_obj_scroll_to_view(row,LV_ANIM_OFF);lv_obj_update_layout(ui.screen);advance(200);
    lv_area_t area;lv_obj_get_coords(row,&area);pointer=(lv_point_t){.x=area.x1+60,.y=area.y1+32};held=true;advance(80);
    assert(lv_obj_has_state(row,LV_STATE_PRESSED));assert(!connections);
    /* Production discovery replaces a non-audio entry when its 16-slot list is
     * full. Do so during the physical DOWN -> UP interval and let 2 UI polls run. */
    devices[0].address[5]=99;devices[0].audio_device=true;strcpy(devices[0].name,"New headphones");advance(650);
    assert(find(ui.screen,"Old target")); /* Visible row is stable while held. */
    held=false;advance(80);
    assert(connections==1);if(connected_address[5]!=1)fprintf(stderr,"WRONG DEVICE: pressed address ..01, connected ..%02x\n",connected_address[5]);
    assert(connected_address[5]==1);
    puts("PASS actual pointer DOWN/650ms discovery replacement/UP connects the originally pressed address");
    advance(350);assert(find(ui.screen,"New headphones"));
    lv_obj_scroll_to_view(row,LV_ANIM_OFF);lv_obj_update_layout(ui.screen);advance(100);
    lv_obj_get_coords(row,&area);pointer=(lv_point_t){.x=area.x1+60,.y=area.y1+32};held=true;advance(80);
    pointer.y-=50;advance(100);held=false;advance(100);assert(connections==1);
    strcpy(devices[0].name,"After canceled drag");advance(350);
    assert(find(ui.screen,"After canceled drag"));
    puts("PASS actual pointer scrolling cancels the selected device and resumes discovery updates without connecting");
    lv_obj_scroll_to_view(row,LV_ANIM_OFF);lv_obj_update_layout(ui.screen);advance(100);
    lv_obj_get_coords(row,&area);pointer=(lv_point_t){.x=area.x1+60,.y=area.y1+32};held=true;advance(80);
    assert(lv_obj_has_state(row,LV_STATE_PRESSED));
    held=false;lv_indev_reset(in,NULL);
    strcpy(devices[0].name,"After input reset");advance(350);
    assert(connections==1&&!lv_obj_has_state(row,LV_STATE_PRESSED)&&find(ui.screen,"After input reset"));
    puts("PASS input reset cancels the device selection and resumes discovery without connecting");
    status.last_error=ESP_FAIL;status.acl_status=9;status.connection_attempts=1;status.state=AETHER_BT_IDLE;advance(350);
    lv_obj_t *error=find(ui.screen,"ACL 连接失败：0x09");assert(error);
    lv_area_t message_area,list_area;lv_obj_get_coords(error,&message_area);lv_obj_get_coords(lv_obj_get_parent(row),&list_area);
    assert(message_area.y2<list_area.y1);
    status.last_error=ESP_OK;status.state=AETHER_BT_DISCONNECTING;status.connected=true;status.sample_rate=44100;advance(350);
    lv_obj_t *route=lv_obj_get_parent(find(ui.screen,"使用耳机"));
    lv_obj_t *scan=lv_obj_get_parent(find(ui.screen,"搜索耳机"));
    assert(lv_obj_has_state(route,LV_STATE_DISABLED)&&lv_obj_has_state(scan,LV_STATE_DISABLED)&&lv_obj_has_state(row,LV_STATE_DISABLED));
    status.state=AETHER_BT_STOPPING;status.connected=false;advance(350);
    assert(lv_obj_has_state(route,LV_STATE_DISABLED)&&lv_obj_has_state(scan,LV_STATE_DISABLED)&&lv_obj_has_state(row,LV_STATE_DISABLED));
    puts("PASS ACL failure detail stays above device list; disconnecting/late connected and stopping disable scan/select/output");
    status.state=AETHER_BT_CONNECTED;status.connected=true;status.channels=2;
    strcpy(status.peer.name,"Paired headphones");audio.applied_output=APP_AUDIO_OUTPUT_BLUETOOTH;
    advance(350);
    lv_obj_t *speaker=lv_obj_get_parent(find(ui.screen,"使用扬声器"));
    assert(find(ui.screen,"Paired headphones")&&find(ui.screen,"44100 Hz · 立体声"));
    assert(lv_obj_has_state(route,LV_STATE_CHECKED)&&!lv_obj_has_state(speaker,LV_STATE_CHECKED));
    assert(lv_obj_get_height(route)>=44&&lv_obj_get_height(row)>=44);
    audio.applied_output=APP_AUDIO_OUTPUT_SPEAKER;advance(350);
    assert(!lv_obj_has_state(route,LV_STATE_CHECKED)&&lv_obj_has_state(speaker,LV_STATE_CHECKED));
    puts("PASS audio format and applied output stay visible, with full-size controls");
    if(ui.timer)lv_timer_delete(ui.timer);
    if(ui.page_cleanup)ui.page_cleanup(&ui);
    lv_obj_delete(ui.screen);lv_indev_delete(in);assert(lv_mem_test()==LV_RESULT_OK);lv_display_delete(d);lv_deinit();
}
