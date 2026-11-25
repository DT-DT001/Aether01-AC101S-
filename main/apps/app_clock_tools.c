#include "app_ui_internal.h"
#include "app_utilities.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>

enum { STOPWATCH_LAPS = 24 };

static struct {
    int64_t started_us, elapsed_us;
    int64_t laps[STOPWATCH_LAPS + 1];
    unsigned lap_count;
    bool running;
} s_stopwatch;

typedef struct {
    lv_obj_t *readout, *fraction, *primary, *secondary, *secondary_button;
    lv_obj_t *details;
} stopwatch_page_t;

typedef struct {
    lv_obj_t *readout, *state, *bar, *picker, *fields[3], *primary, *reset_button;
    bool start_error;
} countdown_page_t;

static lv_obj_t *clock_card(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, 408, height);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_radius(card, 17, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x304767), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x172641), 0);
    return card;
}

static lv_obj_t *clock_button(lv_obj_t *parent, int x, int y, bool primary,
                              const char *text, lv_event_cb_t handler, app_ui_t *ui)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 196, 54);
    lv_obj_set_style_radius(button, 15, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x64D2C5 : 0x283C5B), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x4E7696), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x22314B), LV_STATE_DISABLED);
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0,
                    primary ? 0x102937 : 0xEAF2FF, app_ui_chinese_font_size(24));
    lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, ui);
    return button;
}

static void clock_button_enabled(lv_obj_t *button, bool enabled)
{
    if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
}

static void clock_tools_cleanup(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
}

static void clock_tools_resume(app_ui_t *ui, bool suspended)
{
    if (!suspended && ui->timer) lv_timer_ready(ui->timer);
}

static void clock_tools_setup(app_ui_t *ui, void *page, lv_obj_t *content)
{
    ui->page_context = page;
    ui->page_cleanup = clock_tools_cleanup;
    ui->page_suspend = clock_tools_resume;
    ui->page_back = app_open_clock;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(content, 12, 0);
}

static int64_t stopwatch_elapsed(void)
{
    return s_stopwatch.elapsed_us + (s_stopwatch.running ? esp_timer_get_time() - s_stopwatch.started_us : 0);
}

static void clock_format_time(char *buffer, size_t size, uint64_t seconds)
{
    unsigned hours = (unsigned)(seconds / 3600);
    if (hours > 999) hours = 999;
    snprintf(buffer, size, "%02u:%02u:%02u", hours, (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
}

static void stopwatch_display(stopwatch_page_t *page)
{
    const int64_t elapsed = stopwatch_elapsed();
    char time[16];
    clock_format_time(time, sizeof(time), elapsed / 1000000);
    app_ui_label_text(page->readout, time);
    app_ui_label_fmt(page->fraction, ".%02u", (unsigned)(elapsed / 10000 % 100));
}

static void stopwatch_tick(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    if (s_stopwatch.running) stopwatch_display(ui->page_context);
}

static void stopwatch_refresh(stopwatch_page_t *page)
{
    stopwatch_display(page);
    app_ui_label_text(page->primary, s_stopwatch.running ? "暂停" : s_stopwatch.elapsed_us ? "继续" : "开始");
    app_ui_label_text(page->secondary, s_stopwatch.running ? "计次" : "复位");
    clock_button_enabled(page->secondary_button, s_stopwatch.running || s_stopwatch.elapsed_us != 0);
}

static void stopwatch_laps(stopwatch_page_t *page)
{
    lv_obj_t *list = page->details;
    lv_obj_clean(list);
    for (unsigned offset = 0; offset < s_stopwatch.lap_count && offset < STOPWATCH_LAPS; ++offset) {
        const unsigned number = s_stopwatch.lap_count - offset;
        const int64_t elapsed = s_stopwatch.laps[number % (STOPWATCH_LAPS + 1)];
        const int64_t split = elapsed - s_stopwatch.laps[(number - 1) % (STOPWATCH_LAPS + 1)];
        const int y = (int)offset * 32;
        char time[16];
        lv_obj_t *label = app_ui_add_text(list, "", LV_ALIGN_TOP_LEFT, 12, y, 0x8FA9CD, &lv_font_montserrat_16);
        app_ui_label_fmt(label, "%02u", number);
        clock_format_time(time, sizeof(time), split / 1000000);
        label = app_ui_add_text(list, "", LV_ALIGN_TOP_MID, 0, y, 0xEBF4FF, &lv_font_montserrat_16);
        app_ui_label_fmt(label, "%s.%02u", time, (unsigned)(split / 10000 % 100));
        clock_format_time(time, sizeof(time), elapsed / 1000000);
        label = app_ui_add_text(list, "", LV_ALIGN_TOP_RIGHT, -12, y, 0x9CDCD8, &lv_font_montserrat_16);
        app_ui_label_fmt(label, "%s.%02u", time, (unsigned)(elapsed / 10000 % 100));
    }
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}

static void stopwatch_toggle(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (s_stopwatch.running) s_stopwatch.elapsed_us = stopwatch_elapsed();
    else s_stopwatch.started_us = esp_timer_get_time();
    s_stopwatch.running = !s_stopwatch.running;
    stopwatch_refresh(ui->page_context);
}

static void stopwatch_secondary(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    stopwatch_page_t *page = ui->page_context;
    if (s_stopwatch.running) {
        if (s_stopwatch.lap_count >= 9999) return;
        const unsigned number = ++s_stopwatch.lap_count;
        s_stopwatch.laps[number % (STOPWATCH_LAPS + 1)] = stopwatch_elapsed();
    } else {
        s_stopwatch.elapsed_us = 0;
        s_stopwatch.lap_count = 0;
        s_stopwatch.laps[0] = 0;
    }
    stopwatch_laps(page);
    stopwatch_refresh(page);
}

void app_open_stopwatch(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "秒表");
    stopwatch_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF9C85, app_ui_chinese_font());
        ui->page_back = app_open_clock;
        return;
    }
    clock_tools_setup(ui, page, content);
    lv_obj_t *card = clock_card(content, 0, 112);
    page->readout = app_ui_add_text(card, "", LV_ALIGN_CENTER, -24, -4, 0xEAF2FF, &lv_font_montserrat_48);
    page->fraction = app_ui_add_text(card, "", LV_ALIGN_LEFT_MID, 295, 4, 0x76DAD0, &lv_font_montserrat_28);
    page->secondary_button = clock_button(content, 0, 126, false, "复位", stopwatch_secondary, ui);
    page->secondary = lv_obj_get_child(page->secondary_button, 0);
    page->primary = lv_obj_get_child(clock_button(content, 212, 126, true, "开始", stopwatch_toggle, ui), 0);

    card = clock_card(content, 194, 174);
    const lv_font_t *font = app_ui_chinese_font();
    app_ui_add_text(card, "计次", LV_ALIGN_TOP_LEFT, 12, 8, 0xAFC1DD, font);
    app_ui_add_text(card, "分段用时", LV_ALIGN_TOP_MID, 0, 8, 0xAFC1DD, font);
    app_ui_add_text(card, "累计用时", LV_ALIGN_TOP_RIGHT, -12, 8, 0xAFC1DD, font);
    page->details = lv_obj_create(card);
    lv_obj_set_pos(page->details, 0, 39);
    lv_obj_set_size(page->details, 406, 132);
    lv_obj_set_style_pad_all(page->details, 0, 0);
    lv_obj_set_style_bg_opa(page->details, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page->details, 0, 0);
    lv_obj_set_scroll_dir(page->details, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page->details, LV_SCROLLBAR_MODE_AUTO);
    stopwatch_laps(page);
    stopwatch_refresh(page);
    app_ui_set_timer(ui, stopwatch_tick, 40);
}

static unsigned countdown_selected(const countdown_page_t *page)
{
    return 3600 * lv_dropdown_get_selected(page->fields[0]) +
        60 * lv_dropdown_get_selected(page->fields[1]) + lv_dropdown_get_selected(page->fields[2]);
}

static void countdown_refresh(countdown_page_t *page)
{
    app_countdown_snapshot_t snapshot;
    app_countdown_get(&snapshot);
    const bool idle = snapshot.state == APP_COUNTDOWN_IDLE;
    const bool finished = snapshot.state == APP_COUNTDOWN_FINISHED;
    const bool running = snapshot.state == APP_COUNTDOWN_RUNNING;
    if (idle || finished) lv_obj_remove_flag(page->picker, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(page->picker, LV_OBJ_FLAG_HIDDEN);
    const unsigned seconds = idle ? countdown_selected(page) : snapshot.remaining_seconds;
    char time[16];
    clock_format_time(time, sizeof(time), seconds);
    app_ui_label_text(page->readout, time);
    app_ui_label_text(page->state, page->start_error ? "暂时无法开始" : finished ? "倒计时结束" :
                      running ? "计时中" : idle ? "" : "已暂停");
    app_ui_label_text(page->primary, running ? "暂停" : idle ? "开始" : finished ? "再计一次" : "继续");
    clock_button_enabled(page->reset_button, !idle);
    clock_button_enabled(lv_obj_get_parent(page->primary), (!idle && !finished) || countdown_selected(page) != 0);
    lv_bar_set_value(page->bar, idle ? 100 : (int)(100 * seconds / snapshot.duration_seconds), LV_ANIM_OFF);
}

static void countdown_tick(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    countdown_refresh(ui->page_context);
}

static void countdown_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    countdown_page_t *page = ui->page_context;
    app_countdown_snapshot_t snapshot;
    app_countdown_get(&snapshot);
    const unsigned seconds = countdown_selected(page);
    page->start_error = false;
    if (snapshot.state == APP_COUNTDOWN_RUNNING) app_countdown_pause();
    else if (snapshot.state == APP_COUNTDOWN_PAUSED) app_countdown_resume();
    else if (seconds) page->start_error = app_countdown_start(seconds) != ESP_OK;
    countdown_refresh(page);
}

static void countdown_reset(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    countdown_page_t *page = ui->page_context;
    page->start_error = false;
    app_countdown_reset();
    countdown_refresh(page);
}

static void countdown_edit(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    countdown_page_t *page = ui->page_context;
    page->start_error = false;
    countdown_refresh(page);
}

void app_open_countdown(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "倒计时");
    countdown_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF9C85, app_ui_chinese_font());
        ui->page_back = app_open_clock;
        return;
    }
    clock_tools_setup(ui, page, content);
    lv_obj_t *card = clock_card(content, 0, 138);
    page->readout = app_ui_add_text(card, "", LV_ALIGN_TOP_MID, 0, 13, 0xEAF2FF, &lv_font_montserrat_48);
    page->state = app_ui_add_text(card, "", LV_ALIGN_TOP_MID, 0, 76, 0x99D3D9, app_ui_chinese_font());
    page->bar = lv_bar_create(card);
    lv_obj_set_size(page->bar, 336, 5);
    lv_obj_align(page->bar, LV_ALIGN_BOTTOM_MID, 0, -19);
    lv_obj_set_style_bg_color(page->bar, lv_color_hex(0x335070), LV_PART_MAIN);
    lv_obj_set_style_bg_color(page->bar, lv_color_hex(0x61CEC3), LV_PART_INDICATOR);

    app_countdown_snapshot_t snapshot;
    app_countdown_get(&snapshot);
    const unsigned parts[] = {snapshot.duration_seconds / 3600, snapshot.duration_seconds / 60 % 60,
                              snapshot.duration_seconds % 60};
    const char *const names[] = {"时", "分", "秒"};
    page->picker = clock_card(content, 151, 105);
    for (unsigned i = 0; i < 3; ++i) {
        const int x = 13 + (int)i * 132;
        app_ui_add_text(page->picker, names[i], LV_ALIGN_TOP_LEFT, x + 4, 7, 0xAFC1DD, app_ui_chinese_font());
        lv_obj_t *field = lv_dropdown_create(page->picker);
        page->fields[i] = field;
        lv_obj_set_size(field, 116, 54);
        lv_obj_set_pos(field, x, 36);
        lv_obj_set_style_radius(field, 10, 0);
        lv_obj_set_style_text_font(field, &lv_font_montserrat_24, 0);
        lv_obj_set_style_bg_color(field, lv_color_hex(0x2B4163), 0);
        lv_obj_set_style_text_color(field, lv_color_hex(0xECF3FF), 0);
        lv_obj_set_style_border_width(field, 0, 0);
        char options[180];
        const unsigned count = i == 0 ? 24 : 60;
        for (unsigned value = 0; value < count; ++value) {
            options[3 * value] = '0' + value / 10;
            options[3 * value + 1] = '0' + value % 10;
            options[3 * value + 2] = value + 1 == count ? '\0' : '\n';
        }
        lv_dropdown_set_options(field, options);
        lv_dropdown_set_selected(field, parts[i]);
        lv_obj_add_event_cb(field, countdown_edit, LV_EVENT_VALUE_CHANGED, ui);
    }
    page->reset_button = clock_button(content, 0, 270, false, "复位", countdown_reset, ui);
    page->primary = lv_obj_get_child(clock_button(content, 212, 270, true, "开始", countdown_action, ui), 0);
    countdown_refresh(page);
    app_ui_set_timer(ui, countdown_tick, 150);
}
