#pragma once

#include "lvgl.h"

#define APP_SCROLL_LIST_ROWS 12U

typedef struct {
    lv_obj_t *viewport;
    lv_obj_t *spacer;
    lv_obj_t *rows[APP_SCROLL_LIST_ROWS];
    size_t indices[APP_SCROLL_LIST_ROWS];
    size_t count;
    int32_t row_step;
    void (*bind_row)(lv_obj_t *row, size_t index, void *context);
    void *context;
} app_scroll_list_t;

static inline void app_scroll_list_update(app_scroll_list_t *list)
{
    const int32_t scroll = lv_obj_get_scroll_y(list->viewport);
    size_t first = scroll > 0 ? (size_t)(scroll / list->row_step) : 0U;
    if (first > 0U) --first;

    for (size_t i = first; i < first + APP_SCROLL_LIST_ROWS; ++i) {
        const size_t slot = i % APP_SCROLL_LIST_ROWS;
        lv_obj_t *row = list->rows[slot];
        if (i >= list->count) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            list->indices[slot] = SIZE_MAX;
        } else if (list->indices[slot] != i) {
            list->indices[slot] = i;
            lv_obj_set_y(row, (int32_t)i * list->row_step);
            lv_obj_set_user_data(row, (void *)(uintptr_t)i);
            list->bind_row(row, i, list->context);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static inline void app_scroll_list_event(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event))
        app_scroll_list_update(lv_event_get_user_data(event));
}

static inline void app_scroll_list_key(lv_event_t *event)
{
    app_scroll_list_t *list = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_target_obj(event);
    const uint32_t key = lv_event_get_key(event);
    const size_t current = (size_t)(uintptr_t)lv_obj_get_user_data(row);
    size_t next = current;
    if (key == LV_KEY_UP && current) --next;
    else if (key == LV_KEY_DOWN && current + 1U < list->count) ++next;
    if (next == current) return;

    const int32_t y = (int32_t)next * list->row_step;
    const int32_t scroll = lv_obj_get_scroll_y(list->viewport);
    const int32_t height = lv_obj_get_content_height(list->viewport);
    if (y < scroll) lv_obj_scroll_to_y(list->viewport, y, LV_ANIM_OFF);
    else if (y + list->row_step > scroll + height)
        lv_obj_scroll_to_y(list->viewport, y + list->row_step - height, LV_ANIM_OFF);
    lv_obj_t *next_row = list->rows[next % APP_SCROLL_LIST_ROWS];
    if (list->indices[next % APP_SCROLL_LIST_ROWS] == next && lv_obj_get_group(next_row))
        lv_group_focus_obj(next_row);
}

static inline void app_scroll_list_init(app_scroll_list_t *list, lv_obj_t *viewport,
    int32_t row_step, lv_obj_t *(*create_row)(lv_obj_t *parent, void *context),
    void (*bind_row)(lv_obj_t *row, size_t index, void *context), void *context)
{
    list->viewport = viewport;
    list->row_step = row_step;
    list->bind_row = bind_row;
    list->context = context;
    lv_obj_add_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);

    /* One transparent child fixes the native scroll range while a bounded
     * pool of real buttons follows the visible part of the catalog. */
    list->spacer = lv_obj_create(viewport);
    lv_obj_remove_style_all(list->spacer);
    lv_obj_remove_flag(list->spacer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(list->spacer, 1, 1);
    lv_obj_set_pos(list->spacer, 0, 0);

    for (size_t i = 0; i < APP_SCROLL_LIST_ROWS; ++i) {
        lv_obj_t *row = create_row(viewport, context);
        list->rows[i] = row;
        list->indices[i] = SIZE_MAX;
        lv_obj_set_align(row, LV_ALIGN_TOP_LEFT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row, app_scroll_list_key, LV_EVENT_KEY, list);
    }
    lv_obj_add_event_cb(viewport, app_scroll_list_event, LV_EVENT_SCROLL, list);
}

static inline void app_scroll_list_set_count(app_scroll_list_t *list, size_t count)
{
    lv_obj_stop_scroll_anim(list->viewport);
    list->count = count;
    lv_obj_set_height(list->spacer, count ? (int32_t)count * list->row_step : 1);
    for (size_t i = 0; i < APP_SCROLL_LIST_ROWS; ++i) list->indices[i] = SIZE_MAX;
    lv_obj_scroll_to_y(list->viewport, 0, LV_ANIM_OFF);
    app_scroll_list_update(list);
}

static inline void app_scroll_list_deinit(app_scroll_list_t *list)
{
    if (!list->viewport) return;
    lv_obj_remove_event_cb_with_user_data(list->viewport, app_scroll_list_event, list);
    for (size_t i = 0; i < APP_SCROLL_LIST_ROWS; ++i)
        lv_obj_remove_event_cb_with_user_data(list->rows[i], app_scroll_list_key, list);
    list->viewport = NULL;
}
