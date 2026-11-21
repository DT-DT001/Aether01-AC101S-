#include "app_ui_internal.h"
#include "app_telemetry.h"
#include "app_tools.h"
#include "app_tools_math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float zero_pressure;
    float pressure_min;
    float pressure_max;
    char message[160];
} sensor_page_t;

static void sensor_cleanup(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
}

static sensor_page_t *sensor_context(app_ui_t *ui)
{
    sensor_page_t *page = calloc(1, sizeof(*page));
    if (!page) return NULL;
    page->pressure_min = INFINITY;
    page->pressure_max = -INFINITY;
    ui->page_context = page;
    ui->page_cleanup = sensor_cleanup;
    return page;
}

static lv_obj_t *sensor_text(lv_obj_t *parent, int y, uint32_t color, unsigned size)
{
    lv_obj_t *label = app_ui_add_text(parent, "", LV_ALIGN_TOP_MID, 0, y, color, app_ui_chinese_font_size(size));
    lv_obj_set_width(label, 402);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(label, 6, 0);
    return label;
}

static lv_obj_t *sensor_button(app_ui_t *ui, const char *text, int x, int y, int width, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(ui->content);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 44);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2768D8), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    return app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
}

static void reference_change(app_ui_t *ui, float delta)
{
    app_tools_snapshot_t tools;
    app_tools_get_snapshot(&tools);
    const esp_err_t err = app_tools_set_reference_pressure(tools.reference_pressure_pa + delta);
    sensor_page_t *page = ui->page_context;
    if (page) snprintf(page->message, sizeof(page->message), "%s%s",
                        err == ESP_OK ? "海平面气压已更新" : "设置失败：", err == ESP_OK ? "" : esp_err_to_name(err));
}

static void reference_less(lv_event_t *event) { reference_change(lv_event_get_user_data(event), -100); }
static void reference_more(lv_event_t *event) { reference_change(lv_event_get_user_data(event), 100); }

static void barometer_zero(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    sensor_page_t *page = ui->page_context;
    if (!page) return;
    app_telemetry_snapshot_t telemetry;
    app_telemetry_get(&telemetry);
    if (telemetry.barometer_error != ESP_OK || !isfinite(telemetry.barometer.pressure_pa) ||
        telemetry.barometer.pressure_pa <= 0) {
        strlcpy(page->message, "暂无有效气压，不能归零", sizeof(page->message));
        return;
    }
    page->zero_pressure = telemetry.barometer.pressure_pa;
    strlcpy(page->message, "相对高度已归零", sizeof(page->message));
}

static void update_barometer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    sensor_page_t *page = ui->page_context;
    if (!page) return;
    app_telemetry_snapshot_t telemetry;
    app_telemetry_get(&telemetry);
    app_tools_snapshot_t tools;
    app_tools_get_snapshot(&tools);
    app_ui_label_fmt(ui->app_obj[2], "海平面参考 %.2f hPa", tools.reference_pressure_pa / 100);
    if (telemetry.barometer_error != ESP_OK || !isfinite(telemetry.barometer.pressure_pa) ||
        !isfinite(telemetry.barometer.temperature_c) || telemetry.barometer.pressure_pa <= 0) {
        app_ui_label_fmt(ui->app_obj[0], "气压：%s", telemetry.barometer_error == ESP_OK ? "采样数据异常" : esp_err_to_name(telemetry.barometer_error));
        app_ui_label_text(ui->app_obj[1], "等待有效采样");
        return;
    }
    const dps368_sample_t *sample = &telemetry.barometer;
    if (sample->pressure_pa < page->pressure_min) page->pressure_min = sample->pressure_pa;
    if (sample->pressure_pa > page->pressure_max) page->pressure_max = sample->pressure_pa;
    const float altitude = app_pressure_altitude(sample->pressure_pa, tools.reference_pressure_pa);
    app_ui_label_fmt(ui->app_obj[0], "%.2f hPa   %.1f ℃", sample->pressure_pa / 100, sample->temperature_c);
    if (page->zero_pressure > 0) {
        app_ui_label_fmt(ui->app_obj[1], "估算海拔 %.1f m\n相对高度 %+.1f m", altitude,
                         app_pressure_altitude(sample->pressure_pa, page->zero_pressure));
    } else app_ui_label_fmt(ui->app_obj[1], "估算海拔 %.1f m\n点击下方按钮设置相对零点", altitude);
    app_ui_label_fmt(ui->app_obj[3], "本页最低 %.2f / 最高 %.2f hPa\n%s\n%s",
                     page->pressure_min / 100, page->pressure_max / 100, page->message,
                     !tools.ready ? "参考参数正在加载" : tools.preferences_deferred ? "待媒体暂停后保存" :
                     tools.preferences_pending ? "后台保存中" :
                     tools.preferences_error != ESP_OK ? esp_err_to_name(tools.preferences_error) :
                     "海拔随天气变化，请按当地海平面气压校正");
}

void app_open_barometer(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "气压与高度");
    if (!sensor_context(ui)) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8C8C, app_ui_chinese_font());
        return;
    }
    app_telemetry_set_interest(APP_TELEMETRY_BAROMETER);
    ui->app_obj[0] = sensor_text(content, 8, 0x77B8FF, 24);
    ui->app_obj[1] = sensor_text(content, 64, 0xEAF2FF, 16);
    ui->app_obj[2] = sensor_text(content, 138, 0xC9DCF8, 16);
    sensor_button(ui, "-1", 4, 176, 74, reference_less);
    sensor_button(ui, "相对高度归零", 94, 176, 220, barometer_zero);
    sensor_button(ui, "+1", 330, 176, 74, reference_more);
    ui->app_obj[3] = sensor_text(content, 246, 0x8FA9CD, 16);
    app_ui_set_timer(ui, update_barometer, 700);
    update_barometer(ui->timer);
}
