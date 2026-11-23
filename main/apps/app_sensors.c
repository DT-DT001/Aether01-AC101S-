#include "app_ui_internal.h"
#include "app_instrument_ui.h"
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
    lv_obj_t *pressure, *temperature, *altitude, *relative, *minimum, *maximum, *reference, *notice;
    char message[80];
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

static lv_obj_t *sensor_button(app_ui_t *ui, const char *text, int x, int y, int width, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(ui->content);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 44);
    instrument_button_style(button, width > 92 ? INSTRUMENT_PRIMARY : INSTRUMENT_SECONDARY);
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
                        err == ESP_OK ? "参考已更新" : "设置失败 ", err == ESP_OK ? "" : esp_err_to_name(err));
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
        snprintf(page->message, sizeof(page->message), "气压无效，无法归零");
        return;
    }
    page->zero_pressure = telemetry.barometer.pressure_pa;
    snprintf(page->message, sizeof(page->message), "相对高度已归零");
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
    app_ui_label_fmt(page->reference, "%.2f hPa", tools.reference_pressure_pa / 100);
    if (telemetry.barometer_error != ESP_OK || !isfinite(telemetry.barometer.pressure_pa) ||
        !isfinite(telemetry.barometer.temperature_c) || telemetry.barometer.pressure_pa <= 0) {
        app_ui_label_text(page->pressure, "--");
        app_ui_label_text(page->temperature, "--");
        app_ui_label_text(page->altitude, "--");
        app_ui_label_text(page->relative, "--");
        app_ui_label_text(page->notice, telemetry.barometer_error == ESP_OK ? "采样异常" : "等待气压采样");
        return;
    }
    const dps368_sample_t *sample = &telemetry.barometer;
    if (sample->pressure_pa < page->pressure_min) page->pressure_min = sample->pressure_pa;
    if (sample->pressure_pa > page->pressure_max) page->pressure_max = sample->pressure_pa;
    const float altitude = app_pressure_altitude(sample->pressure_pa, tools.reference_pressure_pa);
    app_ui_label_fmt(page->pressure, "%.2f", sample->pressure_pa / 100);
    app_ui_label_fmt(page->temperature, "%.1f ℃", sample->temperature_c);
    if (isfinite(altitude)) app_ui_label_fmt(page->altitude, "%.1f m", altitude);
    else app_ui_label_text(page->altitude, "--");
    const float relative = page->zero_pressure > 0 ? app_pressure_altitude(sample->pressure_pa, page->zero_pressure) : NAN;
    if (isfinite(relative)) app_ui_label_fmt(page->relative, "%+.1f m", relative);
    else app_ui_label_text(page->relative, "--");
    app_ui_label_fmt(page->minimum, "%.2f", page->pressure_min / 100);
    app_ui_label_fmt(page->maximum, "%.2f", page->pressure_max / 100);
    app_ui_label_text(page->notice, !tools.ready ? "正在加载参考气压" :
        tools.preferences_error != ESP_OK ? esp_err_to_name(tools.preferences_error) :
        tools.preferences_deferred ? "待媒体暂停后保存" : tools.preferences_pending ? "正在保存参考气压" :
        page->message);
}

void app_open_barometer(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "气压与高度");
    sensor_page_t *page = sensor_context(ui);
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8C8C, app_ui_chinese_font());
        return;
    }
    app_telemetry_set_interest(APP_TELEMETRY_BAROMETER);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    const lv_font_t *small = app_ui_chinese_font();
    lv_obj_t *hero = instrument_card(content, 0, 0, 408, 113);
    instrument_text(hero, "当前气压", 16, 13, 160, INSTRUMENT_MUTED, small);
    page->pressure = instrument_text(hero, "--", 15, 36, 252, INSTRUMENT_BLUE, &lv_font_montserrat_48);
    instrument_text(hero, "hPa", 245, 65, 52, INSTRUMENT_BLUE, &lv_font_montserrat_16);
    instrument_text(hero, "温度", 316, 22, 75, INSTRUMENT_MUTED, small);
    page->temperature = instrument_text(hero, "--", 300, 52, 100, INSTRUMENT_TEAL, app_ui_chinese_font_size(24));

    lv_obj_t *altitude = instrument_card(content, 0, 123, 198, 75);
    lv_obj_t *relative = instrument_card(content, 210, 123, 198, 75);
    instrument_text(altitude, "估算海拔", 15, 10, 165, INSTRUMENT_MUTED, small);
    instrument_text(relative, "相对高度", 15, 10, 165, INSTRUMENT_MUTED, small);
    page->altitude = instrument_text(altitude, "--", 15, 32, 172, INSTRUMENT_TEXT, &lv_font_montserrat_28);
    page->relative = instrument_text(relative, "--", 15, 32, 172, INSTRUMENT_TEAL, &lv_font_montserrat_28);

    lv_obj_t *range = instrument_card(content, 0, 208, 408, 52);
    instrument_text(range, "最低", 15, 7, 52, INSTRUMENT_MUTED, small);
    instrument_text(range, "最高", 150, 7, 52, INSTRUMENT_MUTED, small);
    page->minimum = instrument_text(range, "--", 15, 24, 106, INSTRUMENT_TEXT, &lv_font_montserrat_16);
    page->maximum = instrument_text(range, "--", 150, 24, 106, INSTRUMENT_TEXT, &lv_font_montserrat_16);
    instrument_text(range, "海平面参考", 276, 7, 122, INSTRUMENT_MUTED, small);
    page->reference = instrument_text(range, "", 276, 24, 122, INSTRUMENT_BLUE, &lv_font_montserrat_16);
    sensor_button(ui, "-1", 0, 271, 84, reference_less);
    sensor_button(ui, "相对高度归零", 94, 271, 220, barometer_zero);
    sensor_button(ui, "+1", 324, 271, 84, reference_more);
    page->notice = instrument_text(content, "", 6, 325, 396, INSTRUMENT_MUTED, small);
    app_ui_set_timer(ui, update_barometer, 700);
    update_barometer(ui->timer);
}
