#include "app_ui_internal.h"
#include "app_audio_system.h"
#include "app_display_metrics.h"
#include "app_display_sync.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const lv_font_t font_16,font_24;
static uint16_t framebuffer[480*480],draw_buffer[480*40],pixels[480*480];
static aether_video_status_t video;
static aether_nes_status_t nes;
static bool sleeping;
bool app_power_is_sleeping(const void *p){(void)p;return sleeping;}
static app_io_request_t request;
static app_audio_system_snapshot_t master={.ready=true,.master_volume=35};
static app_ui_t *active_ui;
static lv_indev_t *input;
static bool held,touch_enabled=true,immediate_preview;
static bool storage_mounted=true,usb_blocked;
static unsigned catalog_calls;
static size_t catalog_fixture_count;
static lv_point_t point;
static int64_t now_us;
static const char *output;
static unsigned video_starts,video_stops,video_releases,nes_starts,nes_stops,nes_releases;
static unsigned media_release_posts,user_exit_arms;
static bool nes_viewport_enabled;
bool app_display_sync_present_nes(lv_obj_t *image, const uint16_t *pixels,
    uint16_t width, uint16_t height)
{
    assert(image&&pixels&&width==384&&height==336&&nes_viewport_enabled);
    return false; /* Exercise the software-composed fallback. */
}
void app_display_sync_set_nes_viewport(lv_display_t *display,const lv_area_t *area)
{
    assert(display==lv_display_get_default());
    nes_viewport_enabled=area!=NULL;
    if(area) assert(area->x1==48&&area->y1==96&&area->x2==431&&area->y2==431);
}
esp_err_t app_io_release_media(app_io_handle_t io){assert(io);++media_release_posts;return ESP_OK;}
void app_ui_set_user_key_exit(app_ui_t *ui,bool armed){assert(ui);if(armed)++user_exit_arms;}
static unsigned seek_attempts,seek_resets,seek_busy,volume_updates,volume_commits;
static uint32_t seek_target;
static uint8_t started_fps,set_fps;
static unsigned background_presses,background_clicks;
static bool measure_flush;
static unsigned flush_calls;
static int flush_ymin,flush_ymax;
static uint64_t flushed_pixels;
static esp_err_t play_error,rate_error,nes_error,seek_error;
static aether_video_frame_cb_t video_callback;
static aether_nes_frame_cb_t nes_callback;
static void *video_context,*nes_context;
struct test_mutex {bool held;};
SemaphoreHandle_t xSemaphoreCreateMutex(void){return calloc(1,sizeof(struct test_mutex));}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t t){(void)t;if(m->held)return pdFALSE;m->held=true;return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m){assert(m->held);m->held=false;return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t m){assert(!m->held);free(m);}
void *heap_caps_malloc(size_t n,unsigned caps){assert(caps==3);return malloc(n);}
size_t strlcpy(char *out,const char *in,size_t size){size_t n=strlen(in);if(size){size_t c=n<size-1?n:size-1;memcpy(out,in,c);out[c]=0;}return n;}
const char *esp_err_to_name(esp_err_t e){return e==ESP_OK?"ESP_OK":e==ESP_FAIL?"ESP_FAIL":e==ESP_ERR_INVALID_STATE?"ESP_ERR_INVALID_STATE":"ESP_ERR_NOT_SUPPORTED";}
int64_t esp_timer_get_time(void){return now_us;}
bool aether_storage_is_mounted(aether_storage_handle_t h){(void)h;return storage_mounted;}
bool app_io_usb_storage_blocked(app_io_handle_t h){(void)h;return usb_blocked;}
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *s){*s=master;}
esp_err_t app_audio_system_set_volume(uint8_t v,bool persist){assert(v<=100);master.master_volume=v;++volume_updates;if(persist)++volume_commits;return ESP_OK;}
esp_err_t app_audio_system_toggle_mute(bool p){return app_audio_system_set_volume(master.master_volume?0:70,p);}
const lv_font_t *app_ui_chinese_font_size(unsigned px){static lv_font_t s,l;static bool init;if(!init){s=font_16;l=font_24;s.fallback=&lv_font_montserrat_16;l.fallback=&lv_font_montserrat_24;init=true;}return px>=24?&l:&s;}
const lv_font_t *app_ui_chinese_font(void){return app_ui_chinese_font_size(16);}
void app_touch_set_enabled(lv_indev_t *i,bool enabled){assert(i);touch_enabled=enabled;}
void app_display_metrics_get(app_display_metrics_t *s){*s=(app_display_metrics_t){.available=true,.active=true,.refresh_fps_x100=2345};}
esp_err_t aether_video_player_play(aether_video_player_handle_t h,const char *p,uint8_t fps,aether_video_frame_cb_t cb,void *ctx)
{assert(h&&p&&cb&&ctx);++video_starts;started_fps=fps;if(play_error)return play_error;video_callback=cb;video_context=ctx;video=(aether_video_status_t){.state=AETHER_VIDEO_STARTING};return ESP_OK;}
esp_err_t aether_video_player_get_status(aether_video_player_handle_t h,aether_video_status_t *s){assert(h);*s=video;return ESP_OK;}
esp_err_t aether_video_player_set_fps(aether_video_player_handle_t h,uint8_t fps){assert(h);set_fps=fps;return rate_error;}
esp_err_t aether_video_player_set_paused(aether_video_player_handle_t h,bool p){assert(h);video.state=p?AETHER_VIDEO_PAUSED:AETHER_VIDEO_PLAYING;return ESP_OK;}
esp_err_t aether_video_player_stop(aether_video_player_handle_t h){assert(h);++video_stops;video.state=AETHER_VIDEO_STOPPED;return ESP_OK;}
void aether_video_player_release_frame(aether_video_player_handle_t h){assert(h);++video_releases;}
esp_err_t aether_video_player_seek_with_reset(aether_video_player_handle_t h,uint32_t ms,void(*reset)(void*),void *ctx)
{
    assert(h&&reset&&ctx);++seek_attempts;if(seek_busy){--seek_busy;return ESP_ERR_INVALID_STATE;}if(seek_error)return seek_error;
    assert(video.seek_supported&&ms<video.duration_ms);seek_target=ms;reset(ctx);++seek_resets;
    assert(lv_image_get_src(((app_ui_t*)ctx)->app_obj[0])==NULL);video.seeking=true;video.seek_requested_ms=ms;
    if(immediate_preview){for(unsigned i=0;i<320*180;++i)pixels[i]=0xFFE0;video.position_ms=ms;video.seeking=false;video_callback(pixels,320,180,99,video_context);}
    return ESP_OK;
}
esp_err_t aether_nes_start(aether_nes_handle_t h,const char *p,aether_nes_frame_cb_t cb,void *ctx)
{assert(h&&p&&cb&&ctx);++nes_starts;if(nes_error)return nes_error;nes_callback=cb;nes_context=ctx;nes.state=AETHER_NES_RUNNING;return ESP_OK;}
esp_err_t aether_nes_get_status(aether_nes_handle_t h,aether_nes_status_t *s){assert(h);*s=nes;return ESP_OK;}
esp_err_t aether_nes_stop(aether_nes_handle_t h){assert(h);++nes_stops;nes.state=AETHER_NES_STOPPED;return ESP_OK;}
void aether_nes_release_frame(aether_nes_handle_t h){assert(h);++nes_releases;}
void app_ui_label_text(lv_obj_t *o,const char *s){if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
void app_ui_label_fmt(lv_obj_t *o,const char *fmt,...){char s[1024];va_list a;va_start(a,fmt);vsnprintf(s,sizeof(s),fmt,a);va_end(a);app_ui_label_text(o,s);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *s,lv_align_t a,int32_t x,int32_t y,uint32_t c,const lv_font_t *f)
{lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_style_text_font(o,f?f:app_ui_chinese_font(),0);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_align(o,a,x,y);return o;}
static void header_back(lv_event_t *e){(void)app_ui_go_back(lv_event_get_user_data(e));}
lv_obj_t *app_ui_begin_page(app_ui_t *ui,const char *title)
{
    ++ui->page_generation;ui->io_complete=NULL;if(ui->timer){lv_timer_delete(ui->timer);ui->timer=NULL;}
    if(ui->page_cleanup){void(*c)(app_ui_t*)=ui->page_cleanup;ui->page_cleanup=NULL;c(ui);}
    ui->page_context=NULL;ui->page_back=NULL;memset(ui->app_obj,0,sizeof(ui->app_obj));ui->app_path[0]=0;ui->app_value=0;ui->media_started=false;
    lv_obj_t *old=ui->screen;ui->screen=lv_obj_create(NULL);lv_obj_remove_flag(ui->screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen,lv_color_hex(0x070A12),0);lv_obj_set_style_text_font(ui->screen,app_ui_chinese_font(),0);
    lv_obj_t *b=lv_button_create(ui->screen);lv_obj_set_pos(b,14,12);lv_obj_set_size(b,72,44);lv_obj_add_event_cb(b,header_back,LV_EVENT_CLICKED,ui);
    app_ui_add_text(b,LV_SYMBOL_LEFT,LV_ALIGN_CENTER,0,0,0xFFFFFF,&lv_font_montserrat_16);app_ui_add_text(ui->screen,title,LV_ALIGN_TOP_MID,0,20,0xFFFFFF,app_ui_chinese_font_size(24));
    ui->content=lv_obj_create(ui->screen);lv_obj_set_size(ui->content,448,394);lv_obj_align(ui->content,LV_ALIGN_BOTTOM_MID,0,-14);lv_obj_set_style_pad_all(ui->content,20,0);lv_obj_set_style_border_width(ui->content,0,0);
    lv_screen_load(ui->screen);if(old)lv_obj_delete(old);return ui->content;
}
void app_ui_show_home(app_ui_t *ui){app_ui_begin_page(ui,"主页");}
bool app_ui_go_back(app_ui_t *ui)
{
    if (ui->page_back) ui->page_back(ui);
    else app_ui_begin_page(ui,"应用抽屉");
    return true;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t ms){if(ui->timer)lv_timer_delete(ui->timer);ui->timer=lv_timer_create(cb,ms,ui);}
void app_ui_request_io_ex(app_ui_t *ui,const app_io_request_t *q,void(*c)(app_ui_t*,const app_io_result_t*)){assert(!ui->io_complete);if(q->operation==APP_IO_CATALOG)++catalog_calls;request=*q;ui->io_complete=c;}
static void deliver(app_ui_t *ui,esp_err_t error)
{
    assert(ui->io_complete);
    void (*complete)(app_ui_t*,const app_io_result_t*)=ui->io_complete;
    ui->io_complete=NULL;
    app_io_result_t r={.operation=request.operation,.error=error};
    strlcpy(r.path,request.path,sizeof(r.path));
    if (request.operation == APP_IO_CATALOG && catalog_fixture_count) {
        r.count=r.total_count=catalog_fixture_count;
        r.entries=calloc(r.count,sizeof(*r.entries));
        assert(r.entries);
        const bool games=request.filter==APP_IO_NES;
        for (size_t i=0;i<r.count;++i)
            snprintf(r.entries[i].path,sizeof(r.entries[i].path),"/sdcard/%s/item-%03u.%s",
                games?"ROMs":"Video",(unsigned)i,games?"nes":"mp4");
    }
    complete(ui,&r);
    free(r.entries);
}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *bytes)
{
    if(measure_flush){++flush_calls;flushed_pixels+=(uint64_t)lv_area_get_width(a)*lv_area_get_height(a);if(a->y1<flush_ymin)flush_ymin=a->y1;if(a->y2>flush_ymax)flush_ymax=a->y2;}
    const uint16_t *p=(const uint16_t*)bytes;for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x){assert(x>=0&&x<480&&y>=0&&y<480);framebuffer[y*480+x]=*p++;}lv_display_flush_ready(d);
}
static void read_input(lv_indev_t *i,lv_indev_data_t *d){(void)i;d->point=point;d->state=held&&touch_enabled?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void advance(unsigned ms){for(unsigned i=0;i<ms;++i){now_us+=1000;lv_tick_inc(1);lv_timer_handler();if(active_ui&&active_ui->media_active&&(lv_tick_get()%8)==0)app_frames_present(active_ui->frames,active_ui->app_obj[0]);}}
static lv_obj_t *find(lv_obj_t *p,const char *s){if(lv_obj_check_type(p,&lv_label_class)&&!strcmp(lv_label_get_text(p),s))return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *o=find(lv_obj_get_child(p,(int32_t)i),s);if(o)return o;}return NULL;}
static bool hidden(lv_obj_t *o){for(;o;o=lv_obj_get_parent(o))if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return true;return false;}
static lv_obj_t *slider(lv_obj_t *p,unsigned range){if(lv_obj_check_type(p,&lv_slider_class)&&(unsigned)lv_slider_get_max_value(p)==range)return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *o=slider(lv_obj_get_child(p,(int32_t)i),range);if(o)return o;}return NULL;}
static lv_area_t area(lv_obj_t *o){lv_obj_update_layout(o);lv_area_t a;lv_obj_get_coords(o,&a);return a;}
static void labels(lv_obj_t *p){if(!lv_obj_is_visible(p))return;if(lv_obj_check_type(p,&lv_label_class)){lv_area_t a=area(p);if(a.x1<0||a.y1<0||a.x2>=480||a.y2>=480)fprintf(stderr,"outside %s (%d,%d)-(%d,%d)\n",lv_label_get_text(p),(int)a.x1,(int)a.y1,(int)a.x2,(int)a.y2);assert(a.x1>=0&&a.y1>=0&&a.x2<480&&a.y2<480);}for(unsigned i=0;i<lv_obj_get_child_count(p);++i)labels(lv_obj_get_child(p,(int32_t)i));}
static void capture(app_ui_t *ui,const char *name){lv_refr_now(NULL);labels(ui->screen);char path[512];snprintf(path,sizeof(path),"%s/fullscreen-%s.ppm",output,name);FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");for(unsigned i=0;i<480*480;++i){uint16_t p=framebuffer[i];uint8_t c[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(c,1,3,f)==3);}assert(!fclose(f));}
static void tap(int x,int y){point=(lv_point_t){x,y};held=true;advance(80);held=false;advance(100);}
static void background_trace(lv_event_t *e){if(lv_event_get_code(e)==LV_EVENT_PRESSED)++background_presses;if(lv_event_get_code(e)==LV_EVENT_CLICKED)++background_clicks;}
static void click(app_ui_t *ui,const char *s){lv_obj_t *o=find(ui->screen,s);assert(o&&!hidden(o));lv_area_t a=area(lv_obj_get_parent(o));tap((a.x1+a.x2)/2,(a.y1+a.y2)/2);}
static void send_click(app_ui_t *ui,const char *s){lv_obj_t *o=find(ui->screen,s);assert(o);lv_obj_send_event(lv_obj_get_parent(o),LV_EVENT_CLICKED,NULL);advance(120);}
static void drag(lv_obj_t *o,unsigned target){lv_area_t a=area(o);int max=lv_slider_get_max_value(o),w=lv_obj_get_width(o),start=a.x1+w*lv_slider_get_value(o)/max,end=a.x1+w*(int)target/max,y=(a.y1+a.y2)/2;point=(lv_point_t){start,y};held=true;advance(80);for(int i=1;i<=16;++i){point.x=start+(end-start)*i/16;advance(25);}held=false;advance(120);}
static void fixture(unsigned w,unsigned h,bool game){for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){unsigned r=3+x*12/w,g=12+y*24/h,b=12+x*12/w;if(game){r=3;g=y<h*3/4?24:42;b=y<h*3/4?25:5;if((x/16+y/16)%2==0&&y>h*3/4){r=6;g=46;}if(x>100&&x<116&&y>h*3/4-25&&y<h*3/4){r=28;g=8;b=5;}if(y>38&&y<52&&((x>30&&x<78)||(x>160&&x<210))){r=28;g=56;b=28;}}else{if(y>h/2+(x%70)/4){r=3;g=18;b=14;}if(y>h*4/5){r=11;g=26;b=13;}if(x>w*3/4-16&&x<w*3/4+16&&y>20&&y<50){r=30;g=49;b=13;}}pixels[y*w+x]=(uint16_t)((r<<11)|(g<<5)|b);}}
static void show_video(app_ui_t *ui,unsigned w,unsigned h){fixture(w,h,false);video_callback(pixels,w,h,1,video_context);advance(120);assert(lv_image_get_src(ui->app_obj[0]));}
static void dimensions(app_ui_t *ui,unsigned w,unsigned h,unsigned tw,unsigned th){lv_obj_t *o=ui->app_obj[0];assert(lv_obj_get_width(o)==(int)tw&&lv_obj_get_height(o)==(int)th);assert(lv_image_get_src_width(o)==(int)w&&lv_image_get_src_height(o)==(int)h);assert(lv_image_get_inner_align(o)==LV_IMAGE_ALIGN_CONTAIN);assert(abs(lv_image_get_transformed_width(o)-(int)tw)<=1&&abs(lv_image_get_transformed_height(o)-(int)th)<=1);assert(lv_image_get_scale_x(o)==lv_image_get_scale_y(o));}
static void test_video(app_ui_t *ui)
{
    app_open_video_path(ui,"/sdcard/Video/中文全屏视频.mp4");deliver(ui,ESP_OK);assert(video_starts&&started_fps==0&&ui->media_active);
    video=(aether_video_status_t){.state=AETHER_VIDEO_PLAYING,.width=320,.height=180,.duration_ms=90000,.position_ms=12000,.seek_supported=true,.actual_fps_x100=2400};
    show_video(ui,320,180);dimensions(ui,320,180,480,270);capture(ui,"video-controls");
    assert(!hidden(ui->app_obj[2]));
    /* A real moving stream redraws translucent controls even without input. */
    for(unsigned i=0;i<66;++i){video.position_ms+=50;video_callback(pixels,320,180,i+2,video_context);advance(50);}
    assert(hidden(ui->app_obj[2]));capture(ui,"video-clean");
    advance(120);measure_flush=true;flush_calls=0;flushed_pixels=0;flush_ymin=480;flush_ymax=-1;
    video_callback(pixels,320,180,100,video_context);advance(80);measure_flush=false;
    printf("MEASURE fitted frame flush=%llu pixels calls=%u y=%d..%d\n",(unsigned long long)flushed_pixels,flush_calls,flush_ymin,flush_ymax);
    assert(flush_calls&&flush_ymin>=103&&flush_ymax<=376&&flushed_pixels<=480U*274U);
    printf("PASS continuous moving video hides controls; fitted frame flush=%llu pixels y=%d..%d (full viewport=230400)\n",(unsigned long long)flushed_pixels,flush_ymin,flush_ymax);
    assert(framebuffer[80*480+240]==0&&framebuffer[240*480+2]!=0&&framebuffer[400*480+240]==0);
    lv_obj_add_event_cb(ui->app_obj[4],background_trace,LV_EVENT_ALL,NULL);
    tap(240,220);assert(background_presses==1&&background_clicks==1&&!hidden(ui->app_obj[2]));point=(lv_point_t){200,220};held=true;advance(60);point.x=280;advance(80);held=false;advance(100);assert(!hidden(ui->app_obj[2]));
    click(ui,"暂停");assert(video.state==AETHER_VIDEO_PAUSED);advance(3300);assert(!hidden(ui->app_obj[2]));click(ui,"继续");assert(video.state==AETHER_VIDEO_PLAYING);
    unsigned commits=volume_commits;drag(slider(ui->screen,100),70);assert(master.master_volume>=68&&master.master_volume<=72&&volume_updates>2&&volume_commits==commits+1&&!hidden(ui->app_obj[2]));
    click(ui,"静音");assert(master.master_volume==0);click(ui,"恢复");assert(master.master_volume==70);
    video.state=AETHER_VIDEO_PAUSED;advance(120);lv_obj_t *bar=slider(ui->screen,1000);assert(bar&&!lv_obj_has_state(bar,LV_STATE_DISABLED));
    seek_busy=3;immediate_preview=true;unsigned attempts=seek_attempts,resets=seek_resets;drag(bar,750);advance(600);
    assert(seek_attempts==attempts+4&&seek_resets==resets+1&&seek_target>65000&&seek_target<70000&&video.state==AETHER_VIDEO_PAUSED);
    assert(lv_image_get_src(ui->app_obj[0])&&((const lv_image_dsc_t*)lv_image_get_src(ui->app_obj[0]))->data[0]==0xE0);capture(ui,"video-paused-seek");immediate_preview=false;
    video.state=AETHER_VIDEO_FINISHED;advance(120);assert(lv_obj_has_state(bar,LV_STATE_DISABLED));click(ui,"重播");assert(video.state==AETHER_VIDEO_STARTING);
    video.state=AETHER_VIDEO_PLAYING;video.elapsed_ms=1000;show_video(ui,320,180);assert(lv_obj_has_state(slider(ui->screen,1000),LV_STATE_DISABLED));
    send_click(ui,"停止");assert(!ui->media_active&&!ui->media_started);send_click(ui,"返回");assert(!ui->timer);
    puts("PASS fullscreen contain/black bars; real taps/swipe rejection, 3s hide, paused controls, volume/mute and no click-through");
    puts("PASS progress drag retains target across busy; reset preserves immediate paused preview; finished/unknown duration cannot seek");
}
static void test_seek_feedback(app_ui_t *ui)
{
    app_open_video_path(ui,"/sdcard/Video/seek-feedback.mp4");deliver(ui,ESP_OK);
    video=(aether_video_status_t){.state=AETHER_VIDEO_PLAYING,.width=320,.height=180,.duration_ms=90000,.position_ms=12000,.seek_supported=true};show_video(ui,320,180);
    lv_obj_t *bar=slider(ui->screen,1000);lv_area_t a=area(bar);
    const int actual=lv_slider_get_value(bar);
    point=(lv_point_t){a.x1+lv_obj_get_width(bar)*actual/1000,(a.y1+a.y2)/2};
    held=true;advance(80);
    point.x=a.x1+lv_obj_get_width(bar)*3/4;advance(80);
    assert(lv_slider_get_value(bar)>actual+200&&lv_obj_has_state(bar,LV_STATE_PRESSED));
    unsigned canceled_attempts=seek_attempts;
    held=false;lv_indev_reset(input,NULL);advance(120);
    assert(seek_attempts==canceled_attempts&&!lv_obj_has_state(bar,LV_STATE_PRESSED));
    assert(lv_slider_get_value(bar)==actual);
    puts("PASS input reset cancels a real video seek drag without submitting it");
    point=(lv_point_t){a.x1+lv_obj_get_width(bar)*lv_slider_get_value(bar)/1000,(a.y1+a.y2)/2};held=true;advance(80);
    unsigned held_attempts=seek_attempts;
    for(unsigned i=0;i<70;++i){video.position_ms+=50;video_callback(pixels,320,180,i+2,video_context);advance(50);}
    assert(!hidden(ui->app_obj[2])&&seek_attempts==held_attempts);held=false;advance(120);assert(seek_attempts==held_attempts+1);
    video.seeking=false;video.position_ms=seek_target;show_video(ui,320,180);
    unsigned attempts=seek_attempts,resets=seek_resets;
    seek_error=ESP_ERR_NOT_SUPPORTED;drag(bar,600);advance(150);
    assert(seek_attempts==attempts+1&&seek_resets==resets&&ui->media_active&&ui->media_started);
    assert(strstr(lv_label_get_text(ui->app_obj[1]),"提交跳转失败")&&strstr(lv_label_get_text(ui->app_obj[1]),"0x106"));
    advance(500);assert(seek_attempts==attempts+1);seek_error=ESP_OK;
    drag(bar,700);assert(seek_attempts==attempts+2&&seek_resets==resets+1&&video.seeking);
    advance(3300);assert(!hidden(ui->app_obj[2]));
    video.state=AETHER_VIDEO_ERROR;video.last_error=ESP_FAIL;strlcpy(video.detail,"MP4 seek reset failed: 0x108; backend failure remains visible",sizeof(video.detail));advance(120);
    assert(!ui->media_started&&!ui->media_active&&strstr(lv_label_get_text(ui->app_obj[1]),"播放错误")&&strstr(lv_label_get_text(ui->app_obj[1]),"backend failure remains visible"));
    assert(lv_label_get_long_mode(ui->app_obj[1])==LV_LABEL_LONG_WRAP&&!hidden(ui->app_obj[2]));
    click(ui,"重试");assert(video.state==AETHER_VIDEO_STARTING&&ui->media_started);send_click(ui,"返回");
    puts("PASS seek submission rejection is distinct from full backend failure; neither is hidden and retry retains real seeking");
}
static void test_errors(app_ui_t *ui)
{
    app_open_video_path(ui,"/sdcard/Video/test.avi");deliver(ui,ESP_FAIL);advance(120);assert(strstr(lv_label_get_text(ui->app_obj[1]),"视频准备失败"));
    assert(lv_label_get_long_mode(ui->app_obj[1])==LV_LABEL_LONG_WRAP&&lv_obj_get_self_height(ui->app_obj[1])<=260);capture(ui,"video-prepare-error");
    click(ui,"重试");assert(ui->io_complete&&request.operation==APP_IO_PREPARE&&!strcmp(request.path,"/sdcard/Video/test.avi"));play_error=ESP_FAIL;deliver(ui,ESP_OK);advance(500);assert(strstr(lv_label_get_text(ui->app_obj[1]),"启动失败"));play_error=ESP_OK;
    click(ui,"重试");assert(ui->media_started&&!hidden(ui->app_obj[0]));video=(aether_video_status_t){.state=AETHER_VIDEO_PLAYING,.width=320,.height=240,.elapsed_ms=65000};show_video(ui,320,240);
    assert(lv_obj_has_state(slider(ui->screen,1000),LV_STATE_DISABLED)&&find(ui->screen,"01:05 / --:--"));
    const uint8_t rates[]={24,30,10,15,0};for(unsigned i=0;i<5;++i){lv_obj_send_event(lv_obj_get_parent(ui->app_obj[3]),LV_EVENT_CLICKED,NULL);advance(120);assert(set_fps==rates[i]&&ui->app_value==rates[i]);}
    rate_error=ESP_FAIL;lv_obj_send_event(lv_obj_get_parent(ui->app_obj[3]),LV_EVENT_CLICKED,NULL);assert(ui->app_value==0);rate_error=ESP_OK;
    video.state=AETHER_VIDEO_ERROR;video.last_error=ESP_ERR_NOT_SUPPORTED;strlcpy(video.detail,"Unsupported JPEG dimensions 480x270: both dimensions must be multiples of 16. Full reason ends here.",sizeof(video.detail));advance(120);
    assert(lv_obj_has_flag(ui->app_obj[0],LV_OBJ_FLAG_HIDDEN)&&strstr(lv_label_get_text(ui->app_obj[1]),"Full reason ends here."));assert(lv_obj_get_self_height(ui->app_obj[1])<=260);capture(ui,"video-error");advance(3300);assert(!hidden(ui->app_obj[2]));
    unsigned stops=video_stops,releases=video_releases;click(ui,"返回");assert(video_stops==stops+1&&video_releases==releases+1&&!ui->media_active);
    assert(find(ui->screen,"应用抽屉")&&!ui->page_back);
    app_open_video_path(ui,"/sdcard/Video/test.mp4");deliver(ui,ESP_OK);video.state=AETHER_VIDEO_ERROR;video.last_error=ESP_ERR_NOT_SUPPORTED;strlcpy(video.detail,"Unsupported H.264 Main profile. Use constrained-baseline avc1 video, progressive I420, no B frames.",sizeof(video.detail));advance(120);
    assert(strstr(lv_label_get_text(ui->app_obj[1]),"no B frames.")&&strstr(lv_label_get_text(ui->app_obj[1]),"受限基线"));capture(ui,"video-mp4-error");app_open_video(ui);deliver(ui,ESP_OK);capture(ui,"video-empty");
    puts("PASS prepare barrier retries, persistent complete errors, restored fullscreen, MJPEG native/fixed FPS without fake seek/duration");
}
static void test_nes(app_ui_t *ui)
{
    app_open_nes_path(ui,"/sdcard/ROMs/全屏测试.nes");deliver(ui,ESP_OK);assert(ui->media_active&&!touch_enabled&&user_exit_arms&&nes_viewport_enabled);fixture(384,336,true);nes_callback(pixels,384,336,1,nes_context);nes.emulation_fps_x100=5980;nes.render_fps_x100=3000;advance(220);
    lv_obj_t *image=ui->app_obj[0];lv_area_t viewport=area(image);
    assert(viewport.x1==48&&viewport.y1==96&&viewport.x2==431&&viewport.y2==431);
    assert(lv_image_get_src_width(image)==384&&lv_image_get_src_height(image)==336);
    assert(lv_image_get_inner_align(image)==LV_IMAGE_ALIGN_CENTER&&!lv_image_get_antialias(image));
    capture(ui,"nes-playing");
    const char *buttons[]={"A","B","SEL","STA","暂停","重启"};
    for(unsigned i=0;i<sizeof(buttons)/sizeof(*buttons);++i)assert(!find(ui->screen,buttons[i]));
    assert(hidden(ui->app_obj[1])&&hidden(ui->app_obj[2])&&lv_obj_get_child_count(ui->screen)==3);
    nes.external_buttons=AETHER_NES_INPUT_A;advance(350);assert(lv_display_get_inactive_time(NULL)<220);
    sleeping=true;advance(700);assert(lv_display_get_inactive_time(NULL)>500);
    sleeping=false;nes.external_buttons=0;
    nes.state=AETHER_NES_PAUSED;advance(220);assert(hidden(ui->app_obj[1])&&hidden(ui->app_obj[2]));capture(ui,"nes-paused");nes.state=AETHER_NES_RUNNING;advance(220);
    nes.state=AETHER_NES_ERROR;nes.last_error=ESP_ERR_NOT_SUPPORTED;strlcpy(nes.detail,"Unsupported mapper test error",sizeof(nes.detail));advance(220);assert(!ui->media_active&&!nes_viewport_enabled&&ui->touch_restore_pending&&strstr(lv_label_get_text(ui->app_obj[1]),"Unsupported mapper"));capture(ui,"nes-error");
    unsigned stops=nes_stops,releases=nes_releases;send_click(ui,"返回");assert(nes_stops==stops+1&&nes_releases==releases+1);touch_enabled=true;
    nes_error=ESP_FAIL;app_open_nes_path(ui,"/sdcard/ROMs/test.nes");deliver(ui,ESP_OK);assert(!ui->media_active&&!ui->media_started);nes_error=ESP_OK;
    app_open_nes_path(ui,"/sdcard/ROMs/test.nes");deliver(ui,ESP_OK);nes.state=AETHER_NES_STOPPED;advance(220);assert(!ui->media_active&&!ui->timer);
    puts("PASS NES TE-budgeted 384x336 nearest viewport, equal axes, no overlays, pause/error/stop and touch handoff");
}
static void test_storage_ready(app_ui_t *ui)
{
    void(*openers[])(app_ui_t*)={app_open_video,app_open_nes};
    for(unsigned i=0;i<2;++i){
        storage_mounted=false;usb_blocked=true;openers[i](ui);assert(!ui->io_complete);
        unsigned before=catalog_calls;storage_mounted=true;advance(550);
        assert(catalog_calls==before&&!ui->io_complete);
        usb_blocked=false;advance(550);assert(catalog_calls==before+1&&ui->io_complete);
        deliver(ui,ESP_OK);advance(550);assert(catalog_calls==before+1&&!ui->io_complete);
    }
    puts("PASS video/NES catalogs wait for remount plus USB-gate release, then auto-refresh exactly once");
}
static void test_catalog_end(app_ui_t *ui)
{
    void (*openers[])(app_ui_t*)={app_open_video,app_open_nes};
    catalog_fixture_count=96;
    for (unsigned i=0;i<2;++i) {
        const char *first=i?"item-000.nes":"item-000.mp4";
        const char *last=i?"item-095.nes":"item-095.mp4";
        const char *picker_title=i?"选择 NES 游戏":"选择视频";
        const app_io_operation_t filter=i?APP_IO_NES:APP_IO_VIDEO;
        openers[i](ui);
        assert(ui->io_complete&&request.offset==0&&request.limit==4096);
        deliver(ui,ESP_OK);
        advance(120);
        lv_obj_t *label=find(ui->screen,first);
        assert(label&&lv_obj_is_visible(label));
        lv_obj_t *list=lv_obj_get_parent(lv_obj_get_parent(label));
        assert(lv_obj_get_child_count(list)<13&&!find(ui->screen,"下一页"));
        capture(ui, i ? "nes-catalog" : "video-catalog");
        const unsigned before=catalog_calls;
        lv_obj_scroll_to_y(list,INT32_MAX,LV_ANIM_OFF);
        advance(120);
        click(ui,last);
        assert(ui->io_complete&&catalog_calls==before&&request.operation==APP_IO_PREPARE&&
               request.filter==filter&&strstr(request.path,last)&&ui->page_back==openers[i]);

        deliver(ui,ESP_OK);
        assert(ui->media_active && ui->media_started);
        if (i) {
            const unsigned stops=nes_stops;
            assert(!touch_enabled);
            nes.state=AETHER_NES_STOPPED;
            advance(220);
            assert(nes_stops==stops+1 && ui->touch_restore_pending);
            touch_enabled=true;
            ui->touch_restore_pending=false;
        } else {
            const unsigned stops=video_stops;
            advance(120);
            click(ui,"返回");
            assert(video_stops==stops+1);
        }
        assert(!ui->media_active && !ui->page_back && find(ui->screen,picker_title));
        assert(ui->io_complete&&catalog_calls==before+1&&request.operation==APP_IO_CATALOG&&request.filter==filter);
        deliver(ui,ESP_OK);
        advance(120);
        click(ui,LV_SYMBOL_LEFT);
        assert(!ui->io_complete && find(ui->screen,"应用抽屉"));
    }
    catalog_fixture_count=0;
    puts("PASS Video/NES catalogs scroll to the correct offscreen source; video Back and NES stop return to their picker; picker Back opens drawer");
}
int main(int argc,char **argv)
{
    assert(argc==2);output=argv[1];setbuf(stdout,NULL);lv_init();lv_display_t *d=lv_display_create(480,480);assert(d);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_display(input,d);lv_indev_set_read_cb(input,read_input);lv_timer_set_period(lv_indev_get_read_timer(input),8);
    app_ui_t ui={.storage=(void*)1,.io=(void*)2,.video_player=(void*)1,.nes=(void*)1,.touch_indev=input};assert(app_frames_new(&ui.frames)==ESP_OK);active_ui=&ui;
    test_video(&ui);test_seek_feedback(&ui);test_errors(&ui);test_storage_ready(&ui);test_catalog_end(&ui);test_nes(&ui);
    for(unsigned i=0;i<100;++i){app_open_video_path(&ui,"/sdcard/Video/test.avi");deliver(&ui,ESP_OK);app_open_nes_path(&ui,"/sdcard/ROMs/test.nes");deliver(&ui,ESP_OK);app_ui_show_home(&ui);assert(!ui.media_active&&!ui.timer&&!ui.page_context&&lv_mem_test()==LV_RESULT_OK);}
    active_ui=NULL;lv_obj_delete(ui.screen);app_frames_delete(ui.frames);lv_indev_delete(input);lv_display_delete(d);assert(lv_mem_test()==LV_RESULT_OK);lv_deinit();puts("PASS 100 fullscreen video/NES/home rebuilds with actual app_frames and LVGL allocator integrity; synthetic clips are not board performance");return 0;
}
