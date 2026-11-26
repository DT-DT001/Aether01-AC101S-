#include "app_connection_style.h"
#include "app_arcade.h"
#include "app_motion_filter.h"
#include "app_telemetry.h"
#include "app_tools.h"

#include <math.h>
#include <stdlib.h>
#include "esp_random.h"
#include "esp_timer.h"

typedef enum { GAME_TILES, GAME_CRATES, GAME_SNAKE, GAME_JUMP, GAME_DICE, GAME_FALL, GAME_COUNT } game_kind_t;
typedef enum { FALL_LEFT, FALL_RIGHT, FALL_ROTATE, FALL_SOFT, FALL_DROP, FALL_PAUSE, FALL_CONTROLS } fall_control_t;

static const char *const game_titles[GAME_COUNT] = {
    "2048", "推箱子", "贪吃蛇", "跳一跳", "摇骰子", "落块挑战",
};

static app_tools_arcade_preferences_t game_preferences(void)
{
#ifdef AETHER_UI_SELFTEST
    /* The diagnostic gestures use touch by default and the first Sokoban map. */
    return (app_tools_arcade_preferences_t){0};
#else
    return app_tools_get_arcade_preferences();
#endif
}

typedef struct {
    uint32_t random;
    uint32_t start_ms;
    uint32_t imu_stamp;
    uint8_t faces[3];
    uint8_t count, frame, motion_samples;
    bool rolling, shake, armed;
} dice_game_t;

typedef struct {
    app_ui_t *ui;
    game_kind_t kind;
    union {
        arcade_tiles_t tiles;
        arcade_crates_t crates;
        arcade_snake_t snake;
        arcade_jump_t jump;
        dice_game_t dice;
        arcade_fall_t fall;
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
    lv_obj_t *fall_controls[FALL_CONTROLS];
    lv_indev_t *jump_input;
    lv_indev_t *swipe_input;
    lv_point_t swipe_origin;
    arcade_tilt_t tilt;
    arcade_accel_t acceleration;
    app_motion_filter_t attitude;
    uint32_t snake_due_ms;
    uint32_t fall_due_ms;
    uint32_t sleep_ms;
    uint8_t displayed_exponents[16];
    uint8_t level;
    uint8_t neutral_samples;
    int8_t sensor_status;
    bool tilt_enabled;
    bool accel_enabled;
    bool neutral_set;
    bool snake_start_requested;
    bool snake_started;
    bool fall_started;
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
    TILES_ACCEL_POLL_MS = 6,
    TILE_SWIPE_DISTANCE = 30,
    SNAKE_START_GRACE_MS = 500,
    DICE_POLL_MS = 50,
    DICE_ROLL_MS = 650,
    FALL_POLL_MS = 32,
    FALL_SIDE = 17,
    FALL_PAD = 4,
    FALL_CARD_X = 196,
    FALL_CARD_WIDTH = 202,
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
    else if (page->accel_enabled && page->sensor_status < 0) hint = "传感器暂不可用 · 可切回滑动";
    else if (page->accel_enabled && !page->acceleration.ready) hint = "请拿稳设备";
    else if (page->accel_enabled && !page->acceleration.armed) hint = "停稳后继续";
    else if (game->won) hint = "达到 2048";
    else if (page->accel_enabled) hint = "沿屏幕方向轻推";
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

static void die_draw(lv_layer_t *layer, int x, int y, int size, int height, unsigned value)
{
    static const uint16_t pips[] = {0x010, 0x044, 0x054, 0x145, 0x155, 0x16D};
    const int radius = size / 6;
    const int dot = size / 8;
    draw_rect(layer, x + 3, y + 8, size, height, radius, 0x132E44);
    draw_rect(layer, x, y + 3, size, height, radius, 0x8AA8C6);
    draw_rect(layer, x, y, size, height - 2, radius, 0xE8F1FF);
    draw_rect(layer, x + radius, y + 3, size - 2 * radius, 2, 1, 0xFFFFFF);
    for (unsigned dots = pips[value - 1U]; dots; dots &= dots - 1U) {
        const unsigned i = __builtin_ctz(dots);
        const int px = x + size * (24 + (int)(i % 3U) * 26) / 100 - dot / 2;
        const int py = y + height * (24 + (int)(i / 3U) * 26) / 100 - dot / 2;
        draw_rect(layer, px, py, dot, dot, LV_RADIUS_CIRCLE, value == 1 ? 0xDF5179 : 0x1F4766);
    }
}

static void dice_draw(lv_event_t *event)
{
    const game_page_t *page = lv_event_get_user_data(event);
    const dice_game_t *dice = &page->model.dice;
    lv_area_t area;
    lv_obj_get_coords(page->board, &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    const int size = dice->count == 1 ? 134 : 102;
    const int total = size * dice->count + 14 * (dice->count - 1);
    const int left = area.x1 + (lv_area_get_width(&area) - total) / 2;
    const int center_y = area.y1 + lv_area_get_height(&area) / 2;
    static const int8_t bounce[] = {0, -16, -30, -38, -36, -27, -14, 0, -12, -17, -11, -4, 0};
    for (unsigned i = 0; i < dice->count; ++i) {
        const int step = dice->rolling ? bounce[dice->frame] : 0;
        const int height = size + step / 2;
        const int offset_x = dice->rolling && (dice->frame & 1U) ? (i & 1U ? -3 : 3) : 0;
        die_draw(layer, left + (int)i * (size + 14) + offset_x, center_y - height / 2 + step,
                 size, height, dice->faces[i]);
    }
}

static void dice_update_score(game_page_t *page)
{
    const dice_game_t *dice = &page->model.dice;
    unsigned total = 0;
    for (unsigned i = 0; i < dice->count; ++i) total += dice->faces[i];
    app_ui_label_fmt(page->score, "总点数  %u", total);
}

static void dice_start(game_page_t *page, uint32_t now)
{
    dice_game_t *dice = &page->model.dice;
    if (dice->rolling || page->sleeping || app_power_is_sleeping(page->ui->power)) return;
    dice->start_ms = now;
    dice->frame = 0;
    dice->rolling = true;
    dice->motion_samples = 0;
    dice->armed = false;
    app_ui_label_text(page->score, "摇动中");
    set_enabled(page->start_button, false);
    set_enabled(page->undo, false);
    app_ui_set_timer(page->ui, game_timer, DICE_POLL_MS);
}

static void dice_tick(game_page_t *page, uint32_t now)
{
    dice_game_t *dice = &page->model.dice;
    if (dice->rolling) {
        const uint32_t elapsed = now - dice->start_ms;
        dice->frame = elapsed < DICE_ROLL_MS ? elapsed / DICE_POLL_MS : 0;
        for (unsigned i = 0; i < dice->count; ++i) dice->faces[i] = arcade_roll_die(&dice->random);
        if (elapsed >= DICE_ROLL_MS) {
            dice->rolling = false;
            dice_update_score(page);
            set_enabled(page->start_button, true);
            set_enabled(page->undo, true);
            if (!dice->shake) app_ui_clear_timer(page->ui);
        }
        lv_obj_invalidate(page->board);
        return;
    }
    if (!dice->shake) return;
    app_telemetry_snapshot_t sample;
    app_telemetry_get(&sample);
    if (sample.imu_error != ESP_OK || now_ms() - sample.imu_updated_ms >= 150U) {
        dice->motion_samples = 0;
        dice->armed = false;
        return;
    }
    if (sample.imu_updated_ms == dice->imu_stamp) return;
    dice->imu_stamp = sample.imu_updated_ms;
    const float *a = sample.imu.accel_mps2;
    const float energy = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    if (energy > 70.0f && energy < 145.0f) dice->armed = true;
    if (!dice->armed || energy < 280.0f || !isfinite(energy)) {
        dice->motion_samples = 0;
    } else if (++dice->motion_samples >= 2) {
        dice_start(page, now);
    }
}

static void dice_roll_click(lv_event_t *event)
{
    dice_start(lv_event_get_user_data(event), now_ms());
}

static void dice_count_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    dice_game_t *dice = &page->model.dice;
    if (dice->rolling) return;
    dice->count = dice->count % 3U + 1U;
    app_ui_label_fmt(lv_obj_get_child(page->undo, 0), "%u 枚", (unsigned)dice->count);
    dice_update_score(page);
    lv_obj_invalidate(page->board);
}

static void dice_shake_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    dice_game_t *dice = &page->model.dice;
    dice->shake = !dice->shake;
    dice->armed = false;
    dice->motion_samples = 0;
    app_ui_label_text(page->mode_label, dice->shake ? "感应开" : "感应关");
    app_ui_label_text(page->hint, dice->shake ? "摇动设备或点击骰子" : "点击骰子或掷骰");
    app_telemetry_set_interest(dice->shake ? APP_TELEMETRY_IMU : 0);
    if (dice->shake || dice->rolling) app_ui_set_timer(page->ui, game_timer, DICE_POLL_MS);
    else app_ui_clear_timer(page->ui);
}

static const uint32_t fall_colors[ARCADE_FALL_PIECES] = {
    0x66D5EB, 0xF3CE76, 0xAE9BFB, 0x8BDAA5, 0xEE879C, 0x87AFF7, 0xF2A678,
};

static void fall_piece_draw(lv_layer_t *layer, int x, int y, int side,
                            unsigned shape, unsigned piece, int min_row, bool ghost)
{
    const uint32_t color = fall_colors[piece];
    for (; shape; shape &= shape - 1U) {
        const unsigned bit = __builtin_ctz(shape);
        const int cell_x = x + (int)(bit % 4U) * side, cell_y = y + (int)(bit / 4U) * side;
        if (cell_y < min_row) continue;
        draw_rect(layer, cell_x + 1, cell_y + 1, side - 2, side - 2, 3,
                  ghost ? 0x31516B : color);
        if (ghost) draw_rect(layer, cell_x + 3, cell_y + 3, side - 6, side - 6, 2, BOARD_INK);
    }
}

static void fall_draw(lv_event_t *event)
{
    const game_page_t *page = lv_event_get_user_data(event);
    const arcade_fall_t *fall = &page->model.fall;
    lv_area_t area;
    lv_obj_get_coords(page->board, &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    const int x = area.x1 + FALL_PAD, y = area.y1 + FALL_PAD;
    for (int i = 1; i < ARCADE_FALL_COLUMNS; ++i)
        draw_rect(layer, x + i * FALL_SIDE, y, 1, FALL_SIDE * ARCADE_FALL_ROWS, 0, 0x182940);
    for (int i = 1; i < ARCADE_FALL_ROWS; ++i)
        draw_rect(layer, x, y + i * FALL_SIDE, FALL_SIDE * ARCADE_FALL_COLUMNS, 1, 0, 0x182940);
    for (int row = 0; row < ARCADE_FALL_ROWS; ++row) {
        for (int col = 0; col < ARCADE_FALL_COLUMNS; ++col) {
            const unsigned cell = fall->cells[row][col];
            if (cell) fall_piece_draw(layer, x + col * FALL_SIDE, y + row * FALL_SIDE,
                                      FALL_SIDE, 1, cell - 1U, y, false);
        }
    }
    if (fall->over) return;
    const unsigned shape = arcade_fall_shape(fall->piece, fall->rotation);
    const int landing = arcade_fall_landing(fall);
    if (landing > fall->y) fall_piece_draw(layer, x + fall->x * FALL_SIDE, y + landing * FALL_SIDE,
                                           FALL_SIDE, shape, fall->piece, y, true);
    fall_piece_draw(layer, x + fall->x * FALL_SIDE, y + fall->y * FALL_SIDE,
                    FALL_SIDE, shape, fall->piece, y, false);
}

static void fall_next_draw(lv_event_t *event)
{
    const game_page_t *page = lv_event_get_user_data(event);
    const unsigned piece = page->model.fall.next;
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    fall_piece_draw(lv_event_get_layer(event), area.x1 + (piece < 2 ? 1 : 8), area.y1 + 2,
                    14, arcade_fall_shape(piece, 0), piece, area.y1, false);
}

static void fall_render(game_page_t *page)
{
    const arcade_fall_t *fall = &page->model.fall;
    app_ui_label_fmt(page->score, "得分  %lu", (unsigned long)fall->score);
    app_ui_label_fmt(page->hint, "%lu 行  ·  Lv.%u", (unsigned long)fall->lines, arcade_fall_level(fall));
    const bool running = page->fall_started && !page->paused && !fall->over;
    set_visible(page->start_overlay, !running);
    for (unsigned i = 0; i < FALL_CONTROLS; ++i)
        set_enabled(page->fall_controls[i], running);
    app_ui_label_text(page->start_label, fall->over ? "再玩一次" : page->paused ? "继续" : "开始");
    app_ui_label_text(page->start_note, fall->over ? "游戏结束" : page->paused ? "已暂停" : "移动方块填满整行");
    lv_obj_invalidate(page->board);
    lv_obj_invalidate(page->next_level);
}

static void fall_start_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    if (page->sleeping || app_power_is_sleeping(page->ui->power)) return;
    if (page->model.fall.over) arcade_fall_init(&page->model.fall, esp_random());
    page->fall_started = true;
    page->paused = false;
    page->fall_due_ms = now_ms() + arcade_fall_interval(&page->model.fall);
    app_ui_set_timer(page->ui, game_timer, FALL_POLL_MS);
    fall_render(page);
}

static void fall_control_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    arcade_fall_t *fall = &page->model.fall;
    if (!page->fall_started || page->paused || fall->over || page->sleeping ||
        app_power_is_sleeping(page->ui->power)) return;
    lv_obj_t *control = lv_event_get_target_obj(event);
    if (control == page->fall_controls[FALL_PAUSE]) {
        page->paused = true;
        app_ui_clear_timer(page->ui);
    } else if (control == page->fall_controls[FALL_DROP]) {
        arcade_fall_drop(fall);
        page->fall_due_ms = now_ms() + arcade_fall_interval(fall);
        if (fall->over) app_ui_clear_timer(page->ui);
    } else if (control == page->fall_controls[FALL_ROTATE]) {
        if (!arcade_fall_rotate(fall)) return;
    } else if (control == page->fall_controls[FALL_SOFT]) {
        if (!arcade_fall_move(fall, 0, 1)) return;
        page->fall_due_ms = now_ms() + arcade_fall_interval(fall);
    } else if (!arcade_fall_move(fall, control == page->fall_controls[FALL_LEFT] ? -1 : 1, 0)) {
        return;
    }
    fall_render(page);
}

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
    page->neutral_set = false;
    page->neutral_samples = 0;
    page->sensor_status = 0;
    page->tilt = (arcade_tilt_t){0};
    app_motion_filter_init(&page->attitude);
    app_telemetry_set_interest(enabled && !page->model.snake.over ? APP_TELEMETRY_IMU : 0);
}

static void reset_tiles_sensor(game_page_t *page)
{
    page->acceleration = (arcade_accel_t){0};
    page->swipe_input = NULL;
    page->sensor_status = 0;
    const bool active = page->accel_enabled && !page->model.tiles.over && !page->sleeping;
    app_telemetry_set_interest(active ? APP_TELEMETRY_IMU_GAME : 0);
    if (active) app_ui_set_timer(page->ui, game_timer, TILES_ACCEL_POLL_MS);
    else app_ui_clear_timer(page->ui);
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
    const bool enabled = !page->accel_enabled && !page->model.tiles.over &&
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
    snake_render_status(page);
}

static void tilt_tick(game_page_t *page, uint32_t now)
{
    if (!page->tilt_enabled || page->model.snake.over || page->paused) return;
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
    if (recovered) snake_render_status(page);
    if (!page->neutral_set) {
        if (page->attitude.rotation > 18 || fabsf(page->attitude.acceleration - 9.81f) > 1.8f) {
            page->neutral_samples = 0;
            return;
        }
        if (++page->neutral_samples < 3) return;
        app_motion_filter_zero(&page->attitude);
        page->neutral_set = true;
        if (page->snake_started) page->snake_due_ms = now + arcade_snake_interval(&page->model.snake);
        snake_start_when_ready(page, now);
        snake_render_status(page);
    }
    if (!page->snake_started) return;
    const arcade_direction_t previous_tilt = page->tilt.held;
    /* In the AC101S screen orientation, positive pitch points to the right. */
    const arcade_direction_t direction = arcade_tilt_update(&page->tilt, page->attitude.pitch,
                                                            page->attitude.roll);
    if (move_game(page, direction)) {
        lv_display_trigger_activity(lv_obj_get_display(page->board));
    } else if (direction != ARCADE_NONE &&
               page->model.snake.next != page->model.snake.direction &&
               page->model.snake.next != direction) {
        /* Let a confirmed turn retry once Snake has used its pending turn. */
        page->tilt.held = previous_tilt;
    }
}

static void tiles_accel_tick(game_page_t *page)
{
    app_telemetry_snapshot_t sample;
    app_telemetry_get(&sample);
    const uint32_t age_ms = now_ms() - sample.imu_updated_ms;
    const bool ready = page->acceleration.ready;
    const bool armed = page->acceleration.armed;
    const int8_t previous_status = page->sensor_status;
    arcade_direction_t move = ARCADE_NONE;
    if (sample.imu_error == ESP_ERR_NOT_FINISHED || sample.gyro_calibrating) {
        page->sensor_status = 0;
    } else if (sample.imu_error == ESP_OK && age_ms < 150U &&
               arcade_accel_update(&page->acceleration, sample.imu.accel_mps2, sample.imu.gyro_dps,
                                   sample.imu_updated_ms, &move)) {
        page->sensor_status = 1;
    } else {
        page->sensor_status = -1;
    }
    if (page->sensor_status <= 0) page->acceleration = (arcade_accel_t){0};
    if (ready != page->acceleration.ready || armed != page->acceleration.armed || previous_status != page->sensor_status)
        tiles_render_status(page);
    if (move_game(page, move)) lv_display_trigger_activity(lv_obj_get_display(page->board));
}

static void game_suspend(app_ui_t *ui, bool suspended)
{
    game_page_t *page = ui->page_context;
    if (!page || page->sleeping == suspended) return;
    const uint32_t now = now_ms();
    page->sleeping = suspended;
    if (suspended) {
        page->sleep_ms = now;
        page->swipe_input = NULL;
        if (page->kind == GAME_TILES) reset_tiles_sensor(page);
        else if (page->tilt_enabled) app_telemetry_set_interest(0);
        if (page->kind == GAME_JUMP) {
            page->jump_input = NULL;
            arcade_jump_cancel(&page->model.jump);
        } else if (page->kind == GAME_DICE) {
            app_telemetry_set_interest(0);
            page->model.dice.armed = false;
            page->model.dice.motion_samples = 0;
            app_ui_clear_timer(ui);
        } else if (page->kind == GAME_FALL) {
            app_ui_clear_timer(ui);
        }
        return;
    }
    const uint32_t sleep = now - page->sleep_ms;
    if (page->tilt_enabled) set_tilt(page, true);
    if (page->kind == GAME_TILES) {
        reset_tiles_sensor(page);
        tiles_render_status(page);
    }
    if (page->kind == GAME_SNAKE) page->snake_due_ms += sleep;
    if (page->kind == GAME_JUMP) {
        page->model.jump.phase_ms += sleep;
        jump_render(page, now, true);
    } else if (page->kind == GAME_DICE) {
        dice_game_t *dice = &page->model.dice;
        if (dice->shake) app_telemetry_set_interest(APP_TELEMETRY_IMU);
        if (dice->rolling || dice->shake) {
            dice->start_ms += sleep;
            app_ui_set_timer(ui, game_timer, DICE_POLL_MS);
        }
    } else if (page->kind == GAME_FALL && page->fall_started && !page->paused && !page->model.fall.over) {
        page->fall_due_ms += sleep;
        app_ui_set_timer(ui, game_timer, FALL_POLL_MS);
    }
}

static void game_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    const bool suspended = app_power_is_sleeping(ui->power);
    game_suspend(ui, suspended);
    game_page_t *page = ui->page_context;
    if (!page || suspended) return;
    const uint32_t now = now_ms();
    if (page->kind == GAME_JUMP) {
        const arcade_jump_phase_t old = page->model.jump.phase;
        arcade_jump_step(&page->model.jump, now);
        if (old != page->model.jump.phase || (old != ARCADE_JUMP_OVER && old != ARCADE_JUMP_READY)) {
            jump_render(page, now, old != page->model.jump.phase);
        }
        if (page->model.jump.phase == ARCADE_JUMP_READY || page->model.jump.phase == ARCADE_JUMP_OVER) {
            app_ui_clear_timer(ui);
        }
    } else if (page->kind == GAME_DICE) {
        dice_tick(page, now);
    } else if (page->kind == GAME_FALL) {
        if ((int32_t)(now - page->fall_due_ms) >= 0) {
            arcade_fall_step(&page->model.fall);
            page->fall_due_ms = now + arcade_fall_interval(&page->model.fall);
            if (page->model.fall.over) app_ui_clear_timer(ui);
            fall_render(page);
        }
    } else if (page->kind == GAME_TILES) {
        if (page->accel_enabled && !page->model.tiles.over) tiles_accel_tick(page);
    } else if (page->kind == GAME_SNAKE) {
        tilt_tick(page, now);
        snake_tick(page, now);
        if (page->model.snake.over) {
            app_telemetry_set_interest(0);
            app_ui_clear_timer(ui);
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
#ifndef AETHER_UI_SELFTEST
    app_tools_set_crates_level(page->level);
#endif
    crates_render(page);
}

static void tiles_set_mode(game_page_t *page, bool acceleration)
{
    page->accel_enabled = acceleration;
    reset_tiles_sensor(page);
    app_ui_label_text(page->mode_label, acceleration ? "加速度" : "滑动");
    set_visible(page->recenter, acceleration && !page->model.tiles.over);
    tiles_render_status(page);
}

static void tiles_mode_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    tiles_set_mode(page, !page->accel_enabled);
#ifndef AETHER_UI_SELFTEST
    /* Retain the existing on-flash bit for the selected 2048 sensor mode. */
    app_tools_set_arcade_tilt(APP_TOOLS_ARCADE_TILES, page->accel_enabled);
#endif
}

static void snake_mode_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    const bool tilt = !page->tilt_enabled;
    set_tilt(page, tilt);
#ifndef AETHER_UI_SELFTEST
    app_tools_set_arcade_tilt(APP_TOOLS_ARCADE_SNAKE, tilt);
#endif
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
    if (!page->accel_enabled || page->model.tiles.over || page->sleeping) return;
    tiles_set_mode(page, true);
}

static void restart_click(lv_event_t *event)
{
    game_page_t *page = lv_event_get_user_data(event);
    switch (page->kind) {
    case GAME_TILES:
        arcade_tiles_init(&page->model.tiles, esp_random());
        tiles_set_mode(page, page->accel_enabled);
        tiles_render(page);
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
    case GAME_FALL:
        arcade_fall_init(&page->model.fall, esp_random());
        page->fall_started = false;
        page->paused = false;
        app_ui_clear_timer(page->ui);
        fall_render(page);
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
    lv_obj_t *mode = button(parent, 172, 0, 80, 34, "滑动", app_ui_chinese_font(), tiles_mode_click, page);
    page->mode_label = lv_obj_get_child(mode, 0);
    page->recenter = button(parent, 260, 0, 43, 34, LV_SYMBOL_GPS, &lv_font_montserrat_20, zero_click, page);
    arcade_tiles_init(&page->model.tiles, esp_random());
    tiles_render(page);
    const app_tools_arcade_preferences_t preferences = game_preferences();
    tiles_set_mode(page, preferences.tilt_modes & APP_TOOLS_ARCADE_TILES);
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
    const app_tools_arcade_preferences_t preferences = game_preferences();
    if (preferences.crates_level < arcade_crates_levels()) page->level = preferences.crates_level;
    arcade_crates_init(&page->model.crates, page->level);
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
    const app_tools_arcade_preferences_t preferences = game_preferences();
    page->tilt_enabled = (preferences.tilt_modes & APP_TOOLS_ARCADE_SNAKE) != 0;
    lv_obj_t *mode = button(parent, 102, 0, 113, 34, page->tilt_enabled ? "模式 · 倾斜" : "模式 · 触摸",
                            app_ui_chinese_font(), snake_mode_click, page);
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

static void build_dice(game_page_t *page)
{
    lv_obj_t *parent = page->ui->content;
    dice_game_t *dice = &page->model.dice;
    dice->random = esp_random();
    dice->count = 1;
    for (unsigned i = 0; i < sizeof(dice->faces); ++i) dice->faces[i] = arcade_roll_die(&dice->random);
    page->board = block(parent, 4, 47, 394, 220, 20, 0x23465D);
    lv_obj_set_style_border_color(page->board, lv_color_hex(0x427290), 0);
    lv_obj_set_style_border_width(page->board, 2, 0);
    lv_obj_add_flag(page->board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page->board, dice_draw, LV_EVENT_DRAW_MAIN, page);
    lv_obj_add_event_cb(page->board, dice_roll_click, LV_EVENT_CLICKED, page);
    page->undo = button(parent, 4, 278, 78, 53, "1 枚", app_ui_chinese_font_size(24), dice_count_click, page);
    page->start_button = button(parent, 92, 278, 212, 53, "掷骰", app_ui_chinese_font_size(24),
                                dice_roll_click, page);
    lv_obj_set_style_bg_color(page->start_button, lv_color_hex(0x275C72), 0);
    lv_obj_t *shake = button(parent, 314, 278, 84, 53, "感应关", app_ui_chinese_font(), dice_shake_click, page);
    page->mode_label = lv_obj_get_child(shake, 0);
    app_ui_label_text(page->hint, "点击骰子或掷骰");
    dice_update_score(page);
}

static void build_fall(game_page_t *page)
{
    lv_obj_t *parent = page->ui->content;
    const lv_font_t *font = app_ui_chinese_font();
    const lv_font_t *symbols = &lv_font_montserrat_20;
    const int width = ARCADE_FALL_COLUMNS * FALL_SIDE + 2 * FALL_PAD;
    const int height = ARCADE_FALL_ROWS * FALL_SIDE + 2 * FALL_PAD;
    page->board = block(parent, 4, 3, width, height, 10, BOARD_INK);
    lv_obj_add_event_cb(page->board, fall_draw, LV_EVENT_DRAW_MAIN, page);
    lv_obj_set_pos(page->score, FALL_CARD_X, 5);
    lv_obj_set_style_text_font(page->score, font, 0);
    lv_obj_align(page->hint, LV_ALIGN_TOP_LEFT, FALL_CARD_X, 31);
    lv_obj_t *preview = block(parent, FALL_CARD_X, 55, FALL_CARD_WIDTH, 78, 13, 0x1A2946);
    app_ui_add_text(preview, "下一块", LV_ALIGN_LEFT_MID, 15, 0, TEXT_MUTED, font);
    page->next_level = block(preview, 106, 20, 62, 40, 6, 0x101B30);
    lv_obj_add_event_cb(page->next_level, fall_next_draw, LV_EVENT_DRAW_MAIN, page);
    page->fall_controls[FALL_LEFT] = button(parent, FALL_CARD_X, 146, 96, 55, LV_SYMBOL_LEFT, symbols,
                                           fall_control_click, page);
    page->fall_controls[FALL_RIGHT] = button(parent, 302, 146, 96, 55, LV_SYMBOL_RIGHT, symbols,
                                            fall_control_click, page);
    page->fall_controls[FALL_ROTATE] = button(parent, FALL_CARD_X, 209, 96, 55, "旋转", font,
                                             fall_control_click, page);
    page->fall_controls[FALL_SOFT] = button(parent, 302, 209, 96, 55, LV_SYMBOL_DOWN, symbols,
                                           fall_control_click, page);
    page->fall_controls[FALL_DROP] = button(parent, FALL_CARD_X, 272, 135, 62, "快速落下", font,
                                           fall_control_click, page);
    lv_obj_set_style_bg_color(page->fall_controls[FALL_DROP], lv_color_hex(0x3D3B70), 0);
    page->fall_controls[FALL_PAUSE] = button(parent, 341, 272, 57, 62, LV_SYMBOL_PAUSE, symbols,
                                            fall_control_click, page);
    page->start_overlay = block(page->board, 0, 0, 165, 108, 12, 0x203552);
    lv_obj_center(page->start_overlay);
    page->start_note = app_ui_add_text(page->start_overlay, "", LV_ALIGN_TOP_MID, 0, 12, TEXT_MUTED, font);
    page->start_button = button(page->start_overlay, 12, 42, 141, 51, "开始",
                                app_ui_chinese_font_size(24), fall_start_click, page);
    lv_obj_set_style_bg_color(page->start_button, lv_color_hex(0x405783), 0);
    page->start_label = lv_obj_get_child(page->start_button, 0);
    arcade_fall_init(&page->model.fall, esp_random());
    fall_render(page);
}

static void remove_game_callbacks(lv_obj_t *obj, game_page_t *page)
{
    lv_obj_remove_event_cb_with_user_data(obj, NULL, page);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        remove_game_callbacks(lv_obj_get_child(obj, i), page);
}

static void game_cleanup(app_ui_t *ui)
{
    game_page_t *page = ui->page_context;
    if (page) {
        /* An old screen may still dispatch draw or input events. */
        remove_game_callbacks(ui->content, page);
        if (page->touch_surface)
            lv_obj_remove_event_cb_with_user_data(page->touch_surface, NULL, page);
    }
    free(page);
    ui->page_context = NULL;
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
    ui->page_suspend = game_suspend;
    page->score = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 4, TEXT_BRIGHT, app_ui_chinese_font_size(24));
    page->hint = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 335, TEXT_MUTED, app_ui_chinese_font());
    if (kind != GAME_DICE)
        button(content, 350, 0, 48, 34, LV_SYMBOL_REFRESH, &lv_font_montserrat_20, restart_click, page);
    switch (kind) {
    case GAME_TILES: build_tiles(page); break;
    case GAME_CRATES: build_crates(page); break;
    case GAME_SNAKE: build_snake(page); break;
    case GAME_JUMP: build_jump(page); break;
    case GAME_DICE: build_dice(page); break;
    case GAME_FALL: build_fall(page); break;
    default: return;
    }
    if (kind == GAME_SNAKE) app_ui_set_timer(ui, game_timer, TILT_POLL_MS);
}

static void select_tiles(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_TILES); }
static void select_crates(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_CRATES); }
static void select_snake(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_SNAKE); }
static void select_jump(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_JUMP); }
static void select_dice(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_DICE); }
static void select_fall(lv_event_t *event) { open_game(lv_event_get_user_data(event), GAME_FALL); }

static void dice_preview(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    die_draw(layer, area.x1 + 39, area.y1 + 17, 39, 39, 3);
    die_draw(layer, area.x1 + 92, area.y1 + 10, 45, 45, 5);
}

static void fall_preview(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    const int x = area.x1, y = area.y1;
    fall_piece_draw(layer, x + 15, y + 20, 15, arcade_fall_shape(2, 2), 2, y, false);
    fall_piece_draw(layer, x + 65, y + 14, 15, arcade_fall_shape(5, 0), 5, y, false);
    fall_piece_draw(layer, x + 115, y + 27, 15, arcade_fall_shape(3, 0), 3, y, false);
}

static void game_preview(lv_obj_t *card, game_kind_t kind)
{
    lv_obj_t *art = block(card, 10, 5, 174, 74, 12, 0x102034);
    switch (kind) {
    case GAME_TILES: {
        static const char *const values[] = {"2", "4", "16", "64"};
        static const uint32_t colors[] = {0xDBE8FF, 0xB7E5FD, 0x4FD4AD, 0xE1D871};
        for (unsigned i = 0; i < 4; ++i) {
            lv_obj_t *tile = block(art, 53 + (i % 2) * 33, 5 + (i / 2) * 33, 30, 30, 6, colors[i]);
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
        block(art, 23, 59, 48, 14, 5, 0x927BD4);
        block(art, 108, 55, 44, 15, 5, 0x4DC0BC);
        block(art, 73, 21, 24, 27, 7, 0xEDF2FF);
        block(art, 78, 25, 15, 10, 4, 0x434B76);
        block(art, 84, 28, 6, 3, 1, 0x8CE8F1);
        break;
    case GAME_DICE:
        lv_obj_add_event_cb(art, dice_preview, LV_EVENT_DRAW_MAIN, NULL);
        break;
    case GAME_FALL:
        lv_obj_add_event_cb(art, fall_preview, LV_EVENT_DRAW_MAIN, NULL);
        break;
    default:
        break;
    }
}

void app_open_game(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "小游戏");
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    const lv_event_cb_t callbacks[] = {select_tiles, select_crates, select_snake, select_jump, select_dice, select_fall};
    for (unsigned i = 0; i < GAME_COUNT; ++i) {
        lv_obj_t *card = button(content, 4 + (i % 2) * 203, 3 + (i / 2) * 117, 194, 112,
                                NULL, NULL, callbacks[i], ui);
        app_connection_button_style(card, false);
        lv_obj_set_style_bg_color(card, lv_color_hex(CONNECTION_PANEL), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(CONNECTION_BORDER), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        game_preview(card, (game_kind_t)i);
        app_ui_add_text(card, game_titles[i], LV_ALIGN_BOTTOM_LEFT, 14, -4,
                        TEXT_BRIGHT, app_ui_chinese_font_size(24));
        app_ui_add_text(card, LV_SYMBOL_RIGHT, LV_ALIGN_BOTTOM_RIGHT, -14, -8,
                        CONNECTION_ACCENT, &lv_font_montserrat_16);
    }
}
