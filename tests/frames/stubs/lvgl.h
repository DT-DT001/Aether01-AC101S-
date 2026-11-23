#pragma once
#include <stdbool.h>
#include <stdint.h>
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565 0x12
typedef struct {
    struct {
        uint32_t magic;
        uint32_t cf;
        uint32_t w;
        uint32_t h;
        uint32_t stride;
    } header;
    uint32_t data_size;
    const uint8_t *data;
} lv_image_dsc_t;
typedef enum { LV_EVENT_REFR_START, LV_EVENT_DELETE } lv_event_code_t;
typedef struct { void *user_data, *target; } lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *);
typedef struct { unsigned resume, ready; } lv_timer_t;
typedef struct {
    unsigned activity;
    lv_timer_t timer;
    lv_event_cb_t refresh;
    void *refresh_context;
} lv_display_t;
typedef struct {
    const lv_image_dsc_t *source;
    unsigned updates;
    unsigned invalidations;
    unsigned centers;
    int32_t width, height;
    unsigned resizes;
    lv_event_cb_t on_delete;
    void *delete_context;
} lv_obj_t;
void lv_image_set_src(lv_obj_t *image, const void *source);
const void *lv_image_get_src(const lv_obj_t *image);
void lv_obj_center(lv_obj_t *object);
void lv_obj_invalidate(lv_obj_t *object);
lv_display_t *lv_obj_get_display(const lv_obj_t *object);
void lv_display_trigger_activity(lv_display_t *display);
void lv_display_add_event_cb(lv_display_t *,lv_event_cb_t,lv_event_code_t,void *);
uint32_t lv_display_remove_event_cb_with_user_data(lv_display_t *,lv_event_cb_t,void *);
void lv_obj_add_event_cb(lv_obj_t *,lv_event_cb_t,lv_event_code_t,void *);
void lv_obj_remove_event_cb_with_user_data(lv_obj_t *,lv_event_cb_t,void *);
void *lv_event_get_user_data(lv_event_t *);
lv_obj_t *lv_event_get_target_obj(lv_event_t *);
lv_timer_t *lv_display_get_refr_timer(lv_display_t *);
void lv_timer_resume(lv_timer_t *);
void lv_timer_ready(lv_timer_t *);

int32_t lv_obj_get_width(const lv_obj_t *object);
int32_t lv_obj_get_height(const lv_obj_t *object);
void lv_obj_set_size(lv_obj_t *object, int32_t width, int32_t height);
