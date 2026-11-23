#include "app_ui_internal.h"
#include "app_utilities.h"
#include "app_ui_modal.h"
#include "app_text_input.h"
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
static app_alarm_snapshot_t alarm_state;
static app_alarm_slot_t alarm_slots[APP_ALARM_SLOTS];
static unsigned previews, preview_tone, preview_cancels;
static esp_err_t preview_error;
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
static unsigned wifi_connections, wifi_preference_changes;

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
esp_err_t app_alarm_get_slot(unsigned slot, app_alarm_slot_t *out)
{
    assert(slot < APP_ALARM_SLOTS && out);
    if (!alarm_state.ready) return ESP_ERR_INVALID_STATE;
    *out = alarm_slots[slot];
    return ESP_OK;
}
esp_err_t app_alarm_set_slot(unsigned slot, unsigned hour, unsigned minute, bool enabled, unsigned tone)
{
    assert(slot < APP_ALARM_SLOTS && hour < 24 && minute < 60 && tone < APP_ALARM_TONE_COUNT);
    alarm_slots[slot] = (app_alarm_slot_t){.configured = true, .enabled = enabled,
                                          .hour = hour, .minute = minute, .tone = tone};
    alarm_state.pending = true;
    return ESP_OK;
}
esp_err_t app_alarm_remove_slot(unsigned slot)
{
    assert(slot < APP_ALARM_SLOTS && alarm_slots[slot].configured);
    alarm_slots[slot] = (app_alarm_slot_t){0};
    alarm_state.pending = true;
    return ESP_OK;
}
esp_err_t app_alarm_preview(unsigned tone)
{
    assert(tone < APP_ALARM_TONE_COUNT);
    ++previews;
    preview_tone = tone;
    return preview_error;
}
void app_alarm_cancel_preview(void) { ++preview_cancels; }
const char *app_alarm_tone_name(unsigned tone)
{
    static const char *const names[] = {"清晨", "双响", "风铃", "急促"};
    assert(tone < sizeof(names) / sizeof(names[0]));
    return names[tone];
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
esp_err_t aether_wifi_set_enabled_preference(aether_wifi_handle_t handle, bool enabled)
{
    assert(handle == &wifi_handle);
    ++wifi_preference_changes;
    wifi.enabled_preference = enabled;
    if (!wifi.peer_radio) {
        wifi.enabled = enabled;
        wifi.connected = false;
        wifi.state = enabled ? AETHER_WIFI_IDLE : AETHER_WIFI_OFF;
    }
    return ESP_OK;
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
    ui->page_back = NULL;
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

static uint16_t ui_buffer[480 * 16];
static uint16_t ui_pixels[480 * 480];
static bool capture_pixels;
static lv_point_t alarm_point;
static bool alarm_held;

static void ui_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    assert(area && pixels);
    if (capture_pixels) {
        const size_t width = (size_t)lv_area_get_width(area);
        assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < 480 && area->y2 < 480);
        for (int32_t y = area->y1; y <= area->y2; ++y)
            memcpy(ui_pixels + y * 480 + area->x1,
                   pixels + (size_t)(y - area->y1) * width * 2, width * 2);
    }
    lv_display_flush_ready(display);
}

static int32_t sampled_caret_x(lv_obj_t *field, uint32_t color)
{
    const uint16_t rgb = lv_color_to_u16(lv_color_hex(color));
    lv_area_t area;
    lv_obj_get_coords(field, &area);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 480 && area.y2 < 480);
    for (int32_t x = area.x1; x <= area.x2; ++x) {
        unsigned consecutive = 0;
        for (int32_t y = area.y1; y <= area.y2; ++y) {
            consecutive = ui_pixels[y * 480 + x] == rgb ? consecutive + 1 : 0;
            if (consecutive >= 12) return x;
        }
    }
    return -1;
}

static int32_t rendered_caret_x(lv_display_t *display, lv_obj_t *field, uint32_t color)
{
    capture_pixels = true;
    lv_obj_invalidate(field);
    lv_refr_now(display);
    capture_pixels = false;
    return sampled_caret_x(field, color);
}

static void advance_caret(lv_display_t *display, unsigned duration_ms)
{
    capture_pixels = true;
    for (unsigned elapsed = 0; elapsed < duration_ms; elapsed += 20) advance(20);
    lv_refr_now(display);
    capture_pixels = false;
}

static int32_t wait_for_caret(lv_display_t *display, lv_obj_t *field, uint32_t color)
{
    int32_t x = rendered_caret_x(display, field, color);
    for (unsigned elapsed = 0; x < 0 && elapsed < 600; elapsed += 20) {
        advance_caret(display, 20);
        x = sampled_caret_x(field, color);
    }
    return x;
}

static void assert_caret_blinks(lv_display_t *display, lv_obj_t *field, uint32_t color)
{
    const int32_t x = rendered_caret_x(display, field, color);
    assert(x >= 0);
    advance_caret(display, 600);
    assert(sampled_caret_x(field, color) == -1);
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED));
    advance_caret(display, 500);
    assert(sampled_caret_x(field, color) == x);
}

static void alarm_pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = alarm_point;
    data->state = alarm_held ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void alarm_pointer_sample(lv_indev_t *input, int x, int y, bool pressed)
{
    alarm_point = (lv_point_t){x, y};
    alarm_held = pressed;
    advance(22);
    lv_indev_read(input);
}

static void alarm_tap(lv_indev_t *input, lv_obj_t *target)
{
    assert(target);
    lv_obj_update_layout(lv_obj_get_screen(target));
    assert(lv_obj_is_visible(target));
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    const int x = (area.x1 + area.x2) / 2, y = (area.y1 + area.y2) / 2;
    alarm_pointer_sample(input, x, y, true);
    alarm_pointer_sample(input, x, y, false);
}

static void alarm_click(lv_indev_t *input, lv_obj_t *root, const char *text)
{
    lv_obj_t *label = find_label(root, text);
    if (!label) fprintf(stderr, "Missing interaction label: %s\n", text);
    assert(label);
    lv_obj_t *button = lv_obj_get_parent(label);
    lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
    alarm_tap(input, button);
}

static bool visible_label(lv_obj_t *root, const char *text)
{
    lv_obj_t *label = find_label(root, text);
    return label && lv_obj_is_visible(label);
}

typedef struct {
    lv_area_t areas[64];
    bool drawn[64];
} keyboard_draw_t;

static void capture_key_area(lv_event_t *event)
{
    keyboard_draw_t *draw = lv_event_get_user_data(event);
    lv_draw_task_t *task = lv_event_get_draw_task(event);
    if (lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_FILL) return;
    const lv_draw_dsc_base_t *descriptor = lv_draw_task_get_draw_dsc(task);
    if (descriptor->part != LV_PART_ITEMS) return;
    assert(descriptor->id1 < sizeof(draw->drawn) / sizeof(draw->drawn[0]));
    lv_draw_task_get_area(task, &draw->areas[descriptor->id1]);
    draw->drawn[descriptor->id1] = true;
}

static void capture_keyboard(lv_display_t *display, lv_obj_t *keyboard, keyboard_draw_t *draw)
{
    memset(draw, 0, sizeof(*draw));
    lv_obj_invalidate(keyboard);
    lv_refr_now(display);
}

static lv_point_t key_center(lv_obj_t *keyboard, const keyboard_draw_t *draw, const char *key)
{
    for (uint32_t index = 0; index < sizeof(draw->drawn) / sizeof(draw->drawn[0]); ++index) {
        const char *label = lv_buttonmatrix_get_button_text(keyboard, index);
        if (label && !strcmp(label, key)) {
            assert(draw->drawn[index]);
            const lv_area_t *area = &draw->areas[index];
            return (lv_point_t){(area->x1 + area->x2) / 2, (area->y1 + area->y2) / 2};
        }
    }
    assert(false);
    return (lv_point_t){0};
}

static void tap_key(lv_indev_t *input, lv_obj_t *keyboard, const keyboard_draw_t *draw, const char *key)
{
    const lv_point_t point = key_center(keyboard, draw, key);
    alarm_pointer_sample(input, point.x, point.y, true);
    alarm_pointer_sample(input, point.x, point.y, false);
}

static void slide_to(lv_indev_t *input, lv_point_t from, lv_point_t to)
{
    for (int step = 1; step <= 12; ++step)
        alarm_pointer_sample(input, from.x + (to.x - from.x) * step / 12,
                             from.y + (to.y - from.y) * step / 12, true);
}

static void swipe_trace(lv_indev_t *input, lv_obj_t *keyboard, const keyboard_draw_t *draw,
                        const char *word, bool brush_edge, int32_t shift)
{
    char letter[] = {word[0], '\0'};
    lv_point_t point = key_center(keyboard, draw, letter);
    if (shift) { point.x += shift; point.y -= 6; }
    alarm_pointer_sample(input, point.x, point.y, true);
    if (brush_edge) {
        const lv_point_t nearby = {point.x, point.y + 18};
        alarm_pointer_sample(input, nearby.x, nearby.y, true);
        alarm_pointer_sample(input, point.x, point.y + 53, true);
        alarm_pointer_sample(input, point.x + 5, point.y + 53, true);
        alarm_pointer_sample(input, point.x + 10, point.y + 53, true);
        alarm_pointer_sample(input, nearby.x, nearby.y, true);
        alarm_pointer_sample(input, point.x, point.y, true);
    }
    for (size_t i = 1; word[i]; ++i) {
        letter[0] = word[i];
        lv_point_t next = key_center(keyboard, draw, letter);
        if (shift) {
            next.x += shift + 1;
            next.y -= 5;
            for (int32_t step = 1; step <= 3; ++step)
                alarm_pointer_sample(input, point.x + (next.x - point.x) * step / 3,
                    point.y + (next.y - point.y) * step / 3, true);
        } else slide_to(input, point, next);
        point = next;
    }
    alarm_pointer_sample(input, point.x, point.y, false);
}

static void swipe_letters(lv_indev_t *input, lv_obj_t *keyboard, const keyboard_draw_t *draw, const char *word)
{
    swipe_trace(input, keyboard, draw, word, false, 0);
}

static uint32_t key_pixels(lv_point_t point)
{
    uint32_t value = 2166136261U;
    for (int y = -7; y <= 7; ++y)
        for (int x = -7; x <= 7; ++x)
            value = (value ^ ui_pixels[(point.y + y) * 480 + point.x + x]) * 16777619U;
    return value;
}

static void test_pinyin_pointer(lv_display_t *display, lv_obj_t *field, lv_obj_t *keyboard)
{
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, alarm_pointer_read);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    assert(!lv_obj_is_visible(keyboard));
    alarm_tap(input, field);
    assert(lv_obj_is_visible(keyboard));
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED));
    assert_caret_blinks(display, field, 0x92E7F1);

    keyboard_draw_t keys;
    lv_obj_add_event_cb(keyboard, capture_key_area, LV_EVENT_DRAW_TASK_ADDED, &keys);
    capture_keyboard(display, keyboard, &keys);
    lv_obj_t *dock = lv_obj_get_parent(keyboard);
    lv_obj_t *actions = find_type(dock, &lv_buttonmatrix_class);
    assert(actions && actions != keyboard);
    keyboard_draw_t action_keys;
    lv_obj_add_flag(actions, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(actions, capture_key_area, LV_EVENT_DRAW_TASK_ADDED, &action_keys);
    capture_keyboard(display, actions, &action_keys);
    lv_area_t field_area, dock_area;
    lv_obj_get_coords(field, &field_area);
    lv_obj_get_coords(dock, &dock_area);
    assert(field_area.y2 < dock_area.y1);
    assert(lv_obj_get_content_height(field) >= 2 * lv_font_get_line_height(app_ui_chinese_font_size(24)) +
           lv_obj_get_style_text_line_space(field, LV_PART_MAIN));
    lv_area_t keyboard_area, action_area;
    lv_obj_get_coords(keyboard, &keyboard_area);
    lv_obj_get_coords(actions, &action_area);
    assert(lv_area_get_width(&keyboard_area) == 472 && lv_area_get_height(&keyboard_area) == 136);
    assert(lv_area_get_height(&action_area) == 38);
    assert(action_area.y1 - keyboard_area.y2 >= 10);
    const lv_point_t bottom_letter = key_center(keyboard, &keys, "m");
    const lv_point_t enter = key_center(actions, &action_keys, LV_SYMBOL_NEW_LINE);
    alarm_pointer_sample(input, enter.x, enter.y, true);
    assert(!strcmp(lv_textarea_get_text(field), ""));
    slide_to(input, enter, bottom_letter);
    alarm_pointer_sample(input, bottom_letter.x, bottom_letter.y, false);
    alarm_pointer_sample(input, bottom_letter.x, bottom_letter.y, true);
    slide_to(input, bottom_letter, enter);
    alarm_pointer_sample(input, enter.x, enter.y, false);
    assert(!strcmp(lv_textarea_get_text(field), ""));
    app_text_input_show(dock, false);
    app_text_input_show(dock, true);
    const lv_point_t q = key_center(keyboard, &keys, "q");
    alarm_pointer_sample(input, keyboard_area.x1 - 4, q.y, true);
    alarm_pointer_sample(input, keyboard_area.x1 - 4, q.y, false);
    assert(visible_label(dock, "q") && !strcmp(lv_textarea_get_text(field), ""));
    tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
    assert(!visible_label(dock, "q"));
    alarm_pointer_sample(input, q.x, q.y, true);
    alarm_pointer_sample(input, q.x + 17, q.y, true);
    alarm_pointer_sample(input, q.x + 17, q.y, false);
    assert(visible_label(dock, "q") && !strcmp(lv_textarea_get_text(field), ""));
    tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);

    const lv_point_t g = key_center(keyboard, &keys, "g");
    const lv_point_t h = key_center(keyboard, &keys, "h");
    for (int error = 1; error <= 3; ++error) {
        const int x = (g.x + h.x) / 2 - error;
        alarm_pointer_sample(input, x, g.y, true);
        alarm_pointer_sample(input, x, g.y, false);
        assert(visible_label(dock, "g"));
        tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
        tap_key(input, keyboard, &keys, "c");
        alarm_pointer_sample(input, x, g.y, true);
        alarm_pointer_sample(input, x, g.y, false);
        assert(visible_label(dock, "ch"));
        tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
        tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
    }
    tap_key(input, keyboard, &keys, "c");
    tap_key(input, keyboard, &keys, "g");
    assert(visible_label(dock, "cg"));
    tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
    tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
    puts("PASS dynamic Pinyin touch boundaries correct 1–3 px errors while preserving neutral and deliberate taps");

    tap_key(input, keyboard, &keys, "n");
    assert(visible_label(dock, "n") && !strcmp(lv_textarea_get_text(field), ""));
    tap_key(input, keyboard, &keys, "i");
    assert(visible_label(dock, "ni") && !strcmp(lv_textarea_get_text(field), ""));
    alarm_click(input, dock, "你");
    assert(!strcmp(lv_textarea_get_text(field), "你"));
    swipe_letters(input, keyboard, &keys, "nishi");
    alarm_click(input, dock, "你是");
    swipe_letters(input, keyboard, &keys, "nishishui");
    alarm_click(input, dock, "你是谁");
    swipe_letters(input, keyboard, &keys, "nishishei");
    alarm_click(input, dock, "你是谁");
    assert(!strcmp(lv_textarea_get_text(field), "你你是你是谁你是谁"));
    lv_textarea_set_text(field, "");
    swipe_letters(input, keyboard, &keys, "nichifanlema");
    alarm_click(input, dock, "你吃饭了吗");
    assert(!strcmp(lv_textarea_get_text(field), "你吃饭了吗"));
    swipe_trace(input, keyboard, &keys, "nichifanlema", true, 0);
    alarm_click(input, dock, "你吃饭了吗");
    assert(!strcmp(lv_textarea_get_text(field), "你吃饭了吗你吃饭了吗"));
    swipe_trace(input, keyboard, &keys, "nichifanlema", false, -16);
    alarm_click(input, dock, "你吃饭了吗");
    assert(!strcmp(lv_textarea_get_text(field), "你吃饭了吗你吃饭了吗你吃饭了吗"));
    swipe_trace(input, keyboard, &keys, "nichifanlema", false, 12);
    alarm_click(input, dock, "你吃饭了吗");
    assert(!strcmp(lv_textarea_get_text(field), "你吃饭了吗你吃饭了吗你吃饭了吗你吃饭了吗"));
    puts("PASS 你吃饭了吗 with centered, edge-brushing and deliberately off-center sparse continuous trajectories");
    lv_textarea_set_text(field, "");
    swipe_letters(input, keyboard, &keys, "nichilema");
    tap_key(input, actions, &action_keys, " ");
    assert(!strcmp(lv_textarea_get_text(field), "你吃了吗"));
    swipe_trace(input, keyboard, &keys, "nichiwufanlema", false, -12);
    alarm_click(input, dock, "你吃午饭了吗");
    assert(!strcmp(lv_textarea_get_text(field), "你吃了吗你吃午饭了吗"));
    swipe_letters(input, keyboard, &keys, "diyici");
    alarm_click(input, dock, "第一次");
    assert(!strcmp(lv_textarea_get_text(field), "你吃了吗你吃午饭了吗第一次"));
    puts("PASS enlarged touch keyboard: meal questions and genuine `ci` commit the expected text");
    lv_textarea_set_text(field, "你");

    capture_pixels = true;
    lv_obj_invalidate(keyboard);
    lv_refr_now(display);
    const lv_point_t head = key_center(keyboard, &keys, "o");
    const uint32_t idle_pixels = key_pixels(head);
    swipe_letters(input, keyboard, &keys, "nihao");
    assert(visible_label(dock, "你好") && !strcmp(lv_textarea_get_text(field), "你"));
    advance(32);
    lv_refr_now(display);
    const uint32_t near_pixels = key_pixels(head);
    advance(44);
    lv_refr_now(display);
    const uint32_t fade_pixels = key_pixels(head);
    assert(near_pixels != idle_pixels && fade_pixels != near_pixels && fade_pixels != idle_pixels);
    advance(180);
    lv_refr_now(display);
    assert(key_pixels(head) == idle_pixels);
    capture_pixels = false;
    alarm_click(input, dock, "你好");
    assert(!strcmp(lv_textarea_get_text(field), "你你好"));

    swipe_letters(input, keyboard, &keys, "woqu");
    assert(visible_label(dock, "woqu"));
    alarm_click(input, dock, "我");
    assert(visible_label(dock, "qu"));
    alarm_click(input, dock, "去");
    assert(!strcmp(lv_textarea_get_text(field), "你你好我去"));
    swipe_letters(input, keyboard, &keys, "woxianghecha");
    assert(visible_label(dock, "woxianghecha"));
    alarm_click(input, dock, "我想喝茶");
    assert(!strcmp(lv_textarea_get_text(field), "你你好我去我想喝茶"));
    swipe_letters(input, keyboard, &keys, "xian");
    assert(visible_label(dock, "xian"));
    alarm_click(input, dock, "西安");
    assert(!strcmp(lv_textarea_get_text(field), "你你好我去我想喝茶西安"));
    lv_textarea_set_text(field, "");
    swipe_letters(input, keyboard, &keys, "mingtian");
    swipe_letters(input, keyboard, &keys, "woxiang");
    swipe_letters(input, keyboard, &keys, "genni");
    swipe_letters(input, keyboard, &keys, "yiqi");
    swipe_letters(input, keyboard, &keys, "qu");
    swipe_letters(input, keyboard, &keys, "gongyuan");
    assert(!strcmp(lv_textarea_get_text(field), ""));
    const uint32_t note_limit = lv_textarea_get_max_length(field);
    lv_textarea_set_max_length(field, 6);
    alarm_click(input, dock, "明天我想跟你一起去公园");
    assert(visible_label(dock, "输入框空间不足") && !strcmp(lv_textarea_get_text(field), ""));
    lv_textarea_set_max_length(field, note_limit);
    alarm_click(input, dock, "明天我想跟你一起去公园");
    assert(!strcmp(lv_textarea_get_text(field), "明天我想跟你一起去公园"));
    swipe_letters(input, keyboard, &keys, "woxianghechayexiangtingyinyue");
    alarm_click(input, dock, "我想喝茶也想听音乐");
    assert(!strcmp(lv_textarea_get_text(field), "明天我想跟你一起去公园我想喝茶也想听音乐"));
    lv_textarea_set_text(field, "你你好");

    const lv_point_t n = key_center(keyboard, &keys, "n"), i = key_center(keyboard, &keys, "i");
    alarm_pointer_sample(input, n.x, n.y, true);
    slide_to(input, n, i);
    lv_indev_reset(input, NULL);
    alarm_pointer_sample(input, i.x, i.y, false);
    assert(!visible_label(dock, "ni") && !strcmp(lv_textarea_get_text(field), "你你好"));

    swipe_letters(input, keyboard, &keys, "shi");
    lv_obj_t *first = find_label(dock, "是");
    assert(first && lv_obj_is_visible(first));
    lv_obj_t *strip = lv_obj_get_parent(lv_obj_get_parent(first));
    lv_area_t strip_area;
    lv_obj_update_layout(strip);
    lv_obj_get_coords(strip, &strip_area);
    const int32_t strip_y = (strip_area.y1 + strip_area.y2) / 2;
    const lv_point_t right = {strip_area.x2 - 16, strip_y}, left = {strip_area.x1 + 24, strip_y};
    alarm_pointer_sample(input, right.x, right.y, true);
    slide_to(input, right, left);
    alarm_pointer_sample(input, left.x, left.y, false);
    assert(lv_obj_get_scroll_x(strip) > 0 && !strcmp(lv_textarea_get_text(field), "你你好"));
    tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
    assert(!visible_label(dock, "shi") && !strcmp(lv_textarea_get_text(field), "你你好"));
    tap_key(input, keyboard, &keys, "n");
    swipe_letters(input, keyboard, &keys, "shi");
    assert(visible_label(dock, "nshi"));
    tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
    assert(visible_label(dock, "n") && !strcmp(lv_textarea_get_text(field), "你你好"));
    tap_key(input, keyboard, &keys, LV_SYMBOL_BACKSPACE);
    tap_key(input, actions, &action_keys, ",");
    tap_key(input, actions, &action_keys, LV_SYMBOL_NEW_LINE);
    assert(!strcmp(lv_textarea_get_text(field), "你你好，\n"));
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED));
    assert(wait_for_caret(display, field, 0x92E7F1) >= 0);

    tap_key(input, actions, &action_keys, "1#");
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_SPECIAL);
    assert(!lv_obj_is_visible(actions));
    capture_keyboard(display, keyboard, &keys);
    tap_key(input, keyboard, &keys, "1");
    assert(!strcmp(lv_textarea_get_text(field), "你你好，\n1"));
    tap_key(input, keyboard, &keys, "abc");
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_1);
    assert(lv_obj_is_visible(actions));
    alarm_click(input, dock, "中");
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);
    capture_keyboard(display, keyboard, &keys);
    tap_key(input, keyboard, &keys, "a");
    assert(!strcmp(lv_textarea_get_text(field), "你你好，\n1a"));
    alarm_click(input, dock, "EN");
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_1);

    alarm_pointer_sample(input, n.x, n.y, true);
    slide_to(input, n, i);
    keyboard_present = true;
    advance(180);
    assert(!lv_obj_is_visible(keyboard));
    alarm_pointer_sample(input, i.x, i.y, false);
    const int32_t before = wait_for_caret(display, field, 0x92E7F1);
    for (uint32_t letter = 'a'; letter <= 'c'; ++letter) lv_obj_send_event(field, LV_EVENT_KEY, &letter);
    assert(before >= 0 && rendered_caret_x(display, field, 0x92E7F1) > before);
    keyboard_present = false;
    advance(180);
    assert(lv_obj_is_visible(keyboard) && !visible_label(dock, "ni") &&
           !strcmp(lv_textarea_get_text(field), "你你好，\n1aabc"));
    lv_obj_send_event(field, LV_EVENT_DEFOCUSED, NULL);
    assert(rendered_caret_x(display, field, 0x92E7F1) == -1);
    advance_caret(display, 1100);
    assert(sampled_caret_x(field, 0x92E7F1) == -1);
    lv_obj_remove_event_cb(keyboard, capture_key_area);
    lv_obj_remove_event_cb(actions, capture_key_area);
    lv_indev_delete(input);
    puts("PASS meteor remains after release, fades and redraws the keys without residue");
    puts("PASS expanded edge hit target and precise undo of a glide after typed Pinyin");
    puts("PASS dedicated action bar ignores cross-row drags and accepts intentional punctuation and Enter");
    puts("PASS rendered Notes caret blinks, retains focus through Pinyin and USB keypad input, then hides on defocus");
    puts("PASS 你是 and both pronunciations of 你是谁 commit as whole phrases");
    puts("PASS whole-sentence glide and multiple strokes beyond the old limit both commit full text");
    puts("PASS woqu commits in two parts and an unlisted long swipe composes 我想喝茶");
    puts("PASS real pointer Pinyin taps, single-stroke phrase, candidate scrolling and selection, touch reset, punctuation, symbols, ASCII and USB takeover");
}

static lv_obj_t *alarm_row(lv_obj_t *screen, const char *time)
{
    lv_obj_t *label = find_label(screen, time);
    assert(label);
    return lv_obj_get_parent(lv_obj_get_parent(label));
}

static lv_obj_t *alarm_editor(lv_obj_t *screen, const char *heading)
{
    lv_obj_t *title = find_label(screen, heading);
    assert(title && lv_obj_is_visible(title));
    return lv_obj_get_parent(title);
}

static lv_obj_t *alarm_control(lv_obj_t *editor, const lv_obj_class_t *type, unsigned index)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(editor); ++i) {
        lv_obj_t *child = lv_obj_get_child(editor, i);
        if (lv_obj_check_type(child, type) && index-- == 0) return child;
    }
    assert(false);
    return NULL;
}

static void alarm_set_controls(lv_obj_t *editor, unsigned h, unsigned m, unsigned tone)
{
    lv_dropdown_set_selected(alarm_control(editor, &lv_dropdown_class, 0), h);
    lv_dropdown_set_selected(alarm_control(editor, &lv_dropdown_class, 1), m);
    lv_dropdown_set_selected(alarm_control(editor, &lv_dropdown_class, 2), tone);
}

static void alarm_previous_page(app_ui_t *ui) { assert(ui); }

static void test_alarm_ui(app_ui_t *ui, lv_display_t *display)
{
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, alarm_pointer_read);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    app_open_alarm(ui);
    ui->page_back = alarm_previous_page;
    lv_obj_update_layout(ui->screen);
    assert(visible_label(ui->screen, "正在加载"));
    assert(lv_obj_has_state(lv_obj_get_parent(find_label(ui->screen, "添加")), LV_STATE_DISABLED));
    alarm_state.ready = true;
    advance(320);
    assert(visible_label(ui->screen, "暂无闹钟"));

    alarm_click(input, ui->screen, "添加");
    lv_obj_t *editor = alarm_editor(ui->screen, "新建闹钟");
    assert(lv_dropdown_get_selected(alarm_control(editor, &lv_dropdown_class, 0)) == 7);
    lv_obj_t *tone = alarm_control(editor, &lv_dropdown_class, 2);
    assert(lv_dropdown_get_option_count(tone) == APP_ALARM_TONE_COUNT);
    assert(strstr(lv_dropdown_get_options(tone), "清晨\n双响\n风铃\n急促"));
    assert(!visible_label(ui->screen, "删除"));
    alarm_set_controls(editor, 6, 30, 2);
    alarm_click(input, editor, "试听");
    assert(previews == 1 && preview_tone == 2 && visible_label(editor, "正在试听"));
    preview_error = ESP_ERR_INVALID_STATE;
    alarm_click(input, editor, "试听");
    assert(previews == 2 && visible_label(editor, "暂时无法试听"));
    preview_error = ESP_OK;
    alarm_click(input, editor, "保存");
    assert(alarm_slots[0].configured && alarm_slots[0].enabled && alarm_slots[0].hour == 6 &&
           alarm_slots[0].minute == 30 && alarm_slots[0].tone == 2 && ui->page_back == alarm_previous_page);
    assert(preview_cancels == 1);
    assert(visible_label(ui->screen, "06:30") && visible_label(ui->screen, "风铃") &&
           visible_label(ui->screen, "正在保存"));
    alarm_state.save_error = ESP_FAIL;
    advance(320);
    assert(visible_label(ui->screen, "保存失败"));
    alarm_state.save_error = ESP_OK;
    alarm_state.pending = false;
    advance(320);
    assert(visible_label(ui->screen, "1 / 6"));

    alarm_click(input, ui->screen, "添加");
    editor = alarm_editor(ui->screen, "新建闹钟");
    alarm_set_controls(editor, 7, 15, 1);
    alarm_click(input, editor, "保存");
    assert(alarm_slots[1].configured && alarm_slots[1].enabled && alarm_slots[1].tone == 1);
    alarm_tap(input, find_type(alarm_row(ui->screen, "06:30"), &lv_switch_class));
    assert(!alarm_slots[0].enabled && alarm_slots[1].enabled);
    alarm_click(input, ui->screen, "06:30");
    editor = alarm_editor(ui->screen, "编辑闹钟");
    assert(lv_dropdown_get_selected(alarm_control(editor, &lv_dropdown_class, 2)) == 2);
    assert(!lv_obj_has_state(alarm_control(editor, &lv_switch_class, 0), LV_STATE_CHECKED));
    alarm_set_controls(editor, 6, 45, 3);
    ui->page_back(ui);
    assert(alarm_slots[0].minute == 30 && ui->page_back == alarm_previous_page);
    alarm_click(input, ui->screen, "06:30");
    editor = alarm_editor(ui->screen, "编辑闹钟");
    alarm_set_controls(editor, 6, 45, 3);
    alarm_click(input, editor, "保存");
    assert(alarm_slots[0].minute == 45 && alarm_slots[0].tone == 3 && !alarm_slots[0].enabled);

    alarm_state.ringing = true;
    alarm_state.hour = 7;
    alarm_state.minute = 15;
    alarm_state.ringing_slot = 1;
    alarm_state.ringing_count = 2;
    app_utilities_alarm_ui(ui);
    assert(popup && find_label(popup, "07:15") && find_label(popup, "2 个闹钟同时响铃"));
    alarm_state.minute = 16;
    alarm_state.ringing_count = 1;
    app_utilities_alarm_ui(ui);
    assert(popup && find_label(popup, "07:16"));
    alarm_click(input, popup, "关闭");
    assert(!alarm_state.ringing);
    ui->media_active = true;
    alarm_state.ringing = true;
    app_utilities_alarm_ui(ui);
    assert(visible_label(ui->screen, LV_SYMBOL_BELL "  07:16"));
    app_alarm_dismiss();
    app_utilities_alarm_ui(ui);
    assert(!find_label(ui->screen, LV_SYMBOL_BELL "  07:16"));
    ui->media_active = false;

    alarm_click(input, ui->screen, "06:45");
    editor = alarm_editor(ui->screen, "编辑闹钟");
    alarm_click(input, editor, "删除");
    assert(popup && find_label(popup, "每天 06:45"));
    alarm_click(input, popup, "取消");
    assert(alarm_slots[0].configured);
    alarm_click(input, editor, "删除");
    alarm_click(input, popup, "确认删除");
    assert(!alarm_slots[0].configured && alarm_slots[1].configured && !visible_label(ui->screen, "06:45"));

    for (unsigned i = 0; i < APP_ALARM_SLOTS - 1; ++i) {
        alarm_click(input, ui->screen, "添加");
        editor = alarm_editor(ui->screen, "新建闹钟");
        alarm_set_controls(editor, 8 + i, 10, i % APP_ALARM_TONE_COUNT);
        alarm_click(input, editor, "保存");
    }
    lv_obj_t *add = lv_obj_get_parent(find_label(ui->screen, "添加"));
    assert(lv_obj_has_state(add, LV_STATE_DISABLED));
    alarm_tap(input, add);
    assert(!visible_label(ui->screen, "新建闹钟"));
    lv_obj_t *list = lv_obj_get_parent(alarm_row(ui->screen, "08:10"));
    lv_obj_update_layout(ui->screen);
    lv_area_t area;
    lv_obj_get_coords(list, &area);
    const int x = area.x1 + 135, bottom = area.y2 - 15;
    alarm_pointer_sample(input, x, bottom, true);
    for (int step = 1; step <= 6; ++step) alarm_pointer_sample(input, x, bottom - 30 * step, true);
    alarm_pointer_sample(input, x, bottom - 180, false);
    advance(420);
    assert(lv_obj_get_scroll_y(list) > 0 && visible_label(ui->screen, "12:10"));
    alarm_click(input, ui->screen, "12:10");
    editor = alarm_editor(ui->screen, "编辑闹钟");
    assert(lv_dropdown_get_selected(alarm_control(editor, &lv_dropdown_class, 0)) == 12);
    alarm_click(input, editor, "删除");
    assert(find_type(ui->screen, &lv_msgbox_class));
    const unsigned cancels = preview_cancels;
    app_open_alarm(ui);
    assert(!find_type(ui->screen, &lv_msgbox_class) && alarm_slots[APP_ALARM_SLOTS - 1].configured &&
           preview_cancels == cancels + 1);
    for (unsigned i = 0; i < 20; ++i) {
        app_open_alarm(ui);
        advance(40);
        assert(lv_mem_test() == LV_RESULT_OK);
    }
    lv_indev_delete(input);
    puts("PASS real pointer: six alarms, add/edit/toggle/delete, ringtone preview, scrolling, actual ring time and 20 repeat openings");
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, ui_buffer, NULL, sizeof(ui_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, ui_flush);
    app_ui_t ui = {0};
    app_open_notes(&ui);
    lv_obj_t *field = find_type(ui.screen, &lv_textarea_class);
    lv_obj_t *keyboard = find_type(ui.screen, &lv_keyboard_class);
    assert(field && keyboard && !lv_obj_is_visible(keyboard));
    test_pinyin_pointer(display, field, keyboard);
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

    test_alarm_ui(&ui, display);

    app_open_wifi(&ui);
    lv_obj_t *ap_row = find_label(ui.screen, "Test network");
    assert(ap_row && find_label(lv_obj_get_parent(ap_row), "WPA · -40 dBm"));
    lv_obj_update_layout(ui.screen);
    assert(lv_obj_get_height(lv_obj_get_parent(ap_row)) >= 44);
    lv_obj_send_event(lv_obj_get_parent(ap_row), LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(ui.screen);
    field = find_type(ui.screen, &lv_textarea_class);
    keyboard = find_type(ui.screen, &lv_keyboard_class);
    assert(field && keyboard && lv_obj_is_visible(keyboard));
    lv_area_t password_area, ascii_area;
    lv_obj_get_coords(field, &password_area);
    lv_obj_get_coords(keyboard, &ascii_area);
    assert(password_area.y2 < ascii_area.y1 && lv_area_get_height(&ascii_area) == 195);
    lv_indev_t *wifi_pointer = lv_indev_create();
    lv_indev_set_type(wifi_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(wifi_pointer, display);
    lv_indev_set_read_cb(wifi_pointer, alarm_pointer_read);
    lv_indev_set_mode(wifi_pointer, LV_INDEV_MODE_EVENT);
    alarm_tap(wifi_pointer, field);
    assert_caret_blinks(display, field, 0x106777);
    press_key(keyboard, "a");
    press_key(keyboard, LV_SYMBOL_OK);
    assert(find_label(ui.screen, "密码需要 8–63 个字符") && wifi_connections == 0);
    assert(strcmp(lv_textarea_get_text(field), "a") == 0);
    keyboard_present = true;
    advance(180);
    assert(!lv_obj_is_visible(keyboard));
    const int32_t password_caret = wait_for_caret(display, field, 0x106777);
    for (uint32_t letter = 'b'; letter <= 'h'; ++letter) lv_obj_send_event(field, LV_EVENT_KEY, &letter);
    assert(password_caret >= 0 && rendered_caret_x(display, field, 0x106777) > password_caret);
    puts("PASS rendered Wi-Fi password caret blinks and follows USB keypad input");
    lv_obj_send_event(field, LV_EVENT_READY, NULL);
    assert(wifi_connections == 1 && !find_type(ui.screen, &lv_textarea_class));
    lv_indev_delete(wifi_pointer);
    access_point.password_required = false;
    ++wifi.scan_generation;
    advance(360);
    lv_obj_send_event(lv_obj_get_parent(ap_row), LV_EVENT_CLICKED, NULL);
    assert(!find_type(ui.screen, &lv_keyboard_class));
    lv_obj_t *connect = find_label(ui.screen, "连接");
    lv_obj_send_event(lv_obj_get_parent(connect), LV_EVENT_CLICKED, NULL);
    assert(wifi_connections == 2);
    lv_obj_t *toggle = find_label(ui.screen, "关闭 Wi-Fi");
    assert(toggle);
    lv_obj_send_event(lv_obj_get_parent(toggle), LV_EVENT_CLICKED, NULL);
    advance(360);
    assert(wifi_preference_changes == 1 && find_label(ui.screen, "开启 Wi-Fi") && !wifi.enabled);
    wifi.peer_radio = true;
    toggle = find_label(ui.screen, "开启 Wi-Fi");
    lv_obj_send_event(lv_obj_get_parent(toggle), LV_EVENT_CLICKED, NULL);
    advance(360);
    assert(wifi_preference_changes == 2 && find_label(ui.screen, "关闭 Wi-Fi"));
    assert(wifi.peer_radio && !wifi.enabled && wifi.enabled_preference);
    lv_obj_t *scan = find_label(ui.screen, "扫描网络");
    assert(scan && lv_obj_has_state(lv_obj_get_parent(scan), LV_STATE_DISABLED));
    close_page(&ui);
    puts("Real LVGL Wi-Fi/keyboard, notes, calendar, weather and alarm interactions passed");
    return 0;
}
