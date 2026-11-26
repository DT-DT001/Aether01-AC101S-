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
    REMOTE_MOUSE, REMOTE_OBS, REMOTE_MOUSE_SETTINGS,
} remote_view_t;

enum {
    ACTION_ENABLE = 1,
    ACTION_LEFT, ACTION_RIGHT, ACTION_MIDDLE, ACTION_RECENTER,
    ACTION_EDIT, ACTION_SAVE, ACTION_CANCEL,
    ACTION_CONFIRM, ACTION_REJECT, ACTION_PAIR_OFF, ACTION_CALIBRATE,
    ACTION_MOUSE_SETTINGS, ACTION_MOUSE_SLOWER, ACTION_MOUSE_DEFAULT, ACTION_MOUSE_FASTER,
    ACTION_SHORTCUT_BASE = 32, ACTION_SCENE_BASE = 64,
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

static const struct { const char *label; app_pc_shortcut_t binding; } mouse_shortcuts[] = {
    {"回车", {0, 0x28}}, {"Esc", {0, 0x29}}, {"空格", {0, 0x2c}}, {"切应用", {0x04, 0x2b}},
};

typedef struct {
    app_ui_t *ui;
    remote_view_t view;
    app_ui_gyro_calibration_t calibration;
    lv_obj_t *state, *hint, *toggle, *pair_box, *pair_error;
    uint32_t state_color, pair_code, notice_tick, motion_tick;
    bool pair_replied, motion_contact, motion_held;
    char notice[96];
    lv_obj_t *direct_keys[ARRAY_COUNT(mouse_shortcuts)];
    lv_obj_t *move, *mouse_hint, *mouse_buttons[3], *recenter, *calibrate;
    lv_obj_t *sensitivity, *slower, *restore_speed, *faster;
    lv_obj_t *edit, *instructions, *grid, *editor, *edit_title, *key_selector;
    lv_obj_t *scenes[APP_PC_REMOTE_SCENES], *bindings[APP_PC_REMOTE_SCENES];
    lv_obj_t *mod_buttons[ARRAY_COUNT(modifiers)];
    lv_obj_t *rename_box, *rename_field, *rename_error;
    bool edit_select, editor_open;
    unsigned edit_slot, rename_slot;
} remote_page_t;

static void page_cleanup(app_ui_t *ui);
static void button_clicked(lv_event_t *event);
static void refresh(remote_page_t *page);
static void open_mouse_settings(app_ui_t *ui);

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
    app_connection_button_style(object, action == ACTION_ENABLE || action == ACTION_SAVE);
    lv_obj_set_style_bg_color(object, lv_color_hex(CONNECTION_BLUE), LV_STATE_CHECKED);
    lv_obj_set_style_opa(object, LV_OPA_60, LV_STATE_USER_1);
    if (action) {
        lv_obj_set_user_data(object, (void *)action);
        const lv_event_code_t click = action >= ACTION_SCENE_BASE ? LV_EVENT_SHORT_CLICKED : LV_EVENT_CLICKED;
        lv_obj_add_event_cb(object, button_clicked, click, page->ui);
    }
    app_ui_add_text(object, text, LV_ALIGN_CENTER, 0, 0, CONNECTION_TEXT, app_ui_chinese_font());
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

static void build_mouse(remote_page_t *page, lv_obj_t *content)
{
    lv_obj_t *parent = panel(content, 54, 296);
    page->move = button(page, parent, "按住移动", 0, 0, REMOTE_WIDTH, 110, 0);
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
    page->mouse_hint = label(parent, "", 115, REMOTE_WIDTH, 25, 0xAFC6E9);
    page->mouse_buttons[0] = button(page, parent, "左键", 0, 141, 128, 46, ACTION_LEFT);
    page->mouse_buttons[1] = button(page, parent, "右键", 138, 141, 128, 46, ACTION_RIGHT);
    page->mouse_buttons[2] = button(page, parent, "中键", 276, 141, 128, 46, ACTION_MIDDLE);
    for (size_t i = 0; i < ARRAY_COUNT(mouse_shortcuts); ++i)
        page->direct_keys[i] = button(page, parent, mouse_shortcuts[i].label, (int)i * 103, 195, 95, 44,
                                      ACTION_SHORTCUT_BASE + i);
    page->recenter = button(page, parent, "回中", 0, 247, 196, 46, ACTION_RECENTER);
    (void)button(page, parent, "配置", 208, 247, 196, 46, ACTION_MOUSE_SETTINGS);
}

static void build_mouse_settings(remote_page_t *page, lv_obj_t *content)
{
    lv_obj_t *card = lv_obj_create(content);
    lv_obj_set_pos(card, 0, 57);
    lv_obj_set_size(card, REMOTE_WIDTH, 178);
    app_connection_card_style(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    app_ui_add_text(card, "灵敏度", LV_ALIGN_TOP_MID, 0, 10, CONNECTION_TEXT, app_ui_chinese_font_size(24));
    page->sensitivity = app_ui_add_text(card, "", LV_ALIGN_TOP_MID, 0, 47,
                                        CONNECTION_ACCENT, &lv_font_montserrat_28);
    page->slower = button(page, card, "-", 14, 98, 94, 56, ACTION_MOUSE_SLOWER);
    page->restore_speed = button(page, card, "恢复默认", 119, 98, 166, 56, ACTION_MOUSE_DEFAULT);
    page->faster = button(page, card, "+", 296, 98, 94, 56, ACTION_MOUSE_FASTER);
    lv_obj_set_style_text_font(lv_obj_get_child(page->slower, 0), &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(page->faster, 0), &lv_font_montserrat_28, 0);
    page->calibrate = button(page, content, "漂移校准", 0, 245, REMOTE_WIDTH, 56, ACTION_CALIBRATE);
    page->mouse_hint = label(content, "", 315, REMOTE_WIDTH, 28, CONNECTION_DETAIL);
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
                      "请选择场景" : "长按场景名称修改");
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

static void update_scene_name(remote_page_t *page, unsigned slot)
{
    char name[APP_PC_REMOTE_SCENE_BYTES];
    if (app_pc_remote_get_scene_name(slot, name) == ESP_OK)
        app_ui_label_text(lv_obj_get_child(page->scenes[slot], 0), name);
}

static void rename_deleted(lv_event_t *event)
{
    remote_page_t *page = lv_event_get_user_data(event);
    page->rename_box = page->rename_field = page->rename_error = NULL;
}

static void rename_cancel(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (page && page->rename_box) lv_msgbox_close_async(page->rename_box);
}

static void rename_changed(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (page && page->rename_error) app_ui_label_text(page->rename_error, "");
}

static void rename_save(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (!page || !page->rename_box) return;
    const esp_err_t error = app_pc_remote_set_scene_name(page->rename_slot,
                                                        lv_textarea_get_text(page->rename_field));
    if (error != ESP_OK) {
        if (error == ESP_ERR_INVALID_ARG) app_ui_label_text(page->rename_error, "请输入 1 至 16 个字符");
        else app_ui_label_fmt(page->rename_error, "保存失败 %s", esp_err_to_name(error));
        return;
    }
    update_scene_name(page, page->rename_slot);
    show_result(page, ESP_OK, "场景名已修改");
    lv_msgbox_close_async(page->rename_box);
    refresh(page);
}

static void rename_layout(void *context, bool keyboard_visible)
{
    remote_page_t *page = context;
    if (page->rename_box)
        lv_obj_align(page->rename_box, LV_ALIGN_CENTER, 0, keyboard_visible ? -128 : 0);
}

static void rename_scene(lv_event_t *event)
{
    remote_page_t *page = current_page(event);
    if (!page || page->view != REMOTE_OBS || page->rename_box || page->pair_box || page->editor_open) return;
    const uintptr_t action = (uintptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(event));
    const unsigned slot = (unsigned)(action - ACTION_SCENE_BASE);
    char name[APP_PC_REMOTE_SCENE_BYTES];
    if (app_pc_remote_get_scene_name(slot, name) != ESP_OK) return;
    lv_obj_t *box = app_ui_modal_msgbox(page->ui->screen);
    if (!box) { show_result(page, ESP_ERR_NO_MEM, NULL); return; }
    page->rename_box = box;
    page->rename_slot = slot;
    lv_obj_add_event_cb(box, rename_deleted, LV_EVENT_DELETE, page);
    lv_obj_set_style_bg_color(lv_obj_get_parent(box), lv_color_hex(0x05080E), 0);
    lv_obj_set_style_bg_opa(lv_obj_get_parent(box), LV_OPA_80, 0);
    lv_obj_set_width(box, 424);
    lv_obj_set_style_bg_color(box, lv_color_hex(CONNECTION_PANEL), 0);
    lv_obj_set_style_text_color(box, lv_color_hex(CONNECTION_TEXT), 0);
    lv_obj_set_style_text_font(box, app_ui_chinese_font(), 0);
    lv_obj_t *title = lv_msgbox_add_title(box, "重命名场景");
    lv_obj_set_style_text_font(title, app_ui_chinese_font_size(24), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(CONNECTION_TEXT), 0);
    lv_obj_set_style_bg_color(lv_obj_get_parent(title), lv_color_hex(CONNECTION_CONTROL), 0);
    lv_obj_t *field = lv_textarea_create(lv_msgbox_get_content(box));
    page->rename_field = field;
    lv_obj_set_size(field, LV_PCT(100), 52);
    lv_obj_set_style_text_font(field, app_ui_chinese_font_size(24), 0);
    app_connection_button_style(field, false);
    lv_obj_set_style_pad_hor(field, 12, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(CONNECTION_TEXT), 0);
    app_text_input_style_cursor(field, 0x92E7F1);
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_max_length(field, APP_PC_REMOTE_SCENE_CHARACTERS);
    lv_textarea_set_text(field, name);
    page->rename_error = lv_msgbox_add_text(box, "");
    lv_obj_set_style_text_color(page->rename_error, lv_color_hex(0xFFC28A), 0);
    lv_obj_add_event_cb(field, rename_changed, LV_EVENT_VALUE_CHANGED, page->ui);
    lv_obj_add_event_cb(field, rename_save, LV_EVENT_READY, page->ui);
    lv_obj_add_event_cb(field, rename_cancel, LV_EVENT_CANCEL, page->ui);
    lv_obj_t *cancel = lv_msgbox_add_footer_button(box, "取消");
    lv_obj_set_size(cancel, 170, 46);
    app_connection_button_style(cancel, false);
    lv_obj_add_event_cb(cancel, rename_cancel, LV_EVENT_CLICKED, page->ui);
    lv_obj_t *save = lv_msgbox_add_footer_button(box, "保存名称");
    lv_obj_set_size(save, 170, 46);
    app_connection_button_style(save, true);
    lv_obj_add_event_cb(save, rename_save, LV_EVENT_CLICKED, page->ui);
    lv_obj_center(box);
    lv_obj_t *dock = app_text_input_create(lv_obj_get_parent(box), field, true, rename_layout, page);
    if (dock) app_text_input_show(dock, true);
    else app_ui_label_text(page->rename_error, "屏幕键盘不可用");
    app_ui_modal_focus(box, field);
    if (!lv_obj_has_state(field, LV_STATE_FOCUSED)) lv_obj_send_event(field, LV_EVENT_FOCUSED, NULL);
    lv_textarea_set_cursor_pos(field, LV_TEXTAREA_CURSOR_LAST);
}

static void build_obs(remote_page_t *page, lv_obj_t *content)
{
    page->instructions = label(content, "", 63, 270, 24, CONNECTION_DETAIL);
    page->edit = button(page, content, "编辑快捷键", 282, 50, 122, 44, ACTION_EDIT);
    page->grid = panel(content, 98, 252);
    static const uint32_t colors[] = {0x8AC9FF, 0x7FDFCE, 0xBAAFF8, 0x95C2CE};
    for (unsigned i = 0; i < APP_PC_REMOTE_SCENES; ++i) {
        /* Physical order is left top/bottom, then right top/bottom. */
        lv_obj_t *card = button(page, page->grid, "", (int)(i / 2) * 206, (int)(i % 2) * 128,
                                198, 124, ACTION_SCENE_BASE + i);
        page->scenes[i] = card;
        lv_obj_add_event_cb(card, rename_scene, LV_EVENT_LONG_PRESSED, page->ui);
        lv_obj_set_style_bg_color(card, lv_color_hex(CONNECTION_PANEL), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(CONNECTION_BORDER), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        app_ui_add_text(card, LV_SYMBOL_VIDEO, LV_ALIGN_TOP_MID, 0, 12,
                        colors[i], &lv_font_montserrat_16);
        lv_obj_t *title = lv_obj_get_child(card, 0);
        lv_obj_set_style_text_font(title, app_ui_chinese_font_size(24), 0);
        lv_obj_set_size(title, 180, 34);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 39);
        update_scene_name(page, i);
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
    if (page->view != REMOTE_MOUSE && status.save_error != ESP_OK)
        app_ui_label_text(page->hint, "保存失败，稍后重试");
    else if (page->view != REMOTE_MOUSE && status.save_pending)
        app_ui_label_text(page->hint, page->view == REMOTE_OBS ? "正在保存场景…" : "正在保存设置…");
    else if (page->notice[0] && lv_tick_elaps(page->notice_tick) < 2800)
        app_ui_label_text(page->hint, page->notice);
    else if (status.reconnecting) app_ui_label_text(page->hint, "请保持对方蓝牙开启");
    else if (status.last_error != ESP_OK)
        app_ui_label_fmt(page->hint, "蓝牙 %s", esp_err_to_name(status.last_error));
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
    } else if (page->view == REMOTE_MOUSE) {
        for (size_t i = 0; i < ARRAY_COUNT(page->direct_keys); ++i) indicate_ready(page->direct_keys[i], ready);
        for (size_t i = 0; i < ARRAY_COUNT(page->mouse_buttons); ++i) indicate_ready(page->mouse_buttons[i], ready);
        const bool can_move = ready && status.gyro_ready && status.gyro_calibrated;
        if (!can_move) stop_motion(page);
        indicate_ready(page->move, can_move);
        indicate_ready(page->recenter, ready && status.gyro_ready);
        app_ui_label_text(page->mouse_hint, !status.gyro_ready ? "未检测到陀螺仪，仍可使用鼠标按键。" :
            !ready ? "连接电脑后可移动鼠标。" : !status.gyro_calibrated ? "平放设备，或在配置中校准" :
            page->motion_held ? "松手停止" : "按住后转动设备");
    } else {
        app_ui_label_fmt(page->sensitivity, "%u%%", (unsigned)status.sensitivity);
        set_enabled(page->slower, status.sensitivity > APP_PC_MOUSE_MIN);
        set_enabled(page->faster, status.sensitivity < APP_PC_MOUSE_MAX);
        set_enabled(page->restore_speed, status.sensitivity != APP_PC_MOUSE_DEFAULT);
        set_enabled(page->calibrate, status.gyro_ready);
        app_ui_label_text(page->mouse_hint, status.gyro_ready ? "设备平放，静止时校准" : "未检测到陀螺仪");
    }
    if (!page->calibration.box && !page->rename_box) refresh_pairing(page, &status);
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
    else if (action == ACTION_MOUSE_SETTINGS && page->view == REMOTE_MOUSE) {
        open_mouse_settings(page->ui);
        return;
    } else if (action == ACTION_CALIBRATE && page->view == REMOTE_MOUSE_SETTINGS) {
        if (!status.gyro_ready) return;
        app_ui_gyro_calibration_open(page->ui->screen, &page->calibration);
    } else if (page->view == REMOTE_MOUSE_SETTINGS && action >= ACTION_MOUSE_SLOWER && action <= ACTION_MOUSE_FASTER) {
        unsigned percent = APP_PC_MOUSE_DEFAULT;
        if (action == ACTION_MOUSE_SLOWER)
            percent = LV_MAX(APP_PC_MOUSE_MIN, (int)status.sensitivity - APP_PC_MOUSE_STEP);
        else if (action == ACTION_MOUSE_FASTER)
            percent = LV_MIN(APP_PC_MOUSE_MAX, (int)status.sensitivity + APP_PC_MOUSE_STEP);
        show_result(page, app_pc_remote_set_sensitivity(percent), NULL);
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
    } else if (page->view == REMOTE_MOUSE && action >= ACTION_SHORTCUT_BASE &&
               action < ACTION_SHORTCUT_BASE + ARRAY_COUNT(mouse_shortcuts)) {
        if (require_link(page, ready)) {
            const app_pc_shortcut_t binding = mouse_shortcuts[action - ACTION_SHORTCUT_BASE].binding;
            show_result(page, app_pc_remote_key(binding.modifiers, binding.key), NULL);
        }
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
    if (page->rename_box) lv_msgbox_close(page->rename_box);
    free(page);
    ui->page_context = NULL;
}

static void page_suspend(app_ui_t *ui, bool suspended)
{
    remote_page_t *page = ui->page_context;
    if (suspended) {
        app_ui_gyro_calibration_close(&page->calibration);
        release_motion(page);
        app_pc_remote_release_controls();
    } else if (page->view == REMOTE_MOUSE) app_pc_remote_set_air_mode(true);
}

static void open_page(app_ui_t *ui, remote_view_t view, const char *title)
{
    lv_obj_t *content = app_ui_begin_page(ui, title);
    if (view == REMOTE_MOUSE_SETTINGS) ui->page_back = app_open_pc_remote;
    remote_page_t *page = calloc(1, sizeof(*page));
    if (!page) { label(content, "内存不足", 0, REMOTE_WIDTH, 24, 0xFFAAAA); return; }
    page->ui = ui;
    page->view = view;
    ui->page_context = page;
    ui->page_cleanup = page_cleanup;
    ui->page_suspend = page_suspend;
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
    else if (view == REMOTE_MOUSE) {
        build_mouse(page, content);
        app_pc_remote_set_air_mode(true);
    } else build_mouse_settings(page, content);
    app_ui_set_timer(ui, page_timer, 150);
    refresh(page);
}

void app_open_pc_remote(app_ui_t *ui) { open_page(ui, REMOTE_MOUSE, "空中飞鼠"); }
void app_open_obs_switcher(app_ui_t *ui) { open_page(ui, REMOTE_OBS, "OBS 切换台"); }
static void open_mouse_settings(app_ui_t *ui) { open_page(ui, REMOTE_MOUSE_SETTINGS, "飞鼠配置"); }
