#include "app_ui_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const lv_font_t font_16,font_24;
static uint16_t framebuffer[480*480],buffer[480*40];
static bool mounted=true,held,usb_mounted;
static bool usb_blocked,failed_fs;
static unsigned retry_requests;
static unsigned catalog_calls, usb_generation=1,tf_generation=1;
static size_t catalog_entries=5;
static lv_point_t point;
static app_io_request_t request;
static unsigned route;
static char opened[192];
static const app_io_entry_t entries[]={
    {.path="/sdcard/Books",.directory=true},
    {.path="/sdcard/Mysterious Nue.m4a",.size=3265245},
    {.path="/sdcard/小说阅读测试：在山川与河流之间继续昨天未完的故事.txt",.size=280000},
    {.path="/sdcard/旅行视频.mp4",.size=127800000},
    {.path="/sdcard/backup.bin",.size=4096},
};
uint32_t aether_media_fs_generation(aether_storage_handle_t h,const char *p){(void)h;return !strncmp(p,"/usb",4)?usb_generation:tf_generation;}
bool aether_media_fs_path_ready(aether_storage_handle_t h,const char *p){(void)h;return !strncmp(p,"/usb",4)?usb_mounted:mounted;}
esp_err_t aether_storage_get_diagnostics(aether_storage_handle_t h,aether_storage_diagnostics_t *d){(void)h;*d=(aether_storage_diagnostics_t){.attempted=failed_fs,.layout=AETHER_STORAGE_LAYOUT_MBR,.filesystem=AETHER_STORAGE_FS_FAT32};return ESP_OK;}
esp_err_t aether_storage_request_retry(aether_storage_handle_t h){(void)h;++retry_requests;return ESP_OK;}
bool aether_storage_is_mounted(aether_storage_handle_t h){(void)h;return mounted;}
bool app_io_usb_storage_blocked(app_io_handle_t h){(void)h;return usb_blocked;}
esp_err_t aether_storage_get_status(aether_storage_handle_t h,aether_storage_status_t *s){(void)h;*s=(aether_storage_status_t){.mounted=mounted,.card_inserted=mounted||failed_fs};return ESP_OK;}
const char *esp_err_to_name(esp_err_t e){return e?"ESP_ERR_TIMEOUT":"ESP_OK";}
size_t strlcpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}
const lv_font_t *app_ui_chinese_font_size(unsigned px){static lv_font_t s,l;s=font_16;l=font_24;s.fallback=&lv_font_montserrat_16;l.fallback=&lv_font_montserrat_24;return px>=24?&l:&s;}
const lv_font_t *app_ui_chinese_font(void){return app_ui_chinese_font_size(16);}
void app_ui_label_text(lv_obj_t *o,const char *s){if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
void app_ui_label_fmt(lv_obj_t *o,const char *f,...){char s[512];va_list a;va_start(a,f);vsnprintf(s,sizeof(s),f,a);va_end(a);app_ui_label_text(o,s);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *s,lv_align_t a,int32_t x,int32_t y,uint32_t c,const lv_font_t *f){lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_style_text_font(o,f?f:app_ui_chinese_font(),0);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_align(o,a,x,y);return o;}
lv_obj_t *app_ui_begin_page(app_ui_t *ui,const char *title)
{
    ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(ui->screen,lv_color_hex(0x080D18),0);lv_obj_set_style_text_font(ui->screen,app_ui_chinese_font(),0);
    lv_obj_t *back=lv_button_create(ui->screen);lv_obj_set_pos(back,14,12);lv_obj_set_size(back,72,44);lv_obj_set_style_radius(back,18,0);lv_obj_set_style_bg_color(back,lv_color_hex(0x18243A),0);app_ui_add_text(back,"<",LV_ALIGN_CENTER,0,0,0xBFD7FF,&lv_font_montserrat_16);
    app_ui_add_text(ui->screen,title,LV_ALIGN_TOP_MID,0,20,0xEAF2FF,app_ui_chinese_font_size(24));
    ui->content=lv_obj_create(ui->screen);lv_obj_set_size(ui->content,448,394);lv_obj_align(ui->content,LV_ALIGN_BOTTOM_MID,0,-14);lv_obj_set_style_pad_all(ui->content,20,0);lv_obj_set_style_border_width(ui->content,0,0);lv_obj_set_style_radius(ui->content,24,0);lv_obj_set_style_bg_color(ui->content,lv_color_hex(0x10192A),0);lv_screen_load(ui->screen);return ui->content;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t p){ui->timer=lv_timer_create(cb,p,ui);}
void app_ui_request_io_ex(app_ui_t *ui,const app_io_request_t *r,void (*cb)(app_ui_t *,const app_io_result_t *)){++catalog_calls;request=*r;ui->io_complete=cb;}
static void deliver(app_ui_t *ui,esp_err_t error)
{
    assert(ui->io_complete);
    const size_t count = catalog_entries < request.limit ? catalog_entries : request.limit;
    app_io_entry_t *items = calloc(count, sizeof(*items));
    assert(items && count >= sizeof(entries) / sizeof(entries[0]));
    memcpy(items, entries, sizeof(entries));
    for (size_t i = sizeof(entries) / sizeof(entries[0]); i < count; ++i)
        snprintf(items[i].path, sizeof(items[i].path), "/sdcard/track-%04u.m4a", (unsigned)i);
    app_io_result_t r = {.operation=request.operation,.error=error,.entries=items,.count=count,
        .total_count=catalog_entries,.truncated=count < catalog_entries};
    void (*cb)(app_ui_t*,const app_io_result_t*) = ui->io_complete;
    ui->io_complete = NULL;
    cb(ui,&r);
    free(items);
}
#define OPEN(kind,number) void app_open_##kind##_path(app_ui_t *u,const char *p){(void)u;route=number;strlcpy(opened,p,sizeof(opened));}
OPEN(reader,1) OPEN(music,2) OPEN(video,3) OPEN(nes,4)
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){uint16_t *v=(uint16_t*)p;for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x)framebuffer[y*480+x]=*v++;lv_display_flush_ready(d);}
static void read_input(lv_indev_t *i,lv_indev_data_t *d){(void)i;d->point=point;d->state=held?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void advance(unsigned ms){for(unsigned i=0;i<ms;++i){lv_tick_inc(1);lv_timer_handler();}}
static lv_obj_t *find(lv_obj_t *p,const char *s){if(lv_obj_check_type(p,&lv_label_class)&&strstr(lv_label_get_text(p),s))return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *f=find(lv_obj_get_child(p,i),s);if(f)return f;}return NULL;}
static void tap(lv_obj_t *o){lv_area_t a;lv_obj_get_coords(o,&a);point=(lv_point_t){.x=(a.x1+a.x2)/2,.y=(a.y1+a.y2)/2};held=true;advance(80);held=false;advance(80);}
static void snapshot(lv_display_t *d,const char *dir,const char *name){lv_refr_now(d);char p[1024];snprintf(p,sizeof(p),"%s/%s.ppm",dir,name);FILE *f=fopen(p,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");for(unsigned i=0;i<480*480;++i){uint16_t v=framebuffer[i];uint8_t rgb[]={(v>>11)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}fclose(f);}
int main(int argc,char **argv)
{
    assert(argc==3);bool before=!strcmp(argv[2],"before");setbuf(stdout,NULL);lv_init();lv_display_t *d=lv_display_create(480,480);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    lv_indev_t *in=lv_indev_create();lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);lv_indev_set_display(in,d);lv_indev_set_read_cb(in,read_input);
    app_ui_t ui={.storage=(void*)1};app_open_files(&ui);deliver(&ui,ESP_OK);advance(200);snapshot(d,argv[1],before?"files-before":"files-after");
    lv_obj_t *name=find(ui.screen,"Mysterious Nue.m4a");assert(name);tap(lv_obj_get_parent(name));
    if(before){assert(route==0&&find(ui.screen,"暂不支持"));puts("BASELINE actual file browser rejects supported .m4a and creates circular-scroll filenames");}
    else{
        assert(route==2&&!strcmp(opened,entries[1].path));
        lv_obj_t *list=lv_obj_get_parent(lv_obj_get_parent(name));
        route=0;point=(lv_point_t){.x=250,.y=300};held=true;advance(60);
        for(int y=290;y>=220;y-=10){point.y=y;advance(30);}held=false;advance(1100);
        assert(!route&&!ui.io_complete&&lv_obj_get_scroll_y(list)>40);
        snapshot(d,argv[1],"files-after-scrolled");
        point=(lv_point_t){.x=250,.y=280};held=true;advance(60);
        for(int y=290;y<=370;y+=10){point.y=y;advance(30);}held=false;advance(1100);
        assert(!route&&!ui.io_complete);lv_obj_scroll_to_y(list,0,LV_ANIM_OFF);advance(80);
        lv_obj_t *refresh=lv_obj_get_parent(find(ui.screen,LV_SYMBOL_REFRESH));tap(refresh);assert(ui.io_complete&&lv_obj_has_state(refresh,LV_STATE_DISABLED));
        deliver(&ui,ESP_ERR_TIMEOUT);advance(80);assert(find(ui.screen,"读取失败"));assert(!lv_obj_has_state(refresh,LV_STATE_DISABLED));snapshot(d,argv[1],"files-after-error");
        assert(!find(ui.screen,"上一页")&&!find(ui.screen,"下一页"));
        catalog_entries=4100;
        tap(refresh);
        assert(request.limit==4096&&request.offset==0);
        deliver(&ui,ESP_OK);advance(80);
        assert(lv_obj_get_child_count(list)==13&&find(ui.screen,"部分项目未索引"));
        const unsigned scans=catalog_calls;
        lv_obj_scroll_to_y(list,INT32_MAX,LV_ANIM_OFF);advance(120);
        lv_obj_t *last=find(ui.screen,"track-4095.m4a");
        assert(last&&lv_obj_is_visible(last)&&catalog_calls==scans);
        tap(lv_obj_get_parent(last));
        assert(route==2&&!strcmp(opened,"/sdcard/track-4095.m4a"));
        catalog_entries=5;
        tap(refresh);assert(ui.io_complete);deliver(&ui,ESP_OK);mounted=false;advance(550);assert(find(ui.screen,"存储设备已移除"));
        assert(lv_obj_has_state(refresh,LV_STATE_DISABLED));mounted=true;advance(550);assert(ui.io_complete);deliver(&ui,ESP_OK);assert(find(ui.screen,"Mysterious Nue.m4a"));
        lv_timer_delete(ui.timer);ui.page_cleanup(&ui);lv_obj_delete(ui.screen);ui=(app_ui_t){.storage=(void*)1};
        app_open_files_path(&ui,"/sdcard/Recordings");assert(!strcmp(request.path,"/sdcard/Recordings"));
        deliver(&ui,ESP_ERR_NOT_FOUND);advance(80);refresh=lv_obj_get_parent(find(ui.screen,LV_SYMBOL_REFRESH));
        assert(!lv_obj_has_state(refresh,LV_STATE_DISABLED));tap(refresh);assert(ui.io_complete);deliver(&ui,ESP_OK);
        tap(lv_obj_get_parent(find(ui.screen,LV_SYMBOL_UP)));assert(!strcmp(request.path,"/sdcard"));deliver(&ui,ESP_OK);
        mounted=false;usb_blocked=true;advance(550);const unsigned before_ready=catalog_calls;
        mounted=true;advance(550);assert(catalog_calls==before_ready&&!ui.io_complete&&lv_obj_has_state(refresh,LV_STATE_DISABLED));
        usb_blocked=false;advance(550);assert(catalog_calls==before_ready+1&&ui.io_complete);deliver(&ui,ESP_OK);
        advance(550);assert(catalog_calls==before_ready+1&&!ui.io_complete&&!lv_obj_has_state(refresh,LV_STATE_DISABLED));
        ++tf_generation;advance(550);assert(ui.io_complete&&!strcmp(request.path,"/sdcard"));deliver(&ui,ESP_OK);
        lv_obj_t *source=NULL;
        for(unsigned i=0;i<lv_obj_get_child_count(ui.content);++i){lv_obj_t *o=lv_obj_get_child(ui.content,i);if(lv_obj_check_type(o,&lv_dropdown_class))source=o;}
        assert(source);usb_mounted=true;lv_dropdown_set_selected(source,1);lv_obj_send_event(source,LV_EVENT_VALUE_CHANGED,NULL);
        assert(ui.io_complete&&!strcmp(request.path,"/usb"));deliver(&ui,ESP_OK);
        assert(lv_obj_has_state(lv_obj_get_parent(find(ui.screen,LV_SYMBOL_UP)),LV_STATE_DISABLED));
        ++usb_generation;advance(550);
        assert(ui.io_complete&&!strcmp(request.path,"/usb"));deliver(&ui,ESP_OK);
        usb_mounted=false;advance(550);assert(find(ui.screen,"USB U盘未就绪")&&lv_obj_has_state(refresh,LV_STATE_DISABLED));
        assert(mounted);usb_mounted=true;advance(550);assert(ui.io_complete&&!strcmp(request.path,"/usb"));deliver(&ui,ESP_OK);
        lv_dropdown_set_selected(source,0);lv_obj_send_event(source,LV_EVENT_VALUE_CHANGED,NULL);
        assert(ui.io_complete&&!strcmp(request.path,"/sdcard"));deliver(&ui,ESP_OK);
        puts("PASS real file volume switch selects USB root, prevents root escape and tracks USB detach independently of present TF");
        mounted=false;failed_fs=true;advance(550);
        assert(find(ui.screen,"TF 挂载失败")&&!lv_obj_has_state(refresh,LV_STATE_DISABLED));
        tap(refresh);assert(retry_requests==1&&lv_obj_has_state(refresh,LV_STATE_DISABLED));
        advance(1100);assert(!lv_obj_has_state(refresh,LV_STATE_DISABLED));
        failed_fs=false;mounted=true;advance(550);assert(ui.io_complete);deliver(&ui,ESP_OK);
        puts("PASS FAT32 mount failure shows cached format/layout, permits bounded manual retry without formatting, and refreshes after recovery");
        puts("PASS files auto-refresh waits for both remount and USB-gate release; exactly one fresh catalog replaces the blocked state");
        puts("PASS file browser pointer scrolling, catalog-tail selection with 12 recycled rows, error/retry, hotplug and parent navigation");
    }
    lv_timer_delete(ui.timer);ui.page_cleanup(&ui);lv_obj_delete(ui.screen);lv_indev_delete(in);assert(lv_mem_test()==LV_RESULT_OK);lv_display_delete(d);lv_deinit();
}
