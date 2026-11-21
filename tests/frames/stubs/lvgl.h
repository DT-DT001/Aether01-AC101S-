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
typedef struct { unsigned activity; } lv_display_t;
typedef struct {
    const lv_image_dsc_t *source;
    unsigned updates;
    unsigned invalidations;
    unsigned centers;
    int32_t width, height;
    unsigned resizes;
} lv_obj_t;
void lv_image_set_src(lv_obj_t *image, const void *source);
void lv_obj_center(lv_obj_t *object);
void lv_obj_invalidate(lv_obj_t *object);
lv_display_t *lv_obj_get_display(const lv_obj_t *object);
void lv_display_trigger_activity(lv_display_t *display);

int32_t lv_obj_get_width(const lv_obj_t *object);
int32_t lv_obj_get_height(const lv_obj_t *object);
void lv_obj_set_size(lv_obj_t *object, int32_t width, int32_t height);
