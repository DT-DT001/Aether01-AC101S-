#include "app_ui_internal.h"
#include "app_tools.h"

#include <stdlib.h>
#include "esp_random.h"
#include "esp_timer.h"

#define GAME_DURATION_US (30LL * 1000000)

typedef struct {
    uint32_t score;
    int64_t deadline_us;
    int64_t remaining_us;
    bool running;
    bool paused;
    bool finished;
} game_page_t;

static void game_cleanup(app_ui_t *ui)
{
    game_page_t *game = ui->page_context;
    if (game) app_tools_record_score(game->score);
    free(game);
    ui->page_context = NULL;
}

static void move_target(lv_obj_t *target)
{
    /* Keep the entire circle above the message and transport controls. */
    lv_obj_set_pos(target, 4 + (int)(esp_random() % 334U), 78 + (int)(esp_random() % 88U));
}

static void game_expire(app_ui_t *ui)
{
    game_page_t *game = ui->page_context;
    if (!game || !game->running) return;
    const int64_t now = esp_timer_get_time();
    game->remaining_us = game->deadline_us > now ? game->deadline_us - now : 0;
    if (!game->remaining_us) {
        game->running = false;
        game->finished = true;
        app_tools_record_score(game->score);
        lv_obj_add_state(ui->app_obj[1], LV_STATE_DISABLED);
        lv_obj_add_state(ui->app_obj[5], LV_STATE_DISABLED);
        app_ui_label_text(ui->app_obj[4], "暂停");
    }
}

static void game_refresh(app_ui_t *ui)
{
    game_page_t *game = ui->page_context;
    if (!game) return;
    game_expire(ui);
    app_tools_snapshot_t tools;
    app_tools_get_snapshot(&tools);
    const uint32_t tenth_seconds = (uint32_t)((game->remaining_us + 99999) / 100000);
    app_ui_label_fmt(ui->app_obj[0], "剩余 %lu.%lu 秒   得分 %lu\n最高分 %lu",
                     (unsigned long)(tenth_seconds / 10), (unsigned long)(tenth_seconds % 10),
                     (unsigned long)game->score, (unsigned long)tools.best_score);
    const char *state = game->finished ? "时间到！点击重开再试一次" : game->paused ? "已暂停" :
                        game->running ? "点击圆点得分，30 秒挑战" : "点击开始，挑战你的反应速度";
    app_ui_label_fmt(ui->app_obj[2], "%s\n%s", state, !tools.ready ? "正在加载最高分…" :
                     tools.preferences_deferred ? "设置待媒体暂停后保存" : tools.preferences_pending ? "设置后台保存中" :
                     tools.preferences_error != ESP_OK ? "设置存储异常" : "");
}

static void target_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    game_page_t *game = ui->page_context;
    game_expire(ui);
    if (!game || !game->running) return;
    if (game->score < 1000000) ++game->score;
    move_target(ui->app_obj[1]);
    game_refresh(ui);
}

static void game_start(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    game_page_t *game = ui->page_context;
    if (!game) return;
    app_tools_snapshot_t tools;
    app_tools_get_snapshot(&tools);
    if (!tools.ready) {
        app_ui_label_text(ui->app_obj[2], "最高分加载中，请稍候");
        return;
    }
    app_tools_record_score(game->score);
    game->score = 0;
    game->remaining_us = GAME_DURATION_US;
    game->deadline_us = esp_timer_get_time() + GAME_DURATION_US;
    game->running = true;
    game->paused = false;
    game->finished = false;
    lv_obj_remove_state(ui->app_obj[1], LV_STATE_DISABLED);
    lv_obj_remove_state(ui->app_obj[5], LV_STATE_DISABLED);
    app_ui_label_text(ui->app_obj[3], "重开");
    app_ui_label_text(ui->app_obj[4], "暂停");
    move_target(ui->app_obj[1]);
    game_refresh(ui);
}

static void game_pause(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    game_page_t *game = ui->page_context;
    if (!game) return;
    game_expire(ui);
    if (game->running) {
        game->running = false;
        game->paused = true;
        lv_obj_add_state(ui->app_obj[1], LV_STATE_DISABLED);
        app_ui_label_text(ui->app_obj[4], "继续");
    } else if (game->paused && game->remaining_us > 0) {
        game->paused = false;
        game->running = true;
        game->deadline_us = esp_timer_get_time() + game->remaining_us;
        lv_obj_remove_state(ui->app_obj[1], LV_STATE_DISABLED);
        app_ui_label_text(ui->app_obj[4], "暂停");
    }
    game_refresh(ui);
}

static void game_timer(lv_timer_t *timer)
{
    game_refresh(lv_timer_get_user_data(timer));
}

static lv_obj_t *game_button(app_ui_t *ui, const char *text, int x, lv_event_cb_t callback, lv_obj_t **label)
{
    lv_obj_t *button = lv_button_create(ui->content);
    lv_obj_set_pos(button, x, 296);
    lv_obj_set_size(button, 188, 44);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2768D8), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    *label = app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
    return button;
}

void app_open_game(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "反应力小游戏");
    game_page_t *game = calloc(1, sizeof(*game));
    if (!game) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8C8C, app_ui_chinese_font());
        return;
    }
    ui->page_context = game;
    ui->page_cleanup = game_cleanup;
    game->remaining_us = GAME_DURATION_US;
    ui->app_obj[0] = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 0, 0xC9DCF8, app_ui_chinese_font());
    lv_obj_set_style_text_align(ui->app_obj[0], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(ui->app_obj[0], 6, 0);
    lv_obj_t *target = lv_button_create(content);
    lv_obj_set_size(target, 66, 66);
    lv_obj_set_style_radius(target, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(target, lv_color_hex(0xE64646), 0);
    lv_obj_add_event_cb(target, target_event, LV_EVENT_CLICKED, ui);
    lv_obj_add_state(target, LV_STATE_DISABLED);
    app_ui_add_text(target, "点我", LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
    ui->app_obj[1] = target;
    ui->app_obj[2] = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 242, 0x8FA9CD, app_ui_chinese_font());
    lv_obj_set_style_text_align(ui->app_obj[2], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ui->app_obj[2], 402);
    game_button(ui, "开始", 4, game_start, &ui->app_obj[3]);
    ui->app_obj[5] = game_button(ui, "暂停", 210, game_pause, &ui->app_obj[4]);
    lv_obj_add_state(ui->app_obj[5], LV_STATE_DISABLED);
    move_target(target);
    app_ui_set_timer(ui, game_timer, 100);
    game_refresh(ui);
}
