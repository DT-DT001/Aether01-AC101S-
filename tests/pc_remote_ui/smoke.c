#include "app_pc_remote.h"
#include "app_gyro_calibration.h"
#include "app_ui_internal.h"
#include "app_ui_modal.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern const lv_font_t font_16, font_24;
static uint16_t frame[480 * 480], buffer[480 * 20];
static app_pc_remote_status_t status = {.available=true};
static app_pc_shortcut_t shortcuts[] = {{5,0x42},{5,0x43},{5,0x44},{5,0x45}};
static bool pressed, air_mode, moving, confirm_value;
static lv_point_t pointer;
static unsigned confirmations, leaves, motion_on, motion_off, recenters;
static unsigned calibration_cancels;
static app_gyro_calibration_status_t calibration;
static unsigned keys, clicks, text_requests, triggers, saves, last_slot;
static uint8_t last_key, last_click;
static esp_err_t text_error;
static char sent[APP_PC_REMOTE_TEXT_MAX + 1];

esp_err_t app_telemetry_gyro_calibration_begin(void)
{
    calibration = (app_gyro_calibration_status_t){
        .phase = APP_GYRO_CAL_COUNTDOWN, .remaining_ms = 2000,
    };
    return ESP_OK;
}

esp_err_t app_telemetry_gyro_calibration_cancel(void)
{
    if (calibration.phase != APP_GYRO_CAL_COUNTDOWN && calibration.phase != APP_GYRO_CAL_SAMPLING)
        return ESP_ERR_INVALID_STATE;
    calibration.phase = APP_GYRO_CAL_CANCELLED;
    ++calibration_cancels;
    return ESP_OK;
}

void app_telemetry_gyro_calibration_get(app_gyro_calibration_status_t *out) { *out = calibration; }

void app_pc_remote_get_status(app_pc_remote_status_t *out) { *out = status; }
esp_err_t app_pc_remote_set_enabled(bool enabled)
{
    status.requested = status.discoverable = enabled;
    if (!enabled) status.connected = status.confirm_pending = moving = false;
    return ESP_OK;
}
esp_err_t app_pc_remote_confirm(bool accept)
{
    assert(status.requested && status.confirm_pending);
    ++confirmations;
    confirm_value = accept;
    return ESP_OK; /* The worker can clear pending on a later refresh. */
}
esp_err_t app_pc_remote_key(uint8_t modifiers, uint8_t key)
{
    assert(status.requested && status.connected && !status.confirm_pending && modifiers == 0);
    ++keys;
    last_key = key;
    return ESP_OK;
}
esp_err_t app_pc_remote_text(const char *ascii)
{
    assert(status.connected && strlen(ascii) <= APP_PC_REMOTE_TEXT_MAX);
    ++text_requests;
    if (text_error) return text_error;
    snprintf(sent, sizeof(sent), "%s", ascii);
    return ESP_OK;
}
esp_err_t app_pc_remote_click(uint8_t buttons)
{
    assert(status.connected && air_mode);
    ++clicks;
    last_click = buttons;
    return ESP_OK;
}
void app_pc_remote_set_air_mode(bool enabled) { air_mode = enabled; moving = false; }
void app_pc_remote_set_motion(bool enabled)
{
    if (enabled) { assert(air_mode && status.connected && status.gyro_calibrated); ++motion_on; }
    else ++motion_off;
    moving = enabled;
}
void app_pc_remote_recenter(void) { ++recenters; moving = false; }
void app_pc_remote_leave(void)
{
    ++leaves;
    air_mode = moving = false;
    app_pc_remote_set_enabled(false);
}
esp_err_t app_pc_remote_get_shortcut(unsigned slot, app_pc_shortcut_t *out)
{
    assert(slot < APP_PC_REMOTE_SCENES && out);
    *out = shortcuts[slot];
    return ESP_OK;
}
esp_err_t app_pc_remote_set_shortcut(unsigned slot, app_pc_shortcut_t value)
{
    assert(slot < APP_PC_REMOTE_SCENES);
    ++saves;
    last_slot = slot;
    shortcuts[slot] = value;
    status.save_pending = true;
    return ESP_OK;
}
esp_err_t app_pc_remote_trigger(unsigned slot)
{
    assert(status.connected && !status.confirm_pending && slot < APP_PC_REMOTE_SCENES);
    ++triggers;
    last_slot = slot;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t error) { return error ? "ESP_FAIL" : "ESP_OK"; }
esp_err_t app_usb_input_modal_open(lv_obj_t *modal, lv_obj_t *preferred)
{
    assert(modal && preferred);
    const char *label = lv_label_get_text(lv_obj_get_child(preferred, 0));
    assert(!strcmp(label, "拒绝配对") || !strcmp(label, "取消"));
    return ESP_OK;
}
const lv_font_t *app_ui_chinese_font_size(unsigned px)
{
    static lv_font_t small, large;
    small = font_16; large = font_24;
    small.fallback = &lv_font_montserrat_16; large.fallback = &lv_font_montserrat_24;
    return px >= 24 ? &large : &small;
}
const lv_font_t *app_ui_chinese_font(void) { return app_ui_chinese_font_size(16); }
void app_ui_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    app_ui_label_text(label, text);
}
lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text, lv_align_t align, int32_t x,
                          int32_t y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, x, y);
    return label;
}
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    if (ui->page_cleanup) {
        void (*cleanup)(app_ui_t *) = ui->page_cleanup;
        ui->page_cleanup = NULL;
        cleanup(ui);
    }
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = NULL; }
    ui->screen = lv_obj_create(NULL);
    lv_obj_remove_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x070A12), 0);
    lv_obj_set_style_text_font(ui->screen, app_ui_chinese_font(), 0);
    app_ui_add_text(ui->screen, title, LV_ALIGN_TOP_MID, 0, 20, 0xEAF2FF, app_ui_chinese_font_size(24));
    ui->content = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->content, 448, 394);
    lv_obj_align(ui->content, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(ui->content, 24, 0);
    lv_obj_set_style_bg_color(ui->content, lv_color_hex(0x10192A), 0);
    lv_obj_set_style_pad_all(ui->content, 20, 0);
    lv_obj_set_style_border_width(ui->content, 0, 0);
    lv_screen_load_anim(ui->screen, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    return ui->content;
}
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t cb, uint32_t period)
{ ui->timer = lv_timer_create(cb, period, ui); }

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) { lv_tick_inc(1); lv_timer_handler(); }
}
static void input(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = pointer;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 480 && y >= 0 && y < 480);
            frame[y * 480 + x] = *source++;
        }
    lv_display_flush_ready(display);
}
static void snapshot(lv_display_t *display, const char *directory, const char *name)
{
    lv_refr_now(display);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n480 480\n255\n");
    for (unsigned i = 0; i < 480 * 480; ++i) {
        const uint16_t pixel = frame[i];
        const uint8_t rgb[] = {((pixel>>11)&31)*255/31, ((pixel>>5)&63)*255/63, (pixel&31)*255/31};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(!fclose(file));
}
static lv_obj_t *find_label(lv_obj_t *parent, const char *text)
{
    if (lv_obj_check_type(parent, &lv_label_class) && !strcmp(lv_label_get_text(parent), text)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(parent, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_class(lv_obj_t *parent, const lv_obj_class_t *type)
{
    if (lv_obj_check_type(parent, type)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find_class(lv_obj_get_child(parent, (int32_t)i), type);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *control(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = find_label(parent, text);
    assert(label);
    return lv_obj_get_parent(label);
}
static void touch(lv_obj_t *object)
{
    lv_obj_update_layout(lv_obj_get_screen(object));
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    pointer = (lv_point_t){.x=(area.x1+area.x2)/2, .y=(area.y1+area.y2)/2};
    pressed = true;
    advance(90);
}
static void lift(void) { pressed = false; advance(90); }
static void tap(lv_obj_t *object) { touch(object); lift(); }

static bool text_caret_drawn(lv_display_t *display, lv_obj_t *field)
{
    lv_refr_now(display);
    lv_obj_t *label = lv_textarea_get_label(field);
    lv_area_t area;
    lv_obj_get_coords(label, &area);
    lv_point_t glyph;
    lv_label_get_letter_pos(label, lv_textarea_get_cursor_pos(field), &glyph);
    const uint16_t color = lv_color_to_u16(lv_color_hex(0x92E7F1));
    const int x = area.x1 + glyph.x;
    const int y = area.y1 + glyph.y;
    assert(x >= 5 && x + 5 < 480 && y >= 0 && y + 14 < 480);
    unsigned columns = 0;
    for (int dx = -5; dx <= 5; ++dx) {
        unsigned matches = 0;
        for (int dy = 4; dy <= 14; ++dy)
            matches += frame[(y + dy) * 480 + x + dx] == color;
        if (matches >= 8) ++columns;
    }
    assert(columns == 0 || columns == 3);
    return columns == 3;
}

static void assert_text_caret_blinks(lv_display_t *display, lv_obj_t *field)
{
    bool shown = false, hidden = false;
    /* Keystrokes can reposition or restart the caret during either phase. */
    for (unsigned elapsed = 0; elapsed <= 1100; elapsed += 100) {
        assert(lv_obj_has_state(field, LV_STATE_FOCUSED));
        if (text_caret_drawn(display, field)) shown = true;
        else hidden = true;
        if (shown && hidden) return;
        advance(100);
    }
    assert(shown && hidden);
}

static void keyboard_key(lv_obj_t *keyboard, const char *name)
{
    for (uint32_t i = 0;; ++i) {
        const char *label = lv_buttonmatrix_get_button_text(keyboard, i);
        assert(label);
        if (strcmp(label, name)) continue;
        lv_buttonmatrix_set_selected_button(keyboard, i);
        lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setbuf(stdout, NULL);
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_read_cb(indev, input);
    app_ui_t ui = {0};
    app_open_pc_remote(&ui);
    advance(170);
    assert(lv_obj_has_state(control(ui.screen, "发送文本"), LV_STATE_USER_1));
    tap(control(ui.screen, "开启蓝牙"));
    assert(status.requested && find_label(ui.screen, "等待电脑配对"));
    status.reconnecting = true;
    advance(170);
    assert(find_label(ui.screen, "正在重连电脑…"));
    assert(lv_obj_has_state(control(ui.screen, "发送文本"), LV_STATE_USER_1));
    status.reconnecting = false;
    status.confirm_pending = true; status.confirm_code = 12345;
    advance(170);
    lv_obj_t *box = find_class(ui.screen, &lv_msgbox_class);
    assert(box && find_label(box, "012345"));
    snapshot(display, argv[1], "pc-pairing");
    lv_obj_t *old_confirm = control(box, "确认相同");
    status.confirm_code = 23456;
    lv_obj_send_event(old_confirm, LV_EVENT_CLICKED, NULL);
    box = find_class(ui.screen, &lv_msgbox_class);
    assert(!confirmations && box && find_label(box, "023456"));
    tap(control(box, "拒绝配对"));
    assert(confirmations == 1 && !confirm_value && !find_class(ui.screen, &lv_msgbox_class));
    status.confirm_pending = false; advance(170);
    status.confirm_pending = true; status.confirm_code = 34567; advance(170);
    tap(control(find_class(ui.screen, &lv_msgbox_class), "关闭蓝牙"));
    assert(!status.requested && !find_class(ui.screen, &lv_msgbox_class));
    tap(control(ui.screen, "开启蓝牙"));
    status.confirm_pending = true; status.confirm_code = 45678; advance(170);
    tap(control(find_class(ui.screen, &lv_msgbox_class), "确认相同"));
    assert(confirmations == 2 && confirm_value && !find_class(ui.screen, &lv_msgbox_class));
    status.confirm_pending = false; status.connected = true; advance(170);
    puts("PASS pairing code, stale reply rejection, explicit confirm/reject and modal OFF");

    lv_obj_t *text = find_class(ui.screen, &lv_textarea_class);
    lv_obj_t *keyboard = find_class(ui.screen, &lv_keyboard_class);
    assert(text && keyboard && find_label(ui.screen, "电脑已连接"));
    lv_obj_update_layout(ui.screen);
    lv_area_t field_area, keyboard_area, key_area;
    lv_obj_get_coords(text, &field_area);
    lv_obj_get_coords(keyboard, &keyboard_area);
    lv_obj_get_coords(control(ui.screen, "回车"), &key_area);
    assert(field_area.y2 < keyboard_area.y1 && keyboard_area.y2 < key_area.y1);
    tap(text);
    assert(lv_obj_has_state(text, LV_STATE_FOCUSED) && text_caret_drawn(display, text));
    assert(lv_obj_get_style_anim_duration(text, LV_PART_CURSOR) == 500);
    advance(450);
    assert(lv_obj_has_state(text, LV_STATE_FOCUSED) && !text_caret_drawn(display, text));
    advance(500);
    assert(lv_obj_has_state(text, LV_STATE_FOCUSED) && text_caret_drawn(display, text));
    /* The first row's 52 units place q at unit 7, after the 5-unit mode key. */
    pointer = (lv_point_t){keyboard_area.x1 + lv_area_get_width(&keyboard_area) * 7 / 52,
                           keyboard_area.y1 + lv_area_get_height(&keyboard_area) / 8};
    pressed = true;
    advance(90);
    lift();
    assert(!strcmp(lv_textarea_get_text(text), "q") && lv_textarea_get_cursor_pos(text) == 1);
    assert_text_caret_blinks(display, text);
    lv_textarea_add_text(text, "Z1!中文");
    assert(!strcmp(lv_textarea_get_text(text), "qZ1!"));
    snapshot(display, argv[1], "pc-keyboard");
    text_error = ESP_FAIL;
    tap(control(ui.screen, "发送文本"));
    assert(text_requests == 1 && !strcmp(lv_textarea_get_text(text), "qZ1!"));
    assert(!lv_obj_has_state(text, LV_STATE_FOCUSED) && !text_caret_drawn(display, text));
    advance(650);
    assert(!text_caret_drawn(display, text));
    text_error = ESP_OK;
    tap(control(ui.screen, "发送文本"));
    assert(text_requests == 2 && !strcmp(sent, "qZ1!") && !lv_textarea_get_text(text)[0]);
    keyboard_key(keyboard, "x");
    keyboard_key(keyboard, LV_SYMBOL_OK);
    assert(text_requests == 3 && !strcmp(sent, "x"));
    keyboard_key(keyboard, "y");
    keyboard_key(keyboard, LV_SYMBOL_NEW_LINE);
    assert(text_requests == 4 && !strcmp(sent, "y"));
    keyboard_key(keyboard, LV_SYMBOL_KEYBOARD);
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    tap(text);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_state(text, LV_STATE_FOCUSED) && text_caret_drawn(display, text));
    tap(control(ui.screen, "回车"));
    assert(keys == 1 && last_key == 0x28);
    puts("PASS actual LVGL keyboard input, ASCII filter, Enter/OK once, dismiss/restore, retained text on error and direct Enter");
    puts("PASS PC Remote caret blinks on/off/on and after typing, stays hidden when Send takes focus, and returns on touching the field");

    tap(control(ui.screen, "空气鼠标"));
    lv_obj_t *move = control(ui.screen, "按住移动");
    assert(air_mode && lv_obj_has_state(move, LV_STATE_USER_1));
    assert(find_label(ui.screen, "未检测到陀螺仪，仍可使用鼠标按键。"));
    tap(control(ui.screen, "左键"));
    assert(clicks == 1 && last_click == 1);
    tap(control(ui.screen, "右键"));
    assert(clicks == 2 && last_click == 2);
    status.gyro_ready = true; advance(170);
    assert(lv_obj_has_state(move, LV_STATE_USER_1));
    assert(find_label(ui.screen, "平放设备，或点漂移校准"));
    status.connected = false;
    advance(170);
    tap(control(ui.screen, "漂移校准"));
    assert(app_ui_modal_present(ui.screen) && find_label(ui.screen, "屏幕朝上平放\n2 秒后开始"));
    calibration.phase = APP_GYRO_CAL_SAMPLING;
    calibration.remaining_ms = 5000;
    advance(170);
    assert(find_label(ui.screen, "保持静止\n还需 5 秒") && !moving);
    tap(control(ui.screen, "取消"));
    assert(calibration_cancels == 1 && !app_ui_modal_present(ui.screen));
    tap(control(ui.screen, "漂移校准"));
    calibration.phase = APP_GYRO_CAL_SAVING;
    advance(170);
    assert(find_label(ui.screen, "正在保存校准") && lv_obj_has_state(control(ui.screen, "关闭"), LV_STATE_DISABLED));
    calibration.phase = APP_GYRO_CAL_COMPLETE;
    advance(170);
    assert(find_label(ui.screen, "校准完成，补偿已保存。"));
    tap(control(ui.screen, "关闭"));
    assert(calibration_cancels == 1 && !app_ui_modal_present(ui.screen));
    tap(control(ui.screen, "漂移校准"));
    calibration.phase = APP_GYRO_CAL_FAILED;
    calibration.error = ESP_ERR_INVALID_RESPONSE;
    advance(170);
    assert(find_label(ui.screen, "请平放设备并保持静止，再试一次。"));
    calibration.error = ESP_ERR_INVALID_STATE;
    advance(170);
    assert(find_label(ui.screen, "设备忙，请稍后重试。"));
    tap(control(ui.screen, "关闭"));
    status.connected = true;
    advance(170);
    tap(control(ui.screen, "回中"));
    assert(recenters == 1);
    touch(move);
    assert(!moving);
    status.gyro_calibrated = true; advance(70);
    assert(moving);
    advance(130);
    assert(moving && motion_on >= 2);
    snapshot(display, argv[1], "pc-air-mouse");
    lift();
    assert(!moving && motion_off == 1);
    touch(move);
    pointer = (lv_point_t){.x=470, .y=470}; advance(90);
    assert(!moving && motion_off == 2);
    lift();
    touch(move);
    assert(moving);
    pressed = false;
    lv_indev_reset(indev, NULL);
    assert(!moving && motion_off == 3);
    advance(90);
    touch(move);
    assert(moving);
    app_open_obs_switcher(&ui);
    lift();
    assert(leaves == 1 && !moving && !air_mode && !status.requested && !triggers);
    puts("PASS air mode, missing/fresh gyro, hold renewal, release/press lost, input reset and navigation stop");

    lv_obj_t *scenes[4];
    lv_area_t area[4];
    for (unsigned i = 0; i < 4; ++i) {
        char name[24]; snprintf(name, sizeof(name), "场景 %u", i+1);
        scenes[i] = control(ui.screen, name);
        lv_obj_get_coords(scenes[i], &area[i]);
        assert(lv_obj_has_state(scenes[i], LV_STATE_USER_1));
    }
    assert(area[0].x1 == area[1].x1 && area[2].x1 == area[3].x1 && area[0].x1 < area[2].x1);
    assert(area[0].y1 == area[2].y1 && area[1].y1 == area[3].y1 && area[0].y1 < area[1].y1);
    tap(control(ui.screen, "编辑快捷键"));
    tap(scenes[1]);
    assert(!triggers && find_label(ui.screen, "场景 2 快捷键"));
    tap(control(ui.screen, "Ctrl"));
    tap(control(ui.screen, "Shift"));
    lv_obj_t *selector = find_class(ui.screen, &lv_dropdown_class);
    const int32_t f4 = lv_dropdown_get_option_index(selector, "F4");
    assert(f4 >= 0);
    lv_dropdown_set_selected(selector, (uint32_t)f4);
    snapshot(display, argv[1], "obs-edit");
    tap(control(ui.screen, "保存快捷键"));
    assert(saves == 1 && last_slot == 1 && shortcuts[1].modifiers == 6 && shortcuts[1].key == 0x3d);
    assert(find_label(ui.screen, "Alt+Shift+F4") && find_label(ui.screen, "正在保存快捷键…"));
    status.save_pending = false; status.save_error = ESP_FAIL; advance(170);
    assert(find_label(ui.screen, "快捷键保存失败 ESP_FAIL"));
    status.save_error = ESP_OK;
    tap(scenes[0]);
    assert(!triggers && find_label(ui.screen, "电脑尚未连接"));
    tap(control(ui.screen, "开启蓝牙"));
    touch(scenes[0]);
    assert(!triggers);
    status.connected = true;
    lift();
    assert(triggers == 1 && last_slot == 0);
    advance(160);
    snapshot(display, argv[1], "obs-switcher");
    for (unsigned i = 0; i < 4; ++i) { tap(scenes[i]); assert(last_slot == i && triggers == i+2); }
    assert(find_label(ui.screen, "快捷键已提交"));
    app_ui_begin_page(&ui, "首页");
    advance(170);
    assert(leaves == 2 && !status.requested && !app_ui_modal_present(ui.screen));
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_indev_delete(indev);
    lv_display_delete(display);
    lv_deinit();
    puts("PASS OBS quadrant order, offline binding editor, save/error feedback, real triggers and cleanup");
}
