#pragma once

#include "app_battery.h"
#include "app_telemetry.h"
#include "app_tools_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct app_ui {
    aether_board_t *board;
    void *storage;
    lv_obj_t *screen, *content, *app_obj[12];
    lv_timer_t *timer;
    void *page_context;
    void (*page_cleanup)(struct app_ui *);
    void (*page_back)(struct app_ui *);
} app_ui_t;

typedef struct {
    bool mounted, card_inserted;
    uint64_t capacity_bytes;
    esp_err_t last_error;
} aether_storage_status_t;

typedef struct { bool available; esp_err_t setup_error; uint32_t edges, last_period_us; } co5300_te_metrics_t;
typedef struct { const char *version, *idf_ver; } esp_app_desc_t;
enum { MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_SPIRAM = 4 };

esp_err_t aether_storage_get_status(void *, aether_storage_status_t *);
esp_err_t co5300_display_get_te_metrics(void *, co5300_te_metrics_t *);
const esp_app_desc_t *esp_app_get_description(void);
int64_t esp_timer_get_time(void);
size_t esp_psram_get_size(void);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_minimum_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
size_t strlcpy(char *, const char *, size_t);

lv_obj_t *app_ui_begin_page(app_ui_t *, const char *);
lv_obj_t *app_ui_add_text(lv_obj_t *, const char *, lv_align_t, int32_t, int32_t, uint32_t, const lv_font_t *);
const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned);
void app_ui_label_text(lv_obj_t *, const char *);
void app_ui_label_fmt(lv_obj_t *, const char *, ...);
void app_ui_set_timer(app_ui_t *, lv_timer_cb_t, uint32_t);
bool app_ui_input_blocked(const app_ui_t *);
void app_open_status(app_ui_t *);
void app_open_clock(app_ui_t *);
void app_open_alarm(app_ui_t *);
void app_open_stopwatch(app_ui_t *);
void app_open_countdown(app_ui_t *);
void app_open_barometer(app_ui_t *);
