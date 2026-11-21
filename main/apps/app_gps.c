#include "app_gps.h"
#include <app_ui_internal.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPS_SKY_SIZE 200
#define GPS_SKY_RADIUS 68
#define GPS_SIGNAL_BARS 16

typedef struct {
    lv_obj_t *state, *button, *button_text, *metrics, *position, *details, *help;
    lv_obj_t *sky_empty, *signal_empty;
    lv_obj_t *satellite[GNSS_MAX_SATELLITES], *satellite_text[GNSS_MAX_SATELLITES];
    lv_obj_t *bar[GPS_SIGNAL_BARS], *bar_text[GPS_SIGNAL_BARS], *bar_value[GPS_SIGNAL_BARS];
    gnss_satellite_t last_satellites[GNSS_MAX_SATELLITES];
    unsigned last_satellite_count;
    bool satellites_published;
    bool had_fix;
    uint32_t state_color;
    esp_err_t submit_error;
} gps_page_t;

static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y,
                      int width, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = app_ui_add_text(parent, value, LV_ALIGN_TOP_LEFT, x, y, color, font);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *shape(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}

static char system_prefix(gnss_system_t system)
{
    switch (system) {
    case GNSS_SYSTEM_GPS: return 'G';
    case GNSS_SYSTEM_BEIDOU: return 'B';
    case GNSS_SYSTEM_GLONASS: return 'R';
    case GNSS_SYSTEM_GALILEO: return 'E';
    case GNSS_SYSTEM_QZSS: return 'Q';
    case GNSS_SYSTEM_SBAS: return 'S';
    default: return '?';
    }
}

static uint32_t satellite_color(const gnss_satellite_t *satellite)
{
    return satellite->used ? 0x69E4AD : satellite->signal_valid ? 0x75BFFF : 0x66758C;
}

static void hide(lv_obj_t *object, bool hidden)
{
    if (hidden == lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (hidden) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void update_gps(app_ui_t *ui)
{
    gps_page_t *page = ui->page_context;
    if (!page) return;
    app_gps_status_t status = {0};
    app_gps_get_status(&status);
    const gnss_nmea_snapshot_t *nav = &status.navigation;
    const bool running = status.active && status.state != APP_GPS_ERROR && status.state != APP_GPS_STOPPING;
    const bool fixed = running && nav->position_valid && isfinite(nav->latitude_deg) && isfinite(nav->longitude_deg);
    if (!running) page->had_fix = false;
    if (fixed) page->had_fix = true;
    const char *state = "未开启";
    switch (status.state) {
    case APP_GPS_POWERING: state = "正在确认定位电源"; break;
    case APP_GPS_SEARCHING:
        state = page->had_fix ? "定位已丢失" : status.uart_receiving ? "正在搜索卫星" : "等待模块数据"; break;
    case APP_GPS_FIXED: state = fixed ? "卫星定位成功" : "定位已丢失"; break;
    case APP_GPS_STOPPING: state = "正在停止定位"; break;
    case APP_GPS_ERROR: state = "定位启动失败"; break;
    default: break;
    }
    if (!status.available) state = "定位服务不可用";
    if (page->submit_error != ESP_OK) state = "操作未受理";
    app_ui_label_text(page->state, state);
    const uint32_t state_color = fixed ? 0x69E4AD : 0xE8F1FF;
    if (page->state_color != state_color) {
        lv_obj_set_style_text_color(page->state, lv_color_hex(state_color), 0);
        page->state_color = state_color;
    }
    const bool stop = status.requested_enabled || status.active || status.state == APP_GPS_POWERING;
    app_ui_label_text(page->button_text, stop ? "停止定位" : "开启定位");
    if (status.available && status.state != APP_GPS_STOPPING) lv_obj_remove_state(page->button, LV_STATE_DISABLED);
    else lv_obj_add_state(page->button, LV_STATE_DISABLED);

    char hdop[24], supply[24], pps[24];
    if (running && nav->hdop_valid && isfinite(nav->hdop)) snprintf(hdop, sizeof(hdop), "%.1f", (double)nav->hdop);
    else snprintf(hdop, sizeof(hdop), "--");
    if (status.supply_valid) snprintf(supply, sizeof(supply), "%u.%02u V", status.supply_mv / 1000, status.supply_mv % 1000 / 10);
    else snprintf(supply, sizeof(supply), "--");
    if (running && status.pps_seen && status.pps_age_ms < 2500) snprintf(pps, sizeof(pps), "%lu", (unsigned long)status.pps_count);
    else snprintf(pps, sizeof(pps), "--");
    app_ui_label_fmt(page->metrics,
        "可见卫星    %u\n参与定位    %u\n定位模式    %s\nHDOP        %s\nPPS 脉冲    %s\n供电        %s\n电源来源    %s",
        running ? nav->satellite_count : 0, fixed ? nav->satellites_used : 0,
        fixed ? nav->fix_dimension == 3 ? "3D" : nav->fix_dimension == 2 ? "2D" : "有效" : "--", hdop, pps, supply,
        status.external_power ? "USB 输入" : status.supply_valid ? "OTG 输出" : "--");

    char latitude[48], longitude[48], altitude[24], speed[24], utc[48];
    if (fixed) {
        snprintf(latitude, sizeof(latitude), "%c %.6f", nav->latitude_deg < 0 ? 'S' : 'N', fabs(nav->latitude_deg));
        snprintf(longitude, sizeof(longitude), "%c %.6f", nav->longitude_deg < 0 ? 'W' : 'E', fabs(nav->longitude_deg));
    } else { snprintf(latitude, sizeof(latitude), "--"); snprintf(longitude, sizeof(longitude), "--"); }
    if (fixed && nav->altitude_valid && isfinite(nav->altitude_m)) snprintf(altitude, sizeof(altitude), "%.1f m", (double)nav->altitude_m);
    else snprintf(altitude, sizeof(altitude), "--");
    if (fixed && nav->speed_valid && isfinite(nav->speed_kmh)) snprintf(speed, sizeof(speed), "%.1f km/h", (double)nav->speed_kmh);
    else snprintf(speed, sizeof(speed), "--");
    if (running && nav->date_valid && nav->time_valid)
        snprintf(utc, sizeof(utc), "%04u-%02u-%02u %02u:%02u:%02u UTC", nav->year, nav->month, nav->day, nav->hour, nav->minute, nav->second);
    else snprintf(utc, sizeof(utc), "UTC --");
    app_ui_label_fmt(page->position, "纬度  %s\n经度  %s\n海拔  %s     速度  %s\n%s", latitude, longitude, altitude, speed, utc);

    const unsigned count = running && nav->satellite_count <= GNSS_MAX_SATELLITES ? nav->satellite_count : 0;
    const bool satellites_changed = !page->satellites_published || page->last_satellite_count != count ||
        memcmp(page->last_satellites, nav->satellites, count * sizeof(nav->satellites[0])) != 0;
    if (satellites_changed) {
    unsigned plotted = 0;
    unsigned ranked[GPS_SIGNAL_BARS], rank_count = 0;
    for (unsigned i = 0; i < GNSS_MAX_SATELLITES; ++i) {
        const gnss_satellite_t *sat = i < count ? &nav->satellites[i] : NULL;
        const bool geometry = sat && sat->geometry_valid && sat->elevation_deg <= 90 && sat->azimuth_deg <= 360;
        hide(page->satellite[i], !geometry);
        if (geometry) {
            const double angle = sat->azimuth_deg * 0.017453292519943295;
            const double radius = GPS_SKY_RADIUS * (90.0 - sat->elevation_deg) / 90.0;
            lv_obj_set_pos(page->satellite[i], 89 + (int)lround(radius * sin(angle)), 89 - (int)lround(radius * cos(angle)));
            lv_obj_set_style_bg_color(page->satellite[i], lv_color_hex(satellite_color(sat)), 0);
            app_ui_label_fmt(page->satellite_text[i], "%c%u", system_prefix(sat->system), sat->prn);
            ++plotted;
        }
        if (!sat) continue;
        unsigned insertion = 0;
        const unsigned score = sat->signal_valid ? sat->snr_dbhz : 0;
        while (insertion < rank_count) {
            const gnss_satellite_t *ranked_sat = &nav->satellites[ranked[insertion]];
            if ((ranked_sat->signal_valid ? ranked_sat->snr_dbhz : 0) < score) break;
            ++insertion;
        }
        if (insertion < GPS_SIGNAL_BARS) {
            if (rank_count < GPS_SIGNAL_BARS) ++rank_count;
            for (unsigned j = rank_count - 1; j > insertion; --j) ranked[j] = ranked[j - 1];
            ranked[insertion] = i;
        }
    }
    hide(page->sky_empty, plotted != 0);
    hide(page->signal_empty, rank_count != 0);
    for (unsigned i = 0; i < GPS_SIGNAL_BARS; ++i) {
        hide(lv_obj_get_parent(page->bar[i]), i >= rank_count);
        if (i >= rank_count) continue;
        const gnss_satellite_t *sat = &nav->satellites[ranked[i]];
        const unsigned snr = sat->signal_valid ? sat->snr_dbhz : 0;
        unsigned height = snr > 60 ? 90 : snr * 90 / 60;
        lv_obj_set_height(page->bar[i], height ? (int)height : 1);
        lv_obj_set_y(page->bar[i], 106 - (int)height);
        lv_obj_set_style_bg_opa(page->bar[i], snr ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(page->bar[i], lv_color_hex(satellite_color(sat)), 0);
        app_ui_label_fmt(page->bar_text[i], "%c%u", system_prefix(sat->system), sat->prn);
        if (sat->signal_valid) app_ui_label_fmt(page->bar_value[i], "%u", sat->snr_dbhz);
        else app_ui_label_text(page->bar_value[i], "--");
    }
    memcpy(page->last_satellites, nav->satellites, count * sizeof(nav->satellites[0]));
    page->last_satellite_count = count;
    page->satellites_published = true;
    }
    app_ui_label_fmt(page->details, "NMEA %lu   校验失败 %lu   运行 %lu s\nG GPS / B 北斗 / R GLONASS / E Galileo",
        (unsigned long)nav->accepted_sentences, (unsigned long)nav->bad_checksums, (unsigned long)(status.session_ms / 1000));
    const esp_err_t error = page->submit_error != ESP_OK ? page->submit_error : status.last_error;
    if (error != ESP_OK) app_ui_label_fmt(page->help, "供电或串口未就绪：%s\n请检查输入电源 / OTG 状态后重试。", esp_err_to_name(error));
    else if (!stop) app_ui_label_text(page->help, "外部供电无需 OTG；电池模式自动输出 5V。\n退出本页停止定位；不会自动改写 RTC。");
    else if (!status.uart_receiving && status.state != APP_GPS_POWERING)
        app_ui_label_text(page->help, "等待 9600 波特率数据，请检查 TX/RX 交叉。\n移到开阔户外，陶瓷天线朝向天空。");
    else if (status.external_power) app_ui_label_text(page->help,
        "USB 输入供电，不启用 OTG；可保持电脑调试。\n请在开阔户外等待定位，首次搜星可能较慢。\nHDOP 为无量纲几何因子，不是米级精度。");
    else app_ui_label_text(page->help, "电池模式使用 OTG 输出，请避免供电冲突。\n请在开阔户外等待定位，首次搜星可能较慢。\nHDOP 为无量纲几何因子，不是米级精度。");
}

static void gps_timer(lv_timer_t *timer) { update_gps(lv_timer_get_user_data(timer)); }

static void gps_command(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    gps_page_t *page = ui->page_context;
    if (!page) return;
    app_gps_status_t status;
    app_gps_get_status(&status);
    page->submit_error = app_gps_set_enabled(!(status.requested_enabled || status.active || status.state == APP_GPS_POWERING));
    update_gps(ui);
}

static void gps_cleanup(app_ui_t *ui)
{
    app_gps_cancel();
    free(ui->page_context);
    ui->page_context = NULL;
}

void app_open_gps(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "卫星定位");
    gps_page_t *page = calloc(1, sizeof(*page));
    if (!page) { text(content, "定位界面内存不足", 0, 0, 380, 0xFFB7A5, app_ui_chinese_font()); return; }
    ui->page_context = page;
    ui->page_cleanup = gps_cleanup;
    lv_obj_set_style_pad_all(content, 12, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    page->state = text(content, "未开启", 4, 5, 270, 0xE8F1FF, app_ui_chinese_font_size(24));
    page->button = lv_button_create(content);
    lv_obj_set_pos(page->button, 286, 0); lv_obj_set_size(page->button, 132, 38);
    lv_obj_set_style_radius(page->button, 15, 0);
    lv_obj_set_style_bg_color(page->button, lv_color_hex(0x235C55), 0);
    page->button_text = app_ui_add_text(page->button, "开启定位", LV_ALIGN_CENTER, 0, 0, 0xC6FFE4, app_ui_chinese_font());
    lv_obj_add_event_cb(page->button, gps_command, LV_EVENT_CLICKED, ui);

    text(content, "外部供电直接使用；电池供电自动开启 5V OTG", 4, 44, 418, 0x9CCEB9, app_ui_chinese_font());
    lv_obj_t *sky = shape(content, 0, 70, GPS_SKY_SIZE, GPS_SKY_SIZE, 0x0A1424);
    lv_obj_set_style_radius(sky, 20, 0);
    for (unsigned ring_index = 1; ring_index <= 3; ++ring_index) {
        const unsigned r = GPS_SKY_RADIUS * ring_index / 3;
        lv_obj_t *ring = shape(sky, 100 - (int)r, 100 - (int)r, (int)r * 2, (int)r * 2, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(0x263E58), 0);
    }
    shape(sky, 100 - GPS_SKY_RADIUS, 100, GPS_SKY_RADIUS * 2, 1, 0x263E58);
    shape(sky, 100, 100 - GPS_SKY_RADIUS, 1, GPS_SKY_RADIUS * 2, 0x263E58);
    text(sky, "N", 94, 0, 16, 0x9BDDC4, &lv_font_montserrat_16);
    text(sky, "S", 95, 183, 16, 0x7186A2, &lv_font_montserrat_16);
    text(sky, "W", 0, 91, 20, 0x7186A2, &lv_font_montserrat_16);
    text(sky, "E", 184, 91, 16, 0x7186A2, &lv_font_montserrat_16);
    page->sky_empty = text(sky, "等待卫星数据", 20, 91, 160, 0x7186A2, app_ui_chinese_font());
    lv_obj_set_style_text_align(page->sky_empty, LV_TEXT_ALIGN_CENTER, 0);
    for (unsigned i = 0; i < GNSS_MAX_SATELLITES; ++i) {
        page->satellite[i] = shape(sky, 89, 89, 22, 22, 0x75BFFF);
        lv_obj_set_style_radius(page->satellite[i], LV_RADIUS_CIRCLE, 0);
        page->satellite_text[i] = app_ui_add_text(page->satellite[i], "", LV_ALIGN_CENTER, 0, 0, 0x071421, &lv_font_montserrat_14);
        hide(page->satellite[i], true);
    }
    page->metrics = text(content, "", 212, 74, 210, 0xCCDCEF, app_ui_chinese_font());
    lv_obj_set_style_text_line_space(page->metrics, 9, 0);
    text(content, "绿色：参与定位   蓝色：已跟踪", 4, 276, 414, 0x86A6BE, app_ui_chinese_font());
    page->position = text(content, "", 4, 304, 418, 0xE3EDF9, app_ui_chinese_font());
    lv_obj_set_style_text_line_space(page->position, 6, 0);
    text(content, "信号强度 C/N0 (dB-Hz)  ·  最强 16 颗", 4, 410, 418, 0xA9C3DF, app_ui_chinese_font());
    lv_obj_t *signals = shape(content, 0, 436, 424, 140, 0x0A1424);
    lv_obj_add_flag(signals, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(signals, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(signals, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_radius(signals, 12, 0);
    page->signal_empty = text(signals, "尚无可用的卫星信号数据", 20, 60, 380, 0x7186A2, app_ui_chinese_font());
    for (unsigned i = 0; i < GPS_SIGNAL_BARS; ++i) {
        lv_obj_t *column = shape(signals, 8 + (int)i * 34, 4, 32, 126, 0x0A1424);
        page->bar[i] = shape(column, 7, 105, 18, 1, 0x75BFFF);
        lv_obj_set_style_radius(page->bar[i], 3, 0);
        page->bar_value[i] = text(column, "--", 0, 0, 32, 0xD8E9F8, &lv_font_montserrat_14);
        page->bar_text[i] = text(column, "", 0, 110, 32, 0x98AEC8, &lv_font_montserrat_14);
        lv_obj_set_style_text_align(page->bar_value[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(page->bar_text[i], LV_TEXT_ALIGN_CENTER, 0);
        hide(column, true);
    }
    page->details = text(content, "", 4, 588, 418, 0x8198B5, app_ui_chinese_font());
    page->help = text(content, "", 4, 636, 418, 0xA4BCD6, app_ui_chinese_font());
    app_ui_set_timer(ui, gps_timer, 500);
    update_gps(ui);
}
