#include "app_ui_internal.h"
#include "app_telemetry.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G 9.80665f
#define RAD 0.01745329252f
extern const lv_font_t font_16,font_24;
static uint16_t framebuffer[480*480],draw_buffer[480*40];
static int64_t now_us=1000000;
static app_telemetry_snapshot_t sample;
static uint32_t interest;
static unsigned interest_updates,gets,dirty_calls;
static const char *output;

int64_t esp_timer_get_time(void) {return now_us;}
const char *esp_err_to_name(esp_err_t e) {return e==ESP_OK?"ESP_OK":e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":"ESP_ERR_NOT_FOUND";}
bool app_power_is_sleeping(const app_power_t *p) {return p&&p->sleeping;}
void app_telemetry_get(app_telemetry_snapshot_t *s) {++gets;*s=sample;}
void app_telemetry_set_interest(uint32_t mask) {interest=mask;++interest_updates;}
const lv_font_t *app_ui_chinese_font_size(unsigned px)
{
    static lv_font_t small,large;
    static bool initialized;
    if(!initialized){small=font_16;large=font_24;small.fallback=&lv_font_montserrat_16;large.fallback=&lv_font_montserrat_24;initialized=true;}
    return px>=24?&large:&small;
}
const lv_font_t *app_ui_chinese_font(void) {return app_ui_chinese_font_size(16);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *s,lv_align_t a,int32_t x,int32_t y,uint32_t c,const lv_font_t *f)
{
    lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);
    lv_obj_set_style_text_font(o,f?f:app_ui_chinese_font(),0);
    lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_align(o,a,x,y);return o;
}
void app_ui_label_text(lv_obj_t *o,const char *s)
{if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
void app_ui_label_fmt(lv_obj_t *o,const char *fmt,...)
{char s[512];va_list a;va_start(a,fmt);vsnprintf(s,sizeof(s),fmt,a);va_end(a);app_ui_label_text(o,s);}
lv_obj_t *app_ui_begin_page(app_ui_t *ui,const char *title)
{
    app_telemetry_set_interest(0); /* Same page-common subscription ownership. */
    if(ui->timer){lv_timer_delete(ui->timer);ui->timer=NULL;}
    if(ui->page_cleanup){void(*cleanup)(app_ui_t*)=ui->page_cleanup;ui->page_cleanup=NULL;cleanup(ui);}
    assert(!ui->page_context);
    lv_obj_t *old=ui->screen;
    ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen,lv_color_hex(0x070A12),0);
    lv_obj_set_style_bg_opa(ui->screen,LV_OPA_COVER,0);
    lv_obj_set_style_text_font(ui->screen,app_ui_chinese_font(),0);
    lv_obj_t *back=lv_button_create(ui->screen);lv_obj_set_pos(back,14,12);lv_obj_set_size(back,72,44);
    lv_obj_set_style_radius(back,18,0);lv_obj_set_style_bg_color(back,lv_color_hex(0x18243A),0);
    app_ui_add_text(back,"<",LV_ALIGN_CENTER,0,0,0xBFD7FF,app_ui_chinese_font());
    app_ui_add_text(ui->screen,title,LV_ALIGN_TOP_MID,0,20,0xEAF2FF,app_ui_chinese_font_size(24));
    ui->content=lv_obj_create(ui->screen);lv_obj_set_size(ui->content,448,394);
    lv_obj_align(ui->content,LV_ALIGN_BOTTOM_MID,0,-14);lv_obj_set_style_radius(ui->content,24,0);
    lv_obj_set_style_bg_color(ui->content,lv_color_hex(0x10192A),0);
    lv_obj_set_style_border_width(ui->content,0,0);lv_obj_set_style_pad_all(ui->content,20,0);
    lv_screen_load(ui->screen);if(old)lv_obj_delete(old);return ui->content;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t period)
{assert(!ui->timer);ui->timer=lv_timer_create(cb,period,ui);assert(ui->timer);}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *bytes)
{
    const uint16_t *p=(const uint16_t*)bytes;
    for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x){assert(x>=0&&x<480&&y>=0&&y<480);framebuffer[y*480+x]=*p++;}
    lv_display_flush_ready(d);
}
static void advance(unsigned ms)
{for(unsigned i=0;i<ms;++i){++dirty_calls;now_us+=1000;lv_tick_inc(1);lv_timer_handler();}}
static void input(float ax,float ay,float az,float gx,float gy,float gz)
{sample=(app_telemetry_snapshot_t){.imu={.accel_mps2={ax,ay,az},.gyro_dps={gx,gy,gz}},.imu_updated_ms=(uint32_t)(now_us/1000)};}
static void feed(float ax,float ay,float az,float gx,float gy,float gz,unsigned count)
{for(unsigned i=0;i<count;++i){input(ax,ay,az,gx,gy,gz);advance(20);}lv_refr_now(NULL);}
static void pose(float roll,float pitch,unsigned count)
{feed(-sinf(pitch*RAD)*G,sinf(roll*RAD)*cosf(pitch*RAD)*G,cosf(roll*RAD)*cosf(pitch*RAD)*G,0,0,0,count);}
static lv_obj_t *find(lv_obj_t *p,const char *s)
{
    if(lv_obj_check_type(p,&lv_label_class)&&!strcmp(lv_label_get_text(p),s))return p;
    for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *o=find(lv_obj_get_child(p,(int32_t)i),s);if(o)return o;}return NULL;
}
static lv_obj_t *model(app_ui_t *ui)
{
    for(unsigned i=0;i<lv_obj_get_child_count(ui->content);++i){lv_obj_t *o=lv_obj_get_child(ui->content,(int32_t)i);if(lv_obj_get_width(o)==252&&lv_obj_get_height(o)==166)return o;}
    assert(!"model missing");return NULL;
}
static float value(app_ui_t *ui,unsigned index)
{
    for(unsigned i=0;i<lv_obj_get_child_count(ui->content);++i){lv_obj_t *o=lv_obj_get_child(ui->content,(int32_t)i);if(lv_obj_check_type(o,&lv_label_class)&&lv_obj_get_x(o)==8+(int32_t)index*134&&lv_obj_get_y(o)==228)return strtof(lv_label_get_text(o),NULL);}
    assert(!"angle value missing");return NAN;
}
static uint32_t model_hash(app_ui_t *ui)
{
    lv_refr_now(NULL);lv_obj_update_layout(ui->screen);lv_area_t a;lv_obj_get_coords(model(ui),&a);
    uint32_t h=2166136261U;
    for(int y=a.y1+8;y<=a.y2-8;++y)for(int x=a.x1+8;x<=a.x2-8;++x)h=(h^framebuffer[y*480+x])*16777619U;
    return h;
}
static uint32_t next_codepoint(const char **s)
{
    const unsigned char *p=(const unsigned char*)*s;uint32_t c=*p++;
    if(c>=0xf0){c=(c&7)<<18;c|=(uint32_t)(*p++&63)<<12;c|=(uint32_t)(*p++&63)<<6;c|=*p++&63;}
    else if(c>=0xe0){c=(c&15)<<12;c|=(uint32_t)(*p++&63)<<6;c|=*p++&63;}
    else if(c>=0xc0){c=(c&31)<<6;c|=*p++&63;}
    *s=(const char*)p;return c;
}
static void check_labels(lv_obj_t *p)
{
    if(lv_obj_check_type(p,&lv_label_class)){
        lv_area_t a;lv_obj_get_coords(p,&a);
        if(a.x1<0||a.y1<0||a.x2>=480||a.y2>=480)fprintf(stderr,"out of bounds %s (%d,%d)-(%d,%d)\n",lv_label_get_text(p),(int)a.x1,(int)a.y1,(int)a.x2,(int)a.y2);
        assert(a.x1>=0&&a.y1>=0&&a.x2<480&&a.y2<480);
        const lv_font_t *font=lv_obj_get_style_text_font(p,0);
        for(const char *s=lv_label_get_text(p);*s;){uint32_t cp=next_codepoint(&s);if(cp=='\n'||cp=='\r')continue;lv_font_glyph_dsc_t d;
            if(!lv_font_get_glyph_dsc(font,&d,cp,0)){fprintf(stderr,"missing glyph U+%04X in %s\n",(unsigned)cp,lv_label_get_text(p));assert(!"missing supplied/fallback font glyph");}}
    }
    for(unsigned i=0;i<lv_obj_get_child_count(p);++i)check_labels(lv_obj_get_child(p,(int32_t)i));
}
static void layout(app_ui_t *ui)
{
    lv_obj_update_layout(ui->screen);check_labels(ui->screen);
    lv_obj_t *caption=find(ui->screen,"姿态归零"),*footer=find(ui->screen,"六轴航向为相对值，长时间使用会漂移");
    assert(caption&&footer);lv_area_t a,b;lv_obj_get_coords(lv_obj_get_parent(caption),&a);lv_obj_get_coords(footer,&b);
    assert(a.y2<b.y1);assert(lv_obj_get_scroll_y(ui->content)==0);
}
static void capture(app_ui_t *ui,const char *name)
{
    lv_refr_now(NULL);layout(ui);char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",output,name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");
    for(unsigned i=0;i<480*480;++i){uint16_t p=framebuffer[i];uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}assert(!fclose(f));
}
static void level(app_ui_t *ui)
{input(0,0,G,0,0,0);app_open_motion(ui);pose(0,0,10);assert(interest==APP_TELEMETRY_IMU&&find(ui->screen,"静止"));}
static void test_poses(app_ui_t *ui)
{
    level(ui);uint32_t stand=model_hash(ui);capture(ui,"stand");
    pose(40,0,200);assert(find(ui->screen,"倾斜")&&value(ui,0)>35&&fabsf(value(ui,1))<2);
    uint32_t roll=model_hash(ui);assert(roll!=stand);
    pose(0,30,200);assert(value(ui,1)>25&&fabsf(value(ui,0))<2);
    uint32_t pitch=model_hash(ui);assert(pitch!=roll&&pitch!=stand);
    pose(40,-25,200);capture(ui,"tilt");
    pose(0,0,200);uint32_t flat=model_hash(ui);
    feed(0,0,G,0,0,90,35);assert(find(ui->screen,"旋转")&&value(ui,2)>50);
    uint32_t yaw=model_hash(ui);assert(yaw!=flat);capture(ui,"rotate");
    lv_obj_t *zero=find(ui->screen,"姿态归零");assert(zero);
    lv_obj_send_event(lv_obj_get_parent(zero),LV_EVENT_CLICKED,NULL);lv_refr_now(NULL);
    assert(fabsf(value(ui,0))<1&&fabsf(value(ui,1))<1&&fabsf(value(ui,2))<1&&model_hash(ui)!=yaw);
    printf("PASS actual model pixels respond to roll/pitch/yaw; angle labels and zero button reset orientation (hashes %08x %08x %08x %08x)\n",stand,roll,pitch,yaw);
}
static void test_actions(app_ui_t *ui)
{
    level(ui);feed(0,0,G+4,0,0,0,15);assert(find(ui->screen,"移动"));layout(ui);
    level(ui);for(unsigned i=0;i<20;++i)feed(i&1?5:-5,0,G,0,0,0,1);assert(find(ui->screen,"摇晃"));layout(ui);
    level(ui);feed(0,0,G*2.3f,0,0,0,10);assert(find(ui->screen,"冲击"));layout(ui);
    level(ui);feed(0,0,0,0,0,0,10);assert(find(ui->screen,"失重"));layout(ui);
    puts("PASS real telemetry-driven action labels: still, tilted, turning, moving, shaking, impact and freefall");
}
static void test_invalid_and_sleep(app_ui_t *ui)
{
    level(ui);pose(20,15,100);uint32_t hash=model_hash(ui);float roll=value(ui,0),pitch=value(ui,1),yaw=value(ui,2);
    sample.imu.gyro_dps[2]=180;advance(500);assert(model_hash(ui)==hash&&value(ui,0)==roll&&value(ui,1)==pitch&&value(ui,2)==yaw);
    sample.imu_updated_ms-=40;advance(120);assert(model_hash(ui)==hash);
    sample.imu_error=ESP_ERR_TIMEOUT;advance(120);assert(find(ui->screen,"等待采样")&&model_hash(ui)==hash);layout(ui);
    sample.imu_error=ESP_OK;sample.imu_updated_ms=(uint32_t)(now_us/1000);sample.imu.accel_mps2[0]=NAN;
    advance(120);assert(find(ui->screen,"数据异常")&&model_hash(ui)==hash);layout(ui);
    input(0,0,G,0,0,INFINITY);advance(120);assert(find(ui->screen,"数据异常")&&model_hash(ui)==hash);
    unsigned before=interest_updates;ui->power->sleeping=true;advance(40);
    assert(interest==0&&interest_updates==before+1);const unsigned stopped_gets=gets;
    feed(0,0,G,0,0,180,50);assert(interest==0&&interest_updates==before+1&&gets==stopped_gets&&model_hash(ui)==hash);
    ui->power->sleeping=false;sample.imu_error=ESP_ERR_TIMEOUT;advance(40);
    assert(interest==APP_TELEMETRY_IMU&&interest_updates==before+2&&find(ui->screen,"等待采样"));
    pose(0,0,20);assert(find(ui->screen,"静止"));layout(ui);
    puts("PASS duplicate/old timestamps and invalid/stale telemetry never fake motion; sleep releases IMU once and wake restores subscription");
}
int main(int argc,char **argv)
{
    assert(argc==2);output=argv[1];setbuf(stdout,NULL);lv_init();
    lv_display_t *display=lv_display_create(480,480);assert(display);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);app_power_t power={0};app_ui_t ui={.power=&power};
    test_poses(&ui);test_actions(&ui);test_invalid_and_sleep(&ui);
    for(unsigned i=0;i<100;++i){level(&ui);feed(0,0,G,0,0,60,5);app_ui_begin_page(&ui,"主页");assert(!interest&&!ui.timer&&!ui.page_context&&lv_mem_test()==LV_RESULT_OK);}
    lv_obj_delete(ui.screen);lv_display_delete(display);assert(lv_mem_test()==LV_RESULT_OK);lv_deinit();
    printf("PASS 100 actual motion/home rebuilds, real model draws, all labels within 480px, supplied-font glyphs and LVGL allocator integrity (%u scheduler ticks)\n",dirty_calls);
    return 0;
}
