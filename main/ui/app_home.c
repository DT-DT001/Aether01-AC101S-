#include <app_ui_internal.h>
#include "app_home.h"
#include "app_screen.h"
#include "app_battery.h"
#include "app_audio_system.h"
#include "app_telemetry.h"
#include "app_utilities.h"
#include "app_theme.h"
#include "aether_wifi.h"
#include "../assets/icons/app_icons.h"
#include <stdlib.h>

typedef struct {
    const lv_image_dsc_t *icon;
    const char *name;
    uint32_t accent;
    void (*open)(app_ui_t *);
    const char *symbol;
} home_app_t;

static const home_app_t apps[] = {
    {&icon_clock, "时钟", 0xC4AEFF, app_open_clock, NULL},
    {NULL, "天气", 0xF2BD74, app_open_weather, LV_SYMBOL_TINT},
    {NULL, "日历", 0xA8BCFF, app_open_calendar, LV_SYMBOL_LIST},
    {NULL, "记事本", 0xF0D487, app_open_notes, LV_SYMBOL_EDIT},
    {&icon_music, "音乐", 0xFFAC88, app_open_music, NULL},
    {&icon_reader, "阅读", 0x7EDDB6, app_open_reader, NULL},
    {&icon_video, "视频", 0xD4ABFF, app_open_video, NULL},
    {&icon_files, "文件", 0x9ABEFF, app_open_files, NULL},
    {&icon_game, "小游戏", 0xFF9DAE, app_open_game, NULL},
    {&icon_nes, "NES 游戏", 0xBBA8F5, app_open_nes, NULL},
    {NULL, "录音", 0xFF9FAD, app_open_recorder, NULL},
    {&icon_settings, "设置", 0xBCC9E5, app_open_settings, NULL},
    {NULL, "姿态", 0xFFA4CA, app_open_motion, NULL},
    {&icon_barometer, "气压高度", 0xA0DBFF, app_open_barometer, NULL},
    {NULL, "位移测量", 0x97DFCE, app_open_displacement, NULL},
    {NULL, "空中飞鼠", 0x94D9ED, app_open_pc_remote, NULL},
    {NULL, "OBS 切换", 0xD7B3FF, app_open_obs_switcher, LV_SYMBOL_SHUFFLE},
    {&icon_network, "无线对讲", 0x8FDBEC, app_open_intercom, NULL},
    {NULL, "定位", 0x69E4AD, app_open_gps, NULL},
    {NULL, "USB 摄像头", 0x95D9EE, app_open_camera, NULL},
    {NULL, "图库", 0xF5CD91, app_open_gallery, LV_SYMBOL_IMAGE},
    {NULL, "计算器", 0x88C8FA, app_open_calculator, NULL},
    {NULL, "主题", 0xCEACFA, app_open_theme, NULL},
};

size_t app_home_app_count(void) { return sizeof(apps) / sizeof(apps[0]); }

enum {
    HOME_WIFI, HOME_BT, HOME_SD, HOME_DATE, HOME_CITY, HOME_TEMP, HOME_CONDITION,
    HOME_SUN, HOME_CLOUD, HOME_RAIN,
};

static void status_color(lv_obj_t *icon, uint32_t value)
{
    const lv_color_t color = lv_color_hex(value);
    if (!lv_color_eq(lv_obj_get_style_text_color(icon, 0), color))
        lv_obj_set_style_text_color(icon, color, 0);
}

static void update_status(app_ui_t *ui)
{
    const uint32_t inactive = 0x7984A2, waiting = 0xE9BB78, active = 0xA4EBD4;
    aether_wifi_status_t wifi = {0};
    (void)aether_wifi_get_status(aether_wifi_get_default(), &wifi);
    status_color(ui->app_obj[HOME_WIFI], wifi.connected ? active : wifi.enabled ? waiting : inactive);

    aether_bluetooth_handle_t bluetooth = app_audio_system_bluetooth();
    aether_bluetooth_status_t audio = {0};
    aether_bluetooth_hid_status_t input = {0};
    if (bluetooth) {
        (void)aether_bluetooth_get_status(bluetooth, &audio);
        (void)aether_bluetooth_hid_get_status(bluetooth, &input);
    }
    status_color(ui->app_obj[HOME_BT], audio.connected || input.connected ? active :
                 audio.enabled || input.requested ? waiting : inactive);

    aether_storage_status_t storage = {0};
    (void)aether_storage_get_status(ui->storage, &storage);
    status_color(ui->app_obj[HOME_SD], storage.mounted ? active : storage.card_inserted ? waiting : inactive);
}

static void show_when(lv_obj_t *object, bool visible)
{
    if (visible && lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else if (!visible && !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void update_home(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    update_status(ui);
    if (!ui->home_time) return;
    app_telemetry_snapshot_t telemetry;
    app_telemetry_get(&telemetry);
    if (telemetry.rtc_error == ESP_OK && !telemetry.rtc.oscillator_stopped) {
        const pcf85063_time_t *now = &telemetry.rtc;
        static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        app_ui_label_fmt(ui->home_time, "%02u:%02u", now->hour, now->minute);
        app_ui_label_fmt(ui->app_obj[HOME_DATE], "%u月%u日  星期%s", now->month, now->day,
                         now->weekday < 7 ? weekdays[now->weekday] : "?");
    } else {
        app_ui_label_text(ui->home_time, "--:--");
        app_ui_label_text(ui->app_obj[HOME_DATE], "");
    }
    app_weather_refresh(false);
    app_weather_snapshot_t weather;
    app_weather_get(&weather);
    app_ui_label_text(ui->app_obj[HOME_CITY], app_weather_city(weather.city));
    if (weather.valid) app_ui_label_fmt(ui->app_obj[HOME_TEMP], "%d°", weather.temperature_c);
    else app_ui_label_text(ui->app_obj[HOME_TEMP], "--°");
    app_ui_label_text(ui->app_obj[HOME_CONDITION], weather.valid ?
                      app_weather_condition(weather.weather_code) : "");
    show_when(ui->app_obj[HOME_SUN], weather.valid && weather.weather_code <= 2);
    show_when(ui->app_obj[HOME_CLOUD], !weather.valid || weather.weather_code != 0);
    show_when(ui->app_obj[HOME_RAIN], weather.valid && weather.weather_code >= 51);
}

static void back_to_desktop(lv_event_t *event)
{
    app_ui_show_home(lv_event_get_user_data(event));
}

static void open_app(lv_event_t *event)
{
    const home_app_t *app = lv_event_get_user_data(event);
    lv_obj_t *tile = lv_event_get_target_obj(event);
    app_ui_t *ui = lv_obj_get_user_data(tile);
    if (!app || !ui || lv_obj_get_parent(tile) != ui->content) return;
    int32_t y = lv_obj_get_scroll_y(ui->content);
    int32_t end = y + lv_obj_get_scroll_bottom(ui->content);
    ui->drawer_scroll_y = LV_CLAMP(0, y, LV_MAX(0, end));
    ui->drawer_focus_index = (size_t)(app - apps);
    app->open(ui);
}

/* A small vector microphone keeps this entry distinct without another bitmap. */
static lv_obj_t *microphone_icon(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 48, 48);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *capsule = lv_obj_create(icon);
    lv_obj_remove_style_all(capsule);
    lv_obj_remove_flag(capsule, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(capsule, 18, 2);
    lv_obj_set_size(capsule, 12, 25);
    lv_obj_set_style_radius(capsule, 6, 0);
    lv_obj_set_style_bg_color(capsule, lv_color_hex(0xFF9FAD), 0);
    lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, 0);
    static const lv_point_precise_t cradle[] = {
        {10, 18}, {10, 24}, {14, 30}, {20, 33}, {28, 33},
        {34, 30}, {38, 24}, {38, 18}
    };
    lv_obj_t *line = lv_line_create(icon);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_line_set_points(line, cradle, sizeof(cradle) / sizeof(cradle[0]));
    lv_obj_set_style_line_color(line, lv_color_hex(0xFFD2DA), 0);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    static const lv_point_precise_t stand[] = {{24, 34}, {24, 41}, {16, 41}, {32, 41}};
    line = lv_line_create(icon);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_line_set_points(line, stand, sizeof(stand) / sizeof(stand[0]));
    lv_obj_set_style_line_color(line, lv_color_hex(0xFFD2DA), 0);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return icon;
}

static lv_obj_t *gps_icon(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(icon, 48, 48);
    lv_obj_t *ring = lv_obj_create(icon);
    lv_obj_remove_style_all(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ring, 8, 8); lv_obj_set_size(ring, 32, 32);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x69E4AD), 0);
    lv_obj_t *center = lv_obj_create(icon);
    lv_obj_remove_style_all(center);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(center, 19, 19); lv_obj_set_size(center, 10, 10);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(0xB6FCE0), 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    static const lv_point_precise_t ticks[4][2] = {
        {{24, 2}, {24, 12}}, {{24, 36}, {24, 46}},
        {{2, 24}, {12, 24}}, {{36, 24}, {46, 24}},
    };
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_t *line = lv_line_create(icon);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_line_set_points(line, ticks[i], 2);
        lv_obj_set_style_line_width(line, 3, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(0x69E4AD), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
    }
    return icon;
}

static lv_obj_t *shape(lv_obj_t *parent, int w, int h, int radius, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static void status_bar(app_ui_t *ui, lv_obj_t *screen)
{
    const lv_font_t *font = &lv_font_montserrat_20;
    const uint32_t muted = 0x7984A2;
    lv_obj_t *battery = app_battery_create(screen, 70);
    if (battery) lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -APP_HEADER_SIDE_INSET, 18);
    ui->app_obj[HOME_WIFI] = app_ui_add_text(screen, LV_SYMBOL_WIFI, LV_ALIGN_TOP_RIGHT, -192, 21, muted, font);
    ui->app_obj[HOME_BT] = app_ui_add_text(screen, LV_SYMBOL_BLUETOOTH, LV_ALIGN_TOP_RIGHT, -158, 21, muted, font);
    ui->app_obj[HOME_SD] = app_ui_add_text(screen, LV_SYMBOL_SD_CARD, LV_ALIGN_TOP_RIGHT, -124, 21, muted, font);
}

static lv_obj_t *air_mouse_icon(lv_obj_t *parent)
{
    static const lv_point_precise_t outline[] = {
        {14, 8}, {14, 35}, {21, 29}, {26, 39}, {31, 36}, {26, 26}, {36, 26}, {14, 8},
    };
    lv_obj_t *tile = shape(parent, 48, 48, 14, 0x27374A);
    lv_obj_t *cursor = lv_line_create(tile);
    lv_line_set_points(cursor, outline, sizeof(outline) / sizeof(outline[0]));
    lv_obj_set_style_line_width(cursor, 3, 0);
    lv_obj_set_style_line_color(cursor, lv_color_hex(0x94D9ED), 0);
    lv_obj_set_style_line_rounded(cursor, true, 0);
    lv_obj_remove_flag(cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return tile;
}

static void icon_stroke(lv_obj_t *icon, const lv_point_precise_t *points, unsigned count, uint32_t color)
{
    lv_obj_t *edge = lv_line_create(icon);
    lv_line_set_points(edge, points, count);
    lv_obj_set_style_line_width(edge, 2, 0);
    lv_obj_set_style_line_color(edge, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(edge, true, 0);
    lv_obj_remove_flag(edge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *utility_icon(lv_obj_t *parent, const home_app_t *app)
{
    lv_obj_t *icon = shape(parent, 48, 48, 14, 0x292E48);
    if (app->open == app_open_calculator) {
        lv_obj_set_pos(shape(icon, 26, 36, 5, app->accent), 11, 6);
        lv_obj_set_pos(shape(icon, 18, 8, 2, 0x263A58), 15, 10);
        for (unsigned y = 0; y < 3; ++y)
            for (unsigned x = 0; x < 3; ++x)
                lv_obj_set_pos(shape(icon, 4, 4, 1, 0x263A58), 15 + 7 * x, 22 + 6 * y);
    } else if (app->open == app_open_theme) {
        lv_obj_set_pos(shape(icon, 23, 23, LV_RADIUS_CIRCLE, 0xA78EF0), 12, 8);
        lv_obj_set_pos(shape(icon, 21, 21, LV_RADIUS_CIRCLE, 0xFF9EBD), 7, 21);
        lv_obj_set_pos(shape(icon, 21, 21, LV_RADIUS_CIRCLE, 0x85DDDB), 21, 21);
    } else if (app->open == app_open_camera) {
        lv_obj_set_pos(shape(icon, 15, 7, 3, app->accent), 16, 9);
        lv_obj_set_pos(shape(icon, 34, 24, 5, app->accent), 7, 14);
        lv_obj_set_pos(shape(icon, 16, 16, LV_RADIUS_CIRCLE, 0x263A58), 16, 18);
        lv_obj_set_pos(shape(icon, 8, 8, LV_RADIUS_CIRCLE, 0x798CBD), 20, 22);
    } else if (app->open == app_open_motion) {
        static const lv_point_precise_t edges[] = {
            {24, 6}, {39, 14}, {39, 32}, {24, 41}, {9, 32}, {9, 14}, {24, 6},
        };
        static const lv_point_precise_t fold[] = {{9, 14}, {24, 23}, {39, 14}};
        static const lv_point_precise_t axis[] = {{24, 23}, {24, 41}};
        icon_stroke(icon, edges, sizeof(edges) / sizeof(edges[0]), app->accent);
        icon_stroke(icon, fold, sizeof(fold) / sizeof(fold[0]), app->accent);
        icon_stroke(icon, axis, sizeof(axis) / sizeof(axis[0]), app->accent);
    } else {
        static const lv_point_precise_t ruler[] = {
            {8, 32}, {31, 9}, {40, 18}, {17, 41}, {8, 32},
        };
        static const lv_point_precise_t ticks[][2] = {
            {{14, 27}, {18, 31}}, {{20, 21}, {24, 25}}, {{26, 15}, {30, 19}},
        };
        icon_stroke(icon, ruler, sizeof(ruler) / sizeof(ruler[0]), app->accent);
        for (unsigned i = 0; i < sizeof(ticks) / sizeof(ticks[0]); ++i)
            icon_stroke(icon, ticks[i], 2, app->accent);
    }
    return icon;
}

void app_ui_show_drawer(app_ui_t *ui)
{
    lv_obj_t *screen = app_ui_new_screen(ui);
    lv_obj_set_style_bg_color(screen, lv_color_hex(app_theme_background(0x141827)), 0);
    status_bar(ui, screen);
    lv_obj_t *home = lv_button_create(screen);
    lv_obj_set_size(home, 50, 42);
    lv_obj_set_pos(home, APP_HEADER_SIDE_INSET, APP_HEADER_TOP_INSET);
    lv_obj_set_style_bg_color(home, lv_color_hex(app_theme_background(0x242940)), 0);
    lv_obj_set_style_shadow_width(home, 0, 0);
    app_ui_add_text(home, LV_SYMBOL_HOME, LV_ALIGN_CENTER, 0, 0, 0xD6DDF7, &lv_font_montserrat_20);
    lv_obj_add_event_cb(home, back_to_desktop, LV_EVENT_CLICKED, ui);
    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_set_size(grid, 448, 410);
    lv_obj_set_pos(grid, 16, 60);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0x7794BB), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(grid, 3, LV_PART_SCROLLBAR);
    ui->content = grid;
    for (size_t i = 0; i < app_home_app_count(); ++i) {
        lv_obj_t *tile = lv_button_create(grid);
        lv_obj_set_size(tile, 102, 90);
        lv_obj_set_style_radius(tile, 17, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(app_theme_background(0x20253A)), 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(app_theme_background(0x343C5F)), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x2B334D), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(apps[i].accent), LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(tile, ui);
        lv_obj_add_event_cb(tile, open_app, LV_EVENT_CLICKED, (void *)&apps[i]);
        lv_obj_t *icon;
        if (apps[i].icon) {
            icon = lv_image_create(tile);
            lv_image_set_src(icon, apps[i].icon);
        } else if (apps[i].symbol) {
            icon = shape(tile, 48, 48, 14, 0x292E48);
            app_ui_add_text(icon, apps[i].symbol, LV_ALIGN_CENTER, 0, 0, apps[i].accent,
                            &lv_font_montserrat_28);
        } else if (apps[i].open == app_open_gps) {
            icon = gps_icon(tile);
        } else if (apps[i].open == app_open_pc_remote) {
            icon = air_mouse_icon(tile);
        } else if (apps[i].open == app_open_recorder) {
            icon = microphone_icon(tile);
        } else icon = utility_icon(tile, &apps[i]);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t *name = app_ui_add_text(tile, apps[i].name, LV_ALIGN_BOTTOM_MID,
                                        0, -10, 0xE2EBFA, app_ui_chinese_font());
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(name, 94);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    }
    app_ui_load_screen(screen);
    lv_obj_t *focused = lv_obj_get_child(grid, (int32_t)ui->drawer_focus_index);
    if (focused && lv_obj_get_group(focused)) {
        /* Claim focus before the old screen's removal can scroll to another tile. */
        const bool scroll_on_focus = lv_obj_has_flag(focused, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        if (scroll_on_focus) lv_obj_remove_flag(focused, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_group_focus_obj(focused);
        if (scroll_on_focus) lv_obj_add_flag(focused, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    }
    if (ui->drawer_scroll_y) {
        lv_obj_update_layout(grid);
        lv_obj_scroll_to_y(grid, ui->drawer_scroll_y, LV_ANIM_OFF);
    }
    app_ui_set_user_key_exit(ui, true);
    app_ui_set_timer(ui, update_home, 1000);
    update_home(ui->timer);
}

static void desktop_swipe(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        if (lv_event_get_param(event) == ui->home_swipe_input) ui->home_swipe_input = NULL;
        return;
    }
    lv_indev_t *input = lv_event_get_indev(event);
    if (lv_event_get_target_obj(event) != ui->screen ||
        !input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;

    if (code == LV_EVENT_PRESSED) {
        ui->home_swipe_input = input;
        lv_indev_get_point(input, &ui->home_swipe_origin);
        return;
    }
    if (ui->home_swipe_input != input) return;
    if (code != LV_EVENT_PRESSING) {
        ui->home_swipe_input = NULL;
        return;
    }

    lv_point_t point;
    lv_indev_get_point(input, &point);
    int32_t up = ui->home_swipe_origin.y - point.y;
    if (up < 44 || up <= abs(point.x - ui->home_swipe_origin.x)) return;

    ui->home_swipe_input = NULL;
    /* A finger held across the screen change must not select a drawer tile. */
    lv_indev_wait_release(input);
    app_ui_show_drawer(ui);
}

void app_ui_show_home(app_ui_t *ui)
{
    lv_obj_t *screen = app_ui_new_screen(ui);
    ui->home_swipe_input = NULL;
    ui->drawer_scroll_y = 0;
    ui->drawer_focus_index = 0;
    lv_obj_set_style_bg_color(screen, lv_color_hex(app_theme_background(0x1D2540)), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(app_theme_background(0x101322)), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    const bool has_wallpaper = app_theme_wallpaper_view(screen) != NULL;
    if (has_wallpaper) {
        /* Shade the text area and fade toward the lower part of the photo. */
        lv_obj_t *shade = shape(screen, 480, 480, 0, 0x0A101D);
        lv_obj_center(shade);
        lv_obj_set_style_bg_grad_color(shade, lv_color_hex(0x0A101D), 0);
        lv_obj_set_style_bg_grad_dir(shade, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_stop(shade, 156, 0);
        lv_obj_set_style_bg_grad_stop(shade, 229, 0);
        lv_obj_set_style_bg_main_opa(shade, LV_OPA_70, 0);
        lv_obj_set_style_bg_grad_opa(shade, LV_OPA_20, 0);
    }
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, desktop_swipe, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(screen, desktop_swipe, LV_EVENT_PRESSING, ui);
    lv_obj_add_event_cb(screen, desktop_swipe, LV_EVENT_RELEASED, ui);
    lv_obj_add_event_cb(screen, desktop_swipe, LV_EVENT_PRESS_LOST, ui);
    lv_obj_add_event_cb(screen, desktop_swipe, LV_EVENT_INDEV_RESET, ui);
    status_bar(ui, screen);
    ui->home_time = app_ui_add_text(screen, "--:--", LV_ALIGN_TOP_MID,
                                    0, 76, 0xF6F1FF, &lv_font_montserrat_48);
    ui->app_obj[HOME_DATE] = app_ui_add_text(screen, "", LV_ALIGN_TOP_MID,
                                           0, 137, 0xD5D6F3, app_ui_chinese_font_size(24));

    lv_obj_t *weather = shape(screen, 392, 116, 0, 0);
    lv_obj_set_style_bg_opa(weather, LV_OPA_TRANSP, 0);
    lv_obj_align(weather, LV_ALIGN_TOP_MID, 0, 181);
    ui->app_obj[HOME_SUN] = shape(weather, 38, 38, LV_RADIUS_CIRCLE, 0xF8CE82);
    lv_obj_set_pos(ui->app_obj[HOME_SUN], 26, 25);
    lv_obj_t *cloud = shape(weather, 76, 38, 0, 0x262D48);
    lv_obj_set_style_bg_opa(cloud, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(cloud, 27, 38);
    lv_obj_set_pos(shape(cloud, 69, 22, 11, 0xD8DDFB), 0, 16);
    lv_obj_set_pos(shape(cloud, 36, 32, 16, 0xD8DDFB), 9, 0);
    lv_obj_set_pos(shape(cloud, 25, 23, 12, 0xD8DDFB), 38, 10);
    ui->app_obj[HOME_CLOUD] = cloud;
    ui->app_obj[HOME_RAIN] = app_ui_add_text(weather, LV_SYMBOL_TINT, LV_ALIGN_TOP_LEFT, 51, 78,
                                            0x9ACCFF, &lv_font_montserrat_20);
    ui->app_obj[HOME_CITY] = app_ui_add_text(weather, "", LV_ALIGN_TOP_LEFT, 114, 27,
                                            0xEFEBFF, app_ui_chinese_font_size(24));
    ui->app_obj[HOME_CONDITION] = app_ui_add_text(weather, "", LV_ALIGN_TOP_LEFT, 115, 66,
                                                 0xDEE5F8, app_ui_chinese_font());
    ui->app_obj[HOME_TEMP] = app_ui_add_text(weather, "--°", LV_ALIGN_RIGHT_MID, -24, 0,
                                            0xF5EFFF, &lv_font_montserrat_48);
    lv_obj_t *swipe_hint = app_ui_add_text(screen, LV_SYMBOL_UP, LV_ALIGN_BOTTOM_MID,
                                          0, -58, 0xD7DBFA, &lv_font_montserrat_20);
    if (has_wallpaper) {
        lv_obj_set_style_bg_color(swipe_hint, lv_color_hex(0x0A101D), 0);
        lv_obj_set_style_bg_opa(swipe_hint, LV_OPA_50, 0);
        lv_obj_set_style_radius(swipe_hint, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(swipe_hint, 8, 0);
        lv_obj_set_style_pad_ver(swipe_hint, 3, 0);
    }
    lv_obj_t *handle = shape(screen, 112, 5, 3, app_theme_color(0x747AA6));
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -24);
    app_ui_load_screen(screen);
    app_ui_set_timer(ui, update_home, 1000);
    update_home(ui->timer);
}
