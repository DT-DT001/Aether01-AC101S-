#include "app_gps.h"
#include <app_ui_internal.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern const lv_font_t font_16, font_24;
void app_open_gps(app_ui_t *ui);
static uint16_t frame[480 * 480], buffer[480 * 20];
static app_gps_status_t status;
static unsigned starts, stops, cancels;
static esp_err_t command_error;
static lv_point_t pointer;
static bool held;

void app_gps_get_status(app_gps_status_t *out) { *out = status; }
esp_err_t app_gps_set_enabled(bool enabled)
{
    if (enabled) ++starts; else ++stops;
    if (command_error != ESP_OK) return command_error;
    status.requested_enabled = enabled;
    status.state = enabled ? APP_GPS_POWERING : APP_GPS_STOPPING;
    return ESP_OK;
}
void app_gps_cancel(void) { ++cancels; status = (app_gps_status_t){.available=status.available}; }
const char *esp_err_to_name(esp_err_t error) { return error ? "ESP_FAIL" : "ESP_OK"; }
const lv_font_t *app_ui_chinese_font_size(unsigned px)
{
    static lv_font_t small, large;
    small = font_16; large = font_24;
    small.fallback = &lv_font_montserrat_16; large.fallback = &lv_font_montserrat_24;
    return px >= 24 ? &large : &small;
}
const lv_font_t *app_ui_chinese_font(void) { return app_ui_chinese_font_size(16); }
void app_ui_label_text(lv_obj_t *label, const char *value)
{ if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value); }
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char value[512]; va_list args; va_start(args, format);
    vsnprintf(value, sizeof(value), format, args); va_end(args); app_ui_label_text(label, value);
}
lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *value, lv_align_t align,
    int32_t x, int32_t y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent); lv_label_set_text(label, value);
    lv_obj_set_style_text_font(label, font, 0); lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, x, y); return label;
}
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    if (ui->page_cleanup) { void (*cleanup)(app_ui_t *) = ui->page_cleanup; ui->page_cleanup = NULL; cleanup(ui); }
    ui->page_suspend = NULL;
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = NULL; }
    ui->screen = lv_obj_create(NULL); lv_obj_remove_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x070A12), 0);
    lv_obj_set_style_text_font(ui->screen, app_ui_chinese_font(), 0);
    app_ui_add_text(ui->screen, title, LV_ALIGN_TOP_MID, 0, 20, 0xEAF2FF, app_ui_chinese_font_size(24));
    ui->content = lv_obj_create(ui->screen); lv_obj_set_size(ui->content, 448, 394);
    lv_obj_align(ui->content, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(ui->content, 24, 0); lv_obj_set_style_border_width(ui->content, 0, 0);
    lv_obj_set_style_bg_color(ui->content, lv_color_hex(0x10192A), 0);
    lv_screen_load_anim(ui->screen, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true); return ui->content;
}
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback, uint32_t period)
{ ui->timer = lv_timer_create(callback, period, ui); }
static void advance(unsigned milliseconds)
{ for (unsigned i=0; i<milliseconds; ++i) { lv_tick_inc(1); lv_timer_handler(); } }
static void input(lv_indev_t *indev, lv_indev_data_t *data)
{ (void)indev; data->point=pointer; data->state=held?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source=(const uint16_t *)pixels;
    for (int y=area->y1; y<=area->y2; ++y) for (int x=area->x1; x<=area->x2; ++x) {
        assert(x>=0 && x<480 && y>=0 && y<480); frame[y*480+x]=*source++;
    }
    lv_display_flush_ready(display);
}
static lv_obj_t *find(lv_obj_t *parent, const char *value)
{
    if (lv_obj_check_type(parent,&lv_label_class) && !strcmp(lv_label_get_text(parent),value)) return parent;
    for (uint32_t i=0; i<lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *result=find(lv_obj_get_child(parent,(int32_t)i),value); if (result) return result;
    }
    return NULL;
}
static bool contains(lv_obj_t *parent, const char *value)
{
    if (lv_obj_check_type(parent,&lv_label_class) && strstr(lv_label_get_text(parent),value)) return true;
    for (uint32_t i=0; i<lv_obj_get_child_count(parent); ++i)
        if (contains(lv_obj_get_child(parent,(int32_t)i),value)) return true;
    return false;
}
static void tap(lv_obj_t *object)
{
    lv_obj_update_layout(lv_obj_get_screen(object)); lv_area_t area; lv_obj_get_coords(object,&area);
    pointer=(lv_point_t){.x=(area.x1+area.x2)/2,.y=(area.y1+area.y2)/2};
    held=true; advance(90); held=false; advance(90);
}
static void capture(lv_display_t *display, const char *directory, const char *name)
{
    lv_refr_now(display); char path[512]; snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *file=fopen(path,"wb"); assert(file); fprintf(file,"P6\n480 480\n255\n");
    for (unsigned i=0; i<480*480; ++i) { uint16_t p=frame[i]; uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31}; assert(fwrite(rgb,1,3,file)==3); }
    assert(!fclose(file));
}
int main(int argc, char **argv)
{
    assert(argc==2); setbuf(stdout,NULL); lv_init();
    lv_display_t *display=lv_display_create(480,480); lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_indev_t *indev=lv_indev_create(); lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev,display); lv_indev_set_read_cb(indev,input);
    app_ui_t ui={0}; app_open_gps(&ui); advance(600);
    assert(find(ui.screen,"定位服务不可用"));
    lv_obj_t *button=lv_obj_get_parent(find(ui.screen,"开启定位"));
    assert(lv_obj_has_state(button,LV_STATE_DISABLED)); tap(button); assert(starts==0);
    lv_obj_t *power_details = lv_obj_get_parent(find(ui.screen, "可见卫星"));
    assert(contains(power_details, "电池自动 5V OTG"));
    lv_area_t warning; lv_obj_get_coords(power_details,&warning);
    assert(warning.y1>=72 && warning.y2<466);
    status.available=true; advance(600); assert(find(ui.screen,"未开启"));
    command_error=ESP_FAIL; tap(button); assert(starts==1 && find(ui.screen,"操作未受理"));
    command_error=ESP_OK; tap(button); assert(starts==2 && find(ui.screen,"正在确认定位电源"));
    assert(find(ui.screen,"停止定位")); tap(button); assert(stops==1 && find(ui.screen,"正在停止定位"));
    assert(lv_obj_has_state(button,LV_STATE_DISABLED));
    status.state=APP_GPS_OFF; status.requested_enabled=false; advance(600); tap(button);
    status.state=APP_GPS_SEARCHING; status.active=true; advance(600);
    assert(find(ui.screen,"等待模块数据") && find(ui.screen,"等待卫星数据"));
    lv_obj_t *no_signals=find(ui.screen,"尚无可用的卫星信号数据");
    assert(no_signals && !lv_obj_has_flag(no_signals,LV_OBJ_FLAG_HIDDEN));
    lv_obj_t *signals=lv_obj_get_parent(no_signals);
    for (unsigned i=1; i<lv_obj_get_child_count(signals); ++i)
        assert(lv_obj_has_flag(lv_obj_get_child(signals,(int32_t)i),LV_OBJ_FLAG_HIDDEN));
    status.uart_receiving=true; advance(600); assert(find(ui.screen,"正在搜索卫星"));
    status.state=APP_GPS_FIXED; status.supply_valid=true; status.supply_mv=5001;
    status.pps_seen=true; status.pps_count=12; status.session_ms=12345;
    status.navigation=(gnss_nmea_snapshot_t){.position_valid=true,.time_valid=true,.date_valid=true,
        .altitude_valid=true,.speed_valid=true,.hdop_valid=true,.latitude_deg=31.2304,.longitude_deg=121.4737,
        .altitude_m=12.3f,.speed_kmh=4.2f,.hdop=0.9f,.fix_dimension=3,.fix_quality=1,
        .satellites_used=5,.satellite_count=8,.year=2026,.month=11,.day=21,.hour=8,.minute=9,.second=10,
        .accepted_sentences=120,
        .satellites={{.system=GNSS_SYSTEM_GPS,.prn=1,.azimuth_deg=0,.elevation_deg=0,.snr_dbhz=38,.signal_valid=true,.geometry_valid=true},
        {.system=GNSS_SYSTEM_BEIDOU,.prn=2,.azimuth_deg=90,.elevation_deg=45,.snr_dbhz=42,.signal_valid=true,.geometry_valid=true,.used=true},
        {.system=GNSS_SYSTEM_GPS,.prn=3,.azimuth_deg=180,.elevation_deg=50,.snr_dbhz=40,.signal_valid=true,.geometry_valid=true,.used=true},
        {.system=GNSS_SYSTEM_GPS,.prn=6,.azimuth_deg=280,.elevation_deg=25,.snr_dbhz=35,.signal_valid=true,.geometry_valid=true,.used=true},
        {.system=GNSS_SYSTEM_BEIDOU,.prn=8,.azimuth_deg=45,.elevation_deg=60,.snr_dbhz=44,.signal_valid=true,.geometry_valid=true,.used=true},
        {.system=GNSS_SYSTEM_GLONASS,.prn=12,.azimuth_deg=220,.elevation_deg=42,.snr_dbhz=31,.signal_valid=true,.geometry_valid=true,.used=true},
        {.system=GNSS_SYSTEM_GALILEO,.prn=7,.azimuth_deg=310,.elevation_deg=67,.snr_dbhz=28,.signal_valid=true,.geometry_valid=true},
        {.system=GNSS_SYSTEM_GALILEO,.prn=3,.snr_dbhz=0}}};
    advance(600); assert(find(ui.screen,"卫星定位成功"));
    assert(contains(ui.screen,"OTG 输出") && !contains(ui.screen,"USB 输入供电"));
    assert(contains(ui.screen,"N 31.230400") && contains(ui.screen,"E 121.473700"));
    assert(contains(ui.screen,"2026-11-21 08:09:10 UTC"));
    lv_obj_t *north=lv_obj_get_parent(find(ui.screen,"G1"));
    assert(lv_obj_get_x(north)==89 && lv_obj_get_y(north)==21);
    lv_obj_t *east=lv_obj_get_parent(find(ui.screen,"B2"));
    assert(lv_obj_get_x(east)==123 && lv_obj_get_y(east)==89);
    lv_obj_update_layout(ui.screen);
    lv_area_t north_label, north_satellite;
    lv_obj_get_coords(find(ui.screen,"N"),&north_label); lv_obj_get_coords(north,&north_satellite);
    assert(north_label.y2<north_satellite.y1);
    lv_obj_t *untracked_column=lv_obj_get_parent(find(ui.screen,"E3"));
    assert(lv_obj_get_width(untracked_column)==32);
    assert(lv_obj_get_style_bg_opa(lv_obj_get_child(untracked_column,0),LV_PART_MAIN)==LV_OPA_TRANSP);
    assert(lv_obj_has_flag(find(ui.screen,"等待卫星数据"),LV_OBJ_FLAG_HIDDEN));
    lv_obj_update_layout(ui.screen); capture(display,argv[1],"gps-fixed");
    status.external_power=true; advance(600);
    assert(contains(ui.screen,"USB 输入") && contains(ui.screen,"不启用 OTG"));
    assert(!contains(ui.screen,"断开电脑") && !contains(ui.screen,"断开充电器"));
    capture(display,argv[1],"gps-external");
    lv_obj_scroll_to_y(ui.content,1000,LV_ANIM_OFF); lv_obj_update_layout(ui.screen);
    capture(display,argv[1],"gps-signals");
    status.navigation.position_valid=false; status.navigation.date_valid=false;
    status.navigation.time_valid=false; status.navigation.satellite_count=0;
    advance(600); assert(find(ui.screen,"定位已丢失"));
    assert(!contains(ui.screen,"31.230400") && !contains(ui.screen,"121.473700"));
    assert(find(lv_obj_get_parent(find(ui.screen, "纬度")), "--") && contains(ui.screen,"UTC --"));
    assert(!lv_obj_has_flag(find(ui.screen,"等待卫星数据"),LV_OBJ_FLAG_HIDDEN));
    status.state=APP_GPS_ERROR; status.last_error=ESP_FAIL; status.active=false; status.requested_enabled=false;
    advance(600); assert(find(ui.screen,"定位启动失败"));
    puts("PASS unavailable, real touch start/stop, power failure, external USB/no-OTG text, fix/UTC data, satellite geometry and stale fix");
    status=(app_gps_status_t){.available=true,.external_power=true};
    lv_obj_scroll_to_y(ui.content,0,LV_ANIM_OFF); advance(600);
    const unsigned before_external=starts; tap(button);
    assert(starts==before_external+1 && status.external_power && find(ui.screen,"正在确认定位电源"));
    status.state=APP_GPS_SEARCHING; status.active=true; status.supply_valid=true; status.supply_mv=5010;
    advance(600); assert(contains(ui.screen,"USB 输入") && !contains(ui.screen,"断开电脑"));
    tap(button); assert(status.state==APP_GPS_STOPPING);
    puts("PASS external-input startup and cancellation controls");
    assert(ui.page_suspend);
    const unsigned before_suspend = starts;
    status = (app_gps_status_t){.available = true, .requested_enabled = true,
                               .active = true, .state = APP_GPS_SEARCHING};
    ui.page_suspend(&ui, true);
    assert(!status.requested_enabled);
    status.state = APP_GPS_STOPPING;
    ui.page_suspend(&ui, false);
    assert(starts == before_suspend);
    status.state = APP_GPS_OFF;
    advance(600);
    assert(starts == before_suspend + 1 && status.requested_enabled);
    status.requested_enabled = false;
    ui.page_suspend(&ui, true);
    ui.page_suspend(&ui, false);
    advance(600);
    assert(starts == before_suspend + 1);
    puts("PASS navigation suspend, wake during cleanup and no restart when manually off");
    const unsigned before_replacements = cancels;
    for (unsigned i=0; i<100; ++i) { app_open_gps(&ui); advance(5); }
    assert(cancels == before_replacements + 100);
    ui.page_cleanup(&ui); assert(cancels == before_replacements + 101 && !ui.page_context);
    lv_timer_delete(ui.timer); lv_obj_delete(ui.screen); lv_indev_delete(indev); lv_display_delete(display); lv_deinit();
    puts("PASS 100 page replacements and asynchronous cancel cleanup");
}
