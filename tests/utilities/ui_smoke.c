#include "app_ui_internal.h"
#include "app_utilities.h"
#include "app_ui_modal.h"
#include "app_telemetry.h"
#include "app_otg_control.h"
#include "app_usb_input.h"
#include "aether_wifi.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool keyboard_present;
static char notes[APP_NOTE_COUNT][APP_NOTE_CAPACITY];
static app_alarm_snapshot_t alarm_state = {.ready = true, .hour = 7, .minute = 15};
static app_weather_snapshot_t forecast = {.valid = true, .temperature_c = 21, .humidity = 90};
static unsigned refreshes;
static lv_obj_t *popup;
static aether_wifi_status_t wifi = {
    .enabled = true, .connected = true, .state = AETHER_WIFI_CONNECTED,
    .ap_count = 1, .scan_generation = 1,
};
static struct aether_wifi { int unused; } wifi_handle;
static aether_wifi_ap_t access_point = {
    .ssid = "Test network", .password_required = true, .supported = true, .rssi = -40, .channel = 6,
};
static unsigned wifi_connections;

void app_open_wifi(app_ui_t *ui);

extern const lv_font_t font_16, font_24;
const lv_font_t *app_ui_chinese_font(void) { return app_ui_chinese_font_size(16); }
const lv_font_t *app_ui_chinese_font_size(unsigned size)
{
    static lv_font_t small, large;
    small = font_16;
    large = font_24;
    small.fallback = &lv_font_montserrat_16;
    large.fallback = &lv_font_montserrat_24;
    return size == 24 ? &large : &small;
}
const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "OK" : "Error"; }
bool app_power_is_sleeping(app_power_t *power) { (void)power; return false; }
bool app_ui_input_blocked(const app_ui_t *ui) { (void)ui; return false; }
esp_err_t app_otg_get_status(aether_usb_host_status_t *status)
{
    *status = (aether_usb_host_status_t){.active = true, .keyboard_interfaces = keyboard_present};
    return ESP_OK;
}
void app_alarm_get(app_alarm_snapshot_t *out) { *out = alarm_state; }
esp_err_t app_alarm_set(unsigned hour, unsigned minute, bool enabled)
{
    alarm_state.hour = hour;
    alarm_state.minute = minute;
    alarm_state.enabled = enabled;
    return ESP_OK;
}
void app_alarm_dismiss(void) { alarm_state.ringing = false; }
void app_note_get_status(unsigned note, app_note_status_t *out)
{
    assert(note < APP_NOTE_COUNT);
    *out = (app_note_status_t){.ready = true};
}
esp_err_t app_note_read(unsigned note, char out[APP_NOTE_CAPACITY])
{
    memcpy(out, notes[note], APP_NOTE_CAPACITY);
    return ESP_OK;
}
esp_err_t app_note_save(unsigned note, const char *text)
{
    snprintf(notes[note], APP_NOTE_CAPACITY, "%s", text);
    return ESP_OK;
}
void app_telemetry_get(app_telemetry_snapshot_t *out)
{
    *out = (app_telemetry_snapshot_t){.rtc_error = ESP_OK, .rtc = {.year = 2026, .month = 2, .day = 17}};
}
void app_weather_get(app_weather_snapshot_t *weather) { *weather = forecast; }
void app_weather_refresh(bool manual) { refreshes += manual; }
esp_err_t app_weather_select_city(unsigned city) { forecast.city = city; return ESP_OK; }
aether_wifi_handle_t aether_wifi_get_default(void) { return &wifi_handle; }
esp_err_t aether_wifi_get_status(aether_wifi_handle_t handle, aether_wifi_status_t *status)
{
    assert(handle == &wifi_handle);
    *status = wifi;
    return ESP_OK;
}
esp_err_t aether_wifi_get_ap(aether_wifi_handle_t handle, size_t index, aether_wifi_ap_t *ap)
{
    assert(handle == &wifi_handle && index == 0);
    *ap = access_point;
    return ESP_OK;
}
esp_err_t aether_wifi_connect(aether_wifi_handle_t handle, const aether_wifi_ap_t *ap,
                             const char *password, bool save)
{
    assert(handle == &wifi_handle && strcmp(ap->ssid, access_point.ssid) == 0 && save);
    if (ap->password_required && strcmp(password, "abcdefgh") != 0) return ESP_ERR_INVALID_ARG;
    if (!ap->password_required) assert(!*password);
    ++wifi_connections;
    return ESP_OK;
}
esp_err_t aether_wifi_set_enabled(aether_wifi_handle_t handle, bool enabled)
{
    wifi.enabled = enabled;
    return handle ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t aether_wifi_scan(aether_wifi_handle_t handle) { return handle ? ESP_OK : ESP_FAIL; }
esp_err_t aether_wifi_disconnect(aether_wifi_handle_t handle) { return handle ? ESP_OK : ESP_FAIL; }
esp_err_t aether_wifi_forget(aether_wifi_handle_t handle) { return handle ? ESP_OK : ESP_FAIL; }
const char *aether_wifi_security_name(uint8_t authmode) { (void)authmode; return "WPA"; }
esp_err_t app_usb_input_modal_open(lv_obj_t *modal, lv_obj_t *focus)
{
    assert(modal && focus);
    return ESP_OK;
}

lv_obj_t *app_ui_modal_msgbox(lv_obj_t *owner)
{
    popup = lv_msgbox_create(NULL);
    lv_obj_set_parent(lv_obj_get_parent(popup), owner);
    return popup;
}
void app_ui_modal_focus(lv_obj_t *modal, lv_obj_t *preferred) { assert(modal && preferred); }

static void close_page(app_ui_t *ui)
{
    if (ui->page_cleanup) ui->page_cleanup(ui);
    ui->page_context = NULL;
    ui->page_cleanup = NULL;
    if (ui->timer) lv_timer_delete(ui->timer);
    ui->timer = NULL;
}

lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    (void)title;
    close_page(ui);
    lv_obj_t *old = ui->screen;
    ui->screen = lv_obj_create(NULL);
    lv_obj_set_style_text_font(ui->screen, app_ui_chinese_font(), 0);
    lv_obj_remove_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(ui->screen);
    if (old) lv_obj_delete(old);
    lv_obj_t *content = lv_obj_create(ui->screen);
    lv_obj_set_size(content, 448, 394);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -14);
    return content;
}

lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text, lv_align_t align,
                          int32_t x, int32_t y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, align, x, y);
    return label;
}

void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback, uint32_t period)
{
    if (ui->timer) lv_timer_delete(ui->timer);
    ui->timer = lv_timer_create(callback, period, ui);
}

void app_ui_label_text(lv_obj_t *label, const char *text) { lv_label_set_text(label, text); }
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    lv_label_set_text(label, buffer);
}

static lv_obj_t *find_type(lv_obj_t *root, const lv_obj_class_t *type)
{
    if (lv_obj_check_type(root, type)) return root;
    for (uint32_t index = 0; index < lv_obj_get_child_count(root); ++index) {
        lv_obj_t *found = find_type(lv_obj_get_child(root, index), type);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_label(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && strcmp(lv_label_get_text(root), text) == 0)
        return root;
    for (uint32_t index = 0; index < lv_obj_get_child_count(root); ++index) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, index), text);
        if (found) return found;
    }
    return NULL;
}

static void press_key(lv_obj_t *keyboard, const char *key)
{
    for (uint32_t index = 0; ; ++index) {
        const char *text = lv_keyboard_get_button_text(keyboard, index);
        assert(text);
        if (strcmp(text, key) != 0) continue;
        lv_buttonmatrix_set_selected_button(keyboard, index);
        lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }
}

static void advance(unsigned ms)
{
    lv_tick_inc(ms);
    lv_timer_handler();
}

int main(void)
{
    lv_init();
    assert(lv_display_create(480, 480));
    app_ui_t ui = {0};
    app_open_notes(&ui);
    lv_obj_t *field = find_type(ui.screen, &lv_textarea_class);
    lv_obj_t *keyboard = find_type(ui.screen, &lv_keyboard_class);
    assert(field && keyboard && !lv_obj_is_visible(keyboard));
    lv_obj_send_event(field, LV_EVENT_CLICKED, NULL);
    assert(lv_obj_is_visible(keyboard));
    press_key(keyboard, "n");
    press_key(keyboard, "i");
    lv_obj_t *candidate = find_label(lv_obj_get_parent(keyboard), "你");
    assert(candidate);
    lv_obj_send_event(lv_obj_get_parent(candidate), LV_EVENT_CLICKED, NULL);
    assert(strcmp(lv_textarea_get_text(field), "你") == 0);
    for (unsigned index = 0; index < 18; ++index) press_key(keyboard, "a");
    press_key(keyboard, LV_SYMBOL_BACKSPACE);
    keyboard_present = true;
    advance(180);
    assert(!lv_obj_is_visible(keyboard));
    lv_textarea_add_text(field, "abc");
    keyboard_present = false;
    advance(180);
    assert(lv_obj_is_visible(keyboard));
    lv_obj_send_event(field, LV_EVENT_READY, NULL);
    assert(!lv_obj_is_visible(keyboard) && strstr(notes[0], "abc"));
    lv_obj_t *dropdown = find_type(ui.screen, &lv_dropdown_class);
    lv_dropdown_set_selected(dropdown, 1);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    advance(260);
    assert(strcmp(lv_textarea_get_text(field), "") == 0);
    lv_textarea_add_text(field, "second");

    app_open_calendar(&ui);
    assert(strcmp(notes[1], "second") == 0);
    lv_obj_t *calendar = find_type(ui.screen, &lv_calendar_class);
    const lv_calendar_date_t *shown = lv_calendar_get_showed_date(calendar);
    assert(shown->year == 2026 && shown->month == 2);
#if LV_USE_CALENDAR_CHINESE
    lv_calendar_date_t date = {2026, 2, 17};
    assert(strcmp(lv_calendar_get_day_name(&date), "春节") == 0);
#endif
    lv_obj_t *next = find_label(ui.screen, LV_SYMBOL_RIGHT);
    lv_obj_send_event(lv_obj_get_parent(next), LV_EVENT_CLICKED, NULL);
    assert(lv_calendar_get_showed_date(calendar)->month == 3);

    app_open_weather(&ui);
    assert(find_label(ui.screen, "21 °C"));
    dropdown = find_type(ui.screen, &lv_dropdown_class);
    lv_dropdown_set_selected(dropdown, 4);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    assert(forecast.city == 4);
    lv_obj_t *refresh = find_label(ui.screen, LV_SYMBOL_REFRESH);
    lv_obj_send_event(lv_obj_get_parent(refresh), LV_EVENT_CLICKED, NULL);
    assert(refreshes == 1);

    app_open_alarm(&ui);
    lv_obj_t *enabled = find_type(ui.screen, &lv_switch_class);
    lv_obj_add_state(enabled, LV_STATE_CHECKED);
    lv_obj_t *save = find_label(ui.screen, "保存");
    lv_obj_send_event(lv_obj_get_parent(save), LV_EVENT_CLICKED, NULL);
    assert(alarm_state.enabled && alarm_state.hour == 7 && alarm_state.minute == 15);
    alarm_state.ringing = true;
    app_utilities_alarm_ui(&ui);
    assert(popup && find_label(popup, "07:15"));
    lv_obj_t *dismiss = find_label(popup, "关闭");
    lv_obj_send_event(lv_obj_get_parent(dismiss), LV_EVENT_CLICKED, NULL);
    assert(!alarm_state.ringing);
    ui.media_active = true;
    alarm_state.ringing = true;
    app_utilities_alarm_ui(&ui);
    assert(find_label(ui.screen, LV_SYMBOL_BELL "  07:15"));
    app_alarm_dismiss();
    app_utilities_alarm_ui(&ui);
    assert(!find_label(ui.screen, LV_SYMBOL_BELL "  07:15"));
    ui.media_active = false;

    app_open_wifi(&ui);
    lv_obj_t *ap_row = find_label(ui.screen, "Test network\nWPA · -40 dBm · CH6");
    assert(ap_row);
    lv_obj_send_event(lv_obj_get_parent(ap_row), LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(ui.screen);
    field = find_type(ui.screen, &lv_textarea_class);
    keyboard = find_type(ui.screen, &lv_keyboard_class);
    assert(field && keyboard && lv_obj_is_visible(keyboard));
    press_key(keyboard, "a");
    press_key(keyboard, LV_SYMBOL_OK);
    assert(find_label(ui.screen, "密码需要 8–63 个字符") && wifi_connections == 0);
    assert(strcmp(lv_textarea_get_text(field), "a") == 0);
    keyboard_present = true;
    advance(180);
    assert(!lv_obj_is_visible(keyboard));
    lv_textarea_add_text(field, "bcdefgh");
    lv_obj_send_event(field, LV_EVENT_READY, NULL);
    assert(wifi_connections == 1 && !find_type(ui.screen, &lv_textarea_class));
    access_point.password_required = false;
    ++wifi.scan_generation;
    advance(360);
    lv_obj_send_event(lv_obj_get_parent(ap_row), LV_EVENT_CLICKED, NULL);
    assert(!find_type(ui.screen, &lv_keyboard_class));
    lv_obj_t *connect = find_label(ui.screen, "连接");
    lv_obj_send_event(lv_obj_get_parent(connect), LV_EVENT_CLICKED, NULL);
    assert(wifi_connections == 2);
    close_page(&ui);
    puts("Real LVGL Wi-Fi/keyboard, notes, calendar, weather and alarm interactions passed");
    return 0;
}
