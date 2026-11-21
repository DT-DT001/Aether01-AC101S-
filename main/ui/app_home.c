#include <app_ui_internal.h>
#include "app_home.h"
#include "app_battery.h"
#include "app_audio_system.h"
#include "app_telemetry.h"
#include "../assets/icons/app_icons.h"

typedef struct {
    const lv_image_dsc_t *icon;
    const char *name;
    uint32_t accent;
    void (*open)(app_ui_t *);
} home_app_t;

static const home_app_t apps[] = {
    {&icon_music, "音乐", 0xFFAC88, app_open_music},
    {&icon_reader, "阅读", 0x7EDDB6, app_open_reader},
    {&icon_video, "视频", 0xD4ABFF, app_open_video},
    {&icon_files, "文件", 0x9ABEFF, app_open_files},
    {&icon_nes, "NES 游戏", 0xBBA8F5, app_open_nes},
    {&icon_game, "小游戏", 0xFF9DAE, app_open_game},
    {&icon_clock, "时钟", 0xC4AEFF, app_open_clock},
    {&icon_settings, "设置", 0xBCC9E5, app_open_settings},
    {&icon_wifi, "无线网络", 0x85DCFF, app_open_wifi},
    {&icon_bluetooth, "蓝牙耳机", 0xA3C2FF, app_open_bluetooth},
    {&icon_status, "设备状态", 0x92B8FF, app_open_status},
    {&icon_motion, "运动", 0xFFA4CA, app_open_motion},
    {&icon_barometer, "气压高度", 0xA0DBFF, app_open_barometer},
    {NULL, "录音", 0xFF9FAD, app_open_recorder},
    {&icon_bluetooth, "电脑遥控", 0x94D9ED, app_open_pc_remote},
    {&icon_video, "OBS 切换", 0xD7B3FF, app_open_obs_switcher},
    {&icon_motion, "位移测量", 0x97DFCE, app_open_displacement},
    {&icon_network, "无线对讲", 0x8FDBEC, app_open_intercom},
};

size_t app_home_app_count(void) { return sizeof(apps) / sizeof(apps[0]); }

static void update_home(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    app_telemetry_snapshot_t telemetry;
    app_telemetry_get(&telemetry);
    if (telemetry.rtc_error == ESP_OK) {
        const pcf85063_time_t *now = &telemetry.rtc;
        static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        app_ui_label_fmt(ui->home_time, "%02u:%02u", now->hour, now->minute);
        app_ui_label_fmt(ui->home_date, "%u月%u日  星期%s", now->month, now->day,
                         now->weekday < 7 ? weekdays[now->weekday] : "?");
    } else {
        app_ui_label_text(ui->home_time, "--:--");
        app_ui_label_text(ui->home_date, "时钟待校准");
    }
    aether_storage_status_t storage = {0};
    const esp_err_t result = aether_storage_get_status(ui->storage, &storage);
    app_ui_label_text(ui->home_sd, result != ESP_OK ? "TF 不可用" :
                      storage.mounted ? "TF 已就绪" : storage.card_inserted ? "TF 待挂载" : "未插 TF 卡");
    app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    if (!audio.ready) app_ui_label_text(ui->home_audio, "音频待就绪");
    else if (!audio.master_volume) app_ui_label_text(ui->home_audio, "已静音");
    else app_ui_label_fmt(ui->home_audio, "%s %u%%",
                          audio.output == APP_AUDIO_OUTPUT_BLUETOOTH ? "耳机" : "音量",
                          audio.master_volume);
}

static void open_app(lv_event_t *event)
{
    const home_app_t *app = lv_event_get_user_data(event);
    app_ui_t *ui = lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (app && ui) app->open(ui);
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

void app_ui_show_home(app_ui_t *ui)
{
    lv_obj_t *screen = app_ui_new_screen(ui);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080D18), 0);
    ui->home_time = app_ui_add_text(screen, "--:--", LV_ALIGN_TOP_LEFT,
                                    24, 20, 0xF2F6FF, &lv_font_montserrat_28);
    ui->home_date = app_ui_add_text(screen, "", LV_ALIGN_TOP_LEFT,
                                    25, 58, 0x9BAFCB, app_ui_chinese_font());
    lv_obj_t *battery = app_battery_create(screen, 128);
    if (battery) lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -24, 18);
    ui->home_sd = app_ui_add_text(screen, "", LV_ALIGN_TOP_RIGHT,
                                  -24, 48, 0xB5C8DF, app_ui_chinese_font());
    ui->home_audio = app_ui_add_text(screen, "", LV_ALIGN_TOP_RIGHT,
                                     -24, 72, 0x8299B8, app_ui_chinese_font());
    app_ui_add_text(screen, "应用", LV_ALIGN_TOP_LEFT, 25, 100,
                    0xD2DFF1, app_ui_chinese_font());

    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_set_size(grid, 448, 342);
    lv_obj_set_pos(grid, 16, 126);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0x7794BB), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(grid, 3, LV_PART_SCROLLBAR);
    ui->content = grid;
    for (size_t i = 0; i < app_home_app_count(); ++i) {
        lv_obj_t *tile = lv_button_create(grid);
        lv_obj_set_size(tile, 100, 106);
        lv_obj_set_style_radius(tile, 22, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x141E30), 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x263952), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x243249), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(apps[i].accent), LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(tile, ui);
        lv_obj_add_event_cb(tile, open_app, LV_EVENT_CLICKED, (void *)&apps[i]);
        lv_obj_t *icon;
        if (apps[i].icon) {
            icon = lv_image_create(tile);
            lv_image_set_src(icon, apps[i].icon);
        } else icon = microphone_icon(tile);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_t *name = app_ui_add_text(tile, apps[i].name, LV_ALIGN_BOTTOM_MID,
                                        0, -17, 0xE2EBFA, app_ui_chinese_font());
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(name, 94);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    }
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    app_ui_set_timer(ui, update_home, 1000);
    update_home(ui->timer);
}
