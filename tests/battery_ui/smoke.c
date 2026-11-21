#include "app_ui_internal.h"
#include "app_battery.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const lv_font_t font_16, font_24;
static uint16_t framebuffer[480*480], draw_buffer[480*20];
static app_tools_snapshot_t tools;
static uint64_t now_ms=10000;
static unsigned flushes;

void app_tools_get_snapshot(app_tools_snapshot_t *snapshot) {*snapshot=tools;}
int64_t esp_timer_get_time(void) {return (int64_t)(now_ms*1000U);}
const char *esp_err_to_name(esp_err_t error)
{return error==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":error?"ESP_FAIL":"ESP_OK";}
const lv_font_t *app_ui_chinese_font_size(unsigned px)
{static lv_font_t s,l;s=font_16;l=font_24;s.fallback=&lv_font_montserrat_16;l.fallback=&lv_font_montserrat_24;return px>=24?&l:&s;}
const lv_font_t *app_ui_chinese_font(void) {return app_ui_chinese_font_size(16);}
void app_ui_label_text(lv_obj_t *object,const char *text)
{if(strcmp(lv_label_get_text(object),text))lv_label_set_text(object,text);}
lv_obj_t *app_ui_add_text(lv_obj_t *parent,const char *text,lv_align_t align,int32_t x,int32_t y,uint32_t color,const lv_font_t *font)
{lv_obj_t *o=lv_label_create(parent);lv_label_set_text(o,text);lv_obj_set_style_text_font(o,font?font:app_ui_chinese_font(),0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_obj_align(o,align,x,y);return o;}
lv_obj_t *app_ui_new_screen(app_ui_t *ui)
{ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(ui->screen,lv_color_hex(0x070A12),0);lv_obj_set_style_text_font(ui->screen,app_ui_chinese_font(),0);return ui->screen;}
void app_ui_show_home(app_ui_t *ui) {app_ui_begin_page(ui,"首页");}
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{++flushes;uint16_t *s=(uint16_t*)pixels;for(int y=area->y1;y<=area->y2;++y)for(int x=area->x1;x<=area->x2;++x){assert(x>=0&&x<480&&y>=0&&y<480);framebuffer[y*480+x]=*s++;}lv_display_flush_ready(display);}
static void advance(unsigned ms)
{for(unsigned i=0;i<ms;++i){++now_ms;lv_tick_inc(1);lv_timer_handler();}}
static lv_obj_t *find(lv_obj_t *parent,const char *text)
{if(lv_obj_check_type(parent,&lv_label_class)&&!strcmp(lv_label_get_text(parent),text))return parent;for(unsigned i=0;i<lv_obj_get_child_count(parent);++i){lv_obj_t *r=find(lv_obj_get_child(parent,i),text);if(r)return r;}return NULL;}
static unsigned timers(void)
{unsigned count=0;for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))++count;return count;}
static void fresh(void) {tools.battery_updated_ms=(uint32_t)now_ms;}
static lv_obj_t *expect(app_ui_t *ui,const char *text)
{
    advance(1000);lv_obj_t *label=find(ui->screen,text);assert(label);
    lv_obj_t *badge=lv_obj_get_parent(label);assert(lv_obj_get_child_count(badge)==2);
    assert(!lv_obj_has_flag(badge,LV_OBJ_FLAG_CLICKABLE));
    lv_area_t a,b;lv_obj_get_coords(badge,&a);lv_obj_get_coords(label,&b);
    assert(b.x1>=a.x1&&b.x2<=a.x2&&b.y1>=a.y1&&b.y2<=a.y2);
    const lv_font_t *font=lv_obj_get_style_text_font(label,LV_PART_MAIN);
    lv_point_t actual;lv_text_get_size(&actual,text,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    assert(actual.x<=lv_obj_get_width(label));return label;
}
static void detail_contains(const char *a,const char *b)
{char text[320];app_battery_format_detail(&tools,text,sizeof(text));assert(strstr(text,a));if(b)assert(strstr(text,b));}
static void capture(lv_display_t *display,const char *directory,const char *name)
{
    lv_refr_now(display);char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");
    for(unsigned i=0;i<480*480;++i){uint16_t p=framebuffer[i];uint8_t rgb[]={(p>>11)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}fclose(f);
}

int main(int argc,char **argv)
{
    assert(argc==2);setbuf(stdout,NULL);lv_init();lv_display_t *display=lv_display_create(480,480);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);app_ui_t ui={0};
    app_ui_begin_page(&ui,"反应力小游戏");advance(200);
    assert(find(ui.screen,"--%"));capture(display,argv[1],"battery-unknown");
    tools=(app_tools_snapshot_t){.ready=true,.battery={.presence_valid=true,.battery_present=true,
        .percentage_valid=true,.voltage_valid=true,.voltage_mv=3975,.charge_state_valid=true}};
    static const unsigned levels[]={0,1,15,16,20,50,99,100};
    for(unsigned i=0;i<sizeof(levels)/sizeof(levels[0]);++i){
        tools.battery.percentage=(uint8_t)levels[i];fresh();char text[20];snprintf(text,sizeof(text),"%u%%",levels[i]);
        lv_obj_t *label=expect(&ui,text);lv_color_t color=lv_obj_get_style_text_color(label,LV_PART_MAIN);
        assert(lv_color_eq(color,lv_color_hex(levels[i]<=15?0xFF879A:0xD6E5F5)));
    }
    tools.battery.percentage=12;fresh();expect(&ui,"12%");capture(display,argv[1],"battery-low");
    tools.battery.percentage=100;tools.battery.vbus_present=true;tools.battery.charging=true;fresh();
    lv_obj_t *charge=expect(&ui,"100% 充电");detail_contains("100%（TG29估计）","3975 mV");
    lv_obj_t *badge=lv_obj_get_parent(charge),*title=find(ui.screen,"反应力小游戏");
    lv_area_t b,t;lv_obj_get_coords(badge,&b);lv_obj_get_coords(title,&t);
    assert(b.x1==350&&b.x2==465&&b.y1==20&&t.x2<b.x1);
    assert(lv_obj_get_width(lv_obj_get_child(ui.screen,0))==72);capture(display,argv[1],"battery-charging");
    fresh();advance(200);unsigned before=flushes;advance(2000);assert(flushes==before);

    tools.battery.percentage=85;tools.battery.charging=false;tools.battery.charge_done=true;fresh();
    expect(&ui,"85% 已满");detail_contains("充电完成",NULL);
    tools.battery.current_direction=2;fresh();expect(&ui,"85% 放电");detail_contains("电池放电",NULL);
    tools.battery.current_direction=0;tools.battery.charge_state_valid=false;fresh();
    expect(&ui,"85% USB");detail_contains("充电状态未知",NULL);
    tools.battery.percentage_valid=false;fresh();expect(&ui,"--% USB");detail_contains("--%（不可用）",NULL);
    tools.battery.percentage_valid=true;tools.battery.percentage=255;fresh();expect(&ui,"--% USB");

    tools.battery.percentage=87;tools.battery.presence_valid=false;tools.battery.charge_state_valid=true;
    tools.battery.charging=true;fresh();expect(&ui,"--% USB");detail_contains("存在性未知","VBAT 未知");
    tools.battery.presence_valid=true;tools.battery.battery_present=false;fresh();
    expect(&ui,"--% USB");detail_contains("未接电池","无有效电池");capture(display,argv[1],"battery-usb-no-battery");
    tools.battery.battery_present=true;tools.battery_error=ESP_FAIL;fresh();
    expect(&ui,"--%");detail_contains("电池 未知","ESP_FAIL");
    tools.battery_error=ESP_OK;fresh();tools.battery_updated_ms=(uint32_t)now_ms-6000U;
    expect(&ui,"87% 充电");expect(&ui,"--%");detail_contains("数据过期",NULL);capture(display,argv[1],"battery-stale");

    now_ms=(uint64_t)UINT32_MAX-100U;tools.battery.percentage=42;fresh();
    expect(&ui,"42% 充电");assert((uint32_t)now_ms<1000U);
    advance(7000);assert(find(ui.screen,"--%"));detail_contains("数据过期",NULL);
    fresh();tools.battery.voltage_mv=0;detail_contains("VBAT 未知",NULL);
    tools.battery.voltage_mv=0x3FFF;detail_contains("VBAT 未知",NULL);
    tools.battery.voltage_mv=3900;tools.battery.voltage_valid=false;detail_contains("VBAT 未知",NULL);
    tools.ready=false;fresh();expect(&ui,"--%");detail_contains("等待就绪",NULL);
    char tiny[3]={1,2,3};app_battery_format_detail(NULL,tiny,1);assert(!tiny[0]&&tiny[1]==2&&tiny[2]==3);

    tools.ready=true;tools.battery.voltage_valid=true;tools.battery.charging=false;tools.battery.charge_done=false;fresh();
    app_ui_begin_page(&ui,"设备状态");advance(200);char description[320];
    app_battery_format_detail(&tools,description,sizeof(description));
    lv_obj_t *details=app_ui_add_text(ui.content,description,LV_ALIGN_TOP_LEFT,0,0,0xC9DCF8,app_ui_chinese_font());
    lv_obj_set_width(details,402);lv_label_set_long_mode(details,LV_LABEL_LONG_WRAP);capture(display,argv[1],"battery-details");
    unsigned count=timers();fprintf(stderr,"battery UI: replacing 24 active pages\n");
    for(unsigned i=0;i<24;++i){fresh();app_ui_begin_page(&ui,"设备状态");advance(50);assert(timers()==count);assert(lv_mem_test()==LV_RESULT_OK);}
    fprintf(stderr,"battery UI: page replacement timer/heap checks passed\n");
    unsigned with_badge=timers();lv_obj_t *blank=lv_obj_create(NULL);
    lv_screen_load_anim(blank,LV_SCREEN_LOAD_ANIM_NONE,0,0,true);
    assert(timers()==with_badge-1);advance(1100);assert(lv_mem_test()==LV_RESULT_OK);
    lv_display_delete(display);lv_deinit();
    puts("PASS battery production UI: true 0..100/low colors; charging/done/USB/discharging priority; absent/unknown/invalid/error/stale; uint32 wrap; no idle repaint; exact common-header geometry; 24 screen replacements and timer cleanup; detailed text and heap");
}
