#include "app_battery.h"
#include <app_ui_internal.h>
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>

#define BATTERY_MAX_AGE_MS 7000U

typedef struct {
    lv_obj_t *icon, *label;
    lv_timer_t *timer;
    uint32_t color;
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

static const char *charge_text(const tg29_battery_state_t *battery)
{
    if (!present(battery)) return battery->vbus_present ? "USB" : "";
    if (battery->current_direction == 2) return "放电";
    if (battery->charge_state_valid && battery->vbus_present) {
        if (battery->charging) return "充电";
        if (battery->charge_done) return "已满";
    }
    return battery->vbus_present ? "USB" : "";
}

static void update_badge(battery_badge_t *badge)
{
    app_tools_snapshot_t snapshot;
    app_tools_get_snapshot(&snapshot);
    const tg29_battery_state_t *battery = &snapshot.battery;
    const bool readable = snapshot_error(&snapshot) == ESP_OK;
    const bool valid = readable && percentage_valid(battery);
    const char *suffix = readable ? charge_text(battery) : "";
    const char *icon = LV_SYMBOL_BATTERY_EMPTY;
    uint32_t color = 0x8299B8;
    char text[40];
    if (valid) {
        const unsigned percent = battery->percentage;
        icon = percent > 75 ? LV_SYMBOL_BATTERY_FULL : percent > 50 ? LV_SYMBOL_BATTERY_3 :
               percent > 25 ? LV_SYMBOL_BATTERY_2 : percent ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        color = percent <= 15 ? 0xFF879A : 0xD6E5F5;
        if (percent > 15 && battery->current_direction != 2 && battery->vbus_present && battery->charge_state_valid &&
            (battery->charging || battery->charge_done)) color = 0x8FE0BA;
        snprintf(text, sizeof(text), "%u%%%s%s", percent, suffix[0] ? " " : "", suffix);
    } else {
        if (readable && battery->vbus_present) { icon = LV_SYMBOL_USB; color = 0x9BBAD8; }
        snprintf(text, sizeof(text), "--%%%s%s", suffix[0] ? " " : "", suffix);
    }
    app_ui_label_text(badge->icon, icon);
    app_ui_label_text(badge->label, text);
    if (badge->color != color) {
        badge->color = color;
        lv_obj_set_style_text_color(badge->icon, lv_color_hex(color), 0);
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

lv_obj_t *app_battery_create(lv_obj_t *parent, int32_t width)
{
    battery_badge_t *badge = calloc(1, sizeof(*badge));
    if (!badge) return NULL;
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, 26);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(object, battery_delete, LV_EVENT_DELETE, badge);
    badge->icon = app_ui_add_text(object, LV_SYMBOL_BATTERY_EMPTY, LV_ALIGN_LEFT_MID,
                                   0, 0, 0x8299B8, &lv_font_montserrat_20);
    badge->label = app_ui_add_text(object, "--%", LV_ALIGN_LEFT_MID, 31, 0,
                                    0x8299B8, app_ui_chinese_font());
    lv_obj_set_width(badge->label, width - 31);
    lv_label_set_long_mode(badge->label, LV_LABEL_LONG_CLIP);
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
