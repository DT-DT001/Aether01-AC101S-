#include "app_telemetry.h"
#include "app_arcade.h"
#include "app_tools.h"
#include "app_ui_internal.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "ui/app_screen.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern const lv_font_t font_16, font_24;
static uint16_t frame[480 * 480], buffer[480 * 20];
static lv_point_t pointer;
static bool held;
static bool sleeping;
static uint32_t clock_ms;
static uint32_t interest;
static unsigned cleanups;
static lv_obj_t *expected_pointer_cleanup;
static lv_obj_t *expected_draw_cleanup;
static float pitch_angle;
static float pitch_rate;
static float roll_angle;
static float roll_rate;
static esp_err_t imu_status = ESP_OK;
static uint32_t imu_stamp_ms;
static uint32_t callback_elapsed_ms;
static bool imu_frozen;
static bool imu_invalid;
static bool imu_publish_during_read;
static bool imu_gyro_bias_valid = true;
static float imu_gravity = 9.80665f;
static float imu_roll_bias;
static float imu_pitch_bias;
static float imu_translation[3];
static bool imu_accel_invalid;
static app_tools_arcade_preferences_t arcade_preferences;

uint32_t esp_random(void) { return 41; }
int64_t esp_timer_get_time(void) { return (int64_t)(clock_ms + callback_elapsed_ms) * 1000; }
bool app_power_is_sleeping(const app_power_t *power) { (void)power; return sleeping; }
app_tools_arcade_preferences_t app_tools_get_arcade_preferences(void) { return arcade_preferences; }
void app_tools_set_arcade_tilt(app_tools_arcade_control_t control, bool tilt)
{
    if (tilt) arcade_preferences.tilt_modes |= control;
    else arcade_preferences.tilt_modes &= ~control;
}
void app_tools_set_crates_level(uint8_t level) { arcade_preferences.crates_level = level; }
void app_telemetry_set_interest(uint32_t mask) { interest = mask; }
void app_telemetry_get(app_telemetry_snapshot_t *snapshot)
{
    if (imu_publish_during_read) {
        imu_publish_during_read = false;
        callback_elapsed_ms = 1;
        imu_stamp_ms = clock_ms + callback_elapsed_ms;
    }
    *snapshot = (app_telemetry_snapshot_t){
        .imu = {.accel_mps2 = {imu_accel_invalid ? NAN : imu_gravity * sinf(pitch_angle) *
                              cosf(roll_angle) + imu_translation[0],
                             imu_gravity * sinf(roll_angle) + imu_translation[1],
                             imu_gravity * cosf(pitch_angle) * cosf(roll_angle) + imu_translation[2]},
                .gyro_dps = {roll_rate + imu_roll_bias, pitch_rate + imu_pitch_bias, imu_invalid ? NAN : 0},
                .gyro_bias_valid = imu_gyro_bias_valid},
        .imu_updated_ms = imu_stamp_ms,
        .imu_error = imu_status,
    };
}

const lv_font_t *app_ui_chinese_font_size(unsigned px)
{
    static lv_font_t small, large;
    small = font_16;
    large = font_24;
    small.fallback = &lv_font_montserrat_16;
    large.fallback = &lv_font_montserrat_24;
    return px >= 24 ? &large : &small;
}

const lv_font_t *app_ui_chinese_font(void) { return app_ui_chinese_font_size(16); }

void app_ui_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char text[100];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    app_ui_label_text(label, text);
}

lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text, lv_align_t align,
                         int32_t x, int32_t y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font ? font : app_ui_chinese_font(), 0);
    lv_obj_align(label, align, x, y);
    return label;
}

void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback, uint32_t period_ms)
{
    lv_timer_set_cb(ui->timer, callback);
    lv_timer_set_period(ui->timer, period_ms);
    lv_timer_reset(ui->timer);
    lv_timer_resume(ui->timer);
}

void app_ui_clear_timer(app_ui_t *ui) { lv_timer_pause(ui->timer); }

static void header_back(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (ui->page_back) ui->page_back(ui);
    else app_ui_begin_page(ui, "应用列表");
}

lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    interest = 0;
    if (ui->page_cleanup) {
        void (*cleanup)(app_ui_t *) = ui->page_cleanup;
        ui->page_cleanup = NULL;
        cleanup(ui);
        ++cleanups;
        assert(!ui->page_context);
        if (expected_pointer_cleanup) {
            assert(lv_obj_get_event_count(expected_pointer_cleanup) == 0);
            expected_pointer_cleanup = NULL;
        }
        if (expected_draw_cleanup) {
            assert(lv_obj_get_event_count(expected_draw_cleanup) == 0);
            expected_draw_cleanup = NULL;
        }
    }
    app_ui_clear_timer(ui);
    ui->page_back = NULL;
    ui->screen = lv_obj_create(NULL);
    lv_obj_remove_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x070A12), 0);
    lv_obj_set_style_text_font(ui->screen, app_ui_chinese_font(), 0);
    lv_obj_t *back = lv_button_create(ui->screen);
    lv_obj_set_size(back, 48, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, APP_HEADER_SIDE_INSET, APP_HEADER_TOP_INSET);
    app_ui_add_text(back, LV_SYMBOL_LEFT, LV_ALIGN_CENTER, 0, 0, 0xBFD7FF, &lv_font_montserrat_20);
    lv_obj_add_event_cb(back, header_back, LV_EVENT_CLICKED, ui);
    app_ui_add_text(ui->screen, title, LV_ALIGN_TOP_MID, 0, 20, 0xEAF2FF, app_ui_chinese_font_size(24));
    ui->content = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->content, 448, 394);
    lv_obj_align(ui->content, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(ui->content, 24, 0);
    lv_obj_set_style_bg_color(ui->content, lv_color_hex(0x10192A), 0);
    lv_obj_set_style_pad_all(ui->content, 20, 0);
    lv_obj_set_style_border_width(ui->content, 0, 0);
    app_ui_load_screen(ui->screen);
    return ui->content;
}

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) {
        pitch_angle += pitch_rate * -0.00001745329252f;
        roll_angle += roll_rate * 0.00001745329252f;
        ++clock_ms;
        callback_elapsed_ms = 0;
        const unsigned imu_period = interest & APP_TELEMETRY_IMU_GAME ? 10U : 20U;
        if (!imu_frozen && clock_ms % imu_period == 0) imu_stamp_ms = clock_ms;
        lv_tick_inc(1);
        lv_timer_handler();
    }
}

static void rotate(float pitch, float roll, unsigned duration_ms)
{
    pitch_rate = pitch;
    roll_rate = roll;
    advance(duration_ms);
    pitch_rate = roll_rate = 0;
}

static void input_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = pointer;
    data->state = held ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 480 && y >= 0 && y < 480);
            frame[y * 480 + x] = *source++;
        }
    }
    lv_display_flush_ready(display);
}

static void screenshot(lv_display_t *display, const char *dir, const char *name)
{
    lv_refr_now(display);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fprintf(file, "P6\n480 480\n255\n") > 0);
    for (unsigned i = 0; i < 480 * 480; ++i) {
        const uint16_t p = frame[i];
        const uint8_t rgb[] = {((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        assert(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
    }
    assert(!fclose(file));
}

static lv_obj_t *find(lv_obj_t *parent, const char *text)
{
    if (lv_obj_check_type(parent, &lv_label_class) && !strcmp(lv_label_get_text(parent), text)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find(lv_obj_get_child(parent, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = find(parent, text);
    assert(label);
    lv_obj_t *obj = lv_obj_get_parent(label);
    assert(lv_obj_check_type(obj, &lv_button_class));
    return obj;
}

static bool label_visible(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = find(parent, text);
    if (!label) return false;
    for (lv_obj_t *object = label; object; object = lv_obj_get_parent(object)) {
        if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

static void press(lv_obj_t *obj, unsigned duration)
{
    lv_obj_update_layout(lv_obj_get_screen(obj));
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    pointer = (lv_point_t){.x = (area.x1 + area.x2) / 2, .y = (area.y1 + area.y2) / 2};
    held = true;
    advance(duration);
}

static void release(void)
{
    held = false;
    advance(45);
}

static void extend_drag(int32_t dx, int32_t dy)
{
    const lv_point_t start = pointer;
    for (int32_t step = 1; step <= 4; ++step) {
        pointer = (lv_point_t){start.x + dx * step / 4, start.y + dy * step / 4};
        advance(35);
    }
}

static void swipe(lv_obj_t *board, int32_t dx, int32_t dy)
{
    press(board, 45);
    extend_drag(dx, dy);
    release();
}

static void tap(lv_obj_t *obj)
{
    press(obj, 45);
    release();
}

static void click(lv_obj_t *parent, const char *label) { tap(button(parent, label)); }

static lv_obj_t *region_surface(lv_obj_t *board);

static lv_point_t region_point(lv_obj_t *board, arcade_direction_t direction)
{
    lv_obj_update_layout(lv_obj_get_screen(board));
    lv_area_t area;
    lv_obj_get_coords(direction == ARCADE_NONE ? region_surface(board) : board, &area);
    lv_point_t point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    if (direction == ARCADE_UP) point.y = area.y1 + lv_area_get_height(&area) / 7;
    if (direction == ARCADE_RIGHT) point.x = area.x2 - lv_area_get_width(&area) / 7;
    if (direction == ARCADE_DOWN) point.y = area.y2 - lv_area_get_height(&area) / 7;
    if (direction == ARCADE_LEFT) point.x = area.x1 + lv_area_get_width(&area) / 7;
    return point;
}

static lv_obj_t *region_board(lv_obj_t *content, int width, int height)
{
    lv_obj_update_layout(content);
    for (uint32_t i = 0; i < lv_obj_get_child_count(content); ++i) {
        lv_obj_t *child = lv_obj_get_child(content, (int32_t)i);
        if (lv_obj_get_width(child) == width && lv_obj_get_height(child) == height) {
            assert(!lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE));
            assert(!lv_obj_has_flag(content, LV_OBJ_FLAG_CLICKABLE));
            return child;
        }
    }
    assert(false && "drawn board is missing");
    return NULL;
}

static lv_obj_t *region_surface(lv_obj_t *board)
{
    lv_area_t board_area;
    lv_obj_get_coords(board, &board_area);
    lv_obj_t *screen = lv_obj_get_screen(board);
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
        lv_area_t area;
        lv_obj_get_coords(child, &area);
        if (area.x1 == 0 && area.x2 == 479 && area.y1 == board_area.y1 && area.y2 == 479 &&
            lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) return child;
    }
    assert(false && "full-width direction surface is missing");
    return NULL;
}

static void press_point(lv_point_t point)
{
    pointer = point;
    held = true;
    advance(45);
}

static void tap_point(lv_point_t point)
{
    press_point(point);
    release();
}

static void press_region(lv_obj_t *board, arcade_direction_t direction)
{
    press_point(region_point(board, direction));
}

static void tap_region(lv_obj_t *board, arcade_direction_t direction)
{
    press_region(board, direction);
    release();
}

static uint32_t board_checksum(lv_display_t *display, lv_obj_t *board)
{
    lv_refr_now(display);
    lv_area_t area;
    lv_obj_get_coords(board, &area);
    uint32_t hash = 5381;
    for (int y = area.y1; y <= area.y2; ++y) {
        for (int x = area.x1; x <= area.x2; ++x) hash = 33 * hash + frame[y * 480 + x];
    }
    return hash;
}

static lv_point_t snake_head(lv_display_t *display, lv_obj_t *board)
{
    lv_refr_now(display);
    lv_area_t area;
    lv_obj_get_coords(board, &area);
    const uint16_t head_green = (19U << 11) | (58U << 5) | 24U;
    for (unsigned row = 0; row < ARCADE_SNAKE_ROWS; ++row) {
        for (unsigned col = 0; col < ARCADE_SNAKE_COLUMNS; ++col) {
            const int x = area.x1 + 14 + col * 18;
            const int y = area.y1 + 15 + row * 18;
            if (frame[y * 480 + x] == head_green) return (lv_point_t){col, row};
        }
    }
    assert(false);
    return (lv_point_t){0};
}

static lv_point_t crate_player(lv_display_t *display, lv_obj_t *board)
{
    lv_refr_now(display);
    lv_area_t area;
    lv_obj_get_coords(board, &area);
    const uint16_t player_blue = (19U << 11) | (41U << 5) | 31U;
    for (unsigned row = 0; row < ARCADE_CRATE_SIDE; ++row) {
        for (unsigned col = 0; col < ARCADE_CRATE_SIDE; ++col) {
            const int x = area.x1 + 20 + col * 40;
            const int y = area.y1 + 27 + row * 40;
            if (frame[y * 480 + x] == player_blue) return (lv_point_t){col, row};
        }
    }
    assert(false);
    return (lv_point_t){0};
}

static void assert_tiles_at(lv_obj_t *board, const arcade_tiles_t *expected, unsigned line)
{
    for (unsigned i = 0; i < 16; ++i) {
        lv_obj_t *cell = lv_obj_get_child(board, i);
        char text[12] = "";
        if (expected->cells[i]) snprintf(text, sizeof(text), "%u", 1U << expected->cells[i]);
        const char *actual = lv_label_get_text(lv_obj_get_child(cell, 0));
        if (strcmp(actual, text)) printf("2048 line %u, cell %u: expected '%s', got '%s'\n", line, i, text, actual);
        assert(!strcmp(actual, text));
    }
}
#define assert_tiles(board, expected) assert_tiles_at(board, expected, __LINE__)

int main(int argc, char **argv)
{
    assert(argc == 2);
    setbuf(stdout, NULL);
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, input_read);
    app_ui_t ui = {0};
    ui.timer = lv_timer_create(NULL, 20, &ui);
    app_open_game(&ui);
    advance(150);
    assert(lv_obj_get_child_count(ui.content) == 6);
    screenshot(display, argv[1], "game-hub");
    click(ui.content, "2048");
    advance(120);
    assert(ui.page_back == app_open_game);
    assert(!interest && label_visible(ui.screen, "滑动棋盘") && label_visible(ui.screen, "滑动"));
    assert(!label_visible(ui.screen, LV_SYMBOL_GPS));
    assert(!find(ui.content, LV_SYMBOL_LIST));
    lv_obj_t *board = lv_obj_get_child(ui.content, 3);
    assert(lv_obj_get_width(board) >= 280 && lv_obj_get_height(board) >= 280);
    arcade_tiles_t expected_swipe;
    arcade_tiles_init(&expected_swipe, esp_random());
    swipe(board, 18, 0);
    swipe(board, 70, 70);
    press(board, 45);
    extend_drag(80, 0);
    lv_indev_reset(input, NULL);
    lv_indev_wait_release(input);
    release();
    press(board, 45);
    extend_drag(0, 85);
    sleeping = true;
    ui.page_suspend(&ui, true);
    advance(70);
    sleeping = false;
    ui.page_suspend(&ui, false);
    advance(40);
    release();
    assert_tiles(board, &expected_swipe);
    static const struct { int32_t x, y; arcade_direction_t direction; } swipes[] = {
        {-80, 4, ARCADE_LEFT}, {80, -8, ARCADE_RIGHT},
        {8, -90, ARCADE_UP}, {-9, 90, ARCADE_DOWN},
    };
    for (unsigned i = 0; i < sizeof(swipes) / sizeof(swipes[0]); ++i) {
        click(ui.content, LV_SYMBOL_REFRESH);
        assert(!interest && label_visible(ui.screen, "滑动"));
        arcade_tiles_init(&expected_swipe, esp_random());
        press(board, 45);
        extend_drag(swipes[i].x, swipes[i].y);
        assert_tiles(board, &expected_swipe);
        release();
        assert(arcade_tiles_move(&expected_swipe, swipes[i].direction));
        assert_tiles(board, &expected_swipe);
    }
    screenshot(display, argv[1], "game-2048-swipe");
    click(ui.content, "滑动");
    assert(interest && label_visible(ui.screen, "加速度") && label_visible(ui.screen, LV_SYMBOL_GPS));
    assert_tiles(board, &expected_swipe);
    swipe(board, 92, 0);
    assert_tiles(board, &expected_swipe);
    click(ui.content, "加速度");
    assert(!interest && label_visible(ui.screen, "滑动") && !label_visible(ui.screen, LV_SYMBOL_GPS));
    assert_tiles(board, &expected_swipe);
    click(ui.content, "滑动");
    click(ui.content, LV_SYMBOL_REFRESH);
    assert(interest == APP_TELEMETRY_IMU_GAME && find(ui.screen, "请拿稳设备"));
    imu_gravity = 8.882f;
    imu_gyro_bias_valid = false;
    imu_roll_bias = 8.58f;
    imu_pitch_bias = 3.19f;
    pitch_angle = -.06066f;
    roll_angle = -.02579f;
    const float still_pitch = pitch_angle;
    const float still_roll = roll_angle;
    advance(220);
    assert(interest == APP_TELEMETRY_IMU_GAME && find(ui.screen, "沿屏幕方向轻推"));
    imu_publish_during_read = true;
    advance(10);
    assert(!imu_publish_during_read && find(ui.screen, "沿屏幕方向轻推"));
    static const struct { unsigned axis; float impulse; arcade_direction_t direction; } pulses[] = {
        {0, -1.55f, ARCADE_LEFT}, {0, 1.55f, ARCADE_RIGHT},
        {1, 1.55f, ARCADE_UP}, {1, -1.55f, ARCADE_DOWN},
    };
    for (unsigned i = 0; i < sizeof(pulses) / sizeof(pulses[0]); ++i) {
        if (i) {
            click(ui.content, LV_SYMBOL_REFRESH);
            advance(220);
        }
        arcade_tiles_t expected;
        arcade_tiles_init(&expected, esp_random());
        assert_tiles(board, &expected);
        if (!i) {
            rotate(-120, 0, 150);
            advance(360);
            rotate(28, 0, 180);
            assert_tiles(board, &expected);
            rotate(24, 0, 540);
            advance(350);
            imu_translation[0] = imu_translation[1] = 3.2f;
            advance(65);
            memset(imu_translation, 0, sizeof(imu_translation));
            advance(355);
            assert_tiles(board, &expected);
        }
        imu_translation[pulses[i].axis] = pulses[i].impulse;
        advance(55);
        assert(arcade_tiles_move(&expected, pulses[i].direction));
        assert_tiles(board, &expected);
        assert(label_visible(ui.screen, "停稳后继续"));
        assert(lv_display_get_inactive_time(display) < 250);
        advance(360);
        imu_translation[pulses[i].axis] = -pulses[i].impulse * 1.2f;
        advance(80);
        memset(imu_translation, 0, sizeof(imu_translation));
        advance(345);
        assert_tiles(board, &expected);
    }
    uint32_t moved = board_checksum(display, board);
    sleeping = true;
    ui.page_suspend(&ui, true);
    assert(!interest);
    imu_translation[0] = 4.5f;
    advance(240);
    assert(board_checksum(display, board) == moved);
    memset(imu_translation, 0, sizeof(imu_translation));
    sleeping = false;
    ui.page_suspend(&ui, false);
    assert(interest && label_visible(ui.screen, "请拿稳设备"));
    advance(220);
    assert(label_visible(ui.screen, "沿屏幕方向轻推") && board_checksum(display, board) == moved);
    imu_frozen = true;
    advance(90);
    assert(board_checksum(display, board) == moved);
    advance(80);
    assert(label_visible(ui.screen, "传感器暂不可用 · 可切回滑动"));
    imu_frozen = false;
    roll_angle = still_roll;
    advance(220);
    assert(find(ui.screen, "沿屏幕方向轻推"));
    imu_accel_invalid = true;
    advance(40);
    assert(label_visible(ui.screen, "传感器暂不可用 · 可切回滑动") && board_checksum(display, board) == moved);
    imu_accel_invalid = false;
    imu_status = ESP_ERR_TIMEOUT;
    advance(100);
    assert(label_visible(ui.screen, "传感器暂不可用 · 可切回滑动"));
    imu_status = ESP_OK;
    pitch_angle = still_pitch;
    click(ui.content, LV_SYMBOL_GPS);
    assert(label_visible(ui.screen, "请拿稳设备"));
    advance(220);
    assert(find(ui.screen, "沿屏幕方向轻推"));
    screenshot(display, argv[1], "game-2048");
    click(ui.screen, LV_SYMBOL_LEFT);
    assert(!ui.page_context && !interest && cleanups == 1);
    pitch_angle = roll_angle = 0;
    imu_gravity = 9.80665f;
    imu_gyro_bias_valid = true;
    imu_roll_bias = imu_pitch_bias = 0;
    puts("PASS LVGL 2048 four-way swipe and acceleration, mode and board retention, tilt/diagonal/brake rejection, sleep, stale samples, error recovery and recenter");

    click(ui.content, "推箱子");
    assert(find(ui.screen, "1 / 6  ·  0 步"));
    board = region_board(ui.content, 320, 320);
    assert(region_surface(board));
    assert(lv_obj_get_parent(find(ui.screen, LV_SYMBOL_UP)) == board);
    lv_obj_t *undo = button(ui.content, "撤销");
    lv_obj_t *next = button(ui.content, LV_SYMBOL_NEXT);
    assert(lv_obj_has_state(undo, LV_STATE_DISABLED) && lv_obj_has_state(next, LV_STATE_DISABLED));
    lv_point_t player = crate_player(display, board);
    assert(player.x == 3 && player.y == 5);
    tap_region(board, ARCADE_NONE);
    const lv_point_t center = region_point(board, ARCADE_NONE);
    tap_point((lv_point_t){center.x + 4, center.y});
    assert(find(ui.screen, "1 / 6  ·  0 步"));
    tap_point((lv_point_t){center.x + 15, center.y});
    player = crate_player(display, board);
    assert(player.x == 4 && player.y == 5);
    tap(undo);
    sleeping = true;
    tap_region(board, ARCADE_UP);
    sleeping = false;
    assert(find(ui.screen, "1 / 6  ·  0 步"));
    press_region(board, ARCADE_UP);
    assert(find(ui.screen, "1 / 6  ·  1 步"));
    player = crate_player(display, board);
    assert(player.x == 3 && player.y == 4);
    pointer = region_point(board, ARCADE_LEFT);
    advance(600);
    release();
    assert(find(ui.screen, "1 / 6  ·  1 步"));
    tap_region(board, ARCADE_DOWN);
    player = crate_player(display, board);
    assert(player.x == 3 && player.y == 5);
    tap_region(board, ARCADE_RIGHT);
    player = crate_player(display, board);
    assert(player.x == 4 && player.y == 5);
    tap_region(board, ARCADE_LEFT);
    player = crate_player(display, board);
    assert(player.x == 3 && player.y == 5);
    lv_point_t margin = region_point(board, ARCADE_NONE);
    tap_point((lv_point_t){8, margin.y});
    player = crate_player(display, board);
    assert(player.x == 2 && player.y == 5);
    tap_point((lv_point_t){471, margin.y});
    player = crate_player(display, board);
    assert(player.x == 3 && player.y == 5);
    tap_point((lv_point_t){margin.x, 479});
    player = crate_player(display, board);
    assert(player.x == 3 && player.y == 6);
    click(ui.content, LV_SYMBOL_REFRESH);
    tap_region(board, ARCADE_UP);
    tap(undo);
    assert(find(ui.screen, "1 / 6  ·  0 步"));
    tap_region(board, ARCADE_UP);
    tap_region(board, ARCADE_UP);
    tap_region(board, ARCADE_RIGHT);
    tap_region(board, ARCADE_UP);
    tap_region(board, ARCADE_LEFT);
    assert(find(ui.screen, "通过  ·  点击右箭头") && !lv_obj_has_state(next, LV_STATE_DISABLED));
    screenshot(display, argv[1], "game-crates");
    tap(next);
    assert(find(ui.screen, "2 / 6  ·  0 步"));
    assert(arcade_preferences.crates_level == 1);
    click(ui.screen, LV_SYMBOL_LEFT);
    assert(cleanups == 2 && !interest);
    puts("PASS LVGL Sokoban 320×320, board and both screen margins/bottom touch, center/hold/sleep guards, undo, completion and next level");

    click(ui.content, "贪吃蛇");
    assert(!interest && label_visible(ui.screen, "开始游戏") && label_visible(ui.screen, "模式 · 触摸"));
    board = region_board(ui.content, 406, 318);
    assert(region_surface(board));
    assert(lv_obj_get_parent(find(ui.screen, LV_SYMBOL_UP)) == board);
    const uint32_t original = board_checksum(display, board);
    tap_region(board, ARCADE_UP);
    advance(600);
    assert(board_checksum(display, board) == original);
    screenshot(display, argv[1], "game-snake-start");
    click(ui.content, "开始游戏");
    assert(!label_visible(ui.screen, "开始游戏"));
    lv_point_t head = snake_head(display, board);
    tap_region(board, ARCADE_NONE);
    advance(450);
    lv_point_t after = snake_head(display, board);
    assert(after.x == head.x + 1 && after.y == head.y);
    static const struct { arcade_direction_t direction; int32_t x, y; } snake_regions[] = {
        {ARCADE_UP, 0, -1}, {ARCADE_LEFT, -1, 0},
        {ARCADE_DOWN, 0, 1}, {ARCADE_RIGHT, 1, 0},
    };
    for (unsigned i = 0; i < sizeof(snake_regions) / sizeof(snake_regions[0]); ++i) {
        head = after;
        if (i == 0) {
            press_region(board, snake_regions[i].direction);
            pointer = region_point(board, ARCADE_LEFT);
            advance(155);
            release();
        } else {
            lv_point_t turn = region_point(board, snake_regions[i].direction);
            if (snake_regions[i].x) turn.x = snake_regions[i].x < 0 ? 8 : 471;
            tap_point(turn);
            advance(155);
        }
        after = snake_head(display, board);
        assert((after.x - head.x) * snake_regions[i].x + (after.y - head.y) * snake_regions[i].y > 0);
        assert(snake_regions[i].x ? after.y == head.y : after.x == head.x);
    }
    click(ui.content, LV_SYMBOL_PAUSE);
    assert(find(ui.screen, "已暂停"));
    moved = board_checksum(display, board);
    tap_region(board, ARCADE_DOWN);
    advance(1000);
    assert(board_checksum(display, board) == moved);
    click(ui.content, "模式 · 触摸");
    assert(interest && label_visible(ui.screen, "模式 · 倾斜") &&
           !label_visible(ui.screen, LV_SYMBOL_UP));
    click(ui.content, LV_SYMBOL_PLAY);
    advance(100);
    head = snake_head(display, board);
    tap_region(board, ARCADE_DOWN);
    advance(180);
    after = snake_head(display, board);
    assert(after.y == head.y);
    head = after;
    rotate(0, -120, 145);
    advance(260);
    after = snake_head(display, board);
    assert(after.y < head.y);
    head = after;
    rotate(-120, 120, 145);
    advance(280);
    after = snake_head(display, board);
    assert(after.x < head.x);
    head = after;
    rotate(120, 120, 145);
    advance(280);
    after = snake_head(display, board);
    assert(after.y > head.y);
    head = after;
    rotate(120, -120, 145);
    advance(280);
    after = snake_head(display, board);
    assert(after.x > head.x);
    screenshot(display, argv[1], "game-snake");
    imu_status = ESP_ERR_TIMEOUT;
    advance(60);
    moved = board_checksum(display, board);
    advance(900);
    assert(board_checksum(display, board) == moved && find(ui.screen, "传感器暂不可用"));
    roll_angle = pitch_angle = 0;
    imu_status = ESP_ERR_NOT_FINISHED;
    click(ui.content, LV_SYMBOL_REFRESH);
    assert(interest && label_visible(ui.screen, "模式 · 倾斜") && label_visible(ui.screen, "开始游戏"));
    click(ui.content, "开始游戏");
    assert(label_visible(ui.screen, "等待校准"));
    advance(900);
    assert(label_visible(ui.screen, "等待校准"));
    imu_status = ESP_ERR_TIMEOUT;
    advance(60);
    assert(label_visible(ui.screen, "传感器暂不可用") && label_visible(ui.screen, "等待校准"));
    click(ui.content, "模式 · 倾斜");
    assert(!interest && !label_visible(ui.screen, "等待校准") &&
           label_visible(ui.screen, LV_SYMBOL_UP));
    head = snake_head(display, board);
    advance(530);
    after = snake_head(display, board);
    assert(after.x == head.x + 1 && after.y == head.y);
    advance(4000);
    assert(label_visible(ui.screen, "再玩一次") && label_visible(ui.screen, "游戏结束"));
    click(ui.content, "再玩一次");
    assert(!label_visible(ui.screen, "再玩一次") && label_visible(ui.screen, "模式 · 触摸"));
    head = snake_head(display, board);
    advance(540);
    assert(snake_head(display, board).x == head.x + 1);

    imu_status = ESP_OK;
    click(ui.content, LV_SYMBOL_REFRESH);
    click(ui.content, "模式 · 触摸");
    advance(100);
    assert(label_visible(ui.screen, "倾斜转向") && label_visible(ui.screen, "开始游戏"));
    click(ui.content, "开始游戏");
    head = snake_head(display, board);
    rotate(0, -200, 70);
    advance(30);
    rotate(200, 200, 70);
    advance(365);
    after = snake_head(display, board);
    assert(after.x == head.x && after.y == head.y - 1);
    advance(230);
    after = snake_head(display, board);
    assert(after.x == head.x + 1 && after.y == head.y - 1);

    imu_gyro_bias_valid = false;
    imu_gravity = 8.9f;
    imu_roll_bias = 8;
    pitch_angle = roll_angle = 0;
    click(ui.content, LV_SYMBOL_REFRESH);
    advance(160);
    assert(label_visible(ui.screen, "倾斜转向") && label_visible(ui.screen, "开始游戏"));
    click(ui.content, "开始游戏");
    head = snake_head(display, board);
    rotate(0, -120, 145);
    advance(420);
    after = snake_head(display, board);
    assert(after.x == head.x && after.y == head.y - 1);
    imu_gyro_bias_valid = true;
    imu_gravity = 9.80665f;
    imu_roll_bias = 0;
    pitch_angle = roll_angle = 0;
    click(ui.screen, LV_SYMBOL_LEFT);
    assert(cleanups == 3);
    puts("PASS LVGL Snake 406×318, board and both screen margin touch, pause/start/center/tilt guards, Replay, calibration fallback and gravity steering");

    click(ui.content, "跳一跳");
    board = lv_obj_get_child(ui.content, 3);
    assert(find(ui.screen, "按住蓄力，松手跳跃"));
    press(board, 300);
    screenshot(display, argv[1], "game-jump-charge");
    release();
    advance(500);
    assert(find(ui.screen, "完美落点  +2") && find(ui.screen, "得分  2"));
    advance(300);
    press(board, 120);
    lv_indev_reset(input, NULL);
    lv_indev_wait_release(input);
    release();
    advance(800);
    assert(find(ui.screen, "按住蓄力，松手跳跃") && find(ui.screen, "得分  2"));
    press(board, 220);
    sleeping = true;
    advance(100);
    release();
    sleeping = false;
    advance(800);
    assert(find(ui.screen, "按住蓄力，松手跳跃") && find(ui.screen, "得分  2"));
    press(board, 30);
    release();
    advance(800);
    assert(find(ui.screen, "点击游戏区再来一次"));
    press(board, 300);
    app_open_game(&ui);
    release();
    assert(!ui.page_context && !interest && cleanups == 4);
    puts("PASS LVGL jump charging, landing, input reset, sleep cancellation, falling and held exit");

    click(ui.content, "摇骰子");
    assert(!interest && label_visible(ui.content, "1 枚"));
    board = lv_obj_get_child(ui.content, 2);
    click(ui.content, "1 枚");
    click(ui.content, "2 枚");
    assert(label_visible(ui.content, "3 枚"));
    tap(board);
    assert(label_visible(ui.content, "摇动中"));
    advance(160);
    const uint32_t rolling_dice = board_checksum(display, board);
    sleeping = true;
    ui.page_suspend(&ui, true);
    advance(1500);
    assert(label_visible(ui.content, "摇动中") && board_checksum(display, board) == rolling_dice);
    sleeping = false;
    ui.page_suspend(&ui, false);
    advance(700);
    assert(!label_visible(ui.content, "摇动中"));
    screenshot(display, argv[1], "game-dice");
    click(ui.content, "3 枚");
    assert(label_visible(ui.content, "1 枚"));
    click(ui.content, "感应关");
    assert(interest == APP_TELEMETRY_IMU && label_visible(ui.content, "感应开"));
    advance(100);
    imu_gravity = 22;
    advance(135);
    assert(label_visible(ui.content, "摇动中"));
    advance(900);
    assert(!label_visible(ui.content, "摇动中"));
    advance(200);
    assert(!label_visible(ui.content, "摇动中"));
    imu_gravity = 9.80665f;
    click(ui.content, "感应开");
    assert(!interest);
    click(ui.content, "掷骰");
    assert(label_visible(ui.content, "摇动中"));
    expected_pointer_cleanup = board;
    tap(lv_obj_get_child(ui.screen, 0));
    assert(!expected_pointer_cleanup && cleanups == 5 && !ui.page_context && !interest);
    puts("PASS LVGL dice tray, 1–3 dice, manual and IMU rolling, sleep, stationary rearm and exit during animation");

    click(ui.content, "落块挑战");
    board = lv_obj_get_child(ui.content, 3);
    assert(lv_obj_get_width(board) == 178 && lv_obj_get_height(board) == 348);
    assert(label_visible(ui.content, "开始") && label_visible(ui.content, "下一块"));
    click(ui.content, "开始");
    uint32_t falling = board_checksum(display, board);
    click(ui.content, LV_SYMBOL_LEFT);
    assert(board_checksum(display, board) != falling);
    click(ui.content, LV_SYMBOL_RIGHT);
    assert(board_checksum(display, board) == falling);
    click(ui.content, "旋转");
    click(ui.content, LV_SYMBOL_DOWN);
    assert(board_checksum(display, board) != falling);
    click(ui.content, "快速落下");
    assert(!label_visible(ui.content, "得分  0"));
    screenshot(display, argv[1], "game-fall");
    click(ui.content, LV_SYMBOL_PAUSE);
    assert(label_visible(ui.content, "已暂停"));
    falling = board_checksum(display, board);
    advance(1700);
    assert(board_checksum(display, board) == falling);
    click(ui.content, "继续");
    falling = board_checksum(display, board);
    sleeping = true;
    ui.page_suspend(&ui, true);
    advance(3000);
    assert(board_checksum(display, board) == falling);
    sleeping = false;
    ui.page_suspend(&ui, false);
    advance(120);
    assert(board_checksum(display, board) == falling);
    advance(800);
    assert(board_checksum(display, board) != falling);
    click(ui.content, LV_SYMBOL_REFRESH);
    assert(label_visible(ui.content, "得分  0") && label_visible(ui.content, "开始"));
    click(ui.content, "开始");
    for (unsigned i = 0; i < 20 && !label_visible(ui.content, "游戏结束"); ++i)
        click(ui.content, "快速落下");
    assert(label_visible(ui.content, "游戏结束") && label_visible(ui.content, "再玩一次"));
    click(ui.content, "再玩一次");
    assert(!label_visible(ui.content, "游戏结束") && label_visible(ui.content, "得分  0"));
    expected_draw_cleanup = board;
    tap(lv_obj_get_child(ui.screen, 0));
    assert(!expected_draw_cleanup && cleanups == 6 && !ui.page_context && !interest);
    puts("PASS LVGL falling blocks movement, rotation, two drop speeds, pause, retained sleep state, reset and replay");

    click(ui.content, "2048");
    assert(interest && label_visible(ui.screen, "加速度"));
    click(ui.content, "加速度");
    board = lv_obj_get_child(ui.content, 3);
    assert(lv_obj_get_event_count(board) == 4);
    press(board, 45);
    extend_drag(84, 0);
    expected_pointer_cleanup = board;
    app_open_game(&ui);
    lv_indev_reset(input, NULL);
    lv_indev_wait_release(input);
    release();
    assert(!expected_pointer_cleanup && !ui.page_context && !interest && cleanups == 7);
    puts("PASS 2048 held-swipe exit unregisters all pointer callbacks before async screen retirement");

    static const char *const region_games[] = {"推箱子", "贪吃蛇"};
    for (unsigned i = 0; i < sizeof(region_games) / sizeof(region_games[0]); ++i) {
        click(ui.content, region_games[i]);
        if (i) {
            assert(interest && label_visible(ui.screen, "模式 · 倾斜"));
            click(ui.content, "模式 · 倾斜");
            click(ui.content, "开始游戏");
        } else assert(find(ui.screen, "2 / 6  ·  0 步"));
        board = i ? region_board(ui.content, 406, 318) : region_board(ui.content, 320, 320);
        lv_obj_t *surface = region_surface(board);
        assert(lv_obj_get_event_count(board) == 1 && lv_obj_get_event_count(surface) > 0);
        const lv_point_t center = region_point(board, ARCADE_NONE);
        press_point((lv_point_t){8, center.y});
        expected_pointer_cleanup = surface;
        expected_draw_cleanup = board;
        app_open_game(&ui);
        lv_indev_reset(input, NULL);
        lv_indev_wait_release(input);
        release();
        assert(!expected_pointer_cleanup && !expected_draw_cleanup && !ui.page_context && !interest);
    }
    assert(cleanups == 9);
    puts("PASS held margin-touch exit retires both game boards' draw and surface callbacks safely");
    puts("PASS reopening 2048 and Snake restores independent control modes; Sokoban resumes its saved level");

    static const char *const games[] = {"2048", "推箱子", "贪吃蛇", "跳一跳", "摇骰子", "落块挑战"};
    for (unsigned i = 0; i < 24; ++i) {
        click(ui.content, games[i % (sizeof(games) / sizeof(games[0]))]);
        app_open_game(&ui);
        advance(40);
        assert(!ui.page_context && !interest && lv_mem_test() == LV_RESULT_OK);
    }
    assert(cleanups == 33);
    click(ui.screen, LV_SYMBOL_LEFT);
    assert(label_visible(ui.screen, "应用列表") && !ui.page_context);
    puts("PASS the common header Back returns from a game to the hub and from the hub to the drawer");
    lv_timer_delete(ui.timer);
    lv_deinit();
    puts("PASS 24 game entry/exit cycles with LVGL");
    return 0;
}
