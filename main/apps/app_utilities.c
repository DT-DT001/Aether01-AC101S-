#include "app_utilities.h"
#include "app_ui_internal.h"
#include "app_ui_modal.h"
#include "app_text_input.h"
#include "app_telemetry.h"
#include "app_tools_math.h"
#include "aether_wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *utility_button(lv_obj_t *parent, const char *label,
                                int x, int y, int width, lv_event_cb_t event, void *context)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 44);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x29486F), 0);
    lv_obj_add_event_cb(button, event, LV_EVENT_CLICKED, context);
    app_ui_add_text(button, label, LV_ALIGN_CENTER, 0, 0, 0xF0F5FF, app_ui_chinese_font());
    return button;
}

static void page_free(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
}

typedef struct alarm_page alarm_page_t;

typedef struct {
    alarm_page_t *page;
    unsigned slot;
    lv_obj_t *body, *time, *tone, *enabled;
} alarm_row_t;

struct alarm_page {
    app_ui_t *ui;
    void (*list_back)(app_ui_t *);
    lv_obj_t *list_view, *editor, *add, *empty, *list_status;
    lv_obj_t *edit_title, *hour, *minute, *tone, *enabled, *remove, *edit_status;
    lv_obj_t *delete_dialog;
    alarm_row_t rows[APP_ALARM_SLOTS];
    unsigned editing_slot;
    bool editing;
    esp_err_t list_error;
};

static void alarm_show(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void alarm_switch_set(lv_obj_t *object, bool enabled)
{
    if (enabled) lv_obj_add_state(object, LV_STATE_CHECKED);
    else lv_obj_remove_state(object, LV_STATE_CHECKED);
}

static void alarm_refresh_list(alarm_page_t *page)
{
    app_alarm_snapshot_t alarm;
    app_alarm_get(&alarm);
    unsigned count = 0;
    bool free_slot = false;
    for (unsigned i = 0; i < APP_ALARM_SLOTS; ++i) {
        alarm_row_t *row = &page->rows[i];
        app_alarm_slot_t slot;
        const bool available = alarm.ready && app_alarm_get_slot(i, &slot) == ESP_OK;
        const bool configured = available && slot.configured;
        alarm_show(row->body, configured);
        if (!configured) {
            free_slot |= available;
            continue;
        }
        ++count;
        app_ui_label_fmt(row->time, "%02u:%02u", slot.hour, slot.minute);
        app_ui_label_text(row->tone, app_alarm_tone_name(slot.tone));
        alarm_switch_set(row->enabled, slot.enabled);
    }
    alarm_show(page->empty, alarm.ready && count == 0);
    if (free_slot) lv_obj_remove_state(page->add, LV_STATE_DISABLED);
    else lv_obj_add_state(page->add, LV_STATE_DISABLED);
    if (!alarm.ready) app_ui_label_text(page->list_status, "正在加载");
    else if (page->list_error != ESP_OK || alarm.save_error != ESP_OK)
        app_ui_label_text(page->list_status, "保存失败");
    else if (alarm.pending) app_ui_label_text(page->list_status, "正在保存");
    else app_ui_label_fmt(page->list_status, "%u / %u", count, (unsigned)APP_ALARM_SLOTS);
}

static void alarm_update(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    alarm_page_t *page = ui->page_context;
    if (page && !page->editing) alarm_refresh_list(page);
}

static void alarm_close_lists(alarm_page_t *page)
{
    lv_dropdown_close(page->hour);
    lv_dropdown_close(page->minute);
    lv_dropdown_close(page->tone);
}

static void alarm_editor_back(app_ui_t *ui)
{
    alarm_page_t *page = ui->page_context;
    if (page->delete_dialog) {
        lv_msgbox_close(page->delete_dialog);
        return;
    }
    app_alarm_cancel_preview();
    alarm_close_lists(page);
    page->editing = false;
    ui->page_back = page->list_back;
    alarm_show(page->editor, false);
    alarm_show(page->list_view, true);
    alarm_refresh_list(page);
    lv_obj_t *entry = page->rows[page->editing_slot].time;
    lv_obj_t *target = lv_obj_is_visible(entry) ? lv_obj_get_parent(entry) : page->add;
    if (lv_obj_get_group(target) && !lv_obj_has_state(target, LV_STATE_DISABLED)) lv_group_focus_obj(target);
}

static void alarm_cancel(lv_event_t *event)
{
    alarm_page_t *page = lv_event_get_user_data(event);
    alarm_editor_back(page->ui);
}

static void alarm_edit(alarm_page_t *page, unsigned slot_index, const app_alarm_slot_t *slot)
{
    page->list_error = ESP_OK;
    page->list_back = page->ui->page_back;
    page->ui->page_back = alarm_editor_back;
    page->editing_slot = slot_index;
    page->editing = true;
    app_ui_label_text(page->edit_title, slot->configured ? "编辑闹钟" : "新建闹钟");
    lv_dropdown_set_selected(page->hour, slot->configured ? slot->hour : 7);
    lv_dropdown_set_selected(page->minute, slot->configured ? slot->minute : 0);
    lv_dropdown_set_selected(page->tone, slot->configured ? slot->tone : 0);
    alarm_switch_set(page->enabled, !slot->configured || slot->enabled);
    alarm_show(page->remove, slot->configured);
    app_ui_label_text(page->edit_status, "");
    alarm_show(page->list_view, false);
    alarm_show(page->editor, true);
    if (lv_obj_get_group(page->hour)) lv_group_focus_obj(page->hour);
}

static void alarm_add(lv_event_t *event)
{
    alarm_page_t *page = lv_event_get_user_data(event);
    if (page->editing) return;
    for (unsigned i = 0; i < APP_ALARM_SLOTS; ++i) {
        app_alarm_slot_t slot;
        if (app_alarm_get_slot(i, &slot) == ESP_OK && !slot.configured) {
            alarm_edit(page, i, &slot);
            return;
        }
    }
}

static void alarm_open_row(lv_event_t *event)
{
    alarm_row_t *row = lv_event_get_user_data(event);
    app_alarm_slot_t slot;
    if (!row->page->editing && app_alarm_get_slot(row->slot, &slot) == ESP_OK && slot.configured)
        alarm_edit(row->page, row->slot, &slot);
}

static void alarm_toggle(lv_event_t *event)
{
    alarm_row_t *row = lv_event_get_user_data(event);
    app_alarm_slot_t slot;
    if (row->page->editing || app_alarm_get_slot(row->slot, &slot) != ESP_OK || !slot.configured) return;
    row->page->list_error = app_alarm_set_slot(row->slot, slot.hour, slot.minute,
        lv_obj_has_state(row->enabled, LV_STATE_CHECKED), slot.tone);
    alarm_refresh_list(row->page);
}

static void alarm_save(lv_event_t *event)
{
    alarm_page_t *page = lv_event_get_user_data(event);
    if (!page->editing) return;
    const esp_err_t error = app_alarm_set_slot(page->editing_slot,
        lv_dropdown_get_selected(page->hour), lv_dropdown_get_selected(page->minute),
        lv_obj_has_state(page->enabled, LV_STATE_CHECKED), lv_dropdown_get_selected(page->tone));
    if (error == ESP_OK) alarm_editor_back(page->ui);
    else app_ui_label_text(page->edit_status, "保存失败");
}

static void alarm_preview(lv_event_t *event)
{
    alarm_page_t *page = lv_event_get_user_data(event);
    if (!page->editing) return;
    const esp_err_t error = app_alarm_preview(lv_dropdown_get_selected(page->tone));
    app_ui_label_text(page->edit_status, error == ESP_OK ? "正在试听" : "暂时无法试听");
}

static void alarm_delete_closed(lv_event_t *event)
{
    alarm_page_t *page = lv_event_get_user_data(event);
    page->delete_dialog = NULL;
}

static void alarm_delete_cancel(lv_event_t *event)
{
    alarm_page_t *page = lv_event_get_user_data(event);
    if (page->delete_dialog) lv_msgbox_close(page->delete_dialog);
}

static void alarm_delete_confirm(lv_event_t *event)
{
    alarm_page_t *page = lv_event_get_user_data(event);
    if (!page->delete_dialog) return;
    lv_msgbox_close(page->delete_dialog);
    if (app_alarm_remove_slot(page->editing_slot) == ESP_OK) alarm_editor_back(page->ui);
    else app_ui_label_text(page->edit_status, "删除失败");
}

static void alarm_delete_request(lv_event_t *event)
{
    alarm_page_t *page = lv_event_get_user_data(event);
    if (!page->editing || page->delete_dialog) return;
    app_alarm_slot_t slot;
    if (app_alarm_get_slot(page->editing_slot, &slot) != ESP_OK || !slot.configured) return;
    lv_obj_t *box = app_ui_modal_msgbox(page->ui->screen);
    if (!box) return;
    page->delete_dialog = box;
    lv_obj_set_width(box, 306);
    lv_obj_set_style_text_font(box, app_ui_chinese_font(), 0);
    lv_obj_add_event_cb(box, alarm_delete_closed, LV_EVENT_DELETE, page);
    lv_msgbox_add_title(box, "删除闹钟");
    char time[24];
    snprintf(time, sizeof(time), "每天 %02u:%02u", slot.hour, slot.minute);
    lv_msgbox_add_text(box, time);
    lv_obj_t *cancel = lv_msgbox_add_footer_button(box, "取消");
    lv_obj_t *confirm = lv_msgbox_add_footer_button(box, "确认删除");
    lv_obj_add_event_cb(cancel, alarm_delete_cancel, LV_EVENT_CLICKED, page);
    lv_obj_add_event_cb(confirm, alarm_delete_confirm, LV_EVENT_CLICKED, page);
    app_ui_modal_focus(box, cancel);
}

static void alarm_cleanup(app_ui_t *ui)
{
    alarm_page_t *page = ui->page_context;
    if (page->delete_dialog) lv_msgbox_close(page->delete_dialog);
    if (page->editing) app_alarm_cancel_preview();
    alarm_close_lists(page);
    page_free(ui);
}

static lv_obj_t *alarm_surface(lv_obj_t *parent, int width, int height)
{
    lv_obj_t *surface = lv_obj_create(parent);
    lv_obj_remove_style_all(surface);
    lv_obj_remove_flag(surface, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(surface, width, height);
    return surface;
}

static lv_obj_t *alarm_dropdown(lv_obj_t *parent, int x, int y, int width, const lv_font_t *font)
{
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_obj_set_pos(dropdown, x, y);
    lv_obj_set_size(dropdown, width, 50);
    lv_obj_set_style_text_font(dropdown, font, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), font, 0);
    return dropdown;
}

static void alarm_number_options(lv_obj_t *dropdown, unsigned count)
{
    char items[3 * 60];
    size_t length = 0;
    for (unsigned value = 0; value < count; ++value)
        length += snprintf(items + length, sizeof(items) - length, "%s%02u", value ? "\n" : "", value);
    lv_dropdown_set_options(dropdown, items);
}

static void alarm_build_list(alarm_page_t *page, lv_obj_t *content)
{
    lv_obj_t *view = alarm_surface(content, LV_PCT(100), LV_PCT(100));
    page->list_view = view;
    app_ui_add_text(view, "每日闹钟", LV_ALIGN_TOP_LEFT, 8, 10, 0xE4EFFF, app_ui_chinese_font_size(24));
    page->add = utility_button(view, "添加", 300, 1, 96, alarm_add, page);
    lv_obj_t *list = alarm_surface(view, 402, 265);
    lv_obj_set_pos(list, 2, 55);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x7896BD), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_row(list, 7, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (unsigned i = 0; i < APP_ALARM_SLOTS; ++i) {
        alarm_row_t *row = &page->rows[i];
        row->page = page;
        row->slot = i;
        row->body = alarm_surface(list, 394, 58);
        lv_obj_set_style_bg_color(row->body, lv_color_hex(0x1D2A42), 0);
        lv_obj_set_style_bg_opa(row->body, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row->body, 12, 0);
        lv_obj_t *edit = lv_button_create(row->body);
        lv_obj_set_size(edit, 295, 58);
        lv_obj_set_pos(edit, 0, 0);
        lv_obj_set_style_radius(edit, 12, 0);
        lv_obj_set_style_bg_color(edit, lv_color_hex(0x253650), 0);
        lv_obj_set_style_shadow_width(edit, 0, 0);
        lv_obj_add_event_cb(edit, alarm_open_row, LV_EVENT_CLICKED, row);
        row->time = app_ui_add_text(edit, "", LV_ALIGN_LEFT_MID, 10, 0, 0xF0F4FF, &lv_font_montserrat_28);
        row->tone = app_ui_add_text(edit, "", LV_ALIGN_LEFT_MID, 121, 0, 0xB6C8E5, app_ui_chinese_font());
        app_ui_add_text(edit, LV_SYMBOL_EDIT, LV_ALIGN_RIGHT_MID, -5, 0, 0xB6C8E5, &lv_font_montserrat_20);
        row->enabled = lv_switch_create(row->body);
        lv_obj_set_size(row->enabled, 59, 31);
        lv_obj_align(row->enabled, LV_ALIGN_RIGHT_MID, -17, 0);
        lv_obj_add_event_cb(row->enabled, alarm_toggle, LV_EVENT_VALUE_CHANGED, row);
    }
    page->empty = app_ui_add_text(view, "暂无闹钟", LV_ALIGN_TOP_MID, 0, 151,
                                  0xA6BDDA, app_ui_chinese_font());
    page->list_status = app_ui_add_text(view, "", LV_ALIGN_TOP_MID, 0, 322,
                                        0xA6BDDA, app_ui_chinese_font());
}

static void alarm_build_editor(alarm_page_t *page, lv_obj_t *content)
{
    lv_obj_t *view = alarm_surface(content, LV_PCT(100), LV_PCT(100));
    page->editor = view;
    page->edit_title = app_ui_add_text(view, "", LV_ALIGN_TOP_LEFT, 8, 10,
                                      0xE4EFFF, app_ui_chinese_font_size(24));
    utility_button(view, "取消", 300, 1, 96, alarm_cancel, page);
    page->hour = alarm_dropdown(view, 8, 65, 128, &lv_font_montserrat_24);
    page->minute = alarm_dropdown(view, 162, 65, 128, &lv_font_montserrat_24);
    alarm_number_options(page->hour, 24);
    alarm_number_options(page->minute, 60);
    app_ui_add_text(view, ":", LV_ALIGN_TOP_LEFT, 145, 77, 0xE4EFFF, &lv_font_montserrat_24);
    page->enabled = lv_switch_create(view);
    lv_obj_set_size(page->enabled, 59, 31);
    lv_obj_set_pos(page->enabled, 326, 71);
    app_ui_add_text(view, "启用", LV_ALIGN_TOP_LEFT, 339, 108, 0xA6BDDA, app_ui_chinese_font());
    app_ui_add_text(view, "铃声", LV_ALIGN_TOP_LEFT, 8, 150, 0xE4EFFF, app_ui_chinese_font());
    page->tone = alarm_dropdown(view, 8, 175, 263, app_ui_chinese_font());
    char items[96] = {0};
    size_t length = 0;
    for (unsigned i = 0; i < APP_ALARM_TONE_COUNT; ++i)
        length += snprintf(items + length, sizeof(items) - length, "%s%s", i ? "\n" : "", app_alarm_tone_name(i));
    lv_dropdown_set_options(page->tone, items);
    utility_button(view, "试听", 286, 178, 110, alarm_preview, page);
    page->remove = utility_button(view, "删除", 8, 255, 105, alarm_delete_request, page);
    lv_obj_set_style_bg_color(page->remove, lv_color_hex(0x673A4D), 0);
    utility_button(view, "保存", 251, 255, 145, alarm_save, page);
    page->edit_status = app_ui_add_text(view, "", LV_ALIGN_TOP_MID, 0, 316,
                                        0xA6BDDA, app_ui_chinese_font());
    alarm_show(view, false);
}

void app_open_alarm(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "闹钟");
    alarm_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF9C85, app_ui_chinese_font());
        return;
    }
    page->ui = ui;
    ui->page_context = page;
    ui->page_cleanup = alarm_cleanup;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    alarm_build_list(page, content);
    alarm_build_editor(page, content);
    alarm_refresh_list(page);
    app_ui_set_timer(ui, alarm_update, 300);
}

static lv_obj_t *s_alarm_popup;
static lv_obj_t *s_alarm_popup_owner;
static uint8_t s_alarm_popup_hour, s_alarm_popup_minute, s_alarm_popup_count;
static bool s_alarm_popup_media;

static void alarm_popup_deleted(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == s_alarm_popup) {
        s_alarm_popup = NULL;
        s_alarm_popup_owner = NULL;
    }
}

static void alarm_popup_ack(lv_event_t *event)
{
    app_alarm_dismiss();
    lv_msgbox_close(lv_event_get_user_data(event));
}

void app_utilities_alarm_ui(app_ui_t *ui)
{
    app_alarm_snapshot_t alarm;
    app_alarm_get(&alarm);
    if (s_alarm_popup && (!alarm.ringing || s_alarm_popup_owner != ui->screen ||
                         s_alarm_popup_media != ui->media_active || s_alarm_popup_hour != alarm.hour ||
                         s_alarm_popup_minute != alarm.minute || s_alarm_popup_count != alarm.ringing_count)) {
        if (s_alarm_popup_media) lv_obj_delete(s_alarm_popup);
        else lv_msgbox_close(s_alarm_popup);
    }
    if (!alarm.ringing || s_alarm_popup || !ui->screen || app_ui_input_blocked(ui) ||
        app_power_is_sleeping(ui->power)) return;
    s_alarm_popup_owner = ui->screen;
    s_alarm_popup_media = ui->media_active;
    s_alarm_popup_hour = alarm.hour;
    s_alarm_popup_minute = alarm.minute;
    s_alarm_popup_count = alarm.ringing_count;
    if (ui->media_active) {
        /* Raw game input and the active audio stream keep their ownership.
         * A temporary visual alert stays above the media's LVGL image. */
        char text[28];
        snprintf(text, sizeof(text), LV_SYMBOL_BELL "  %02u:%02u", alarm.hour, alarm.minute);
        s_alarm_popup = app_ui_add_text(ui->screen, text, LV_ALIGN_TOP_MID, 0, 12,
                                         0xF8F2DE, &lv_font_montserrat_20);
        lv_obj_set_style_bg_color(s_alarm_popup, lv_color_hex(0x354058), 0);
        lv_obj_set_style_bg_opa(s_alarm_popup, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_alarm_popup, 9, 0);
        lv_obj_set_style_radius(s_alarm_popup, 12, 0);
        lv_obj_add_event_cb(s_alarm_popup, alarm_popup_deleted, LV_EVENT_DELETE, NULL);
        return;
    }
    lv_obj_t *popup = app_ui_modal_msgbox(ui->screen);
    if (!popup) return;
    s_alarm_popup = popup;
    lv_obj_add_event_cb(popup, alarm_popup_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_width(popup, 300);
    lv_obj_set_style_text_font(popup, app_ui_chinese_font(), 0);
    lv_msgbox_add_title(popup, "闹钟");
    char time[8];
    snprintf(time, sizeof(time), "%02u:%02u", alarm.hour, alarm.minute);
    lv_obj_t *body = lv_msgbox_add_text(popup, time);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_28, 0);
    if (alarm.ringing_count > 1) {
        char concurrent[32];
        snprintf(concurrent, sizeof(concurrent), "%u 个闹钟同时响铃", alarm.ringing_count);
        lv_msgbox_add_text(popup, concurrent);
    }
    lv_obj_t *ack = lv_msgbox_add_footer_button(popup, "关闭");
    lv_obj_add_event_cb(ack, alarm_popup_ack, LV_EVENT_CLICKED, popup);
    app_ui_modal_focus(popup, ack);
}

typedef struct {
    lv_obj_t *field, *status, *selector, *keyboard;
    unsigned active_note;
    uint32_t edited_at;
    bool loaded, dirty;
} notes_page_t;

static void note_layout(void *context, bool keyboard_visible)
{
    notes_page_t *page = context;
    lv_obj_set_height(page->field, keyboard_visible ? 117 : 287);
}

static bool note_submit(notes_page_t *page)
{
    if (!page->loaded || !page->dirty) return true;
    const esp_err_t error = app_note_save(page->active_note, lv_textarea_get_text(page->field));
    if (error != ESP_OK) {
        app_ui_label_fmt(page->status, "保存失败：%s", esp_err_to_name(error));
        return false;
    }
    page->dirty = false;
    return true;
}

static void notes_update(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    notes_page_t *page = ui->page_context;
    app_note_status_t status;
    app_note_get_status(page->active_note, &status);
    if (!status.ready) {
        app_ui_label_text(page->status, "正在加载");
        return;
    }
    if (!page->loaded) {
        char *text = malloc(APP_NOTE_CAPACITY);
        if (!text) {
            app_ui_label_text(page->status, "内存不足");
            return;
        }
        if (app_note_read(page->active_note, text) == ESP_OK) {
            lv_textarea_set_text(page->field, text);
            lv_textarea_set_cursor_pos(page->field, LV_TEXTAREA_CURSOR_LAST);
            page->loaded = true;
            lv_obj_remove_state(page->field, LV_STATE_DISABLED);
        }
        free(text);
    }
    if (page->dirty && lv_tick_elaps(page->edited_at) >= 1100 && !note_submit(page)) return;
    if (status.save_error != ESP_OK)
        app_ui_label_fmt(page->status, "存储：%s", esp_err_to_name(status.save_error));
    else if (page->dirty || status.pending) app_ui_label_text(page->status, "正在保存");
    else app_ui_label_text(page->status, "");
}

static void edit_note(lv_event_t *event)
{
    notes_page_t *page = lv_event_get_user_data(event);
    if (!page->loaded) return;
    switch (lv_event_get_code(event)) {
        case LV_EVENT_CLICKED:
            app_text_input_show(page->keyboard, true);
            break;
        case LV_EVENT_VALUE_CHANGED:
            page->dirty = true;
            page->edited_at = lv_tick_get();
            break;
        case LV_EVENT_READY:
        case LV_EVENT_CANCEL:
            (void)note_submit(page);
            app_text_input_show(page->keyboard, false);
            break;
        default:
            break;
    }
}

static void select_note(lv_event_t *event)
{
    notes_page_t *page = lv_event_get_user_data(event);
    if (!note_submit(page)) {
        lv_dropdown_set_selected(page->selector, page->active_note);
        return;
    }
    page->active_note = lv_dropdown_get_selected(page->selector);
    page->loaded = false;
    app_text_input_show(page->keyboard, false);
    lv_obj_add_state(page->field, LV_STATE_DISABLED);
    lv_textarea_set_text(page->field, "");
}

static void notes_cleanup(app_ui_t *ui)
{
    notes_page_t *page = ui->page_context;
    (void)note_submit(page);
    if (page->keyboard) lv_obj_delete(page->keyboard);
    page_free(ui);
}

void app_open_notes(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "记事本");
    notes_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF9C85, app_ui_chinese_font());
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = notes_cleanup;
    page->selector = lv_dropdown_create(content);
    lv_obj_set_size(page->selector, 148, 44);
    lv_obj_set_style_text_font(page->selector, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(page->selector), app_ui_chinese_font(), 0);
    lv_dropdown_set_options_static(page->selector, "便签 1\n便签 2\n便签 3\n便签 4");
    lv_obj_add_event_cb(page->selector, select_note, LV_EVENT_VALUE_CHANGED, page);
    page->status = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 165, 14, 0xA6BDDA, app_ui_chinese_font());
    lv_obj_set_width(page->status, 237);
    lv_label_set_long_mode(page->status, LV_LABEL_LONG_DOT);
    page->field = lv_textarea_create(content);
    lv_obj_set_pos(page->field, 0, 56);
    lv_obj_set_size(page->field, 405, 287);
    lv_obj_set_style_text_font(page->field, app_ui_chinese_font_size(24), 0);
    lv_obj_set_style_text_line_space(page->field, 5, 0);
    lv_obj_set_style_pad_all(page->field, 12, 0);
    lv_obj_add_state(page->field, LV_STATE_DISABLED);
    lv_textarea_set_max_length(page->field, APP_NOTE_CHARACTERS);
    lv_textarea_set_placeholder_text(page->field, "写下备忘");
    lv_obj_add_event_cb(page->field, edit_note, LV_EVENT_CLICKED, page);
    lv_obj_add_event_cb(page->field, edit_note, LV_EVENT_VALUE_CHANGED, page);
    lv_obj_add_event_cb(page->field, edit_note, LV_EVENT_READY, page);
    lv_obj_add_event_cb(page->field, edit_note, LV_EVENT_CANCEL, page);
    page->keyboard = app_text_input_create(ui->screen, page->field, true, note_layout, page);
    app_ui_set_timer(ui, notes_update, 250);
    notes_update(ui->timer);
}

typedef struct {
    lv_obj_t *calendar, *title, *selected, *previous, *next;
} calendar_page_t;

static void show_calendar_month(calendar_page_t *page, unsigned year, unsigned month)
{
    lv_calendar_set_month_shown(page->calendar, year, month);
    app_ui_label_fmt(page->title, "%04u 年 %02u 月", year, month);
    if (year == 2000 && month == 1) lv_obj_add_state(page->previous, LV_STATE_DISABLED);
    else lv_obj_remove_state(page->previous, LV_STATE_DISABLED);
    if (year == 2099 && month == 12) lv_obj_add_state(page->next, LV_STATE_DISABLED);
    else lv_obj_remove_state(page->next, LV_STATE_DISABLED);
    app_ui_label_text(page->selected, "");
}

static void calendar_month_event(lv_event_t *event)
{
    calendar_page_t *page = lv_event_get_user_data(event);
    const lv_calendar_date_t *date = lv_calendar_get_showed_date(page->calendar);
    int month = date->month + (lv_event_get_current_target_obj(event) == page->next ? 1 : -1);
    unsigned year = date->year;
    if (month == 0) {
        --year;
        month = 12;
    } else if (month == 13) {
        ++year;
        month = 1;
    }
    if (year >= 2000 && year <= 2099) show_calendar_month(page, year, month);
}

static void calendar_day_event(lv_event_t *event)
{
    calendar_page_t *page = lv_event_get_user_data(event);
    lv_calendar_date_t date;
    if (lv_calendar_get_pressed_date(page->calendar, &date) != LV_RESULT_OK) return;
#if LV_USE_CALENDAR_CHINESE
    const char *name = lv_calendar_get_day_name(&date);
    app_ui_label_fmt(page->selected, "%02u-%02u  %s", date.month, date.day, name ? name : "");
#else
    app_ui_label_fmt(page->selected, "%02u-%02u", date.month, date.day);
#endif
}

void app_open_calendar(app_ui_t *ui)
{
    static const char *week[] = {"日", "一", "二", "三", "四", "五", "六"};
    lv_obj_t *content = app_ui_begin_page(ui, "日历");
    calendar_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF9C85, app_ui_chinese_font());
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = page_free;
    page->previous = utility_button(content, LV_SYMBOL_LEFT, 0, 0, 54, calendar_month_event, page);
    page->next = utility_button(content, LV_SYMBOL_RIGHT, 350, 0, 54, calendar_month_event, page);
    page->title = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 8, 0xEAF2FF, app_ui_chinese_font_size(24));
    page->calendar = lv_calendar_create(content);
    lv_obj_set_pos(page->calendar, 0, 51);
    lv_obj_set_size(page->calendar, 405, 269);
    lv_calendar_set_day_names(page->calendar, week);
    lv_obj_t *grid = lv_calendar_get_btnmatrix(page->calendar);
    lv_obj_set_style_text_font(grid, app_ui_chinese_font(), LV_PART_ITEMS);
    lv_obj_set_style_text_line_space(grid, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_gap(grid, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 3, LV_PART_MAIN);
    page->selected = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 327, 0xBFDBFC, app_ui_chinese_font());
    lv_obj_add_event_cb(page->calendar, calendar_day_event, LV_EVENT_VALUE_CHANGED, page);
    app_telemetry_snapshot_t telemetry;
    app_telemetry_get(&telemetry);
    const pcf85063_time_t *rtc = &telemetry.rtc;
    const bool valid = telemetry.rtc_error == ESP_OK && !rtc->oscillator_stopped &&
        app_calendar_valid(rtc->year, rtc->month, rtc->day, 0, 0, 0);
    if (valid) lv_calendar_set_today_date(page->calendar, rtc->year, rtc->month, rtc->day);
#if LV_USE_CALENDAR_CHINESE
    lv_calendar_set_chinese_mode(page->calendar, true);
#endif
    show_calendar_month(page, valid ? rtc->year : 2000, valid ? rtc->month : 1);
    if (!valid) app_ui_label_text(page->selected, "请在时钟中校准日期");
}

typedef struct {
    lv_obj_t *city, *temperature, *condition, *status;
    lv_obj_t *days[APP_WEATHER_DAYS];
} weather_page_t;

static void weather_update(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    weather_page_t *page = ui->page_context;
    app_weather_refresh(false);
    app_weather_snapshot_t weather;
    app_weather_get(&weather);
    if (lv_dropdown_get_selected(page->city) != weather.city)
        lv_dropdown_set_selected(page->city, weather.city);
    if (weather.valid) {
        app_ui_label_fmt(page->temperature, "%d °C", weather.temperature_c);
        app_ui_label_fmt(page->condition, "%s  %u%%", app_weather_condition(weather.weather_code),
                         weather.humidity);
        for (unsigned day = 0; day < APP_WEATHER_DAYS; ++day) {
            const app_weather_day_t *item = &weather.daily[day];
            app_ui_label_fmt(page->days[day], "%s     %s     %d°  /  %d°",
                item->date + 5, app_weather_condition(item->weather_code), item->min_c, item->max_c);
        }
    } else {
        app_ui_label_text(page->temperature, "—");
        app_ui_label_text(page->condition, "");
        for (unsigned day = 0; day < APP_WEATHER_DAYS; ++day) app_ui_label_text(page->days[day], "");
    }
    aether_wifi_status_t wifi;
    const bool connected = aether_wifi_get_status(aether_wifi_get_default(), &wifi) == ESP_OK && wifi.connected;
    if (weather.fetching) app_ui_label_text(page->status, "正在更新");
    else if (!connected) app_ui_label_text(page->status, "Wi-Fi 未连接");
    else if (weather.error != ESP_OK) app_ui_label_text(page->status, "更新失败 · 点击刷新重试");
    else if (weather.valid) app_ui_label_fmt(page->status, "%s  Open-Meteo", weather.observed);
    else app_ui_label_text(page->status, "Open-Meteo");
}

static void city_changed(lv_event_t *event)
{
    lv_obj_t *city = lv_event_get_current_target_obj(event);
    (void)app_weather_select_city(lv_dropdown_get_selected(city));
}

static void weather_refresh_event(lv_event_t *event)
{
    (void)event;
    app_weather_refresh(true);
}

void app_open_weather(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "天气");
    weather_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF9C85, app_ui_chinese_font());
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = page_free;
    char cities[384];
    size_t length = 0;
    for (unsigned city = 0; city < app_weather_city_count(); ++city) {
        const int written = snprintf(cities + length, sizeof(cities) - length, "%s%s",
                                     city ? "\n" : "", app_weather_city(city));
        if (written < 0 || (size_t)written >= sizeof(cities) - length) break;
        length += (size_t)written;
    }
    page->city = lv_dropdown_create(content);
    lv_obj_set_size(page->city, 164, 44);
    lv_obj_set_style_text_font(page->city, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(page->city), app_ui_chinese_font(), 0);
    lv_dropdown_set_options(page->city, cities);
    lv_obj_add_event_cb(page->city, city_changed, LV_EVENT_VALUE_CHANGED, page);
    utility_button(content, LV_SYMBOL_REFRESH, 350, 0, 54, weather_refresh_event, NULL);
    page->temperature = app_ui_add_text(content, "—", LV_ALIGN_TOP_MID, 0, 66,
                                          0xEDF5FF, &lv_font_montserrat_48);
    page->condition = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 129,
                                        0xC9DDF7, app_ui_chinese_font_size(24));
    for (unsigned day = 0; day < APP_WEATHER_DAYS; ++day) {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_pos(row, 0, 176 + 48 * day);
        lv_obj_set_size(row, 405, 42);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1D304D), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        page->days[day] = app_ui_add_text(row, "", LV_ALIGN_CENTER, 0, 0,
                                            0xEAF2FE, app_ui_chinese_font());
    }
    page->status = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 327,
                                     0xADBCD2, app_ui_chinese_font());
    app_ui_set_timer(ui, weather_update, 650);
    weather_update(ui->timer);
}
