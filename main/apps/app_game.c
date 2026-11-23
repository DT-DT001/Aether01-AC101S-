#include "app_connection_style.h"
#include "app_arcade.h"
#include "app_motion_filter.h"
#include "app_telemetry.h"

#include <math.h>
#include <stdlib.h>
#include "esp_random.h"
#include "esp_timer.h"

typedef enum { GAME_TILES, GAME_CRATES, GAME_SNAKE, GAME_JUMP, GAME_COUNT } game_kind_t;

static const char *const game_titles[GAME_COUNT] = {
    "2048", "推箱子", "贪吃蛇", "跳一跳",
};

typedef struct {
    app_ui_t *ui;
    game_kind_t kind;
    union {
        arcade_tiles_t tiles;
        arcade_crates_t crates;
        arcade_snake_t snake;
        arcade_jump_t jump;
    } model;
    lv_obj_t *board;
    lv_obj_t *touch_surface;
    lv_obj_t *score;
    lv_obj_t *hint;
    lv_obj_t *cells[16];
    lv_obj_t *numbers[16];
    lv_obj_t *touch_cues[4];
    lv_obj_t *undo;
    lv_obj_t *next_level;
    lv_obj_t *mode_label;
    lv_obj_t *recenter;
    lv_obj_t *pause;
    lv_obj_t *pause_label;
    lv_obj_t *start_overlay;
    lv_obj_t *start_note;
    lv_obj_t *start_button;
    lv_obj_t *start_label;
    lv_obj_t *platforms[3];
    lv_obj_t *jumper;
    lv_obj_t *aim;
    lv_obj_t *aim_target;
    lv_obj_t *power_fill;
    lv_indev_t *jump_input;
    lv_indev_t *swipe_input;
    lv_point_t swipe_origin;
    arcade_tilt_t tilt;
    app_motion_filter_t attitude;
    uint32_t snake_due_ms;
    uint32_t sleep_ms;
    uint8_t displayed_exponents[16];
    uint8_t level;
    uint8_t neutral_samples;
    int8_t sensor_status;
    bool tilt_enabled;
    bool neutral_set;
    bool snake_start_requested;
    bool snake_started;
    bool paused;
    bool sleeping;
} game_page_t;

enum {
    BOARD_INK = 0x0B1425,
    BUTTON_INK = 0x273655,
    TEXT_BRIGHT = 0xF0F6FF,
    TEXT_MUTED = 0xABBFD8,
    TILE_SIDE = 68,
    TILE_PITCH = 72,
    TILE_WIDTH = 292,
    CRATE_SIDE = 40,
    SNAKE_SIDE = 18,
    SNAKE_PAD_X = 5,
    SNAKE_PAD_Y = 6,
    REGION_DEAD_PERCENT = 4,
    JUMP_ORIGIN_X = 67,
    JUMP_FLOOR_Y = 211,
    JUMP_POWER_WIDTH = 376,
    TILT_POLL_MS = 10,
    TILE_SWIPE_DISTANCE = 30,
    SNAKE_START_GRACE_MS = 500,
};

static void open_game(app_ui_t *ui, game_kind_t kind);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int width, int height,
                       int radius, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height,
                        const char *caption, const lv_font_t *font,
                        lv_event_cb_t callback, void *context)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(BUTTON_INK), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x39517A), LV_STATE_PRESSED);
    lv_obj_set_style_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
    if (caption) app_ui_add_text(obj, caption, LV_ALIGN_CENTER, 0, 0, TEXT_BRIGHT, font);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, context);
    return obj;
}

static void set_enabled(lv_obj_t *obj, bool enabled)
{
    if (enabled) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void draw_rect(lv_layer_t *layer, int x, int y, int width, int height,
                      int radius, uint32_t color)
{
    const lv_area_t area = {x, y, x + width - 1, y + height - 1};
    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.bg_color = lv_color_hex(color);
    descriptor.radius = radius;
    lv_draw_rect(layer, &descriptor, &area);
}

static void tiles_render(game_page_t *page)
{
    static const uint32_t palette[] = {
        0x273354, 0xDBE8FF, 0xB7E5FD, 0x88DCCE, 0x4FD4AD, 0x9EDB77,
        0xE1D871, 0xFFBF72, 0xFF9A78, 0xFC86A4, 0xE28AE8, 0xAB95FF,
    };
    const arcade_tiles_t *tiles = &page->model.tiles;
    for (unsigned i = 0; i < 16; ++i) {
        const uint8_t exponent = tiles->cells[i];
        if (page->displayed_exponents[i] == exponent) continue;
        page->displayed_exponents[i] = exponent;
        const unsigned index = exponent > 11 ? 11 : exponent;
        lv_obj_set_style_bg_color(page->cells[i], lv_color_hex(palette[index]), 0);
        lv_obj_set_style_text_color(page->numbers[i], lv_color_hex(0x202744), 0);
        lv_obj_set_style_text_font(page->numbers[i], exponent > 12 ? &lv_font_montserrat_20 :
                                   &lv_font_montserrat_28, 0);
        if (exponent > 16) app_ui_label_fmt(page->numbers[i], "2^%u", (unsigned)exponent);
        else if (exponent) app_ui_label_fmt(page->numbers[i], "%lu", (unsigned long)(UINT32_C(1) << exponent));
        else app_ui_label_text(page->numbers[i], "");
    }
    app_ui_label_fmt(page->score, "得分  %lu", (unsigned long)tiles->score);
}

static void tiles_render_status(game_page_t *page)
{
    const arcade_tiles_t *game = &page->model.tiles;
    const char *hint = "滑动棋盘";
    if (game->over) hint = "无可用移动  ·  点击重开";
    else if (page->tilt_enabled && page->sensor_status < 0) hint = "传感器暂不可用 · 可切回滑动";
    else if (page->tilt_enabled && !page->neutral_set) hint = "请拿稳设备";
    else if (game->won) hint = "达到 2048";
    else if (page->tilt_enabled) hint = "轻倾移块，回正后再倾";
    app_ui_label_text(page->hint, hint);
}

static void crates_draw(lv_event_t *event)
{
    const game_page_t *page = lv_event_get_user_data(event);
    const arcade_crates_t *game = &page->model.crates;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(page->board, &area);
    for (unsigned cell = 0; cell < ARCADE_CRATE_SIDE * ARCADE_CRATE_SIDE; ++cell) {
        const uint64_t bit = UINT64_C(1) << cell;
        const int x = area.x1 + (cell % ARCADE_CRATE_SIDE) * CRATE_SIDE;
        const int y = area.y1 + (cell / ARCADE_CRATE_SIDE) * CRATE_SIDE;
        if (game->walls & bit) {
            draw_rect(layer, x + 1, y + 1, 38, 38, 6, 0x40587A);
            draw_rect(layer, x + 5, y + 5, 21, 3, 1, 0x6785A9);
            continue;
        }
        const bool goal = (game->goals & bit) != 0;
        draw_rect(layer, x, y, CRATE_SIDE, CRATE_SIDE, 0,
                  (cell / ARCADE_CRATE_SIDE + cell % ARCADE_CRATE_SIDE) % 2 ? 0x16283B : 0x192E45);
        if (goal) {
            draw_rect(layer, x + 9, y + 9, 22, 22, LV_RADIUS_CIRCLE, 0x50DEC2);
            draw_rect(layer, x + 11, y + 11, 9, 9, LV_RADIUS_CIRCLE, 0x172A3F);
        }
        if (game->turn.boxes & bit) {
            draw_rect(layer, x + 4, y + 4, 32, 32, 6, goal ? 0x338879 : 0x94612F);
            draw_rect(layer, x + 5, y + 5, 30, 26, 5, goal ? 0x5FDFBE : 0xFFCA75);
            draw_rect(layer, x + 10, y + 10, 20, 4, 1, goal ? 0x278877 : 0xB57639);
            draw_rect(layer, x + 10, y + 21, 20, 4, 1, goal ? 0x278877 : 0xB57639);
        }
        if (game->turn.player == cell) {
            draw_rect(layer, x + 6, y + 5, 28, 30, 10, 0x9EA7FF);
            draw_rect(layer, x + 10, y + 10, 20, 12, 5, 0x262C5C);
            draw_rect(layer, x + 14, y + 13, 4, 5, 1, 0xEBF9FF);
            draw_rect(layer, x + 22, y + 13, 4, 5, 1, 0xEBF9FF);
        }
    }
}

static void crates_render(game_page_t *page)
{
    const arcade_crates_t *game = &page->model.crates;
    const bool solved = arcade_crates_solved(game);
    app_ui_label_fmt(page->score, "%u / %u  ·  %lu 步", page->level + 1U,
                     arcade_crates_levels(), (unsigned long)game->turn.moves);
    app_ui_label_text(page->hint, solved ? (page->level + 1U == arcade_crates_levels() ?
                      "全部通关" : "通过  ·  点击右箭头") : "把木箱推到亮点");
    set_enabled(page->undo, game->count > 0);
    set_enabled(page->next_level, solved && page->level + 1U < arcade_crates_levels());
    lv_obj_invalidate(page->board);
}

static void snake_coordinates(const lv_area_t *area, unsigned cell, int *x, int *y)
{
    *x = area->x1 + SNAKE_PAD_X + (int)(cell % ARCADE_SNAKE_COLUMNS) * SNAKE_SIDE;
    *y = area->y1 + SNAKE_PAD_Y + (int)(cell / ARCADE_SNAKE_COLUMNS) * SNAKE_SIDE;
}

static void snake_draw(lv_event_t *event)
{
    const game_page_t *page = lv_event_get_user_data(event);
    const arcade_snake_t *game = &page->model.snake;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(page->board, &area);
    const int width = ARCADE_SNAKE_COLUMNS * SNAKE_SIDE;
    const int height = ARCADE_SNAKE_ROWS * SNAKE_SIDE;
    const int grid_x = area.x1 + SNAKE_PAD_X;
    const int grid_y = area.y1 + SNAKE_PAD_Y;
    draw_rect(layer, grid_x - 1, grid_y - 1, width + 2, height + 2, 1, 0x31536C);
    draw_rect(layer, grid_x, grid_y, width, height, 0, 0x12243A);
    for (int col = 1; col < ARCADE_SNAKE_COLUMNS; ++col) {
        draw_rect(layer, grid_x + col * SNAKE_SIDE, grid_y, 1, height, 0, 0x1C3248);
    }
    for (int row = 1; row < ARCADE_SNAKE_ROWS; ++row) {
        draw_rect(layer, grid_x, grid_y + row * SNAKE_SIDE, width, 1, 0, 0x1C3248);
    }
    if (game->food != ARCADE_SNAKE_NO_CELL) {
        int x, y;
        snake_coordinates(&area, game->food, &x, &y);
        draw_rect(layer, x + 3, y + 5, 11, 10, 5, 0xFD7C94);
        draw_rect(layer, x + 8, y + 2, 6, 3, 2, 0xA4EAA6);
        draw_rect(layer, x + 5, y + 7, 3, 3, 2, 0xFFD4DA);
    }
    for (unsigned segment = game->length; segment > 1; --segment) {
        int x, y;
        snake_coordinates(&area, arcade_snake_cell(game, segment - 1), &x, &y);
        draw_rect(layer, x + 1, y + 1, 16, 16, 5, 0x41BB9B);
        draw_rect(layer, x + 4, y + 3, 9, 3, 2, 0x78E5C4);
    }
    int x, y;
    snake_coordinates(&area, arcade_snake_cell(game, 0), &x, &y);
    draw_rect(layer, x + 1, y + 1, 16, 16, 5, 0x9AEBC4);
    const arcade_direction_t direction = game->direction == ARCADE_NONE ? ARCADE_RIGHT : game->direction;
    if (direction == ARCADE_LEFT || direction == ARCADE_RIGHT) {
        const int eye = direction == ARCADE_RIGHT ? 10 : 4;
        draw_rect(layer, x + eye, y + 4, 3, 3, 1, 0x13394D);
        draw_rect(layer, x + eye, y + 10, 3, 3, 1, 0x13394D);
    } else {
        const int eye = direction == ARCADE_DOWN ? 10 : 4;
        draw_rect(layer, x + 4, y + eye, 3, 3, 1, 0x13394D);
        draw_rect(layer, x + 10, y + eye, 3, 3, 1, 0x13394D);
    }
}

static void snake_invalidate(game_page_t *page, uint16_t cell)
{
    if (cell == ARCADE_SNAKE_NO_CELL) return;
    lv_area_t board;
    lv_obj_get_coords(page->board, &board);
    int x, y;
    snake_coordinates(&board, cell, &x, &y);
    const lv_area_t dirty = {x, y, x + SNAKE_SIDE - 1, y + SNAKE_SIDE - 1};
    lv_obj_invalidate_area(page->board, &dirty);
}

static void snake_render_status(game_page_t *page)
{
    const arcade_snake_t *game = &page->model.snake;
    app_ui_label_fmt(page->score, "得分  %u", (unsigned)(game->length - 3U));
    const bool playing = page->snake_started && !game->over;
    set_enabled(page->pause, playing);
    for (unsigned i = 0; i < 4; ++i) set_visible(page->touch_cues[i], !page->tilt_enabled);
    app_ui_label_text(page->pause_label, page->paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    const char *sensor = page->sensor_status < 0 ? "传感器暂不可用" : "请拿稳设备";
    const char *hint = "";
    set_visible(page->start_overlay, !playing);
    if (!playing) {
        const char *note = page->tilt_enabled ? "倾斜转向" : "点击棋盘边缘转向";
        if (game->over) note = game->won ? "满屏通关" : "游戏结束";
        else if (page->tilt_enabled && !page->neutral_set) note = sensor;
        app_ui_label_text(page->start_note, note);
        app_ui_label_text(page->start_label, game->over ? "再玩一次" :
                          page->snake_start_requested ? "等待校准" : "开始游戏");
        set_enabled(page->start_button, !page->snake_start_requested);
    } else if (page->paused) {
        hint = "已暂停";
    } else if (page->tilt_enabled && !page->neutral_set) {
        hint = sensor;
    }
    app_ui_label_text(page->hint, hint);
    set_visible(page->hint, *hint != '\0');
}

static void snake_tick(game_page_t *page, uint32_t now)
{
    arcade_snake_t *game = &page->model.snake;
    if (game->over || !page->snake_started || page->paused ||
        (page->tilt_enabled && (!page->neutral_set || page->sensor_status <= 0)) ||
        (int32_t)(now - page->snake_due_ms) < 0) return;
    if (game->next == ARCADE_NONE) arcade_snake_direction(game, ARCADE_RIGHT);
    const uint16_t old_head = arcade_snake_cell(game, 0);
    const uint16_t old_tail = arcade_snake_cell(game, game->length - 1);
    const uint16_t old_food = game->food;
    if (arcade_snake_step(game)) {
        snake_invalidate(page, old_head);
        snake_invalidate(page, arcade_snake_cell(game, 0));
        snake_invalidate(page, old_tail);
        if (game->food != old_food) snake_invalidate(page, game->food);
    }
    page->snake_due_ms = now + arcade_snake_interval(game);
    if (game->food != old_food || game->over) snake_render_status(page);
}

static void jump_platform_place(lv_obj_t *base, arcade_platform_t platform, int x)
{
    const int width = platform.radius * 2 + 8;
    static const uint32_t ledge[] = {0xB197FF, 0x8FBCFF, 0x66E0DA, 0xF6A7C8, 0xFFCB80};
    static const uint32_t body[] = {0x684FB6, 0x426CB8, 0x278A8F, 0xA65F8B, 0xAA7046};
    const unsigned index = platform.height / 9;
    lv_obj_set_pos(base, x - width / 2, JUMP_FLOOR_Y - platform.height);
    lv_obj_set_width(base, width);
    lv_obj_set_style_bg_color(base, lv_color_hex(body[index]), 0);
    lv_obj_t *top = lv_obj_get_child(base, 0);
    lv_obj_set_width(top, width);
    lv_obj_set_style_bg_color(top, lv_color_hex(ledge[index]), 0);
    lv_obj_set_width(lv_obj_get_child(base, 1), width - 18);
}

static void jump_render(game_page_t *page, uint32_t now, bool scene_changed)
{
    const arcade_jump_t *game = &page->model.jump;
    const arcade_jump_pose_t pose = arcade_jump_pose(game, now);
    if (scene_changed || game->phase == ARCADE_JUMP_PAN) {
        const int start = JUMP_ORIGIN_X - pose.camera_x;
        jump_platform_place(page->platforms[0], game->source, start);
        jump_platform_place(page->platforms[1], game->target, start + game->target.gap);
        const bool pan = game->phase == ARCADE_JUMP_PAN;
        set_visible(page->platforms[2], pan);
        if (pan) jump_platform_place(page->platforms[2], game->upcoming,
                                     start + game->target.gap + game->upcoming.gap);
    }
    lv_obj_set_pos(page->jumper, JUMP_ORIGIN_X + pose.player_x - pose.camera_x - 13,
                   JUMP_FLOOR_Y - pose.player_y - 28);
    const bool charging = game->phase == ARCADE_JUMP_CHARGING;
    set_visible(page->aim, charging);
    set_visible(page->aim_target, charging);
    set_visible(page->power_fill, charging);
    if (charging) {
        const int distance = arcade_jump_distance(pose.power);
        lv_obj_set_x(page->aim, JUMP_ORIGIN_X + distance - 4);
        if (scene_changed) {
            lv_obj_set_x(page->aim_target, JUMP_ORIGIN_X + game->target.gap - game->target.radius);
            lv_obj_set_width(page->aim_target, 2 * game->target.radius + 1);
        }
        const int offset = distance - (int)game->target.gap;
        lv_obj_set_style_bg_color(page->aim, lv_color_hex(abs(offset) <= game->target.radius ?
                                  0x8EF7CC : 0xF6B1C2), 0);
        const int fill = pose.power * JUMP_POWER_WIDTH / 1000;
        lv_obj_set_width(page->power_fill, fill > 0 ? fill : 1);
    }
    if (scene_changed) {
        app_ui_label_fmt(page->score, "得分  %lu", (unsigned long)game->score);
        const char *hint = "按住蓄力，松手跳跃";
        if (game->phase == ARCADE_JUMP_OVER) hint = "点击游戏区再来一次";
        else if (game->phase == ARCADE_JUMP_PAN && game->perfect) hint = "完美落点  +2";
        else if (game->phase != ARCADE_JUMP_READY) hint = "";
        app_ui_label_text(page->hint, hint);
    }
}

static void game_timer(lv_timer_t *timer);

static void jump_pointer(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    arcade_jump_t *game = &page->model.jump;
    const uint32_t now = now_ms();
    lv_indev_t *input = lv_event_get_param(event);
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
        if (game->phase == ARCADE_JUMP_OVER) arcade_jump_init(game, esp_random());
        if (game->phase != ARCADE_JUMP_READY) return;
        page->jump_input = input;
        arcade_jump_press(game, now);
        app_ui_set_timer(page->ui, game_timer, 25);
        break;
    case LV_EVENT_RELEASED:
        if (page->jump_input != input) return;
        page->jump_input = NULL;
        arcade_jump_release(game, now);
        break;
    case LV_EVENT_PRESS_LOST:
    case LV_EVENT_INDEV_RESET:
        if (page->jump_input != input) return;
        page->jump_input = NULL;
        arcade_jump_cancel(game);
        if (game->phase == ARCADE_JUMP_READY) app_ui_clear_timer(page->ui);
        break;
    default:
        return;
    }
    jump_render(page, now, true);
}

static void set_tilt(game_page_t *page, bool enabled)
{
    page->tilt_enabled = enabled;
    page->swipe_input = NULL;
    page->neutral_set = false;
    page->neutral_samples = 0;
    page->sensor_status = 0;
    page->tilt = (arcade_tilt_t){0};
    app_motion_filter_init(&page->attitude);
    const bool over = page->kind == GAME_TILES ? page->model.tiles.over : page->model.snake.over;
    app_telemetry_set_interest(enabled && !over ? APP_TELEMETRY_IMU : 0);
}

static void snake_start_when_ready(game_page_t *page, uint32_t now)
{
    if (!page->snake_start_requested ||
        (page->tilt_enabled && (!page->neutral_set || page->sensor_status <= 0))) return;
    page->snake_start_requested = false;
    page->snake_started = true;
    page->tilt = (arcade_tilt_t){0};
    page->snake_due_ms = now + SNAKE_START_GRACE_MS;
}

static bool move_game(game_page_t *page, arcade_direction_t direction)
{
    if (direction == ARCADE_NONE) return false;
    if (page->kind == GAME_TILES) {
        const bool moved = arcade_tiles_move(&page->model.tiles, direction);
        if (moved) tiles_render(page);
        tiles_render_status(page);
        if (page->model.tiles.over) {
            app_telemetry_set_interest(0);
            app_ui_clear_timer(page->ui);
        }
        return moved;
    } else if (page->kind == GAME_CRATES) {
        if (!arcade_crates_move(&page->model.crates, direction)) return false;
        crates_render(page);
        return true;
    } else if (page->kind == GAME_SNAKE && page->snake_started && !page->paused) {
        return arcade_snake_direction(&page->model.snake, direction);
    }
    return false;
}

static void tiles_pointer(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    lv_indev_t *input = lv_event_get_param(event);
    const lv_event_code_t code = lv_event_get_code(event);
    const bool enabled = !page->tilt_enabled && !page->model.tiles.over &&
                         !page->sleeping && !app_power_is_sleeping(page->ui->power);
    if (code == LV_EVENT_PRESSED) {
        if (!enabled || !input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
        page->swipe_input = input;
        lv_indev_get_point(input, &page->swipe_origin);
        return;
    }
    if (!page->swipe_input || page->swipe_input != input) return;
    page->swipe_input = NULL;
    if (!enabled || code != LV_EVENT_RELEASED) return;

    lv_point_t point;
    lv_indev_get_point(input, &point);
    const int32_t dx = point.x - page->swipe_origin.x;
    const int32_t dy = point.y - page->swipe_origin.y;
    const bool horizontal = abs(dx) > abs(dy);
    const int32_t primary = abs(horizontal ? dx : dy);
    const int32_t secondary = abs(horizontal ? dy : dx);
    if (primary < TILE_SWIPE_DISTANCE || secondary * 4 > primary * 3) return;
    const arcade_direction_t direction = horizontal ? (dx < 0 ? ARCADE_LEFT : ARCADE_RIGHT) :
                                                    (dy < 0 ? ARCADE_UP : ARCADE_DOWN);
    move_game(page, direction);
}

static void tilt_sensor_wait(game_page_t *page, int8_t status)
{
    if (page->sensor_status == status) return;
    page->neutral_set = false;
    page->neutral_samples = 0;
    page->tilt = (arcade_tilt_t){0};
    app_motion_filter_init(&page->attitude);
    page->sensor_status = status;
    if (page->kind == GAME_TILES) tiles_render_status(page);
    else snake_render_status(page);
}

static void tilt_tick(game_page_t *page, uint32_t now)
{
    if (!page->tilt_enabled) return;
    if (page->kind == GAME_SNAKE && page->model.snake.over) return;
    if (page->paused) return;
    app_telemetry_snapshot_t sample;
    app_telemetry_get(&sample);
    const uint32_t age_ms = now_ms() - sample.imu_updated_ms;
    const int8_t status = sample.imu_error == ESP_OK && age_ms < 150U ? 1 :
                          sample.imu_error == ESP_ERR_NOT_FINISHED ? 0 : -1;
    if (status <= 0) {
        tilt_sensor_wait(page, status);
        return;
    }
    if (page->attitude.initialized && (int32_t)(sample.imu_updated_ms - page->attitude.stamp) <= 0) return;
    if (!app_motion_filter_update_tilt(&page->attitude, sample.imu.accel_mps2, sample.imu.gyro_dps,
                                       sample.imu_updated_ms, sample.imu.gyro_bias_valid)) {
        tilt_sensor_wait(page, -1);
        return;
    }
    const bool recovered = page->sensor_status != status;
    page->sensor_status = status;
    if (recovered) {
        if (page->kind == GAME_TILES) tiles_render_status(page);
        else snake_render_status(page);
    }
    if (!page->neutral_set) {
        if (page->attitude.rotation > 18 || fabsf(page->attitude.acceleration - 9.81f) > 1.8f) {
            page->neutral_samples = 0;
            return;
        }
        if (++page->neutral_samples < 3) return;
        app_motion_filter_zero(&page->attitude);
        page->neutral_set = true;
        if (page->kind == GAME_TILES) {
            tiles_render_status(page);
        } else {
            if (page->snake_started) page->snake_due_ms = now + arcade_snake_interval(&page->model.snake);
            snake_start_when_ready(page, now);
            snake_render_status(page);
        }
    }
    const bool tiles = page->kind == GAME_TILES;
    if (!tiles && !page->snake_started) return;
    const arcade_direction_t previous_tilt = page->tilt.held;
    /* In the AC101S screen orientation, positive pitch points to the right. */
    const arcade_direction_t direction = arcade_tilt_update(&page->tilt,
                                         page->attitude.pitch, page->attitude.roll, tiles);
    if (move_game(page, direction)) {
        lv_display_trigger_activity(lv_obj_get_display(page->board));
    } else if (!tiles && direction != ARCADE_NONE &&
               page->model.snake.next != page->model.snake.direction &&
               page->model.snake.next != direction) {
        /* Let a confirmed turn retry once Snake has used its pending turn. */
        page->tilt.held = previous_tilt;
    }
}

static void game_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    game_page_t *page = ui->page_context;
    if (!page) return;
    const uint32_t now = now_ms();
    if (app_power_is_sleeping(ui->power)) {
        if (!page->sleeping) {
            page->sleeping = true;
            page->sleep_ms = now;
            page->swipe_input = NULL;
            if (page->kind == GAME_JUMP) {
                page->jump_input = NULL;
                arcade_jump_cancel(&page->model.jump);
            }
        }
        return;
    }
    if (page->sleeping) {
        const uint32_t sleep = now - page->sleep_ms;
        page->sleeping = false;
        if (page->tilt_enabled) set_tilt(page, true);
        if (page->kind == GAME_SNAKE) page->snake_due_ms += sleep;
        if (page->kind == GAME_JUMP) {
            page->model.jump.phase_ms += sleep;
            jump_render(page, now, true);
        }
    }
    if (page->kind == GAME_JUMP) {
        const arcade_jump_phase_t old = page->model.jump.phase;
        arcade_jump_step(&page->model.jump, now);
        if (old != page->model.jump.phase || (old != ARCADE_JUMP_OVER && old != ARCADE_JUMP_READY)) {
            jump_render(page, now, old != page->model.jump.phase);
        }
        if (page->model.jump.phase == ARCADE_JUMP_READY || page->model.jump.phase == ARCADE_JUMP_OVER) {
            app_ui_clear_timer(ui);
        }
    } else {
        tilt_tick(page, now);
        if (page->kind == GAME_SNAKE) {
            snake_tick(page, now);
            if (page->model.snake.over) {
                app_telemetry_set_interest(0);
                app_ui_clear_timer(ui);
            }
        }
    }
}

static void direction_point(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    lv_indev_t *input = lv_event_get_param(event);
    if (lv_event_get_target_obj(event) != page->touch_surface || !input ||
        lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER || page->sleeping ||
        app_power_is_sleeping(page->ui->power)) return;
    if (page->kind == GAME_SNAKE && (page->tilt_enabled || !page->snake_started ||
                                    page->paused || page->model.snake.over)) return;

    lv_area_t area;
    lv_point_t point;
    lv_obj_get_coords(page->touch_surface, &area);
    lv_indev_get_point(input, &point);
    if (point.x < area.x1 || point.x > area.x2 || point.y < area.y1 || point.y > area.y2) return;
    const int32_t width = lv_area_get_width(&area);
    const int32_t height = lv_area_get_height(&area);
    const int32_t x = (2 * point.x - area.x1 - area.x2) * height;
    const int32_t y = (2 * point.y - area.y1 - area.y2) * width;
    const int32_t dead = width * height * REGION_DEAD_PERCENT;
    if (abs(x) * 100 < dead && abs(y) * 100 < dead) return;
    const arcade_direction_t direction = abs(x) > abs(y) ?
        (x < 0 ? ARCADE_LEFT : ARCADE_RIGHT) : (y < 0 ? ARCADE_UP : ARCADE_DOWN);
    move_game(page, direction);
}

static void add_touch_regions(game_page_t *page)
{
    static const char *const arrows[] = {LV_SYMBOL_UP, LV_SYMBOL_RIGHT, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT};
    static const lv_align_t sides[] = {LV_ALIGN_TOP_MID, LV_ALIGN_RIGHT_MID,
                                       LV_ALIGN_BOTTOM_MID, LV_ALIGN_LEFT_MID};
    static const lv_point_t insets[] = {{0, 5}, {-5, 0}, {0, -5}, {5, 0}};
    lv_obj_t *screen = page->ui->screen;
    lv_display_t *display = lv_obj_get_display(screen);
    lv_obj_update_layout(page->board);
    lv_area_t board;
    lv_obj_get_coords(page->board, &board);
    page->touch_surface = lv_obj_create(screen);
    lv_obj_remove_style_all(page->touch_surface);
    lv_obj_set_pos(page->touch_surface, 0, board.y1);
    lv_obj_set_size(page->touch_surface, lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display) - board.y1);
    lv_obj_remove_flag(page->touch_surface, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page->touch_surface, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    /* Keep the board's buttons ahead of the surface in pointer hit testing. */
    lv_obj_move_background(page->touch_surface);
    lv_obj_remove_flag(page->ui->content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page->touch_surface, direction_point, LV_EVENT_PRESSED, page);
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_t *cue = app_ui_add_text(page->board, arrows[i], sides[i], insets[i].x, insets[i].y,
                                        0xB7CDDF, &lv_font_montserrat_16);
        lv_obj_remove_flag(cue, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_opa(cue, LV_OPA_60, 0);
        page->touch_cues[i] = cue;
    }
}

static void crates_undo_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    if (arcade_crates_undo(&page->model.crates)) crates_render(page);
}

static void crates_next_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    if (!arcade_crates_solved(&page->model.crates) || page->level + 1U >= arcade_crates_levels()) return;
    arcade_crates_init(&page->model.crates, ++page->level);
    crates_render(page);
}

static void tiles_set_mode(game_page_t *page, bool tilt)
{
    set_tilt(page, tilt);
    app_ui_label_text(page->mode_label, tilt ? "倾斜" : "滑动");
    set_visible(page->recenter, tilt);
    tiles_render_status(page);
}

static void tiles_mode_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    tiles_set_mode(page, !page->tilt_enabled);
}

static void snake_mode_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    const bool tilt = !page->tilt_enabled;
    set_tilt(page, tilt);
    const uint32_t now = now_ms();
    if (!tilt && page->snake_started) page->snake_due_ms = now + arcade_snake_interval(&page->model.snake);
    app_ui_label_text(page->mode_label, tilt ? "模式 · 倾斜" : "模式 · 触摸");
    snake_start_when_ready(page, now);
    snake_render_status(page);
}

static void snake_pause_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    if (page->model.snake.over || !page->snake_started) return;
    page->paused = !page->paused;
    page->snake_due_ms = now_ms() + arcade_snake_interval(&page->model.snake);
    if (!page->paused && page->tilt_enabled) set_tilt(page, true);
    snake_render_status(page);
}

static void snake_reset(game_page_t *page)
{
    arcade_snake_init(&page->model.snake, esp_random());
    page->snake_started = false;
    page->snake_start_requested = false;
    page->paused = false;
    set_tilt(page, page->tilt_enabled);
    snake_render_status(page);
    lv_obj_invalidate(page->board);
}

static void snake_start_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    if (page->model.snake.over) {
        snake_reset(page);
        app_ui_set_timer(page->ui, game_timer, TILT_POLL_MS);
    }
    if (page->snake_started || page->snake_start_requested) return;
    page->snake_start_requested = true;
    snake_start_when_ready(page, now_ms());
    snake_render_status(page);
}

static void zero_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    if (!page->tilt_enabled || page->model.tiles.over) return;
    tiles_set_mode(page, true);
}

static void restart_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    switch (page->kind) {
    case GAME_TILES:
        arcade_tiles_init(&page->model.tiles, esp_random());
        tiles_set_mode(page, page->tilt_enabled);
        tiles_render(page);
        app_ui_set_timer(page->ui, game_timer, TILT_POLL_MS);
        break;
    case GAME_CRATES:
        arcade_crates_init(&page->model.crates, page->level);
        crates_render(page);
        break;
    case GAME_SNAKE:
        snake_reset(page);
        app_ui_set_timer(page->ui, game_timer, TILT_POLL_MS);
        break;
    case GAME_JUMP:
        page->jump_input = NULL;
        arcade_jump_init(&page->model.jump, esp_random());
        jump_render(page, now_ms(), true);
        app_ui_clear_timer(page->ui);
        break;
    default:
        break;
    }
}

static void build_tiles(game_page_t *page)
{
    lv_obj_t *parent = page->ui->content;
    page->board = block(parent, 0, 37, TILE_WIDTH, TILE_WIDTH, 15, BOARD_INK);
    lv_obj_align(page->board, LV_ALIGN_TOP_MID, 0, 37);
    lv_obj_add_flag(page->board, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(page->board, tiles_pointer, LV_EVENT_PRESSED, page);
    lv_obj_add_event_cb(page->board, tiles_pointer, LV_EVENT_RELEASED, page);
    lv_obj_add_event_cb(page->board, tiles_pointer, LV_EVENT_PRESS_LOST, page);
    lv_obj_add_event_cb(page->board, tiles_pointer, LV_EVENT_INDEV_RESET, page);
    for (unsigned i = 0; i < 16; ++i) {
        page->cells[i] = block(page->board, 4 + (int)(i % 4) * TILE_PITCH,
                               4 + (int)(i / 4) * TILE_PITCH, TILE_SIDE, TILE_SIDE, 10, 0x273354);
        page->numbers[i] = app_ui_add_text(page->cells[i], "", LV_ALIGN_CENTER, 0, 0,
                                           0x202744, &lv_font_montserrat_28);
    }
    lv_obj_t *mode = button(parent, 172, 0, 62, 34, "滑动", app_ui_chinese_font(), tiles_mode_click, page);
    page->mode_label = lv_obj_get_child(mode, 0);
    page->recenter = button(parent, 241, 0, 43, 34, LV_SYMBOL_GPS, &lv_font_montserrat_20, zero_click, page);
    arcade_tiles_init(&page->model.tiles, esp_random());
    tiles_render(page);
    tiles_set_mode(page, false);
}

static void build_crates(game_page_t *page)
{
    lv_obj_t *parent = page->ui->content;
    page->board = block(parent, 0, 34, CRATE_SIDE * ARCADE_CRATE_SIDE,
                        CRATE_SIDE * ARCADE_CRATE_SIDE, 8, BOARD_INK);
    lv_obj_align(page->board, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_add_event_cb(page->board, crates_draw, LV_EVENT_DRAW_MAIN, page);
    add_touch_regions(page);
    page->undo = button(parent, 154, 0, 63, 34, "撤销", app_ui_chinese_font(), crates_undo_click, page);
    page->next_level = button(parent, 225, 0, 54, 34, LV_SYMBOL_NEXT, &lv_font_montserrat_20,
                              crates_next_click, page);
    lv_obj_align(page->hint, LV_ALIGN_TOP_MID, 0, 355);
    arcade_crates_init(&page->model.crates, 0);
    crates_render(page);
}

static void build_snake(game_page_t *page)
{
    lv_obj_t *parent = page->ui->content;
    page->board = block(parent, 0, 38, ARCADE_SNAKE_COLUMNS * SNAKE_SIDE + 2 * SNAKE_PAD_X,
                        ARCADE_SNAKE_ROWS * SNAKE_SIDE + 2 * SNAKE_PAD_Y, 11, BOARD_INK);
    lv_obj_align(page->board, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_add_event_cb(page->board, snake_draw, LV_EVENT_DRAW_MAIN, page);
    add_touch_regions(page);
    lv_obj_t *mode = button(parent, 102, 0, 113, 34, "模式 · 触摸", app_ui_chinese_font(), snake_mode_click, page);
    page->mode_label = lv_obj_get_child(mode, 0);
    page->pause = button(parent, 229, 0, 51, 34, LV_SYMBOL_PAUSE, &lv_font_montserrat_20,
                         snake_pause_click, page);
    page->pause_label = lv_obj_get_child(page->pause, 0);
    lv_obj_set_parent(page->hint, page->board);
    lv_obj_align(page->hint, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(page->hint, lv_color_hex(0x192D40), 0);
    lv_obj_set_style_bg_opa(page->hint, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(page->hint, 8, 0);
    lv_obj_set_style_radius(page->hint, 6, 0);
    page->start_overlay = block(page->board, 0, 0, 226, 114, 14, 0x192D40);
    lv_obj_center(page->start_overlay);
    lv_obj_set_style_bg_opa(page->start_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(page->start_overlay, 1, 0);
    lv_obj_set_style_border_color(page->start_overlay, lv_color_hex(0x426582), 0);
    page->start_note = app_ui_add_text(page->start_overlay, "", LV_ALIGN_TOP_MID, 0, 13,
                                       TEXT_MUTED, app_ui_chinese_font());
    page->start_button = button(page->start_overlay, 28, 42, 168, 51, "开始游戏",
                                app_ui_chinese_font_size(24), snake_start_click, page);
    lv_obj_set_style_bg_color(page->start_button, lv_color_hex(0x285A5B), 0);
    page->start_label = lv_obj_get_child(page->start_button, 0);
    snake_reset(page);
}

static void build_jump(game_page_t *page)
{
    lv_obj_t *parent = page->ui->content;
    page->board = block(parent, 4, 38, 394, 275, 14, 0x111632);
    lv_obj_add_flag(page->board, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(page->board, jump_pointer, LV_EVENT_PRESSED, page);
    lv_obj_add_event_cb(page->board, jump_pointer, LV_EVENT_RELEASED, page);
    lv_obj_add_event_cb(page->board, jump_pointer, LV_EVENT_PRESS_LOST, page);
    lv_obj_add_event_cb(page->board, jump_pointer, LV_EVENT_INDEV_RESET, page);
    block(page->board, 260, 22, 76, 76, 38, 0x242B52);
    block(page->board, 277, 36, 41, 41, 20, 0xEEB4A7);
    static const uint16_t stars[][2] = {{32,40}, {56,87}, {99,32}, {129,109}, {194,70}, {238,122}, {357,99}, {369,28}};
    for (unsigned i = 0; i < sizeof(stars) / sizeof(stars[0]); ++i) {
        block(page->board, stars[i][0], stars[i][1], 4, 4, 2, 0x7787B7);
    }
    block(page->board, 0, 238, 394, 38, 0, 0x1A2142);
    block(page->board, 0, 251, 394, 24, 0, 0x23284D);
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *platform = block(page->board, 0, 0, 54, 24, 7, 0x684FB6);
        block(platform, 0, 0, 54, 8, 4, 0xB197FF);
        block(platform, 9, 14, 36, 3, 2, 0x3A376C);
        page->platforms[i] = platform;
    }
    page->aim_target = block(page->board, 0, 249, 30, 3, 1, 0x4B8E87);
    page->aim = block(page->board, 0, 246, 8, 8, 4, 0xF6B1C2);
    page->jumper = block(page->board, 0, 0, 26, 28, 9, 0xEEE6FF);
    block(page->jumper, 4, 5, 18, 11, 5, 0x303A63);
    block(page->jumper, 12, 8, 3, 5, 1, 0x99DEF2);
    block(page->jumper, 17, 8, 3, 5, 1, 0x99DEF2);
    block(page->jumper, 4, 21, 18, 3, 1, 0x9DB1FE);
    lv_obj_t *bar = block(parent, 13, 322, JUMP_POWER_WIDTH, 7, 4, 0x283655);
    page->power_fill = block(bar, 0, 0, 1, 7, 4, 0xAEBBFF);
    arcade_jump_init(&page->model.jump, esp_random());
    jump_render(page, now_ms(), true);
}

static void game_cleanup(app_ui_t *ui)
{
    game_page_t *page = ui->page_context;
    if (page && page->board) {
        /* An old screen may still dispatch draw or pointer-reset events. */
        lv_obj_remove_event_cb_with_user_data(page->board, NULL, page);
        if (page->touch_surface)
            lv_obj_remove_event_cb_with_user_data(page->touch_surface, NULL, page);
    }
    free(page);
    ui->page_context = NULL;
}

static void show_hub(lv_event_t *event)
{
    const game_page_t *page = lv_event_get_user_data(event);
    app_open_game(page->ui);
}

static void open_game(app_ui_t *ui, game_kind_t kind)
{
    lv_obj_t *content = app_ui_begin_page(ui, game_titles[kind]);
    ui->page_back = app_open_game;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    game_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF9BA8, NULL);
        return;
    }
    page->ui = ui;
    page->kind = kind;
    ui->page_context = page;
    ui->page_cleanup = game_cleanup;
    page->score = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 4, TEXT_BRIGHT, app_ui_chinese_font_size(24));
    page->hint = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 335, TEXT_MUTED, app_ui_chinese_font());
    button(content, 294, 0, 48, 34, LV_SYMBOL_LIST, &lv_font_montserrat_20, show_hub, page);
    button(content, 350, 0, 48, 34, LV_SYMBOL_REFRESH, &lv_font_montserrat_20, restart_click, page);
    switch (kind) {
    case GAME_TILES: build_tiles(page); break;
    case GAME_CRATES: build_crates(page); break;
    case GAME_SNAKE: build_snake(page); break;
    case GAME_JUMP: build_jump(page); break;
    default: return;
    }
    if (kind == GAME_TILES || kind == GAME_SNAKE) app_ui_set_timer(ui, game_timer, TILT_POLL_MS);
}

static void select_tiles(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_TILES); }
static void select_crates(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_CRATES); }
static void select_snake(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_SNAKE); }
static void select_jump(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_JUMP); }

static void game_preview(lv_obj_t *card, game_kind_t kind)
{
    lv_obj_t *art = block(card, 10, 10, 174, 97, 12, 0x102034);
    switch (kind) {
    case GAME_TILES: {
        static const char *const values[] = {"2", "4", "16", "64"};
        static const uint32_t colors[] = {0xDBE8FF, 0xB7E5FD, 0x4FD4AD, 0xE1D871};
        for (unsigned i = 0; i < 4; ++i) {
            lv_obj_t *tile = block(art, 49 + (i % 2) * 36, 10 + (i / 2) * 36, 32, 32, 6, colors[i]);
            app_ui_add_text(tile, values[i], LV_ALIGN_CENTER, 0, 0, 0x233152, &lv_font_montserrat_16);
        }
        break;
    }
    case GAME_CRATES:
        block(art, 26, 54, 116, 20, 5, 0x344B6B);
        block(art, 61, 22, 34, 35, 7, 0xFFCA75);
        block(art, 67, 29, 22, 4, 2, 0xAA7046);
        block(art, 67, 43, 22, 4, 2, 0xAA7046);
        block(art, 112, 30, 22, 22, 11, 0x5FDFBE);
        block(art, 117, 35, 12, 12, 6, 0x102034);
        break;
    case GAME_SNAKE:
        block(art, 40, 54, 59, 16, 8, 0x41BB9B);
        block(art, 83, 28, 16, 41, 8, 0x41BB9B);
        block(art, 83, 26, 30, 17, 7, 0x9AEBC4);
        block(art, 103, 29, 4, 4, 2, 0x13394D);
        block(art, 103, 36, 4, 4, 2, 0x13394D);
        block(art, 127, 30, 14, 14, 7, 0xFD7C94);
        break;
    case GAME_JUMP:
        block(art, 23, 65, 48, 15, 5, 0x927BD4);
        block(art, 108, 55, 44, 15, 5, 0x4DC0BC);
        block(art, 73, 21, 24, 27, 7, 0xEDF2FF);
        block(art, 78, 25, 15, 10, 4, 0x434B76);
        block(art, 84, 28, 6, 3, 1, 0x8CE8F1);
        break;
    default:
        break;
    }
}

void app_open_game(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "游戏");
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    const lv_event_cb_t callbacks[] = {select_tiles, select_crates, select_snake, select_jump};
    for (unsigned i = 0; i < GAME_COUNT; ++i) {
        lv_obj_t *card = button(content, 4 + (i % 2) * 203, 5 + (i / 2) * 174, 194, 161,
                                NULL, NULL, callbacks[i], ui);
        app_connection_button_style(card, false);
        lv_obj_set_style_bg_color(card, lv_color_hex(CONNECTION_PANEL), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(CONNECTION_BORDER), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        game_preview(card, (game_kind_t)i);
        app_ui_add_text(card, game_titles[i], LV_ALIGN_BOTTOM_LEFT, 14, -13,
                        TEXT_BRIGHT, app_ui_chinese_font_size(24));
        app_ui_add_text(card, LV_SYMBOL_RIGHT, LV_ALIGN_BOTTOM_RIGHT, -14, -17,
                        CONNECTION_ACCENT, &lv_font_montserrat_16);
    }
}
