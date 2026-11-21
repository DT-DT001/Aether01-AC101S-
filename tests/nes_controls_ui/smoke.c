#include "app_ui_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/app_nes_controls.c"
extern const lv_font_t font_16,font_24;
void app_open_nes_controls(app_ui_t *);
static uint16_t framebuffer[480*480],draw_buffer[480*40];
static TickType_t now;
static bool busy,stored;
static settings_t saved;
static unsigned writes;
TickType_t xTaskGetTickCount(void){return now;}
void vTaskDelay(TickType_t t){now+=t;}
BaseType_t xTaskCreate(void (*f)(void*),const char *name,uint32_t stack,void *ctx,int prio,TaskHandle_t *out)
{assert(f&&!strcmp(name,"nes_keys")&&stack==3072&&!ctx&&prio==2);*out=(void*)1;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t h){assert(h==(void*)1);}
uint32_t ulTaskNotifyTake(BaseType_t b,TickType_t t){(void)b;(void)t;return 0;}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out){assert(!strcmp(name,"nes_keys"));if(mode==NVS_READONLY&&!stored)return ESP_ERR_NVS_NOT_FOUND;*out=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n){assert(h==1&&!strcmp(key,"mapping")&&*n==sizeof(saved));memcpy(p,&saved,sizeof(saved));return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n){assert(h==1&&!strcmp(key,"mapping")&&n==sizeof(saved));++writes;memcpy(&saved,p,n);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);stored=true;return ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1);}
const char *esp_err_to_name(esp_err_t e){return e?"ERROR":"ESP_OK";}
static bool guard(void *ctx){(void)ctx;return busy;}
const lv_font_t *app_ui_chinese_font_size(unsigned px){static lv_font_t a,b;a=font_16;b=font_24;a.fallback=&lv_font_montserrat_16;b.fallback=&lv_font_montserrat_24;return px>=24?&b:&a;}
const lv_font_t *app_ui_chinese_font(void){return app_ui_chinese_font_size(16);}
void app_ui_label_text(lv_obj_t *o,const char *text){if(strcmp(lv_label_get_text(o),text))lv_label_set_text(o,text);}
void app_ui_label_fmt(lv_obj_t *o,const char *f,...){char b[512];va_list a;va_start(a,f);vsnprintf(b,sizeof(b),f,a);va_end(a);app_ui_label_text(o,b);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *text,lv_align_t align,int32_t x,int32_t y,uint32_t color,const lv_font_t *font)
{lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,text);lv_obj_set_style_text_font(o,font?font:app_ui_chinese_font(),0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_obj_align(o,align,x,y);return o;}
lv_obj_t *app_ui_begin_page(app_ui_t *ui,const char *title)
{
 if(ui->timer){lv_timer_delete(ui->timer);ui->timer=NULL;}if(ui->page_cleanup){ui->page_cleanup(ui);ui->page_cleanup=NULL;}
 lv_obj_t *old=ui->screen;ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_style_bg_color(ui->screen,lv_color_hex(0x080D18),0);lv_obj_set_style_text_font(ui->screen,app_ui_chinese_font(),0);
 app_ui_add_text(ui->screen,title,LV_ALIGN_TOP_MID,0,20,0xEAF2FF,app_ui_chinese_font_size(24));
 ui->content=lv_obj_create(ui->screen);lv_obj_set_size(ui->content,448,394);lv_obj_align(ui->content,LV_ALIGN_BOTTOM_MID,0,-14);
 lv_obj_set_style_pad_all(ui->content,20,0);lv_obj_set_style_border_width(ui->content,0,0);lv_obj_set_style_radius(ui->content,24,0);lv_obj_set_style_bg_color(ui->content,lv_color_hex(0x10192A),0);
 lv_screen_load(ui->screen);if(old)lv_obj_delete(old);return ui->content;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t period){ui->timer=lv_timer_create(cb,period,ui);}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){const uint16_t *v=(const uint16_t*)p;for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x){assert(x>=0&&x<480&&y>=0&&y<480);framebuffer[y*480+x]=*v++;}lv_display_flush_ready(d);}
static void advance(unsigned ms){for(unsigned i=0;i<ms;++i){++now;lv_tick_inc(1);lv_timer_handler();}}
static lv_obj_t *dropdown(app_ui_t *ui,unsigned action,unsigned slot)
{for(unsigned i=0;i<lv_obj_get_child_count(ui->content);++i){lv_obj_t *o=lv_obj_get_child(ui->content,i);if(lv_obj_check_type(o,&lv_dropdown_class)&&(uintptr_t)lv_obj_get_user_data(o)==action*2+slot)return o;}return NULL;}
static lv_obj_t *find(lv_obj_t *o,const char *text)
{if(lv_obj_check_type(o,&lv_label_class)&&strstr(lv_label_get_text(o),text))return o;for(unsigned i=0;i<lv_obj_get_child_count(o);++i){lv_obj_t *r=find(lv_obj_get_child(o,i),text);if(r)return r;}return NULL;}
static void choose(app_ui_t *ui,unsigned action,unsigned slot,uint8_t usage)
{lv_obj_t *d=dropdown(ui,action,slot);assert(d);lv_dropdown_set_selected(d,app_nes_controls_key_index(usage));lv_obj_send_event(d,LV_EVENT_VALUE_CHANGED,NULL);}
static uint16_t mapped(uint8_t usage){uint8_t keys[32]={0};keys[usage/8]|=1U<<(usage%8);return app_nes_controls_map(NULL,keys);}
static void snapshot(lv_display_t *d,const char *dir,const char *name)
{lv_refr_now(d);char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");for(unsigned i=0;i<480*480;++i){uint16_t p=framebuffer[i];uint8_t rgb[]={(uint8_t)(((p>>11)&31)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};assert(fwrite(rgb,1,3,f)==3);}assert(!fclose(f));}
int main(int argc,char **argv)
{
 assert(argc==2);setbuf(stdout,NULL);lv_init();lv_display_t *d=lv_display_create(480,480);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
 assert(app_nes_controls_init()==ESP_OK);app_nes_controls_set_persistence_guard(guard,NULL);app_ui_t ui={0};app_open_nes_controls(&ui);advance(100);
 for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a)for(unsigned k=0;k<2;++k){lv_obj_t *o=dropdown(&ui,a,k);assert(o&&lv_dropdown_get_option_count(o)==app_nes_controls_key_count()&&lv_dropdown_get_selected(o)==app_nes_controls_key_index(defaults[a][k]));}
 snapshot(d,argv[1],"nes-keymap-defaults");
 choose(&ui,APP_NES_CONTROL_A,0,0x0a);assert(mapped(0x0a)==16&&mapped(0x1d)==0&&writes==0);
 lv_dropdown_open(dropdown(&ui,APP_NES_CONTROL_B,0));
 choose(&ui,APP_NES_CONTROL_B,0,0x0a);assert(mapped(0x1b)==32&&lv_dropdown_get_selected(dropdown(&ui,APP_NES_CONTROL_B,0))==app_nes_controls_key_index(0x1b));assert(find(ui.screen,"此键已用于"));
 lv_dropdown_close(dropdown(&ui,APP_NES_CONTROL_B,0));
 choose(&ui,APP_NES_CONTROL_A,0,0);choose(&ui,APP_NES_CONTROL_B,0,0x0a);choose(&ui,APP_NES_CONTROL_EXIT,0,0x45);
 assert(mapped(0x0a)==32&&mapped(0x45)==1024&&mapped(0x29)==0);
 busy=true;advance(300);assert(service_once()==250&&writes==0);advance(300);assert(find(ui.screen,"退出媒体后自动保存"));
 busy=false;assert(service_once()==portMAX_DELAY&&writes==1);advance(300);assert(find(ui.screen,"已保存"));
 puts("PASS actual11-action/22-selector LVGL page applies HID keys, rejects/reverts conflicts, supports unbind/alternate/exit binding and displays deferred/saved NVS state");
 lv_obj_t *reset=lv_obj_get_parent(find(ui.screen,"恢复默认"));lv_obj_send_event(reset,LV_EVENT_CLICKED,NULL);assert(mapped(0x1d)==16&&mapped(0x1b)==32&&mapped(0x29)==1024);advance(300);assert(service_once()==portMAX_DELAY);s.view.ready=false;assert(app_nes_controls_init()==ESP_OK);assert(mapped(0x1d)==16&&mapped(0x45)==0);
 lv_obj_scroll_to_y(ui.content,690,LV_ANIM_OFF);advance(300);snapshot(d,argv[1],"nes-keymap-exit-save");
 for(unsigned i=0;i<100;++i){app_open_nes_controls(&ui);advance(1);assert(dropdown(&ui,10,1));}
 if(ui.timer)lv_timer_delete(ui.timer);
 if(ui.page_cleanup)ui.page_cleanup(&ui);
 lv_obj_delete(ui.screen);assert(lv_mem_test()==LV_RESULT_OK);lv_display_delete(d);lv_deinit();
 puts("PASS actual restore-default action persists/reloads all bindings and100 page open/cleanup cycles preserve the LVGL heap");
}
