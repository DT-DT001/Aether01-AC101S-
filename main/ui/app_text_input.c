#include "app_text_input.h"
#include "app_ui_internal.h"
#include "app_otg_control.h"
#include <stdlib.h>
#include <string.h>

enum { CANDIDATE_COUNT = 5, KEYBOARD_HEIGHT = 179, ROW_HEIGHT = 42 };
static const struct {
    const char *spelling;
    const char *candidates;
} pinyin_dictionary[] = {
#include "../assets/pinyin_candidates.inc"
};

typedef struct {
    lv_obj_t *dock, *keyboard, *textarea, *mode, *spelling, *next;
    lv_obj_t *buttons[CANDIDATE_COUNT], *labels[CANDIDATE_COUNT];
    lv_timer_t *timer;
    app_text_input_layout_t layout;
    void *layout_context;
    char syllable[8];
    char candidates[CANDIDATE_COUNT][5];
    size_t candidate_offset;
    bool wanted, visible, pinyin;
} text_input_t;

static size_t encoded_length(const char *character)
{
    unsigned byte = (unsigned char)*character;
    if (byte >= 0xF0) return 4;
    if (byte >= 0xE0) return 3;
    if (byte >= 0xC0) return 2;
    return byte != 0;
}

static const char *pinyin_candidates(const char *spelling)
{
    if (*spelling) {
        for (size_t index = 0; index < sizeof(pinyin_dictionary) / sizeof(pinyin_dictionary[0]); ++index)
            if (strcmp(spelling, pinyin_dictionary[index].spelling) == 0)
                return pinyin_dictionary[index].candidates;
    }
    return "";
}

static void refresh_candidates(text_input_t *input)
{
    if (!input->spelling) return;
    const char *source = pinyin_candidates(input->syllable);
    const size_t bytes = strlen(source);
    source += input->candidate_offset < bytes ? input->candidate_offset : 0;
    app_ui_label_text(input->spelling, input->syllable);
    app_ui_label_text(lv_obj_get_child(input->mode, 0), input->pinyin ? "中" : "EN");
    for (size_t index = 0; index < CANDIDATE_COUNT; ++index) {
        const size_t length = encoded_length(source);
        memcpy(input->candidates[index], source, length);
        input->candidates[index][length] = '\0';
        source += length;
        app_ui_label_text(input->labels[index], input->candidates[index]);
        if (length) lv_obj_remove_flag(input->buttons[index], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(input->buttons[index], LV_OBJ_FLAG_HIDDEN);
    }
    if (*source || input->candidate_offset) lv_obj_remove_flag(input->next, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(input->next, LV_OBJ_FLAG_HIDDEN);
}

static void clear_composition(text_input_t *input)
{
    input->syllable[0] = '\0';
    input->candidate_offset = 0;
    refresh_candidates(input);
}

static void commit_composition(text_input_t *input, const char *text)
{
    if (*text) lv_textarea_add_text(input->textarea, text);
    clear_composition(input);
}

static void candidate_event(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_current_target_obj(event);
    if (button == input->mode) {
        commit_composition(input, input->syllable);
        input->pinyin = !input->pinyin;
        lv_keyboard_set_mode(input->keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        refresh_candidates(input);
        return;
    }
    if (button == input->next) {
        const char *candidates = pinyin_candidates(input->syllable);
        for (size_t index = 0; index < CANDIDATE_COUNT; ++index)
            input->candidate_offset += strlen(input->candidates[index]);
        if (!candidates[input->candidate_offset]) input->candidate_offset = 0;
        refresh_candidates(input);
        return;
    }
    for (size_t index = 0; index < CANDIDATE_COUNT; ++index)
        if (button == input->buttons[index]) {
            commit_composition(input, input->candidates[index]);
            return;
        }
}

static void keyboard_event(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    const uint32_t index = lv_keyboard_get_selected_button(input->keyboard);
    if (index == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *key = lv_keyboard_get_button_text(input->keyboard, index);
    if (!key || !*key) return;
    if (input->pinyin && key[0] >= 'a' && key[0] <= 'z' && !key[1]) {
        size_t length = strlen(input->syllable);
        if (length >= sizeof(input->syllable) - 1) {
            commit_composition(input, input->syllable);
            length = 0;
        }
        input->syllable[length] = key[0];
        input->syllable[length + 1] = '\0';
        input->candidate_offset = 0;
        refresh_candidates(input);
        return;
    }
    if (input->syllable[0] && strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        input->syllable[strlen(input->syllable) - 1] = '\0';
        input->candidate_offset = 0;
        refresh_candidates(input);
        return;
    }
    if (input->syllable[0]) {
        const char *choice = strcmp(key, " ") == 0 && input->candidates[0][0] ?
            input->candidates[0] : input->syllable;
        commit_composition(input, choice);
        if (choice == input->candidates[0]) return;
    }
    lv_keyboard_def_event_cb(event);
}

static void update_visibility(text_input_t *input)
{
    aether_usb_host_status_t status;
    const bool external_keyboard = app_otg_get_status(&status) == ESP_OK &&
        status.active && status.keyboard_interfaces;
    const bool visible = input->wanted && !external_keyboard;
    if (visible == input->visible) return;
    input->visible = visible;
    if (visible) lv_obj_remove_flag(input->dock, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_add_flag(input->dock, LV_OBJ_FLAG_HIDDEN);
        clear_composition(input);
    }
    if (input->layout) input->layout(input->layout_context, visible);
}

static void update_timer(lv_timer_t *timer)
{
    update_visibility(lv_timer_get_user_data(timer));
}

static void dock_deleted(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    if (input->timer) lv_timer_delete(input->timer);
    free(input);
}

static lv_obj_t *candidate_button(text_input_t *input, const char *text,
                                  int32_t x, int32_t width, lv_obj_t **label)
{
    lv_obj_t *button = lv_button_create(input->dock);
    lv_obj_set_pos(button, x, 4);
    lv_obj_set_size(button, width, ROW_HEIGHT - 8);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(button);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x294060), 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_add_event_cb(button, candidate_event, LV_EVENT_CLICKED, input);
    lv_obj_t *title = app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0,
                                      0xF0F6FF, app_ui_chinese_font());
    if (label) *label = title;
    return button;
}

lv_obj_t *app_text_input_create(lv_obj_t *parent, lv_obj_t *textarea, bool pinyin,
                                app_text_input_layout_t layout, void *context)
{
    text_input_t *input = calloc(1, sizeof(*input));
    if (!input) return NULL;
    input->dock = lv_obj_create(parent);
    input->textarea = textarea;
    input->layout = layout;
    input->layout_context = context;
    input->pinyin = pinyin;
    lv_obj_set_user_data(input->dock, input);
    lv_obj_set_size(input->dock, 480, KEYBOARD_HEIGHT + (pinyin ? ROW_HEIGHT : 0));
    lv_obj_align(input->dock, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(input->dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(input->dock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(input->dock, 0, 0);
    lv_obj_set_style_radius(input->dock, 0, 0);
    lv_obj_set_style_border_width(input->dock, 0, 0);
    lv_obj_set_style_bg_color(input->dock, lv_color_hex(0x18243A), 0);
    lv_obj_add_event_cb(input->dock, dock_deleted, LV_EVENT_DELETE, input);

    input->keyboard = lv_keyboard_create(input->dock);
    lv_obj_set_size(input->keyboard, 480, KEYBOARD_HEIGHT);
    lv_obj_set_style_text_font(input->keyboard, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_group_remove_obj(input->keyboard);
    lv_keyboard_set_textarea(input->keyboard, textarea);
    if (pinyin) {
        input->mode = candidate_button(input, "中", 8, 46, NULL);
        input->spelling = app_ui_add_text(input->dock, "", LV_ALIGN_TOP_LEFT, 65, 13,
                                           0xC5D9F4, &lv_font_montserrat_16);
        for (size_t index = 0; index < CANDIDATE_COUNT; ++index)
            input->buttons[index] = candidate_button(input, "", 144 + 58 * index, 51,
                                                       &input->labels[index]);
        input->next = candidate_button(input, LV_SYMBOL_RIGHT, 434, 38, NULL);
        lv_obj_remove_event_cb(input->keyboard, lv_keyboard_def_event_cb);
        lv_obj_add_event_cb(input->keyboard, keyboard_event, LV_EVENT_VALUE_CHANGED, input);
        refresh_candidates(input);
    }
    input->timer = lv_timer_create(update_timer, 160, input);
    return input->dock;
}

void app_text_input_show(lv_obj_t *dock, bool show)
{
    if (!dock) return;
    text_input_t *input = lv_obj_get_user_data(dock);
    input->wanted = show;
    update_visibility(input);
}
