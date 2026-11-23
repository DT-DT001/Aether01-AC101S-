#include "app_connection_style.h"
#include "app_text_input.h"
#include "app_ui_gyro_calibration.h"
#include "app_ui_modal.h"
#include "app_pc_remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REMOTE_WIDTH 404
#define MOTION_RENEW_MS 50U
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef enum {
    REMOTE_KEYBOARD, REMOTE_MOUSE, REMOTE_OBS,
} remote_view_t;

enum {
    ACTION_ENABLE = 1, ACTION_KEYBOARD, ACTION_MOUSE, ACTION_TEXT,
    ACTION_LEFT, ACTION_RIGHT, ACTION_MIDDLE, ACTION_RECENTER,
    ACTION_EDIT, ACTION_SAVE, ACTION_CANCEL,
    ACTION_CONFIRM, ACTION_REJECT, ACTION_PAIR_OFF, ACTION_CALIBRATE,
    ACTION_KEY_BASE = 32, ACTION_SCENE_BASE = 300,
};

typedef struct {
    const char *label;
    uint8_t usage;
} key_choice_t;

static const key_choice_t shortcut_keys[] = {
    {"F1",0x3a}, {"F2",0x3b}, {"F3",0x3c}, {"F4",0x3d}, {"F5",0x3e}, {"F6",0x3f},
    {"F7",0x40}, {"F8",0x41}, {"F9",0x42}, {"F10",0x43}, {"F11",0x44}, {"F12",0x45},
    {"A",0x04}, {"B",0x05}, {"C",0x06}, {"D",0x07}, {"E",0x08}, {"F",0x09}, {"G",0x0a},
    {"H",0x0b}, {"I",0x0c}, {"J",0x0d}, {"K",0x0e}, {"L",0x0f}, {"M",0x10}, {"N",0x11},
    {"O",0x12}, {"P",0x13}, {"Q",0x14}, {"R",0x15}, {"S",0x16}, {"T",0x17}, {"U",0x18},
    {"V",0x19}, {"W",0x1a}, {"X",0x1b}, {"Y",0x1c}, {"Z",0x1d},
    {"1",0x1e}, {"2",0x1f}, {"3",0x20}, {"4",0x21}, {"5",0x22}, {"6",0x23},
    {"7",0x24}, {"8",0x25}, {"9",0x26}, {"0",0x27},
    {"Enter",0x28}, {"Esc",0x29}, {"Backspace",0x2a}, {"Tab",0x2b}, {"Space",0x2c},
    {"Right",0x4f}, {"Left",0x50}, {"Down",0x51}, {"Up",0x52},
};

static const struct { const char *label; uint8_t bit; } modifiers[] = {
    {"Ctrl",0x01}, {"Alt",0x04}, {"Shift",0x02}, {"Win",0x08},
};

typedef struct {
    app_ui_t *ui;
    remote_view_t view;
    app_ui_gyro_calibration_t calibration;
    lv_obj_t *state, *hint, *toggle, *pair_box, *pair_error;
    uint32_t state_color, pair_code, notice_tick, motion_tick;
    bool pair_replied, motion_contact, motion_held;
    char notice[96];
    lv_obj_t *tabs[2], *keyboard_panel, *mouse_panel;
    lv_obj_t *text, *keyboard, *send, *direct_keys[8];
    lv_obj_t *move, *mouse_hint, *mouse_buttons[3], *recenter, *calibrate;
    lv_obj_t *edit, *instructions, *grid, *editor, *edit_title, *key_selector;
    lv_obj_t *scenes[APP_PC_REMOTE_SCENES], *bindings[APP_PC_REMOTE_SCENES];
    lv_obj_t *mod_buttons[ARRAY_COUNT(modifiers)];
    bool edit_select, editor_open;
    unsigned edit_slot;
} remote_page_t;

static void page_cleanup(app_ui_t *ui);
static void button_clicked(lv_event_t *event);
static void refresh(remote_page_t *page);

static remote_page_t *current_page(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || ui->page_cleanup != page_cleanup ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != ui->screen) return NULL;
    return ui->page_context;
}

static void set_enabled(lv_obj_t *object, bool enabled)
{
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void indicate_ready(lv_obj_t *object, bool ready)
{
    /* Cached UI refresh must not gate the start of a touch. The event
     * handler checks the live connection or IMU state before sending. */
    if (ready) lv_obj_remove_state(object, LV_STATE_USER_1);
    else lv_obj_add_state(object, LV_STATE_USER_1);
}

static void set_visible(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void set_checked(lv_obj_t *object, bool checked)
{
    if (checked) lv_obj_add_state(object, LV_STATE_CHECKED);
    else lv_obj_remove_state(object, LV_STATE_CHECKED);
}

static lv_obj_t *panel(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(object, 0, y);
    lv_obj_set_size(object, REMOTE_WIDTH, height);
    return object;
}

static lv_obj_t *button(remote_page_t *page, lv_obj_t *parent, const char *text,
                        int x, int y, int width, int height, uintptr_t action)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    app_connection_button_style(object, action == ACTION_ENABLE || action == ACTION_TEXT || action == ACTION_SAVE);
    lv_obj_set_style_bg_color(object, lv_color_hex(CONNECTION_BLUE), LV_STATE_CHECKED);
    lv_obj_set_style_opa(object, LV_OPA_60, LV_STATE_USER_1);
    if (action) {
        lv_obj_set_user_data(object, (void *)action);
        lv_obj_add_event_cb(object, button_clicked, LV_EVENT_CLICKED, page->ui);
    }
    const bool arrow = action >= ACTION_KEY_BASE + 0x4f && action <= ACTION_KEY_BASE + 0x52;
    app_ui_add_text(object, text, LV_ALIGN_CENTER, 0, 0, CONNECTION_TEXT,
                    arrow ? &lv_font_montserrat_16 : app_ui_chinese_font());
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int y, int width, int height, uint32_t color)
{
    lv_obj_t *object = app_ui_add_text(parent, text, LV_ALIGN_TOP_LEFT, 0, y, color, app_ui_chinese_font());
    lv_obj_set_size(object, width, height);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

static void show_result(remote_page_t *page, esp_err_t error, const char *message)
{
    if (error != ESP_OK)
        snprintf(page->notice, sizeof(page->notice), "操作失败 %s", esp_err_to_name(error));
    else snprintf(page->notice, sizeof(page->notice), "%s", message ? message : "");
    page->notice_tick = lv_tick_get();
}

static bool can_send(const app_pc_remote_status_t *status)
{
    return status->requested && status->connected && !status->confirm_pending;
}

static bool require_link(remote_page_t *page, bool ready)
{
    if (!ready) show_result(page, ESP_OK, "电脑尚未连接");
    return ready;
}

static void stop_motion(remote_page_t *page)
{
    if (!page->motion_held) return;
    page->motion_held = false;
    app_pc_remote_set_motion(false);
}

static void release_motion(remote_page_t *page)
{
    page->motion_contact = false;
    stop_motion(page);
}

static void motion_event(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (!page || page->view != REMOTE_MOUSE) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        release_motion(page);
        return;
    }
    if (code == LV_EVENT_PRESSED) page->motion_contact = true;
    else if (!page->motion_contact || lv_tick_elaps(page->motion_tick) < MOTION_RENEW_MS) return;
    page->motion_tick = lv_tick_get();
    app_pc_remote_status_t status;
    app_pc_remote_get_status(&status);
    if (!can_send(&status) || !status.gyro_ready || !status.gyro_calibrated) {
        stop_motion(page);
        return;
    }
    page->motion_held = true;
    app_pc_remote_set_motion(true);
}

static void remote_view(remote_page_t *page, remote_view_t view)
{
    if (page->view == view) return;
    app_ui_gyro_calibration_close(&page->calibration);
    release_motion(page);
    page->view = view;
    app_pc_remote_set_air_mode(view == REMOTE_MOUSE);
    set_visible(page->keyboard_panel, view == REMOTE_KEYBOARD);
    set_visible(page->mouse_panel, view == REMOTE_MOUSE);
    set_checked(page->tabs[0], view == REMOTE_KEYBOARD);
    set_checked(page->tabs[1], view == REMOTE_MOUSE);
}

static void send_text(remote_page_t *page)
{
    const char *text = lv_textarea_get_text(page->text);
    if (!text[0]) {
        show_result(page, ESP_OK, "请先输入文本");
        return;
    }
    const esp_err_t error = app_pc_remote_text(text);
    show_result(page, error, "文本已提交");
    if (error == ESP_OK) lv_textarea_set_text(page->text, "");
}

static void keyboard_ready(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (!page || page->view != REMOTE_KEYBOARD) return;
    app_pc_remote_status_t status;
    app_pc_remote_get_status(&status);
    if (can_send(&status)) send_text(page);
    refresh(page);
}

static void keyboard_visibility(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (!page || page->view != REMOTE_KEYBOARD) return;
    const bool visible = lv_event_get_code(event) != LV_EVENT_CANCEL;
    set_visible(page->keyboard, visible);
    show_result(page, ESP_OK, visible ? NULL : "点击文本框显示键盘");
    refresh(page);
}

static void build_keyboard(remote_page_t *page, lv_obj_t *content)
{
    static const char accepted[] =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_\140abcdefghijklmnopqrstuvwxyz{|}~";
    static const key_choice_t direct[] = {
        {"Esc",0x29}, {"Tab",0x2b}, {"退格",0x2a}, {LV_SYMBOL_LEFT,0x50},
        {LV_SYMBOL_UP,0x52}, {LV_SYMBOL_DOWN,0x51}, {LV_SYMBOL_RIGHT,0x4f}, {"回车",0x28},
    };
    page->keyboard_panel = panel(content, 108, 242);
    lv_obj_t *parent = page->keyboard_panel;
    page->text = lv_textarea_create(parent);
    lv_textarea_set_one_line(page->text, true);
    lv_obj_set_size(page->text, 286, 44);
    lv_textarea_set_accepted_chars(page->text, accepted);
    lv_textarea_set_max_length(page->text, APP_PC_REMOTE_TEXT_MAX);
    lv_textarea_set_placeholder_text(page->text, "英文、数字和符号");
    lv_obj_set_style_text_font(page->text, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_color(page->text, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_color(page->text, lv_color_hex(0x9CB2D2), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(page->text, lv_color_hex(CONNECTION_PANEL), 0);
    app_text_input_style_cursor(page->text, 0x92E7F1);
    lv_obj_set_style_border_color(page->text, lv_color_hex(CONNECTION_BORDER), 0);
    lv_obj_set_style_radius(page->text, 12, 0);
    page->send = button(page, parent, "发送文本", 296, 0, 108, 44, ACTION_TEXT);
    lv_obj_t *keyboard = lv_keyboard_create(parent);
    page->keyboard = keyboard;
    lv_obj_set_size(keyboard, REMOTE_WIDTH, 140);
    lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 0, 48);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(keyboard, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keyboard, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(keyboard, 7, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(CONNECTION_CONTROL), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(CONNECTION_BLUE), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(0xEAF2FF), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_keyboard_set_textarea(keyboard, page->text);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    /* The keyboard emits READY on the textarea for both Enter and OK. */
    lv_obj_add_event_cb(page->text, keyboard_ready, LV_EVENT_READY, page->ui);
    lv_obj_add_event_cb(page->text, keyboard_visibility, LV_EVENT_CANCEL, page->ui);
    lv_obj_add_event_cb(page->text, keyboard_visibility, LV_EVENT_CLICKED, page->ui);
    for (size_t i = 0; i < ARRAY_COUNT(direct); ++i)
        page->direct_keys[i] = button(page, parent, direct[i].label, (int)i * 51, 196, 47, 44,
                                      ACTION_KEY_BASE + direct[i].usage);
}

static void build_mouse(remote_page_t *page, lv_obj_t *content)
{
    page->mouse_panel = panel(content, 108, 242);
    lv_obj_t *parent = page->mouse_panel;
    page->move = button(page, parent, "按住移动", 0, 0, REMOTE_WIDTH, 96, 0);
    lv_obj_set_style_bg_color(page->move, lv_color_hex(0x1D4563), 0);
    lv_obj_set_style_border_color(page->move, lv_color_hex(0x376884), 0);
    lv_obj_set_style_border_width(page->move, 1, 0);
    lv_obj_t *move_text = lv_obj_get_child(page->move, 0);
    lv_obj_set_style_text_font(move_text, app_ui_chinese_font_size(24), 0);
    lv_obj_align(move_text, LV_ALIGN_CENTER, 0, 16);
    app_ui_add_text(page->move, LV_SYMBOL_GPS, LV_ALIGN_CENTER, 0, -18,
                    CONNECTION_ACCENT, &lv_font_montserrat_16);
    lv_obj_remove_flag(page->move, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(page->move, motion_event, LV_EVENT_PRESSED, page->ui);
    lv_obj_add_event_cb(page->move, motion_event, LV_EVENT_PRESSING, page->ui);
    lv_obj_add_event_cb(page->move, motion_event, LV_EVENT_RELEASED, page->ui);
    lv_obj_add_event_cb(page->move, motion_event, LV_EVENT_PRESS_LOST, page->ui);
    lv_obj_add_event_cb(page->move, motion_event, LV_EVENT_INDEV_RESET, page->ui);
    page->mouse_hint = label(parent, "", 106, REMOTE_WIDTH, 38, 0xAFC6E9);
    page->mouse_buttons[0] = button(page, parent, "左键", 0, 146, 128, 44, ACTION_LEFT);
    page->mouse_buttons[1] = button(page, parent, "右键", 138, 146, 128, 44, ACTION_RIGHT);
    page->mouse_buttons[2] = button(page, parent, "中键", 276, 146, 128, 44, ACTION_MIDDLE);
    page->recenter = button(page, parent, "回中", 0, 198, 196, 44, ACTION_RECENTER);
    page->calibrate = button(page, parent, "漂移校准", 208, 198, 196, 44, ACTION_CALIBRATE);
    set_visible(parent, false);
}

static size_t key_index(uint8_t usage)
{
    for (size_t i = 0; i < ARRAY_COUNT(shortcut_keys); ++i)
        if (shortcut_keys[i].usage == usage) return i;
    return ARRAY_COUNT(shortcut_keys);
}

static void format_shortcut(app_pc_shortcut_t shortcut, char *text, size_t size)
{
    const size_t index = key_index(shortcut.key);
    if (index == ARRAY_COUNT(shortcut_keys)) {
        snprintf(text, size, "待配置");
        return;
    }
    const uint8_t bits = shortcut.modifiers | (shortcut.modifiers >> 4);
    snprintf(text, size, "%s%s%s%s%s", bits & 1 ? "Ctrl+" : "", bits & 4 ? "Alt+" : "",
             bits & 2 ? "Shift+" : "", bits & 8 ? "Win+" : "", shortcut_keys[index].label);
}

static void refresh_obs_layout(remote_page_t *page)
{
    set_visible(page->grid, !page->editor_open);
    set_visible(page->editor, page->editor_open);
    app_ui_label_text(lv_obj_get_child(page->edit, 0), page->edit_select ? "退出编辑" : "编辑快捷键");
    app_ui_label_text(page->instructions, page->editor_open ? "选择组合键" : page->edit_select ?
                      "请选择场景" : "先在 OBS 绑定相同快捷键");
}

static void open_editor(remote_page_t *page, unsigned slot)
{
    app_pc_shortcut_t shortcut = {0};
    const esp_err_t error = app_pc_remote_get_shortcut(slot, &shortcut);
    if (error != ESP_OK) { show_result(page, error, NULL); return; }
    page->edit_slot = slot;
    page->editor_open = true;
    app_ui_label_fmt(page->edit_title, "场景 %u 快捷键", slot + 1);
    const uint8_t bits = shortcut.modifiers | (shortcut.modifiers >> 4);
    for (size_t i = 0; i < ARRAY_COUNT(modifiers); ++i)
        set_checked(page->mod_buttons[i], (bits & modifiers[i].bit) != 0);
    const size_t index = key_index(shortcut.key);
    lv_dropdown_set_selected(page->key_selector, index < ARRAY_COUNT(shortcut_keys) ? (uint32_t)index : 0);
    refresh_obs_layout(page);
}

static void close_editor(remote_page_t *page)
{
    lv_dropdown_close(page->key_selector);
    page->editor_open = false;
    page->edit_select = false;
    refresh_obs_layout(page);
}

static void save_shortcut(remote_page_t *page)
{
    if (!page->editor_open) return;
    const uint32_t index = lv_dropdown_get_selected(page->key_selector);
    if (index >= ARRAY_COUNT(shortcut_keys)) return;
    app_pc_shortcut_t shortcut = {.key = shortcut_keys[index].usage};
    for (size_t i = 0; i < ARRAY_COUNT(modifiers); ++i)
        if (lv_obj_has_state(page->mod_buttons[i], LV_STATE_CHECKED)) shortcut.modifiers |= modifiers[i].bit;
    const esp_err_t error = app_pc_remote_set_shortcut(page->edit_slot, shortcut);
    show_result(page, error, "快捷键已更新");
    if (error == ESP_OK) close_editor(page);
}

static void build_obs(remote_page_t *page, lv_obj_t *content)
{
    page->instructions = label(content, "", 63, 270, 24, CONNECTION_DETAIL);
    page->edit = button(page, content, "编辑快捷键", 282, 50, 122, 44, ACTION_EDIT);
    page->grid = panel(content, 102, 246);
    static const uint32_t colors[] = {0x8AC9FF, 0x7FDFCE, 0xBAAFF8, 0x95C2CE};
    for (unsigned i = 0; i < APP_PC_REMOTE_SCENES; ++i) {
        /* Physical order is left top/bottom, then right top/bottom. */
        char scene[24];
        snprintf(scene, sizeof(scene), "场景 %u", i + 1);
        lv_obj_t *card = button(page, page->grid, scene, (int)(i / 2) * 208, (int)(i % 2) * 126,
                                196, 116, ACTION_SCENE_BASE + i);
        page->scenes[i] = card;
        lv_obj_set_style_bg_color(card, lv_color_hex(CONNECTION_PANEL), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(CONNECTION_BORDER), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        app_ui_add_text(card, LV_SYMBOL_VIDEO, LV_ALIGN_TOP_MID, 0, 12,
                        colors[i], &lv_font_montserrat_16);
        lv_obj_t *title = lv_obj_get_child(card, 0);
        lv_obj_set_style_text_font(title, app_ui_chinese_font_size(24), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);
        page->bindings[i] = app_ui_add_text(card, "待配置", LV_ALIGN_BOTTOM_MID, 0, -13,
                                            CONNECTION_DETAIL, app_ui_chinese_font());
        lv_obj_set_width(page->bindings[i], 180);
        lv_obj_set_style_text_align(page->bindings[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(page->bindings[i], LV_LABEL_LONG_WRAP);
    }
    page->editor = panel(content, 102, 246);
    lv_obj_t *parent = page->editor;
    page->edit_title = app_ui_add_text(parent, "", LV_ALIGN_TOP_LEFT, 0, 0,
                                       0xEAF2FF, app_ui_chinese_font_size(24));
    for (size_t i = 0; i < ARRAY_COUNT(modifiers); ++i) {
        page->mod_buttons[i] = button(page, parent, modifiers[i].label, (int)i * 103, 42, 95, 44, 0);
        lv_obj_add_flag(page->mod_buttons[i], LV_OBJ_FLAG_CHECKABLE);
    }
    page->key_selector = lv_dropdown_create(parent);
    lv_obj_set_pos(page->key_selector, 0, 100);
    lv_obj_set_size(page->key_selector, REMOTE_WIDTH, 44);
    char options[384] = {0};
    size_t offset = 0;
    for (size_t i = 0; i < ARRAY_COUNT(shortcut_keys); ++i) {
        const int written = snprintf(options + offset, sizeof(options) - offset, "%s%s",
                                     i ? "\n" : "", shortcut_keys[i].label);
        if (written < 0 || (size_t)written >= sizeof(options) - offset) break;
        offset += (size_t)written;
    }
    lv_dropdown_set_options(page->key_selector, options);
    lv_obj_set_style_text_font(page->key_selector, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_font(page->key_selector, &lv_font_montserrat_16, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(page->key_selector, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_bg_color(page->key_selector, lv_color_hex(0x18243A), 0);
    lv_obj_t *list = lv_dropdown_get_list(page->key_selector);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x18243A), 0);
    button(page, parent, "保存快捷键", 0, 162, 196, 44, ACTION_SAVE);
    button(page, parent, "取消", 208, 162, 196, 44, ACTION_CANCEL);
    refresh_obs_layout(page);
}

static void close_pairing(remote_page_t *page)
{
    if (!page->pair_box) return;
    lv_msgbox_close(page->pair_box);
    page->pair_box = NULL;
    page->pair_error = NULL;
}

static void pairing_action(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (!page || !page->pair_box) return;
    const uintptr_t action = (uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    app_pc_remote_status_t status;
    app_pc_remote_get_status(&status);
    if (action != ACTION_PAIR_OFF && (!status.requested || !status.confirm_pending ||
                                      status.confirm_code != page->pair_code)) {
        refresh(page);
        return;
    }
    const esp_err_t error = action == ACTION_PAIR_OFF ? app_pc_remote_set_enabled(false) :
        app_pc_remote_confirm(action == ACTION_CONFIRM);
    if (error == ESP_OK) {
        page->pair_replied = true;
        close_pairing(page);
        refresh(page);
    } else app_ui_label_fmt(page->pair_error, "操作失败 %s", esp_err_to_name(error));
}

static void refresh_pairing(remote_page_t *page, const app_pc_remote_status_t *status)
{
    if (!status->requested || !status->confirm_pending) {
        page->pair_replied = false;
        close_pairing(page);
        return;
    }
    if (status->confirm_code != page->pair_code) {
        close_pairing(page);
        page->pair_replied = false;
    }
    if (page->pair_box || page->pair_replied) return;
    page->pair_code = status->confirm_code;
    release_motion(page);
    if (page->editor_open) lv_dropdown_close(page->key_selector);
    lv_obj_t *box = app_ui_modal_msgbox(page->ui->screen);
    if (!box) return;
    page->pair_box = box;
    lv_obj_set_style_bg_color(lv_obj_get_parent(box), lv_color_hex(0x05080E), 0);
    lv_obj_set_style_bg_opa(lv_obj_get_parent(box), LV_OPA_80, 0);
    lv_obj_set_width(box, 404);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x18243A), 0);
    lv_obj_set_style_text_color(box, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(box, app_ui_chinese_font(), 0);
    lv_obj_t *title = lv_msgbox_add_title(box, "确认电脑配对");
    lv_obj_set_style_bg_color(lv_obj_get_parent(title), lv_color_hex(0x243955), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xEAF2FF), 0);
    lv_msgbox_add_text(box, "请核对电脑上显示的配对数字。");
    char number[16];
    snprintf(number, sizeof(number), "%06lu", (unsigned long)page->pair_code);
    lv_obj_t *code = lv_msgbox_add_text(box, number);
    lv_obj_set_style_text_font(code, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
    page->pair_error = lv_msgbox_add_text(box, "仅在数字一致时确认。");
    static const char *const choices[] = {"确认相同", "拒绝配对", "关闭键鼠"};
    static const uintptr_t actions[] = {ACTION_CONFIRM, ACTION_REJECT, ACTION_PAIR_OFF};
    lv_obj_t *reject = NULL;
    for (size_t i = 0; i < ARRAY_COUNT(choices); ++i) {
        lv_obj_t *object = lv_msgbox_add_footer_button(box, choices[i]);
        lv_obj_set_size(object, 112, 44);
        lv_obj_set_style_text_font(object, app_ui_chinese_font(), 0);
        lv_obj_set_style_text_color(object, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_user_data(object, (void *)actions[i]);
        lv_obj_add_event_cb(object, pairing_action, LV_EVENT_CLICKED, page->ui);
        if (actions[i] == ACTION_REJECT) reject = object;
    }
    lv_obj_center(box);
    app_ui_modal_focus(box, reject);
}

static void refresh(remote_page_t *page)
{
    app_ui_gyro_calibration_refresh(&page->calibration);
    app_pc_remote_status_t status;
    app_pc_remote_get_status(&status);
    const bool ready = can_send(&status);
    const char *state = !status.available ? "蓝牙键鼠不可用" : !status.requested ? "蓝牙键鼠已关闭" :
        status.confirm_pending ? (page->pair_replied ? "正在确认配对…" : "请核对配对码") :
        status.connected ? "设备已连接" : status.reconnecting ? "正在重连设备…" :
        status.discoverable ? "等待设备配对" : "正在开启蓝牙…";
    app_ui_label_text(page->state, state);
    const uint32_t color = ready ? 0x8FE0BA : status.confirm_pending ? 0xE8C388 : 0xD9E8FF;
    if (page->state_color != color) {
        page->state_color = color;
        lv_obj_set_style_text_color(page->state, lv_color_hex(color), 0);
    }
    app_ui_label_text(lv_obj_get_child(page->toggle, 0), status.requested ? "关闭键鼠" : "开启键鼠");
    set_enabled(page->toggle, status.available || status.requested);
    if (page->view == REMOTE_OBS && status.save_error != ESP_OK)
        app_ui_label_fmt(page->hint, "快捷键保存失败 %s", esp_err_to_name(status.save_error));
    else if (page->view == REMOTE_OBS && status.save_pending)
        app_ui_label_text(page->hint, "正在保存快捷键…");
    else if (page->notice[0] && lv_tick_elaps(page->notice_tick) < 2800)
        app_ui_label_text(page->hint, page->notice);
    else if (status.reconnecting) app_ui_label_text(page->hint, "请保持对方蓝牙开启");
    else if (status.last_error != ESP_OK)
        app_ui_label_fmt(page->hint, "蓝牙 %s", esp_err_to_name(status.last_error));
    else if (ready && page->view == REMOTE_KEYBOARD)
        app_ui_label_text(page->hint, "使用美式键盘布局");
    else if (ready || !status.requested) app_ui_label_text(page->hint, "");
    else app_ui_label_text(page->hint, "在对方蓝牙列表选 Aether01");

    if (page->view == REMOTE_OBS) {
        for (unsigned i = 0; i < APP_PC_REMOTE_SCENES; ++i) {
            app_pc_shortcut_t shortcut = {0};
            char binding[48];
            (void)app_pc_remote_get_shortcut(i, &shortcut);
            format_shortcut(shortcut, binding, sizeof(binding));
            app_ui_label_text(page->bindings[i], binding);
            indicate_ready(page->scenes[i], ready || page->edit_select);
        }
    } else {
        indicate_ready(page->send, ready);
        for (size_t i = 0; i < ARRAY_COUNT(page->direct_keys); ++i) indicate_ready(page->direct_keys[i], ready);
        for (size_t i = 0; i < ARRAY_COUNT(page->mouse_buttons); ++i) indicate_ready(page->mouse_buttons[i], ready);
        const bool can_move = ready && status.gyro_ready && status.gyro_calibrated;
        if (!can_move) stop_motion(page);
        indicate_ready(page->move, can_move);
        indicate_ready(page->recenter, ready && status.gyro_ready);
        indicate_ready(page->calibrate, status.gyro_ready);
        app_ui_label_text(page->mouse_hint, !status.gyro_ready ? "未检测到陀螺仪，仍可使用鼠标按键。" :
            !ready ? "连接电脑后可移动鼠标。" : !status.gyro_calibrated ? "平放设备，或点漂移校准" :
            page->motion_held ? "松手停止" : "按住后转动设备");
    }
    if (!page->calibration.box) refresh_pairing(page, &status);
}

static void button_clicked(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (!page) return;
    const uintptr_t action = (uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    app_pc_remote_status_t status;
    app_pc_remote_get_status(&status);
    const bool ready = can_send(&status);
    if (action == ACTION_ENABLE) {
        if (status.requested) release_motion(page);
        show_result(page, app_pc_remote_set_enabled(!status.requested), NULL);
    } else if (status.confirm_pending) return;
    else if (action == ACTION_KEYBOARD || action == ACTION_MOUSE) {
        remote_view(page, action == ACTION_KEYBOARD ? REMOTE_KEYBOARD : REMOTE_MOUSE);
    } else if (action == ACTION_TEXT && page->view == REMOTE_KEYBOARD) {
        if (require_link(page, ready)) send_text(page);
    } else if (action == ACTION_CALIBRATE && page->view == REMOTE_MOUSE) {
        release_motion(page);
        app_ui_gyro_calibration_open(page->ui->screen, &page->calibration);
    } else if (action == ACTION_RECENTER && page->view == REMOTE_MOUSE) {
        if (require_link(page, ready)) {
            if (status.gyro_ready) {
                release_motion(page);
                app_pc_remote_recenter();
                show_result(page, ESP_OK, "已回中，请保持设备静止");
            } else show_result(page, ESP_OK, "未检测到陀螺仪");
        }
    } else if (page->view == REMOTE_MOUSE && action >= ACTION_LEFT && action <= ACTION_MIDDLE) {
        if (require_link(page, ready))
            show_result(page, app_pc_remote_click((uint8_t)(1U << (action - ACTION_LEFT))), "点击已提交");
    } else if (page->view == REMOTE_OBS && action == ACTION_EDIT) {
        if (page->edit_select) close_editor(page);
        else { page->edit_select = true; refresh_obs_layout(page); }
    } else if (page->view == REMOTE_OBS && action == ACTION_SAVE) save_shortcut(page);
    else if (page->view == REMOTE_OBS && action == ACTION_CANCEL) close_editor(page);
    else if (page->view == REMOTE_OBS && !page->editor_open && action >= ACTION_SCENE_BASE &&
             action < ACTION_SCENE_BASE + APP_PC_REMOTE_SCENES) {
        const unsigned slot = (unsigned)(action - ACTION_SCENE_BASE);
        if (page->edit_select) open_editor(page, slot);
        else if (require_link(page, ready)) show_result(page, app_pc_remote_trigger(slot), "快捷键已提交");
    } else if (page->view == REMOTE_KEYBOARD && action >= ACTION_KEY_BASE &&
               action <= ACTION_KEY_BASE + UINT8_MAX) {
        if (require_link(page, ready))
            show_result(page, app_pc_remote_key(0, (uint8_t)(action - ACTION_KEY_BASE)), NULL);
    }
    refresh(page);
}

static void page_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    if (ui->page_cleanup == page_cleanup && ui->page_context) refresh(ui->page_context);
}

static void page_cleanup(app_ui_t *ui)
{
    remote_page_t *page = ui->page_context;
    if (!page) return;
    app_ui_gyro_calibration_close(&page->calibration);
    release_motion(page);
    app_pc_remote_release_controls();
    close_pairing(page);
    free(page);
    ui->page_context = NULL;
}

static void open_page(app_ui_t *ui, remote_view_t view, const char *title)
{
    lv_obj_t *content = app_ui_begin_page(ui, title);
    remote_page_t *page = calloc(1, sizeof(*page));
    if (!page) { label(content, "内存不足", 0, REMOTE_WIDTH, 24, 0xFFAAAA); return; }
    page->ui = ui;
    page->view = view;
    ui->page_context = page;
    ui->page_cleanup = page_cleanup;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *status_card = lv_obj_create(content);
    lv_obj_set_size(status_card, REMOTE_WIDTH, 48);
    app_connection_card_style(status_card);
    app_ui_add_text(status_card, LV_SYMBOL_BLUETOOTH, LV_ALIGN_LEFT_MID, 10, 0,
                    CONNECTION_ACCENT, &lv_font_montserrat_16);
    page->state = label(status_card, "", 1, 240, 21, CONNECTION_TEXT);
    page->hint = label(status_card, "", 24, 240, 20, CONNECTION_DETAIL);
    lv_obj_set_x(page->state, 34);
    lv_obj_set_x(page->hint, 34);
    page->toggle = button(page, content, "开启键鼠", 280, 2, 122, 44, ACTION_ENABLE);
    if (view == REMOTE_OBS) build_obs(page, content);
    else {
        page->tabs[0] = button(page, content, "键盘", 0, 54, 196, 44, ACTION_KEYBOARD);
        page->tabs[1] = button(page, content, "空气鼠标", 208, 54, 196, 44, ACTION_MOUSE);
        lv_obj_align(lv_obj_get_child(page->tabs[0], 0), LV_ALIGN_CENTER, 10, 0);
        lv_obj_align(lv_obj_get_child(page->tabs[1], 0), LV_ALIGN_CENTER, 10, 0);
        app_ui_add_text(page->tabs[0], LV_SYMBOL_KEYBOARD, LV_ALIGN_CENTER, -30, 0,
                        CONNECTION_ACCENT, &lv_font_montserrat_16);
        app_ui_add_text(page->tabs[1], LV_SYMBOL_GPS, LV_ALIGN_CENTER, -43, 0,
                        CONNECTION_ACCENT, &lv_font_montserrat_16);
        set_checked(page->tabs[0], true);
        build_keyboard(page, content);
        build_mouse(page, content);
    }
    app_ui_set_timer(ui, page_timer, 150);
    refresh(page);
}

void app_open_pc_remote(app_ui_t *ui) { open_page(ui, REMOTE_KEYBOARD, "电脑键鼠"); }
void app_open_obs_switcher(app_ui_t *ui) { open_page(ui, REMOTE_OBS, "OBS 切换台"); }
