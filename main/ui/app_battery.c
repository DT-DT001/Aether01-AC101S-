#include "app_battery.h"
#include <app_ui_internal.h>
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>

#define BATTERY_MAX_AGE_MS 7000U

typedef struct {
    lv_obj_t *body, *tip, *fill, *charge, *label;
    lv_timer_t *timer;
    uint32_t color;
    unsigned percent;
} battery_badge_t;

static esp_err_t snapshot_error(const app_tools_snapshot_t *snapshot)
{
    if (!snapshot || !snapshot->ready) return ESP_ERR_INVALID_STATE;
    if (snapshot->battery_error != ESP_OK) return snapshot->battery_error;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (uint32_t)(now - snapshot->battery_updated_ms) > BATTERY_MAX_AGE_MS ?
        ESP_ERR_TIMEOUT : ESP_OK;
}

static bool present(const tg29_battery_state_t *battery)
{
    return battery->presence_valid && battery->battery_present;
}

static bool percentage_valid(const tg29_battery_state_t *battery)
{
    return present(battery) && battery->percentage_valid && battery->percentage <= 100;
}

static void update_badge(battery_badge_t *badge)
{
    app_tools_snapshot_t snapshot;
    app_tools_get_snapshot(&snapshot);
    const tg29_battery_state_t *battery = &snapshot.battery;
    const bool readable = snapshot_error(&snapshot) == ESP_OK;
    const bool valid = readable && percentage_valid(battery);
    const bool on_usb = readable && present(battery) && battery->vbus_present &&
        battery->charge_state_valid && battery->current_direction != 2;
    const bool charging = on_usb && battery->charging;
    const unsigned percent = valid ? battery->percentage : 0;
    uint32_t color = valid ? (percent <= 15 ? 0xFF899D : 0xD5DEFB) : 0x7984A2;
    if (charging || (on_usb && battery->charge_done && valid)) color = 0x82EBC1;
    if (charging == lv_obj_has_flag(badge->charge, LV_OBJ_FLAG_HIDDEN)) {
        if (charging) lv_obj_remove_flag(badge->charge, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(badge->charge, LV_OBJ_FLAG_HIDDEN);
    }
    if (valid) app_ui_label_fmt(badge->label, "%u", percent);
    else app_ui_label_text(badge->label, "--");
    if (badge->percent != percent) {
        badge->percent = percent;
        if (percent) {
            lv_obj_set_width(badge->fill, (34 * percent + 99) / 100);
            lv_obj_remove_flag(badge->fill, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(badge->fill, LV_OBJ_FLAG_HIDDEN);
    }
    if (badge->color != color) {
        badge->color = color;
        lv_obj_set_style_border_color(badge->body, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(badge->tip, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(badge->fill, lv_color_hex(color), 0);
        lv_obj_set_style_text_color(badge->label, lv_color_hex(color), 0);
    }
}

static void battery_timer(lv_timer_t *timer)
{
    update_badge(lv_timer_get_user_data(timer));
}

static void battery_delete(lv_event_t *event)
{
    battery_badge_t *badge = lv_event_get_user_data(event);
    if (badge->timer) lv_timer_delete(badge->timer);
    free(badge);
}

static lv_obj_t *badge_shape(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *shape = lv_obj_create(parent);
    lv_obj_remove_style_all(shape);
    lv_obj_remove_flag(shape, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(shape, width, height);
    return shape;
}

lv_obj_t *app_battery_create(lv_obj_t *parent, int32_t width)
{
    battery_badge_t *badge = calloc(1, sizeof(*badge));
    if (!badge) return NULL;
    lv_obj_t *object = badge_shape(parent, width, 26);
    lv_obj_add_event_cb(object, battery_delete, LV_EVENT_DELETE, badge);
    badge->body = badge_shape(object, 40, 23);
    lv_obj_set_style_radius(badge->body, 5, 0);
    lv_obj_set_style_border_width(badge->body, 2, 0);
    lv_obj_align(badge->body, LV_ALIGN_RIGHT_MID, -4, 0);
    badge->tip = badge_shape(object, 3, 9);
    lv_obj_set_style_radius(badge->tip, 2, 0);
    lv_obj_set_style_bg_opa(badge->tip, LV_OPA_COVER, 0);
    lv_obj_align(badge->tip, LV_ALIGN_RIGHT_MID, 0, 0);
    badge->fill = badge_shape(badge->body, 34, 17);
    lv_obj_set_style_radius(badge->fill, 2, 0);
    lv_obj_set_style_bg_opa(badge->fill, LV_OPA_20, 0);
    lv_obj_align(badge->fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_add_flag(badge->fill, LV_OBJ_FLAG_HIDDEN);
    badge->label = app_ui_add_text(badge->body, "--", LV_ALIGN_CENTER, 0, 0,
                                   0x7984A2, &lv_font_montserrat_16);
    badge->charge = app_ui_add_text(object, LV_SYMBOL_CHARGE, LV_ALIGN_RIGHT_MID,
                                    -50, 0, 0x82EBC1, &lv_font_montserrat_16);
    badge->timer = lv_timer_create(battery_timer, 1000, badge);
    if (!badge->timer) { lv_obj_delete(object); return NULL; }
    update_badge(badge);
    return object;
}

void app_battery_format_detail(const app_tools_snapshot_t *snapshot, char *text, size_t size)
{
    if (!text || !size) return;
    const esp_err_t error = snapshot_error(snapshot);
    if (error != ESP_OK) {
        snprintf(text, size, "电池 未知\n电量 --%% / VBAT 未知\n电池采样 %s",
            !snapshot || !snapshot->ready ? "等待就绪" :
            error == ESP_ERR_TIMEOUT ? "数据过期" : esp_err_to_name(error));
        return;
    }
    const tg29_battery_state_t *battery = &snapshot->battery;
    char percent[48], voltage[32];
    if (percentage_valid(battery)) snprintf(percent, sizeof(percent), "%u%%（TG29估计）", battery->percentage);
    else snprintf(percent, sizeof(percent), "--%%（不可用）");
    if (present(battery) && battery->voltage_valid && battery->voltage_mv && battery->voltage_mv < 0x3FFFU)
        snprintf(voltage, sizeof(voltage), "%u mV", battery->voltage_mv);
    else snprintf(voltage, sizeof(voltage), "未知");
    const char *charging = !present(battery) ? "无有效电池" :
        battery->current_direction == 2 ? "电池放电" :
        !battery->charge_state_valid ? "充电状态未知" :
        battery->vbus_present && battery->charging ? "正在充电" :
        battery->vbus_present && battery->charge_done ? "充电完成" : "未充电";
    snprintf(text, size, "电池 %s\n电量 %s\nVBAT %s\nUSB %s / %s\n电量为估计值",
        !battery->presence_valid ? "存在性未知" : battery->battery_present ? "已连接" : "未接电池",
        percent, voltage, battery->vbus_present ? "有效" : "未有效", charging);
}
