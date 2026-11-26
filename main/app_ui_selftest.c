#include "app_ui_selftest.h"
#include "app_pinyin_extended.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aether_media_fs.h"
#include "aether_recorder.h"
#include "aether_storage.h"
#include "aether_wifi.h"
#include "app_audio_system.h"
#include "app_display_metrics.h"
#include "app_home.h"
#include "app_media_image.h"
#include "app_motion_filter.h"
#include "app_otg_control.h"
#include "app_telemetry.h"
#include "app_text_input.h"
#include "app_theme.h"
#include "app_wallpaper_store.h"
#include "app_ui_internal.h"
#include "app_utilities.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "app_ui_selftest_mic.h"
#include "app_ui_selftest_files.h"
#include "app_ui_selftest_nes.h"

#ifndef AETHER_UI_SELFTEST_ROUNDS
#define AETHER_UI_SELFTEST_ROUNDS 8
#endif
#ifndef AETHER_UI_SELFTEST_SILENT
#define AETHER_UI_SELFTEST_SILENT 0
#endif
enum { ROUNDS = AETHER_UI_SELFTEST_ROUNDS, DRAWER_BACK_ROUND = 2, PAGE_TIMEOUT_MS = 5000, POLL_MS = 40 };
static const char *const TAG = "ui_selftest";
static const uint32_t external_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

typedef enum {
    CLOCK, WEATHER, CALENDAR, NOTES, MUSIC, READER, VIDEO, FILES, GAMES, NES,
    RECORDER, SETTINGS, MOTION, PRESSURE, DISPLACEMENT,
    REMOTE, OBS, INTERCOM, GPS, CAMERA, GALLERY, CALCULATOR, THEME,
} route_id_t;

typedef struct {
    const char *id;
    const char *title;
    const char *drawer_label;
} route_t;

static const route_t routes[] = {
    [CLOCK] = {"clock", "时钟与日期", "时钟"},
    [WEATHER] = {"weather", "天气", "天气"},
    [CALENDAR] = {"calendar", "日历", "日历"},
    [NOTES] = {"notes", "记事本", "记事本"},
    [MUSIC] = {"music", "音乐播放器", "音乐"},
    [READER] = {"reader", "小说阅读", "阅读"},
    [VIDEO] = {"video-picker", "选择视频", "视频"},
    [FILES] = {"files", "文件浏览", "文件"},
    [GAMES] = {"games", "小游戏", "小游戏"},
    [NES] = {"nes-picker", "选择 NES 游戏", "NES 游戏"},
    [RECORDER] = {"recorder", "录音机", "录音"},
    [SETTINGS] = {"settings", "设置", "设置"},
    [MOTION] = {"motion", "姿态", "姿态"},
    [PRESSURE] = {"barometer", "气压与高度", "气压高度"},
    [DISPLACEMENT] = {"displacement", "短程位移", "位移测量"},
    [REMOTE] = {"pc-remote", "空中飞鼠", "空中飞鼠"},
    [OBS] = {"obs-switcher", "OBS 切换台", "OBS 切换"},
    [INTERCOM] = {"intercom", "无线对讲", "无线对讲"},
    [GPS] = {"gps", "卫星定位", "定位"},
    [CAMERA] = {"usb-camera", "USB 摄像头", "USB 摄像头"},
    [GALLERY] = {"gallery", "图库", "图库"},
    [CALCULATOR] = {"calculator", "计算器", "计算器"},
    [THEME] = {"theme", "主题", "主题"},
};

typedef struct {
    route_id_t parent;
    const char *button;
    route_t page;
} nested_route_t;

static const nested_route_t nested_routes[] = {
    {CLOCK, "闹钟", {"alarm", "闹钟", NULL}},
    {CLOCK, "秒表", {"stopwatch", "秒表", NULL}},
    {CLOCK, "倒计时", {"countdown", "倒计时", NULL}},
    {MOTION, "水平仪", {"spirit-level", "水平仪", NULL}},
    {REMOTE, "配置", {"air-mouse-settings", "飞鼠配置", NULL}},
    {THEME, "从 TF 卡选择", {"wallpaper-picker", "选择壁纸", NULL}},
    {SETTINGS, "Wi-Fi", {"wifi", "Wi-Fi 网络", NULL}},
    {SETTINGS, "蓝牙", {"bluetooth", "蓝牙", NULL}},
    {SETTINGS, "NES 按键映射", {"nes-keymap", "NES 键盘映射", NULL}},
    {SETTINGS, "传输详情", {"usb-transfer", "USB 文件传输", NULL}},
    {SETTINGS, "设备状态", {"status", "设备状态", NULL}},
    {GAMES, "2048", {"game-2048", "2048", NULL}},
    {GAMES, "推箱子", {"game-crates", "推箱子", NULL}},
    {GAMES, "贪吃蛇", {"game-snake", "贪吃蛇", NULL}},
    {GAMES, "跳一跳", {"game-jump", "跳一跳", NULL}},
    {GAMES, "摇骰子", {"game-dice", "摇骰子", NULL}},
    {GAMES, "落块挑战", {"game-fall", "落块挑战", NULL}},
};

typedef struct {
    app_ui_t *ui;
    uint32_t generation;
    size_t route_count;
    int64_t predecessor_key_us;
    int32_t drawer_scroll_y;
    unsigned routes_passed, nested_passed, media_passed, media_skipped;
    unsigned failed_actions, drawer_back_passed;
    bool drawer_home_clicked;
} probe_t;

/* The AC101S build links an intercom worker. The generic build has none. */
extern bool app_intercom_busy(void) __attribute__((weak));

static void pause_ms(unsigned ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool wait_screen(app_power_t *power, bool asleep)
{
    const int64_t limit = esp_timer_get_time() + INT64_C(3000000);
    app_power_status_t state;
    do {
        pause_ms(20);
        if (app_power_get_status(power, &state) != ESP_OK) return false;
        if (state.sleeping == asleep && app_power_is_sleeping(power) == asleep &&
            state.display_error == ESP_OK) {
            pause_ms(20);
            return true;
        }
    } while (esp_timer_get_time() < limit);
    return false;
}

static bool screen_transition(app_power_t *power, bool asleep)
{
    const esp_err_t error = asleep ? app_power_sleep_now(power) : app_power_wake(power);
    return error == ESP_OK && wait_screen(power, asleep);
}

static bool test_idle_suspend(probe_t *probe)
{
    app_power_t *power = probe->ui->power;
    const uint32_t timeout = app_power_get_timeout(power);
    bool ok = app_power_set_timeout(power, 1, false) == ESP_OK;
    for (unsigned pass = 0; pass < 3 && ok; ++pass) {
        if (!lvgl_port_lock(1000)) { ok = false; break; }
        lv_display_trigger_activity(lv_display_get_default());
        lvgl_port_unlock();
        const bool slept = wait_screen(power, true);
        const uint32_t tick = lv_tick_get();
        pause_ms(300);
        const uint32_t elapsed = lv_tick_get() - tick;
        if (!pass && slept) (void)esp_pm_dump_locks(stdout);
        const bool woke = screen_transition(power, false);
        ok = slept && woke && elapsed <= 2;
        ESP_LOGI(TAG, "CHECK standby-idle pass=%u asleep=%u wake=%u paused_ui_ms=%" PRIu32 " result=%s",
                 pass + 1, slept, woke, elapsed, ok ? "PASS" : "FAIL");
    }
    return app_power_set_timeout(power, timeout, false) == ESP_OK && ok;
}

typedef struct {
    uint32_t free_bytes;
    uint32_t allocated_blocks;
} heap_sample_t;

static heap_sample_t psram_sample(void)
{
    multi_heap_info_t info = {0};
    heap_caps_get_info(&info, external_caps);
    return (heap_sample_t){.free_bytes = (uint32_t)info.total_free_bytes,
                          .allocated_blocks = (uint32_t)info.allocated_blocks};
}

static void report_memory(unsigned round, const char *scope, const char *id, heap_sample_t before)
{
    /* Keep per-route samples after the cleanup gate. Also allow asynchronous
     * storage cancellations to settle before the round and endurance samples. */
    if (ROUNDS <= 8 || !strcmp(scope, "round") || round <= 2 ||
        round == ROUNDS / 2 || round == ROUNDS) pause_ms(100);
    const heap_sample_t after = psram_sample();
    ESP_LOGI(TAG, "HEAP round=%u scope=%s id=%s free_before=%" PRIu32 " free_after=%" PRIu32
             " delta=%" PRId32 " blocks_before=%" PRIu32 " blocks_after=%" PRIu32,
             round, scope, id, before.free_bytes, after.free_bytes,
             (int32_t)after.free_bytes - (int32_t)before.free_bytes,
             before.allocated_blocks, after.allocated_blocks);
}

static lv_obj_t *find_label(lv_obj_t *object, const char *text, bool prefix)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *label = lv_label_get_text(object);
        if (prefix ? strncmp(label, text, strlen(text)) == 0 : strcmp(label, text) == 0)
            return object;
    }
    const uint32_t children = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(object, (int32_t)i), text, prefix);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_class(lv_obj_t *object, const lv_obj_class_t *class)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(object, class)) return object;
    const uint32_t children = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t *found = find_class(lv_obj_get_child(object, (int32_t)i), class);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *button_for(lv_obj_t *scope, const char *text)
{
    lv_obj_t *object = find_label(scope, text, false);
    while (object && object != scope) {
        if (lv_obj_check_type(object, &lv_button_class)) return object;
        object = lv_obj_get_parent(object);
    }
    return NULL;
}

static lv_obj_t *drawer_home_button(lv_obj_t *screen)
{
    lv_obj_t *button = button_for(screen, LV_SYMBOL_HOME);
    return button && lv_obj_get_parent(button) == screen &&
        lv_obj_has_flag(button, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_state(button, LV_STATE_DISABLED) ?
        button : NULL;
}

static bool current_page(probe_t *probe)
{
    app_ui_t *ui = probe->ui;
    return ui->page_generation == probe->generation && ui->screen == lv_screen_active() &&
        !app_ui_input_blocked(ui) && !app_power_is_sleeping(ui->power);
}

static uint64_t completed_regions(void)
{
    app_display_metrics_t display;
    app_display_metrics_get(&display);
    return display.completed_sequence;
}

static bool wait_page(probe_t *probe, const route_t *page, uint64_t before, bool await_io)
{
    const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
    do {
        pause_ms(POLL_MS);
        if (!lvgl_port_lock(100)) continue;
        const bool same = current_page(probe);
        app_ui_t *ui = probe->ui;
        const bool ready = same && ui->content && lv_obj_get_child_count(ui->content) &&
            ui->user_key_exit_armed && !drawer_home_button(ui->screen) &&
            find_label(ui->screen, page->title, false) &&
            !find_label(ui->content, "内存不足", true) &&
            !find_label(ui->content, "阅读器内存分配失败", false) &&
            (!await_io || !ui->io_complete);
        lvgl_port_unlock();
        if (ready && completed_regions() > before) return true;
        if (!same) return false;
    } while (esp_timer_get_time() < deadline);
    return false;
}

static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *data)
{
    const lv_indev_data_t *sample = lv_indev_get_user_data(pointer);
    data->point = sample->point;
    data->state = sample->state;
}

static lv_indev_t *create_pointer(lv_display_t *display, lv_indev_data_t *sample)
{
    lv_indev_t *pointer = lv_indev_create();
    if (!pointer) return NULL;
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    lv_indev_set_display(pointer, display);
    lv_indev_set_user_data(pointer, sample);
    lv_indev_set_read_cb(pointer, read_pointer);
    return pointer;
}

static void delete_pointer(lv_indev_t *pointer)
{
    /* LVGL 9.5 scroll throw animations retain the input device as their target. */
    lv_indev_reset(pointer, NULL);
    lv_anim_delete(pointer, NULL);
    lv_indev_delete(pointer);
}

/* Call with the LVGL lock held. Keep this synthetic pointer separate from the
 * physical touch controller, and finish the press before dispatching a tile. */
static bool desktop_drag(probe_t *probe, int32_t dx, int32_t dy, unsigned steps,
                          bool opens_drawer)
{
    app_ui_t *ui = probe->ui;
    lv_display_t *display = lv_obj_get_display(ui->screen);
    lv_indev_data_t sample = {
        .point = {lv_display_get_horizontal_resolution(display) / 2,
                  lv_display_get_vertical_resolution(display) * 4 / 5},
        .state = LV_INDEV_STATE_PRESSED,
    };
    lv_indev_t *pointer = create_pointer(display, &sample);
    if (!pointer) return false;

    lv_indev_read(pointer);
    for (unsigned i = 0; i < steps && current_page(probe); ++i) {
        sample.point.x += dx;
        sample.point.y += dy;
        lv_indev_read(pointer);
    }
    const uint32_t before_release = ui->page_generation;
    sample.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    delete_pointer(pointer);

    const uint32_t expected = probe->generation + (opens_drawer ? 1U : 0U);
    return before_release == expected && ui->page_generation == expected;
}

static bool open_drawer_tile(probe_t *probe, route_id_t id, bool await_io)
{
    if (!lvgl_port_lock(1000)) return false;
    app_ui_t *ui = probe->ui;
    app_display_metrics_t drawer_before, drawn;
    app_display_metrics_get(&drawer_before);
    bool ok = current_page(probe) && ui->home_time && !ui->page_context;
    if (ok) {
        probe->predecessor_key_us = esp_timer_get_time();
        ok = desktop_drag(probe, 0, -24, 3, true);
        ++probe->generation;
        ok = ok && current_page(probe) && !ui->home_time && ui->content &&
            drawer_home_button(ui->screen) &&
            lv_obj_get_child_count(ui->content) == probe->route_count;
    }
    if (ok && !probe->routes_passed && !probe->nested_passed)
        ESP_LOGI(TAG, "CHECK desktop-gesture upward=opened release=unchanged");
    if (ok) {
        lv_obj_t *tile = button_for(ui->content, routes[id].drawer_label);
        ok = tile != NULL;
        if (ok) lv_obj_scroll_to_view_recursive(tile, LV_ANIM_OFF);
    }
    lvgl_port_unlock();
    if (!ok) return false;

    const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
    do {
        pause_ms(POLL_MS);
        app_display_metrics_get(&drawn);
    } while ((drawn.completed_sequence <= drawer_before.submitted_sequence ||
              drawn.completed_sequence != drawn.submitted_sequence) &&
             esp_timer_get_time() < deadline);
    if (drawn.completed_sequence <= drawer_before.submitted_sequence ||
        drawn.completed_sequence != drawn.submitted_sequence) return false;
    const uint64_t before = completed_regions();
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *tile = current_page(probe) ?
        button_for(ui->content, routes[id].drawer_label) : NULL;
    ok = tile != NULL;
    if (ok) {
        probe->drawer_scroll_y = lv_obj_get_scroll_y(ui->content);
        lv_obj_send_event(tile, LV_EVENT_CLICKED, NULL);
        ++probe->generation;
    }
    lvgl_port_unlock();
    return ok && wait_page(probe, &routes[id], before, await_io);
}

static bool click_label(probe_t *probe, const char *caption)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *button = current_page(probe) ? button_for(probe->ui->content, caption) : NULL;
    const bool ok = button && !lv_obj_has_state(button, LV_STATE_DISABLED);
    if (ok) {
        lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
        lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    }
    lvgl_port_unlock();
    return ok;
}

static bool click_subpage_back(probe_t *probe, const char *duplicate)
{
    if (!lvgl_port_lock(1000)) return false;
    app_ui_t *ui = probe->ui;
    lv_obj_t *back = current_page(probe) ? button_for(ui->screen, LV_SYMBOL_LEFT) : NULL;
    void (*previous)(app_ui_t *) = ui->page_back;
    void *context = ui->page_context;
    const bool has_header = back && lv_obj_get_parent(back) == ui->screen && previous &&
        !lv_obj_has_state(back, LV_STATE_DISABLED) && !button_for(ui->content, duplicate);
    if (has_header) lv_obj_send_event(back, LV_EVENT_CLICKED, NULL);
    const bool returned = has_header && current_page(probe) && ui->page_context == context &&
        ui->page_back != previous;
    lvgl_port_unlock();
    return returned;
}

static bool idle_media(app_ui_t *ui)
{
    bool releasing = true;
    esp_err_t error = ESP_OK;
    if (app_io_media_release_status(ui->io, &releasing, &error) != ESP_OK ||
        releasing || error != ESP_OK) return false;
    if (app_intercom_busy && app_intercom_busy()) return false;
    if (ui->audio_player) {
        aether_audio_player_status_t audio;
        if (aether_audio_player_get_status(ui->audio_player, &audio) != ESP_OK ||
            audio.active || audio.closing || audio.track_count) return false;
    }
    if (ui->video_player) {
        aether_video_status_t video;
        if (aether_video_player_get_status(ui->video_player, &video) != ESP_OK ||
            video.state == AETHER_VIDEO_PLAYING || video.state == AETHER_VIDEO_STARTING ||
            video.state == AETHER_VIDEO_PAUSED) return false;
    }
    aether_recorder_handle_t recorder = aether_recorder_get_default();
    if (recorder) {
        aether_recorder_status_t status;
        if (aether_recorder_get_status(recorder, &status) != ESP_OK || status.active) return false;
    }
    return true;
}

static bool back_to_drawer(probe_t *probe, const route_t *route, bool require_scrolled)
{
    if (!lvgl_port_lock(1000)) return false;
    app_ui_t *ui = probe->ui;
    lv_obj_t *back = current_page(probe) ? button_for(ui->screen, LV_SYMBOL_LEFT) : NULL;
    const bool has_header = back && lv_obj_get_parent(back) == ui->screen &&
        !lv_obj_has_state(back, LV_STATE_DISABLED);
    const uint64_t before = completed_regions();
    if (has_header) {
        lv_obj_send_event(back, LV_EVENT_CLICKED, NULL);
        ++probe->generation;
    }
    lvgl_port_unlock();
    if (!has_header || (require_scrolled && !probe->drawer_scroll_y)) {
        ESP_LOGE(TAG, "FAIL route=%s drawer-back header=%u source_scroll=%" PRId32,
                 route->id, has_header, probe->drawer_scroll_y);
        return false;
    }

    int32_t restored_y = 0;
    const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
    do {
        pause_ms(POLL_MS);
        if (!lvgl_port_lock(100)) continue;
        const bool drawer = current_page(probe) && !ui->home_time && !ui->page_context &&
            !ui->page_cleanup && !ui->page_can_exit && !ui->io_complete && !ui->media_active &&
            !ui->touch_restore_pending && ui->user_key_exit_armed && ui->content &&
            lv_obj_get_child_count(ui->content) == probe->route_count &&
            button_for(ui->content, route->drawer_label) && drawer_home_button(ui->screen);
        if (drawer) {
            lv_obj_update_layout(ui->content);
            restored_y = lv_obj_get_scroll_y(ui->content);
        }
        lvgl_port_unlock();
        if (drawer && restored_y == probe->drawer_scroll_y && completed_regions() > before && idle_media(ui)) {
            ++probe->drawer_back_passed;
            ESP_LOGI(TAG, "CHECK route=%s back=drawer scroll_before=%" PRId32 " scroll_after=%" PRId32,
                     route->id, probe->drawer_scroll_y, restored_y);
            return true;
        }
    } while (esp_timer_get_time() < deadline);
    ESP_LOGE(TAG, "FAIL route=%s drawer-back scroll_before=%" PRId32 " scroll_after=%" PRId32,
             route->id, probe->drawer_scroll_y, restored_y);
    return false;
}

static bool wait_desktop(probe_t *probe)
{
    app_ui_t *ui = probe->ui;
    const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
    do {
        pause_ms(POLL_MS);
        if (!lvgl_port_lock(100)) continue;
        const bool desktop = current_page(probe) && ui->home_time &&
            !drawer_home_button(ui->screen) && !ui->page_context && !ui->page_cleanup &&
            !ui->page_can_exit && !ui->io_complete && !ui->media_active &&
            !ui->touch_restore_pending && !ui->user_key_exit_pending && !ui->user_key_exit_armed;
        lvgl_port_unlock();
        if (desktop && idle_media(ui)) return true;
    } while (esp_timer_get_time() < deadline);
    return false;
}

static bool click_drawer_home(probe_t *probe)
{
    if (!lvgl_port_lock(1000)) return false;
    app_ui_t *ui = probe->ui;
    lv_obj_t *button = current_page(probe) && !ui->home_time ? drawer_home_button(ui->screen) : NULL;
    bool opened = false;
    if (button) {
        lv_obj_update_layout(ui->screen);
        lv_area_t area;
        lv_obj_get_coords(button, &area);
        if (area.x1 >= 0 && area.y1 >= 0 && area.x2 < 100 && area.y2 < 70) {
            lv_indev_data_t sample = {
                .point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2},
                .state = LV_INDEV_STATE_PRESSED,
            };
            lv_indev_t *pointer = create_pointer(lv_obj_get_display(button), &sample);
            if (pointer) {
                lv_indev_read(pointer);
                const bool held_drawer = current_page(probe);
                sample.state = LV_INDEV_STATE_RELEASED;
                lv_indev_read(pointer);
                delete_pointer(pointer);
                opened = held_drawer && ui->page_generation == probe->generation + 1U;
            }
        }
    }
    lvgl_port_unlock();
    if (!opened) return false;
    ++probe->generation;
    return wait_desktop(probe);
}

static bool exit_to_desktop(probe_t *probe, bool check_stale)
{
    app_ui_t *ui = probe->ui;
    if (!lvgl_port_lock(1000)) return false;
    const bool armed = current_page(probe) && ui->user_key_exit_armed;
    if (armed && check_stale) app_ui_queue_user_key(ui, probe->predecessor_key_us);
    lvgl_port_unlock();
    if (!armed) return false;
    if (check_stale) {
        pause_ms(150);
        if (!lvgl_port_lock(1000)) return false;
        const bool unchanged = current_page(probe) && ui->user_key_exit_armed &&
            !ui->user_key_exit_pending;
        lvgl_port_unlock();
        if (!unchanged) return false;
    }
    app_ui_queue_user_key(ui, esp_timer_get_time());
    ++probe->generation;
    return wait_desktop(probe);
}

typedef struct {
    lv_point_t center[26];
    uint32_t seen;
} ime_keys_t;

static void collect_ime_keys(lv_event_t *event)
{
    lv_draw_task_t *task = lv_event_get_draw_task(event);
    if (lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_FILL) return;
    const lv_draw_dsc_base_t *draw = lv_draw_task_get_draw_dsc(task);
    if (draw->part != LV_PART_ITEMS) return;
    const char *key = lv_keyboard_get_button_text(lv_event_get_current_target_obj(event), draw->id1);
    if (!key || key[0] < 'a' || key[0] > 'z' || key[1]) return;
    ime_keys_t *keys = lv_event_get_user_data(event);
    const unsigned letter = (unsigned)(key[0] - 'a');
    lv_area_t area;
    lv_draw_task_get_area(task, &area);
    keys->center[letter] = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    keys->seen |= UINT32_C(1) << letter;
}

static void ime_sample(lv_indev_t *pointer, lv_indev_data_t *sample, lv_point_t point, bool pressed)
{
    sample->point = point;
    sample->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
}

static bool ime_pick_locked(lv_obj_t *dock, lv_indev_t *pointer, lv_indev_data_t *sample,
                            const char *word)
{
    lv_obj_t *button = button_for(dock, word);
    if (!button || lv_obj_has_flag(button, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
    lv_obj_update_layout(button);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    const lv_point_t point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    ime_sample(pointer, sample, point, true);
    ime_sample(pointer, sample, point, false);
    return true;
}

static void ime_trace_locked(lv_indev_t *pointer, lv_indev_data_t *sample,
                             const ime_keys_t *keys, const char *spelling, int32_t shift, bool brush_edge)
{
    lv_point_t from = keys->center[*spelling++ - 'a'];
    if (shift) { from.x += shift; from.y -= 6; }
    ime_sample(pointer, sample, from, true);
    if (brush_edge) {
        const lv_point_t nearby = {from.x, from.y + 18};
        ime_sample(pointer, sample, nearby, true);
        ime_sample(pointer, sample, (lv_point_t){from.x, from.y + 53}, true);
        ime_sample(pointer, sample, (lv_point_t){from.x + 5, from.y + 53}, true);
        ime_sample(pointer, sample, (lv_point_t){from.x + 10, from.y + 53}, true);
        ime_sample(pointer, sample, nearby, true);
        ime_sample(pointer, sample, from, true);
    }
    const int32_t steps = shift ? 3 : 8;
    for (; *spelling; ++spelling) {
        lv_point_t to = keys->center[*spelling - 'a'];
        if (shift) { to.x += shift < 0 ? shift + 1 : *spelling == 'i' ? 7 : 13; to.y -= 5; }
        for (int32_t i = 1; i <= steps; ++i) {
            const lv_point_t point = {from.x + (to.x - from.x) * i / steps,
                                      from.y + (to.y - from.y) * i / steps};
            ime_sample(pointer, sample, point, true);
        }
        from = to;
    }
    ime_sample(pointer, sample, from, false);
}

static void ime_glide_locked(lv_indev_t *pointer, lv_indev_data_t *sample,
                             const ime_keys_t *keys, const char *spelling, bool sparse)
{
    ime_trace_locked(pointer, sample, keys, spelling, sparse ? 12 : 0, false);
}

static bool test_ime_locked(lv_obj_t *screen)
{
    lv_obj_t *scope = lv_obj_create(screen);
    if (!scope) return false;
    lv_obj_set_size(scope, lv_obj_get_width(screen), lv_obj_get_height(screen));
    lv_obj_set_pos(scope, 0, 0);
    lv_obj_set_style_pad_all(scope, 0, 0);
    lv_obj_set_style_border_width(scope, 0, 0);
    lv_obj_remove_flag(scope, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *field = lv_textarea_create(scope);
    if (field) {
        lv_obj_set_pos(field, 24, 24);
        lv_obj_set_size(field, 400, 90);
        lv_obj_set_style_text_font(field, app_ui_chinese_font(), 0);
        lv_group_remove_obj(field);
    }
    lv_obj_t *dock = field ? app_text_input_create(scope, field, true, NULL, NULL) : NULL;
    if (dock) app_text_input_show(dock, true);
    lv_obj_t *keyboard = dock ? find_class(dock, &lv_keyboard_class) : NULL;
    bool typed = false, swiped = false, segmented_swipe = false, homophone_swipe = false;
    bool edge_tap = false, short_tap = false, sparse_glide = false, actions_guarded = false;
    bool sentence_glide = false, sentence_parts = false, requested_words = false, cold_glide = false;
    bool meal_glide = false, edge_glide = false, shifted_meal = false;
    unsigned meal_questions = 0;
    const bool cold_available = app_pinyin_extended_count() > 0;
    int64_t segmented_us = 0, sentence_us = 0;
    unsigned key_count = 0;
    bool pointer_created = false, first_letter = false, tap_composition = false;
    bool tap_candidate = false, swipe_composition = false, swipe_candidate = false;
    bool dynamic_keys = false;
    if (keyboard) {
        ime_keys_t keys = {0};
        lv_obj_add_event_cb(keyboard, collect_ime_keys, LV_EVENT_DRAW_TASK_ADDED, &keys);
        lv_obj_update_layout(scope);
        lv_obj_invalidate(keyboard);
        lv_display_t *display = lv_obj_get_display(keyboard);
        lv_refr_now(display);
        lv_obj_remove_event_cb(keyboard, collect_ime_keys);
        key_count = (unsigned)__builtin_popcount(keys.seen);
        lv_indev_data_t sample = {0};
        lv_indev_t *pointer = keys.seen == ((UINT32_C(1) << 26) - 1) ? create_pointer(display, &sample) : NULL;
        pointer_created = pointer != NULL;
        if (pointer) {
            lv_obj_t *actions = find_class(dock, &lv_buttonmatrix_class);
            if (actions && actions != keyboard) {
                lv_area_t area;
                lv_obj_get_coords(actions, &area);
                const lv_point_t enter = {area.x2 - 18, (area.y1 + area.y2) / 2};
                ime_sample(pointer, &sample, keys.center['m' - 'a'], true);
                ime_sample(pointer, &sample, enter, true);
                ime_sample(pointer, &sample, enter, false);
                const bool drag_safe = !lv_textarea_get_text(field)[0];
                app_text_input_show(dock, false);
                app_text_input_show(dock, true);
                lv_obj_update_layout(scope);
                ime_sample(pointer, &sample, enter, true);
                const bool pressed_safe = !lv_textarea_get_text(field)[0];
                ime_sample(pointer, &sample, enter, false);
                actions_guarded = drag_safe && pressed_safe && !strcmp(lv_textarea_get_text(field), "\n");
                lv_textarea_set_text(field, "");
            }
            lv_area_t keyboard_area;
            lv_obj_get_coords(keyboard, &keyboard_area);
            const lv_point_t edge = {keyboard_area.x1 - 4, keys.center['q' - 'a'].y};
            ime_sample(pointer, &sample, edge, true);
            ime_sample(pointer, &sample, edge, false);
            edge_tap = find_label(dock, "q", false) != NULL;
            app_text_input_show(dock, false);
            app_text_input_show(dock, true);
            lv_obj_update_layout(scope);
            const lv_point_t key_q = keys.center['q' - 'a'];
            const lv_point_t drift = {key_q.x + 17, key_q.y};
            ime_sample(pointer, &sample, key_q, true);
            ime_sample(pointer, &sample, drift, true);
            ime_sample(pointer, &sample, drift, false);
            short_tap = find_label(dock, "q", false) != NULL;
            app_text_input_show(dock, false);
            app_text_input_show(dock, true);
            lv_obj_update_layout(scope);
            const lv_point_t c = keys.center['c' - 'a'], g = keys.center['g' - 'a'];
            const lv_point_t h = keys.center['h' - 'a'];
            const lv_point_t gh_edge = {(g.x + h.x) / 2 - 2, g.y};
            ime_sample(pointer, &sample, c, true);
            ime_sample(pointer, &sample, c, false);
            ime_sample(pointer, &sample, gh_edge, true);
            ime_sample(pointer, &sample, gh_edge, false);
            const bool corrected_edge = find_label(dock, "ch", false) != NULL;
            app_text_input_show(dock, false);
            app_text_input_show(dock, true);
            lv_obj_update_layout(scope);
            ime_sample(pointer, &sample, c, true);
            ime_sample(pointer, &sample, c, false);
            ime_sample(pointer, &sample, g, true);
            ime_sample(pointer, &sample, g, false);
            dynamic_keys = corrected_edge && find_label(dock, "cg", false) != NULL;
            app_text_input_show(dock, false);
            app_text_input_show(dock, true);
            lv_obj_update_layout(scope);
            for (const char *key = "ni"; *key; ++key) {
                const lv_point_t point = keys.center[*key - 'a'];
                ime_sample(pointer, &sample, point, true);
                ime_sample(pointer, &sample, point, false);
                if (*key == 'n') first_letter = find_label(dock, "n", false) != NULL;
            }
            tap_composition = find_label(dock, "ni", false) != NULL;
            tap_candidate = ime_pick_locked(dock, pointer, &sample, "你");
            typed = first_letter && tap_composition && tap_candidate && !strcmp(lv_textarea_get_text(field), "你");
            ime_glide_locked(pointer, &sample, &keys, "nihao", false);
            swipe_composition = find_label(dock, "nihao", false) != NULL;
            swipe_candidate = ime_pick_locked(dock, pointer, &sample, "你好");
            swiped = swipe_composition && swipe_candidate && !strcmp(lv_textarea_get_text(field), "你你好");
            lv_textarea_set_text(field, "");
            const int64_t started = esp_timer_get_time();
            ime_glide_locked(pointer, &sample, &keys, "woxianghecha", false);
            segmented_us = esp_timer_get_time() - started;
            segmented_swipe = find_label(dock, "woxianghecha", false) &&
                ime_pick_locked(dock, pointer, &sample, "我想喝茶") &&
                !strcmp(lv_textarea_get_text(field), "我想喝茶");
            lv_textarea_set_text(field, "");
            ime_glide_locked(pointer, &sample, &keys, "xian", false);
            homophone_swipe = find_label(dock, "xian", false) &&
                ime_pick_locked(dock, pointer, &sample, "西安") &&
                !strcmp(lv_textarea_get_text(field), "西安");
            lv_textarea_set_text(field, "");
            ime_glide_locked(pointer, &sample, &keys, "mudidi", true);
            sparse_glide = ime_pick_locked(dock, pointer, &sample, "目的地") &&
                !strcmp(lv_textarea_get_text(field), "目的地");
            lv_textarea_set_text(field, "");
            ime_glide_locked(pointer, &sample, &keys, "nishi", false);
            const bool nishi = ime_pick_locked(dock, pointer, &sample, "你是");
            ime_glide_locked(pointer, &sample, &keys, "nishishui", false);
            requested_words = nishi && ime_pick_locked(dock, pointer, &sample, "你是谁") &&
                !strcmp(lv_textarea_get_text(field), "你是你是谁");
            lv_textarea_set_text(field, "");
            ime_glide_locked(pointer, &sample, &keys, "nichifanlema", false);
            meal_glide = ime_pick_locked(dock, pointer, &sample, "你吃饭了吗") &&
                !strcmp(lv_textarea_get_text(field), "你吃饭了吗");
            lv_textarea_set_text(field, "");
            ime_trace_locked(pointer, &sample, &keys, "nichifanlema", 0, true);
            edge_glide = ime_pick_locked(dock, pointer, &sample, "你吃饭了吗") &&
                !strcmp(lv_textarea_get_text(field), "你吃饭了吗");
            lv_textarea_set_text(field, "");
            ime_trace_locked(pointer, &sample, &keys, "nichifanlema", -16, false);
            shifted_meal = ime_pick_locked(dock, pointer, &sample, "你吃饭了吗") &&
                !strcmp(lv_textarea_get_text(field), "你吃饭了吗");
            static const struct { const char *spelling, *text; int32_t shift; } meal_cases[] = {
                {"nichilema", "你吃了吗", 0},
                {"nichiwufanlema", "你吃午饭了吗", -12},
                {"diyici", "第一次", 0},
            };
            for (size_t i = 0; i < sizeof(meal_cases) / sizeof(meal_cases[0]); ++i) {
                lv_textarea_set_text(field, "");
                ime_trace_locked(pointer, &sample, &keys, meal_cases[i].spelling, meal_cases[i].shift, false);
                if (ime_pick_locked(dock, pointer, &sample, meal_cases[i].text) &&
                    !strcmp(lv_textarea_get_text(field), meal_cases[i].text)) meal_questions |= 1U << i;
            }
            lv_textarea_set_text(field, "");
            if (cold_available) {
                ime_glide_locked(pointer, &sample, &keys, "shoujike", false);
                cold_glide = ime_pick_locked(dock, pointer, &sample, "手机壳") &&
                    !strcmp(lv_textarea_get_text(field), "手机壳");
                lv_textarea_set_text(field, "");
            }
            const int64_t sentence_start = esp_timer_get_time();
            ime_glide_locked(pointer, &sample, &keys, "woxianghechayexiangtingyinyue", false);
            sentence_us = esp_timer_get_time() - sentence_start;
            sentence_glide = ime_pick_locked(dock, pointer, &sample, "我想喝茶也想听音乐") &&
                !strcmp(lv_textarea_get_text(field), "我想喝茶也想听音乐");
            lv_textarea_set_text(field, "");
            const char *const strokes[] = {"mingtian", "woxiang", "genni", "yiqi", "qu", "gongyuan"};
            for (size_t i = 0; i < sizeof(strokes) / sizeof(strokes[0]); ++i)
                ime_glide_locked(pointer, &sample, &keys, strokes[i], false);
            sentence_parts = ime_pick_locked(dock, pointer, &sample, "明天我想跟你一起去公园") &&
                !strcmp(lv_textarea_get_text(field), "明天我想跟你一起去公园");
            delete_pointer(pointer);
        }
    }
    lv_obj_delete(scope);
    ESP_LOGI(TAG, "CHECK notes-ime keys=%u pointer=%u first_letter=%u tap_composition=%u tap_candidate=%u"
             " swipe_composition=%u swipe_candidate=%u synthetic_key_taps=%u synthetic_swipe=%u"
             " segmented_swipe=%u segmented_us=%" PRIi64 " homophone_swipe=%u edge_tap=%u short_tap=%u dynamic_keys=%u sparse_glide=%u"
             " actions_guarded=%u requested_words=%u meal_glide=%u edge_glide=%u shifted_meal=%u meal_questions=0x%x"
             " cold_available=%u cold_glide=%u"
             " sentence_glide=%u sentence_us=%" PRIi64 " sentence_parts=%u disposable_text=1",
             key_count, pointer_created, first_letter, tap_composition, tap_candidate,
             swipe_composition, swipe_candidate, typed, swiped, segmented_swipe, segmented_us,
             homophone_swipe, edge_tap, short_tap, dynamic_keys, sparse_glide, actions_guarded, requested_words, meal_glide, edge_glide, shifted_meal,
             meal_questions, cold_available, cold_glide, sentence_glide, sentence_us, sentence_parts);
    return typed && swiped && segmented_swipe && homophone_swipe && edge_tap && short_tap && dynamic_keys &&
           sparse_glide && actions_guarded && requested_words && meal_glide && edge_glide && shifted_meal &&
           meal_questions == 7U && (!cold_available || cold_glide) &&
           sentence_glide && sentence_parts;
}

static bool test_notes_keyboard(probe_t *probe)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *field = current_page(probe) ?
        find_class(probe->ui->content, &lv_textarea_class) : NULL;
    const bool can_edit = field && !lv_obj_has_state(field, LV_STATE_DISABLED);
    if (can_edit) lv_obj_send_event(field, LV_EVENT_CLICKED, NULL);
    lv_obj_t *keyboard = can_edit ? find_class(probe->ui->screen, &lv_keyboard_class) : NULL;
    bool input_ok = false;
    if (keyboard && lv_obj_is_visible(keyboard)) {
        lv_obj_update_layout(field);
        lv_area_t field_area, dock_area;
        lv_obj_get_coords(field, &field_area);
        lv_obj_get_coords(lv_obj_get_parent(keyboard), &dock_area);
        const int32_t field_height = lv_obj_get_content_height(field);
        const int32_t width = lv_obj_get_width(keyboard), height = lv_obj_get_height(keyboard);
        const bool fits = field_area.y2 < dock_area.y1 &&
            field_height >= 2 * lv_font_get_line_height(app_ui_chinese_font_size(24)) +
                lv_obj_get_style_text_line_space(field, LV_PART_MAIN);
        ESP_LOGI(TAG, "CHECK notes-layout field_above_keyboard=%u field_height=%" PRId32
                 " keyboard=%" PRId32 "x%" PRId32, fits, field_height, width, height);
        input_ok = test_ime_locked(probe->ui->screen) && fits;
    } else if (can_edit) {
        aether_usb_host_status_t host;
        input_ok = app_otg_get_status(&host) == ESP_OK && host.active && host.keyboard_interfaces;
        if (input_ok) ESP_LOGI(TAG, "SKIP notes-ime on-screen keyboard hidden during external input");
    }
    lvgl_port_unlock();
    return input_ok;
}

static bool test_settings_top(probe_t *probe, const char *entry)
{
    /* Let the old screen retire and any focus-triggered animation finish. */
    pause_ms(800);
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *content = current_page(probe) ? probe->ui->content : NULL;
    const int32_t scroll = content ? lv_obj_get_scroll_y(content) : -1;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "CHECK settings-top entry=%s scroll=%" PRId32, entry, scroll);
    return scroll == 0;
}

static lv_obj_t *find_scrolling_list(lv_obj_t *obj)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_bottom(obj) > 0) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *list = find_scrolling_list(lv_obj_get_child(obj, i));
        if (list) return list;
    }
    return NULL;
}

typedef struct {
    int64_t began;
    uint32_t position_ms, tx_overflows;
    ac101s_audio_write_metrics_t writes;
} playback_probe_t;

static playback_probe_t begin_playback_probe(probe_t *probe)
{
    aether_audio_player_status_t status = {0};
    (void)aether_audio_player_get_status(probe->ui->audio_player, &status);
    playback_probe_t sample = {.position_ms = status.position_ms};
    ac101s_audio_handle_t codec = probe->ui->board->audio;
    (void)ac101s_audio_get_write_metrics(codec, &sample.writes);
#if AETHER_AUDIO_DIAG
    sample.tx_overflows = ac101s_audio_diag_tx_overflows(codec);
#endif
    app_display_metrics_set_probe_active(true);
    sample.began = esp_timer_get_time();
    return sample;
}

static bool finish_playback_probe(probe_t *probe, const aether_audio_player_status_t *before,
                                  const playback_probe_t *start, const char *phase,
                                  uint32_t min_fps, uint32_t max_refresh_gap_us, bool ok)
{
    const uint32_t wall_ms = (uint32_t)((esp_timer_get_time() - start->began) / 1000);
    aether_audio_player_status_t audio;
    const bool audio_ok = aether_audio_player_get_status(probe->ui->audio_player, &audio) == ESP_OK;
    const uint32_t advanced = audio_ok && audio.position_ms >= start->position_ms ?
        audio.position_ms - start->position_ms : 0;
    ac101s_audio_handle_t codec = probe->ui->board->audio;
    ac101s_audio_write_metrics_t writes = {0};
    const bool writes_ok = ac101s_audio_get_write_metrics(codec, &writes) == ESP_OK;
    uint32_t overflows = 0;
#if AETHER_AUDIO_DIAG
    overflows = ac101s_audio_diag_tx_overflows(codec) - start->tx_overflows;
#endif
    app_display_metrics_t display;
    app_display_metrics_get(&display);
    app_display_metrics_set_probe_active(false);
    ok = ok && audio_ok && writes_ok && !overflows &&
        audio.state == AETHER_AUDIO_PLAYING && !strcmp(before->path, audio.path) &&
        advanced >= wall_ms * 9 / 10 &&
        display.completed_refreshes * 1000 >= wall_ms * min_fps &&
        display.max_refresh_gap_us <= max_refresh_gap_us &&
        writes.write_calls > start->writes.write_calls &&
        writes.mutex_timeouts == start->writes.mutex_timeouts &&
        writes.output_errors == start->writes.output_errors &&
        audio.supply_gap_max_us < 25000;
    ESP_LOGI(TAG, "CHECK music-%s position_delta_ms=%" PRIu32 " wall_ms=%" PRIu32
             " lcd_refreshes=%" PRIu32 " lcd_max_gap_us=%" PRIu32 " dma_tx_overflows=%" PRIu32
             " mutex_wait_max_us=%" PRIu32 " supply_gap_max_us=%" PRIu32 " result=%s",
             phase, advanced, wall_ms, display.completed_refreshes, display.max_refresh_gap_us,
             overflows, writes.mutex_wait_max_us, audio_ok ? audio.supply_gap_max_us : 0,
             ok ? "PASS" : "FAIL");
    return ok;
}

static bool test_music_scroll(probe_t *probe, const aether_audio_player_status_t *before)
{
    if (before->duration_ms < before->position_ms + 5000) return true;
    if (!click_label(probe, "曲库") || !lvgl_port_lock(1000)) return false;
    lv_obj_update_layout(probe->ui->content);
    lv_obj_t *list = find_scrolling_list(probe->ui->content);
    if (!list) {
        lvgl_port_unlock();
        ESP_LOGI(TAG, "SKIP music-scroll no overflowing library");
        return click_subpage_back(probe, "返回");
    }
    lv_indev_data_t input = {0};
    lv_indev_t *pointer = create_pointer(lv_obj_get_display(list), &input);
    lv_area_t area;
    lv_obj_get_coords(list, &area);
    lvgl_port_unlock();
    if (!pointer) return false;

    const lv_point_t center = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    const int32_t span = lv_area_get_height(&area) / 2 - 12;
    const playback_probe_t start = begin_playback_probe(probe);
    int32_t traveled = 0;
    uint32_t peak_sample_speed = 0;
    bool ok = true;
    for (int swipe = 0; swipe < 10 && ok; ++swipe) {
        const int32_t delta = swipe % 2 ? -span : span;
        int64_t previous_us = 0;
        for (int step = 0; step <= 4; ++step) {
            if (!lvgl_port_lock(1000)) { ok = false; break; }
            if (!current_page(probe) || !lv_obj_is_valid(list)) {
                lvgl_port_unlock();
                ok = false;
                break;
            }
            const lv_point_t point = {center.x, center.y + delta - LV_MIN(step, 3) * delta * 2 / 3};
            const int32_t previous_y = lv_obj_get_scroll_y(list);
            const int64_t now = esp_timer_get_time();
            ime_sample(pointer, &input, point, step < 4);
            traveled += abs(lv_obj_get_scroll_y(list) - previous_y);
            if (step > 0 && step < 4 && now > previous_us) {
                const uint32_t speed = (uint32_t)(span * 2 * INT64_C(1000000) / 3 / (now - previous_us));
                peak_sample_speed = LV_MAX(peak_sample_speed, speed);
            }
            previous_us = now;
            lvgl_port_unlock();
            pause_ms(step == 4 ? 45 : 5);
        }
    }
    if (lvgl_port_lock(1000)) {
        delete_pointer(pointer);
        lvgl_port_unlock();
    } else ok = false;
    ESP_LOGI(TAG, "music fast input travel_px=%" PRId32 " peak_input_px_s=%" PRIu32,
             traveled, peak_sample_speed);
    /* The input sequence includes a deliberate 45 ms interval between flicks. */
    ok = finish_playback_probe(probe, before, &start, "scroll", 20, 75000,
        ok && traveled >= 500 && peak_sample_speed >= 1800);
    return click_subpage_back(probe, "返回") && ok;
}

static lv_obj_t *find_volume_slider(lv_obj_t *obj)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(obj, &lv_slider_class) && lv_slider_get_max_value(obj) == 100) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *slider = find_volume_slider(lv_obj_get_child(obj, i));
        if (slider) return slider;
    }
    return NULL;
}

static bool test_music_volume(probe_t *probe, const aether_audio_player_status_t *before)
{
    app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    const uint8_t original = audio.master_volume;
    if (!audio.ready || audio.applied_output != APP_AUDIO_OUTPUT_SPEAKER || !lvgl_port_lock(1000)) return false;
    lv_obj_t *slider = current_page(probe) ? find_volume_slider(probe->ui->content) : NULL;
    lv_indev_data_t input = {0};
    lv_indev_t *pointer = slider ? create_pointer(lv_obj_get_display(slider), &input) : NULL;
    lv_area_t area;
    if (slider) lv_obj_get_coords(slider, &area);
    lvgl_port_unlock();
    if (!pointer) return false;

    const playback_probe_t start = begin_playback_probe(probe);
    const int32_t center_y = (area.y1 + area.y2) / 2;
    const int32_t width = area.x2 - area.x1;
    unsigned changes = 0;
    uint8_t minimum = 100, maximum = 0, last = audio.applied_volume;
    bool ok = true;
    for (int step = 0; step <= 41; ++step) {
        if (!lvgl_port_lock(1000)) { ok = false; break; }
        if (!current_page(probe)) { lvgl_port_unlock(); ok = false; break; }
        const int offset = step % 20;
        const int fraction = offset <= 10 ? offset : 20 - offset;
        const lv_point_t point = {area.x1 + width * fraction / 10, center_y};
        ime_sample(pointer, &input, point, step < 41);
        lvgl_port_unlock();
        pause_ms(9);
        app_audio_system_get_snapshot(&audio);
        minimum = LV_MIN(minimum, audio.applied_volume);
        maximum = LV_MAX(maximum, audio.applied_volume);
        if (audio.applied_volume != last) ++changes;
        last = audio.applied_volume;
    }
    if (lvgl_port_lock(1000)) {
        delete_pointer(pointer);
        lvgl_port_unlock();
    } else ok = false;
    /* Measure the drag itself. The settled screen need not draw extra frames
     * after release while awaiting confirmation of the final volume. */
    ok = finish_playback_probe(probe, before, &start, "volume", 30, 45000,
        ok && changes >= 12 && minimum <= 2 && maximum >= 98);
    pause_ms(100);
    app_audio_system_get_snapshot(&audio);
    ESP_LOGI(TAG, "music volume input updates=%u applied_min=%u applied_max=%u target=%u applied=%u",
             changes, minimum, maximum, audio.master_volume, audio.applied_volume);
    const bool settled = !audio.pending && audio.master_volume == last && audio.applied_volume == last;
    return app_audio_system_set_volume(original, false) == ESP_OK && settled && ok;
}

static bool test_music(probe_t *probe)
{
    if (!click_label(probe, "曲库")) return false;
    pause_ms(180);
    if (!click_subpage_back(probe, "返回")) return false;
    aether_audio_player_status_t audio;
    if (!probe->ui->audio_player ||
        aether_audio_player_get_status(probe->ui->audio_player, &audio) != ESP_OK ||
        !audio.track_count) {
        ++probe->media_skipped;
        ESP_LOGI(TAG, "SKIP music-playback no indexed track or audio backend");
        return true;
    }
    if (!click_label(probe, LV_SYMBOL_PLAY)) return false;
    const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
    do {
        pause_ms(POLL_MS);
        if (aether_audio_player_get_status(probe->ui->audio_player, &audio) != ESP_OK ||
            audio.state == AETHER_AUDIO_ERROR) break;
        if (audio.state == AETHER_AUDIO_PLAYING && audio.position_ms >= 100 && audio.sample_rate) {
            ++probe->media_passed;
            ESP_LOGI(TAG, "PASS music-playback position=%" PRIu32 "ms source=%" PRIu32 "Hz",
                     audio.position_ms, audio.sample_rate);
            break;
        }
    } while (esp_timer_get_time() < deadline);
    if (audio.active && audio.state == AETHER_AUDIO_PLAYING && audio.position_ms >= 100 && audio.sample_rate) {
        if (audio.duration_ms < 3000) {
            ESP_LOGI(TAG, "SKIP music-seek track too short");
            return true;
        }
        /* A position advancing past the target requires PCM to reach the
         * output API after the decoder and playback route have repositioned. */
        const uint32_t targets[] = {audio.duration_ms / 8, audio.duration_ms / 16};
        for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
            const uint32_t target = targets[i];
            const int64_t start = esp_timer_get_time();
            if (aether_audio_player_seek(probe->ui->audio_player, target) != ESP_OK) return false;
            do {
                pause_ms(10);
                if (aether_audio_player_get_status(probe->ui->audio_player, &audio) != ESP_OK ||
                    audio.state == AETHER_AUDIO_ERROR) return false;
                if (!audio.seeking && audio.position_ms > target + 25 && audio.position_ms < target + 160) break;
            } while (esp_timer_get_time() - start < PAGE_TIMEOUT_MS * INT64_C(1000));
            const bool reached = !audio.seeking && audio.position_ms > target + 25 && audio.position_ms < target + 160;
            ESP_LOGI(TAG, "CHECK music-seek step=%u mp3=%u target_ms=%" PRIu32 " position_ms=%" PRIu32
                     " elapsed_us=%" PRIi64 " result=%s", (unsigned)i, audio.mp3, target, audio.position_ms,
                     esp_timer_get_time() - start, reached ? "PASS" : "FAIL");
            if (!reached) return false;
        }
        pause_ms(120);
        const bool scroll_ok = test_music_scroll(probe, &audio);
        const bool volume_ok = test_music_volume(probe, &audio);
        app_power_t *power = probe->ui->power;
        const bool slept = screen_transition(power, true);
        aether_audio_player_status_t parked = {0}, after = {0};
        (void)aether_audio_player_get_status(probe->ui->audio_player, &parked);
        pause_ms(500);
        (void)aether_audio_player_get_status(probe->ui->audio_player, &after);
        const bool woke = screen_transition(power, false);
        const bool continued = after.state == AETHER_AUDIO_PLAYING && !strcmp(parked.path, after.path) &&
            after.position_ms >= parked.position_ms + 350;
        ESP_LOGI(TAG, "CHECK standby-music asleep=%u awake=%u continued=%u result=%s",
                 slept, woke, continued, slept && woke && continued ? "PASS" : "FAIL");
        return scroll_ok && volume_ok && slept && woke && continued;
    }
    ESP_LOGE(TAG, "FAIL music-playback state=%d error=%s", audio.state,
             esp_err_to_name(audio.last_error));
    return false;
}

static lv_obj_t *recordings_viewport(lv_obj_t *content)
{
    lv_obj_t *icon = find_class(content, &lv_image_class);
    lv_obj_t *row = icon ? lv_obj_get_parent(icon) : NULL;
    return row && lv_obj_check_type(row, &lv_button_class) ? lv_obj_get_parent(row) : NULL;
}

static bool recordings_header_back(probe_t *probe, route_id_t destination)
{
    if (!lvgl_port_lock(1000)) return false;
    app_ui_t *ui = probe->ui;
    lv_obj_t *back = current_page(probe) ? button_for(ui->screen, LV_SYMBOL_LEFT) : NULL;
    const bool valid = back && lv_obj_get_parent(back) == ui->screen &&
        !lv_obj_has_state(back, LV_STATE_DISABLED);
    const uint64_t before = completed_regions();
    if (valid) lv_obj_send_event(back, LV_EVENT_CLICKED, NULL);
    const bool navigated = valid && ui->page_generation == probe->generation + 1U;
    if (navigated) ++probe->generation;
    lvgl_port_unlock();
    return navigated && wait_page(probe, &routes[destination], before, true);
}

static bool test_recordings_back(probe_t *probe)
{
    static const char directory[] = AETHER_STORAGE_MOUNT_POINT "/Recordings";
    app_ui_t *ui = probe->ui;
    if (!aether_media_fs_path_ready(ui->storage, directory) || app_io_usb_storage_blocked(ui->io)) {
        ++probe->media_skipped;
        ESP_LOGI(TAG, "SKIP recordings-back TF not ready");
        return true;
    }

    const uint64_t before_files = completed_regions();
    if (!click_label(probe, "录音文件")) return false;
    ++probe->generation;
    if (!wait_page(probe, &routes[FILES], before_files, true) || !lvgl_port_lock(1000)) return false;

    const bool listing = current_page(probe) && ui->page_back == app_open_recorder &&
        find_label(ui->content, directory, false);
    if (AETHER_UI_SELFTEST_SILENT) {
        lvgl_port_unlock();
        return listing && recordings_header_back(probe, RECORDER);
    }
    lv_obj_t *viewport = listing ? recordings_viewport(ui->content) : NULL;
    lv_obj_t *fixture = NULL;
    char expected[sizeof(((aether_audio_player_status_t *)0)->path)] = {0};
    for (uint32_t i = 0; viewport && i < lv_obj_get_child_count(viewport); ++i) {
        lv_obj_t *row = lv_obj_get_child(viewport, (int32_t)i);
        if (!lv_obj_check_type(row, &lv_button_class) || lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_obj_t *label = lv_obj_get_child(row, 1);
        lv_obj_t *metadata = lv_obj_get_child(row, 2);
        if (!label || !metadata || strcmp(lv_label_get_text(metadata), "文件夹") == 0) continue;
        const char *name = lv_label_get_text(label);
        const char *ext = strrchr(name, '.');
        if (!ext || strcasecmp(ext, ".wav") != 0) continue;
        const int size = snprintf(expected, sizeof(expected), "%s/%s", directory, name);
        if (size > 0 && (size_t)size < sizeof(expected)) {
            fixture = row;
            break;
        }
    }

    int32_t scroll_y = 0;
    const uint64_t before_music = completed_regions();
    if (fixture) {
        lv_obj_scroll_to_view_recursive(fixture, LV_ANIM_OFF);
        scroll_y = lv_obj_get_scroll_y(viewport);
        lv_obj_send_event(fixture, LV_EVENT_CLICKED, NULL);
    }
    const bool opened_music = fixture && ui->page_generation == probe->generation + 1U;
    if (opened_music) ++probe->generation;
    lvgl_port_unlock();
    if (!listing) return false;
    if (!fixture) {
        ++probe->media_skipped;
        ESP_LOGI(TAG, "SKIP recordings-back no WAV fixture in loaded rows");
        return recordings_header_back(probe, RECORDER);
    }
    if (!opened_music) return false;

    aether_audio_player_status_t audio = {0};
    bool played = false;
    if (wait_page(probe, &routes[MUSIC], before_music, true)) {
        const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
        do {
            if (!ui->audio_player || aether_audio_player_get_status(ui->audio_player, &audio) != ESP_OK ||
                audio.state == AETHER_AUDIO_ERROR) break;
            played = audio.position_ms > 0 && audio.sample_rate && strcmp(audio.path, expected) == 0;
            if (played) break;
            pause_ms(POLL_MS);
        } while (esp_timer_get_time() < deadline);
    }

    if (!recordings_header_back(probe, FILES) || !lvgl_port_lock(1000)) return false;
    const bool restored_directory = current_page(probe) && ui->page_back == app_open_recorder &&
        find_label(ui->content, directory, false);
    viewport = restored_directory ? recordings_viewport(ui->content) : NULL;
    const bool restored_scroll = viewport && lv_obj_get_scroll_y(viewport) == scroll_y;
    lvgl_port_unlock();
    const bool recorder = recordings_header_back(probe, RECORDER);
    if (!played || !restored_directory || !restored_scroll || !recorder) {
        ESP_LOGE(TAG, "FAIL recordings-back played=%u directory=%u scroll=%u recorder=%u error=%s",
                 played, restored_directory, restored_scroll, recorder, esp_err_to_name(audio.last_error));
        return false;
    }
    ++probe->media_passed;
    ESP_LOGI(TAG, "PASS recordings-back WAV_position=%" PRIu32 "ms scroll=%" PRId32 " recorder=1",
             audio.position_ms, scroll_y);
    return true;
}

static lv_obj_t *first_catalog_row(lv_obj_t *object)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(object, &lv_button_class) && lv_obj_get_width(object) >= 350) {
        lv_obj_t *label = lv_obj_get_child(object, 0);
        if (label && lv_obj_check_type(label, &lv_label_class) && lv_label_get_text(label)[0])
            return object;
    }
    const uint32_t children = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t *row = first_catalog_row(lv_obj_get_child(object, (int32_t)i));
        if (row) return row;
    }
    return NULL;
}

static bool test_gallery(probe_t *probe)
{
    if (!lvgl_port_lock(1000)) return false;
    const bool same = current_page(probe);
    lv_obj_t *row = same ? first_catalog_row(probe->ui->content) : NULL;
    const uint64_t before = completed_regions();
    if (row) lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();
    if (!same) return false;
    if (!row) {
        ++probe->media_skipped;
        ESP_LOGI(TAG, "SKIP gallery-decode no indexed image");
        return true;
    }

    const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
    do {
        pause_ms(POLL_MS);
        if (!lvgl_port_lock(100)) continue;
        const bool current = current_page(probe);
        lv_obj_t *content = probe->ui->content;
        lv_obj_t *image = current ? find_class(content, &lv_image_class) : NULL;
        const lv_image_dsc_t *decoded = image ? lv_image_get_src(image) : NULL;
        const unsigned width = decoded ? decoded->header.w : 0, height = decoded ? decoded->header.h : 0;
        const bool rejected = current && find_label(content, "无法打开图片", true);
        lvgl_port_unlock();
        if (!current) return false;
        if (width && height && completed_regions() > before) {
            if (!click_subpage_back(probe, "返回列表")) return false;
            ++probe->media_passed;
            ESP_LOGI(TAG, "PASS gallery-decode rendered_pixels=%ux%u list_return=1", width, height);
            return true;
        }
        if (rejected) {
            ++probe->media_skipped;
            ESP_LOGI(TAG, "SKIP gallery-decode first indexed image rejected by decoder");
            return click_subpage_back(probe, "返回列表");
        }
    } while (esp_timer_get_time() < deadline);
    ESP_LOGE(TAG, "FAIL gallery-decode image open did not finish");
    return false;
}

static bool test_video(probe_t *probe)
{
    if (!lvgl_port_lock(1000)) return false;
    const bool same = current_page(probe);
    lv_obj_t *row = same ? first_catalog_row(probe->ui->content) : NULL;
    if (row) {
        lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
        ++probe->generation;
    }
    lvgl_port_unlock();
    if (!same) return false;
    if (!row) {
        ++probe->media_skipped;
        ESP_LOGI(TAG, "SKIP video-playback no indexed clip");
        return true;
    }

    const uint64_t before = completed_regions();
    const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
    aether_video_status_t video = {0};
    do {
        pause_ms(POLL_MS);
        if (!probe->ui->video_player ||
            aether_video_player_get_status(probe->ui->video_player, &video) != ESP_OK ||
            video.state == AETHER_VIDEO_ERROR) break;
        if (video.frame_number >= 3 && completed_regions() > before) {
            ++probe->media_passed;
            ESP_LOGI(TAG, "PASS video-playback native frame=%" PRIu32 " dimensions=%ux%u",
                     video.frame_number, video.width, video.height);
            app_power_t *power = probe->ui->power;
            const bool slept = screen_transition(power, true);
            pause_ms(100);
            aether_video_status_t resting = {0}, still = {0};
            (void)aether_video_player_get_status(probe->ui->video_player, &resting);
            pause_ms(300);
            (void)aether_video_player_get_status(probe->ui->video_player, &still);
            const bool woke = screen_transition(power, false);
            aether_video_status_t resumed = {0};
            const int64_t resume_deadline = esp_timer_get_time() + INT64_C(1500000);
            do {
                pause_ms(20);
                (void)aether_video_player_get_status(probe->ui->video_player, &resumed);
            } while (resumed.frame_number <= still.frame_number && esp_timer_get_time() < resume_deadline);
            const bool ok = slept && woke && resting.state == AETHER_VIDEO_PAUSED &&
                still.frame_number == resting.frame_number && resumed.frame_number > still.frame_number;
            ESP_LOGI(TAG, "CHECK standby-video paused=%u fixed_frame=%u resumed=%u result=%s",
                     resting.state == AETHER_VIDEO_PAUSED, still.frame_number == resting.frame_number,
                     resumed.frame_number > still.frame_number, ok ? "PASS" : "FAIL");
            return ok;
        }
    } while (esp_timer_get_time() < deadline);
    ESP_LOGE(TAG, "FAIL video-playback state=%d error=%s", video.state,
             esp_err_to_name(video.last_error));
    return false;
}

static void report_imu_window(void)
{
    enum { VALUES = 4, OBSERVATIONS = 24 };
    int32_t low[VALUES], high[VALUES];
    int64_t total[VALUES] = {0};
    for (size_t i = 0; i < VALUES; ++i) {
        low[i] = INT32_MAX;
        high[i] = INT32_MIN;
    }
    unsigned count = 0, calibrated = 0, stationary = 0;
    uint32_t last_stamp = 0;
    for (unsigned i = 0; i < OBSERVATIONS; ++i) {
        pause_ms(25);
        app_telemetry_snapshot_t snapshot;
        app_telemetry_get(&snapshot);
        if (snapshot.imu_error != ESP_OK || snapshot.imu_updated_ms == last_stamp) continue;
        last_stamp = snapshot.imu_updated_ms;
        const qmi8658a_sample_t *imu = &snapshot.imu;
        const float *a = imu->accel_mps2;
        const float *g = imu->gyro_raw_dps;
        const int32_t value[VALUES] = {
            (int32_t)(1000 * sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])),
            (int32_t)(1000 * g[0]), (int32_t)(1000 * g[1]), (int32_t)(1000 * g[2]),
        };
        for (size_t axis = 0; axis < VALUES; ++axis) {
            if (value[axis] < low[axis]) low[axis] = value[axis];
            if (value[axis] > high[axis]) high[axis] = value[axis];
            total[axis] += value[axis];
        }
        ++count;
        calibrated += imu->gyro_bias_valid;
        stationary += imu->gyro_stationary;
    }
    if (!count) {
        ESP_LOGW(TAG, "SENSOR IMU window samples=0");
        return;
    }
    ESP_LOGI(TAG, "SENSOR IMU window samples=%u bias_valid=%u stationary=%u min/mean/max"
             " norm_mm_s2=%d/%d/%d gyro_x_mdps=%d/%d/%d gyro_y_mdps=%d/%d/%d gyro_z_mdps=%d/%d/%d",
             count, calibrated, stationary, (int)low[0], (int)(total[0] / count), (int)high[0],
             (int)low[1], (int)(total[1] / count), (int)high[1],
             (int)low[2], (int)(total[2] / count), (int)high[2],
             (int)low[3], (int)(total[3] / count), (int)high[3]);
}

static void report_sensors(route_id_t id)
{
    if (id != CLOCK && id != MOTION && id != PRESSURE) return;
    app_telemetry_snapshot_t sample;
    app_telemetry_get(&sample);
    const esp_err_t error = id == CLOCK ? sample.rtc_error :
        id == MOTION ? sample.imu_error : sample.barometer_error;
    const uint32_t updated = id == CLOCK ? sample.rtc_updated_ms :
        id == MOTION ? sample.imu_updated_ms : sample.barometer_updated_ms;
    ESP_LOGI(TAG, "CHECK %s live-sample=%s updated_ms=%" PRIu32, routes[id].id,
             esp_err_to_name(error), updated);
    if (error != ESP_OK) return;
    if (id == CLOCK) {
        const pcf85063_time_t *rtc = &sample.rtc;
        ESP_LOGI(TAG, "SENSOR RTC %04u-%02u-%02uT%02u:%02u:%02u oscillator_stopped=%u",
                 rtc->year, rtc->month, rtc->day, rtc->hour, rtc->minute, rtc->second,
                 rtc->oscillator_stopped);
    } else if (id == MOTION) {
        const qmi8658a_sample_t *imu = &sample.imu;
        ESP_LOGI(TAG, "SENSOR IMU acceleration_mm_s2=%d,%d,%d gyro_raw_mdps=%d,%d,%d "
                 "bias_valid=%u stationary=%u",
                 (int)(imu->accel_mps2[0] * 1000), (int)(imu->accel_mps2[1] * 1000),
                 (int)(imu->accel_mps2[2] * 1000), (int)(imu->gyro_raw_dps[0] * 1000),
                 (int)(imu->gyro_raw_dps[1] * 1000), (int)(imu->gyro_raw_dps[2] * 1000),
                 imu->gyro_bias_valid, imu->gyro_stationary);
        report_imu_window();
    } else {
        ESP_LOGI(TAG, "SENSOR DPS368 pressure_pa=%d temperature_centi_c=%d",
                 (int)sample.barometer.pressure_pa, (int)(sample.barometer.temperature_c * 100));
    }
}

static bool tap_game_button(probe_t *probe, const char *caption);
static bool page_has_label(probe_t *probe, const char *label);

static bool calculator_result_is(probe_t *probe, const char *value, const char *expression)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *readout = lv_obj_get_child(probe->ui->content, 0);
    lv_obj_t *formula = expression ? find_label(readout, expression, false) : NULL;
    lv_font_glyph_dsc_t glyph;
    const bool visible_multiply = !expression || (formula &&
        lv_font_get_glyph_dsc(lv_obj_get_style_text_font(formula, 0), &glyph, 0xD7, 0) &&
        !glyph.is_placeholder && glyph.box_w > 3);
    const bool correct = current_page(probe) && find_label(readout, value, false) && visible_multiply;
    lvgl_port_unlock();
    return correct;
}

static bool test_calculator(probe_t *probe)
{
    const bool ok = tap_game_button(probe, "7") && tap_game_button(probe, "×") &&
        tap_game_button(probe, "8") && tap_game_button(probe, "=") &&
        calculator_result_is(probe, "56", "7 × 8 =") &&
        tap_game_button(probe, "科学") && tap_game_button(probe, "C") &&
        tap_game_button(probe, "3") && tap_game_button(probe, "0") &&
        tap_game_button(probe, "sin") && calculator_result_is(probe, "0.5", NULL) &&
        tap_game_button(probe, "进制") && tap_game_button(probe, "1") &&
        tap_game_button(probe, "5") && tap_game_button(probe, "BIN") &&
        calculator_result_is(probe, "1111", NULL) && tap_game_button(probe, "HEX") &&
        calculator_result_is(probe, "F", NULL);
    ESP_LOGI(TAG, "CHECK calculator three modes and multiply glyph=%s", ok ? "PASS" : "FAIL");
    return ok;
}

static bool test_routes(probe_t *probe, unsigned round)
{
    for (unsigned i = 0; i < probe->route_count; i++) {
        const route_t *route = &routes[i];
        const heap_sample_t heap_before = psram_sample();
        const uint64_t before = completed_regions();
        const bool settle = round != ROUNDS || i == MUSIC;
        if (!open_drawer_tile(probe, (route_id_t)i, settle)) {
            ESP_LOGE(TAG, "FAIL round=%u route=%s open/render/catalog", round, route->id);
            return false;
        }
        if (round == 1) {
            pause_ms(160);
            report_sensors((route_id_t)i);
        }
        const bool stress_music = !AETHER_UI_SELFTEST_SILENT && i == MUSIC &&
            (round == 1 || round == ROUNDS / 2 || round == ROUNDS);
        const bool music_ok = !stress_music || test_music(probe);
        const bool actions_ok = music_ok && (i != CALCULATOR || test_calculator(probe)) && (round != 1 ||
            ((i != NOTES || test_notes_keyboard(probe)) &&
             (i != VIDEO || AETHER_UI_SELFTEST_SILENT || test_video(probe)) &&
             (i != RECORDER || test_recordings_back(probe)) &&
             (i != SETTINGS || test_settings_top(probe, "drawer")) &&
             (i != GALLERY || test_gallery(probe))));
        const bool back_ok = round != DRAWER_BACK_ROUND ||
            back_to_drawer(probe, route, i + 1 == probe->route_count);
        const bool use_drawer_home = round == DRAWER_BACK_ROUND && i + 1 == probe->route_count;
        const bool exited = back_ok && (use_drawer_home ? click_drawer_home(probe) :
            exit_to_desktop(probe, round == 1));
        if (use_drawer_home && exited) {
            probe->drawer_home_clicked = true;
            ESP_LOGI(TAG, "CHECK drawer-home touch=desktop route=%s scroll=%" PRId32,
                     route->id, probe->drawer_scroll_y);
        }
        if (!actions_ok) {
            ++probe->failed_actions;
            ESP_LOGE(TAG, "FAIL round=%u route=%s supplemental action", round, route->id);
        }
        if (!exited) {
            ESP_LOGE(TAG, "FAIL round=%u route=%s action=%u cleanup=%u",
                     round, route->id, actions_ok, exited);
            return false;
        }
        ++probe->routes_passed;
        ESP_LOGI(TAG, "PASS round=%u route=%s lcd_regions=%" PRIu64 " cleanup=idle",
                 round, route->id, completed_regions() - before);
        report_memory(round, "route", route->id, heap_before);
    }
    return true;
}

static bool tap_point_locked(lv_obj_t *widget, lv_point_t point)
{
    lv_indev_data_t sample = {.point = point, .state = LV_INDEV_STATE_PRESSED};
    lv_indev_t *pointer = create_pointer(lv_obj_get_display(widget), &sample);
    if (!pointer) return false;
    lv_indev_read(pointer);
    sample.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    delete_pointer(pointer);
    return true;
}

static bool tap_game_button(probe_t *probe, const char *caption)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *button = current_page(probe) ? button_for(probe->ui->content, caption) : NULL;
    bool tapped = false;
    if (button && !lv_obj_has_state(button, LV_STATE_DISABLED)) {
        lv_obj_update_layout(button);
        lv_area_t area;
        lv_obj_get_coords(button, &area);
        const lv_point_t center = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
        tapped = tap_point_locked(button, center);
    }
    lvgl_port_unlock();
    return tapped;
}

static bool snake_start_with_live_imu(probe_t *probe)
{
    if (!click_label(probe, "模式 · 触摸") || !tap_game_button(probe, "开始游戏")) return false;
    const int64_t deadline = esp_timer_get_time() + INT64_C(1500000);
    bool started = false;
    do {
        pause_ms(POLL_MS);
        if (!lvgl_port_lock(POLL_MS)) continue;
        lv_obj_t *content = probe->ui->content;
        lv_obj_t *pause = current_page(probe) ? button_for(content, LV_SYMBOL_PAUSE) : NULL;
        started = pause && !lv_obj_has_state(pause, LV_STATE_DISABLED) &&
            button_for(content, "模式 · 倾斜") && !find_label(content, "开始游戏", false) &&
            !find_label(content, "等待校准", false);
        lvgl_port_unlock();
    } while (!started && esp_timer_get_time() < deadline);

    app_telemetry_snapshot_t sample;
    app_telemetry_get(&sample);
    ESP_LOGI(TAG, "CHECK subpage=game-snake live_tilt_start=%u imu=%s bias_valid=%u stationary=%u",
             started, esp_err_to_name(sample.imu_error), sample.imu.gyro_bias_valid,
             sample.imu.gyro_stationary);
    return started && click_label(probe, "模式 · 倾斜");
}

static bool no_direction_buttons(lv_obj_t *content)
{
    return !button_for(content, LV_SYMBOL_UP) && !button_for(content, LV_SYMBOL_RIGHT) &&
        !button_for(content, LV_SYMBOL_DOWN) && !button_for(content, LV_SYMBOL_LEFT);
}

static lv_obj_t *touch_board(lv_obj_t *content, int32_t min_height)
{
    lv_obj_update_layout(content);
    const uint32_t children = lv_obj_get_child_count(content);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t *child = lv_obj_get_child(content, (int32_t)i);
        if (lv_obj_get_width(child) >= 300 && lv_obj_get_height(child) >= min_height &&
            !lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) return child;
    }
    return NULL;
}

static lv_obj_t *touch_surface(lv_obj_t *board)
{
    if (!board || lv_obj_has_flag(lv_obj_get_parent(board), LV_OBJ_FLAG_CLICKABLE)) return NULL;
    lv_obj_t *screen = lv_obj_get_screen(board);
    lv_display_t *display = lv_obj_get_display(screen);
    const int32_t width = lv_display_get_horizontal_resolution(display);
    const int32_t height = lv_display_get_vertical_resolution(display);
    lv_area_t board_area;
    lv_obj_get_coords(board, &board_area);
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
        lv_area_t area;
        lv_obj_get_coords(child, &area);
        if (area.x1 == 0 && area.x2 == width - 1 && area.y1 == board_area.y1 &&
            area.y2 == height - 1 && lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) return child;
    }
    return NULL;
}

static bool test_crates_touch(probe_t *probe)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *content = probe->ui->content;
    lv_obj_t *board = current_page(probe) && no_direction_buttons(content) ? touch_board(content, 300) : NULL;
    lv_obj_t *surface = touch_surface(board);
    bool moved = false;
    if (surface && find_label(content, "1 / 6  ·  0 步", false)) {
        lv_area_t area, target;
        lv_obj_get_coords(board, &area);
        lv_obj_get_coords(surface, &target);
        const int32_t cx = (target.x1 + target.x2) / 2, cy = (target.y1 + target.y2) / 2;
        const int32_t dx = (area.x2 - area.x1) / 8, dy = (area.y2 - area.y1) / 8;
        const lv_point_t regions[] = {
            {cx, cy}, {cx + 4, cy}, {cx, area.y1 + dy}, {target.x2 - 8, cy},
            {area.x1 + dx, cy}, {cx, area.y2 - dy}, {target.x2 - 8, cy},
            {target.x1 + 8, cy}, {cx, target.y2}, {cx + 15, cy},
        };
        /* Right hits a wall only after taking the correct first up step. */
        const char *const expected[] = {
            "1 / 6  ·  0 步", "1 / 6  ·  0 步", "1 / 6  ·  1 步", "1 / 6  ·  1 步",
            "1 / 6  ·  2 步", "1 / 6  ·  3 步", "1 / 6  ·  4 步",
            "1 / 6  ·  5 步", "1 / 6  ·  6 步", "1 / 6  ·  7 步",
        };
        moved = true;
        for (size_t i = 0; moved && i < sizeof(regions) / sizeof(regions[0]); ++i)
            moved = tap_point_locked(surface, regions[i]) && find_label(content, expected[i], false);
    }
    lvgl_port_unlock();
    if (moved) ESP_LOGI(TAG, "CHECK subpage=game-crates large_board=1 full_width=1 margins=1 bottom=1 center_ignored=1 wall_blocked=1");
    return moved;
}

static bool tiles_read_grid(probe_t *probe, uint32_t values[16])
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *content = probe->ui->content;
    lv_obj_t *board = NULL;
    if (current_page(probe)) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(content); ++i) {
            lv_obj_t *child = lv_obj_get_child(content, (int32_t)i);
            if (lv_obj_get_child_count(child) == 16) { board = child; break; }
        }
    }
    bool ok = board != NULL;
    for (unsigned i = 0; ok && i < 16; ++i) {
        lv_obj_t *cell = lv_obj_get_child(board, (int32_t)i);
        lv_obj_t *label = lv_obj_get_child(cell, 0);
        ok = label && lv_obj_check_type(label, &lv_label_class);
        if (ok) values[i] = (uint32_t)strtoul(lv_label_get_text(label), NULL, 10);
    }
    lvgl_port_unlock();
    return ok;
}

static bool test_tiles_acceleration_idle(probe_t *probe)
{
    const int64_t deadline = esp_timer_get_time() + INT64_C(2200000);
    bool ready = false;
    do {
        pause_ms(POLL_MS);
        ready = page_has_label(probe, "沿屏幕方向轻推");
    } while (!ready && esp_timer_get_time() < deadline);

    uint32_t before[16], after[16];
    bool stayed = ready && tiles_read_grid(probe, before);
    unsigned samples = 0;
    app_telemetry_snapshot_t sample = {0};
    if (stayed) {
        const int64_t until = esp_timer_get_time() + INT64_C(400000);
        uint32_t previous_stamp = 0;
        while (esp_timer_get_time() < until) {
            app_telemetry_get(&sample);
            if (sample.imu_error == ESP_OK && sample.imu_updated_ms != previous_stamp) {
                previous_stamp = sample.imu_updated_ms;
                ++samples;
            }
            pause_ms(4);
        }
        stayed = tiles_read_grid(probe, after) && memcmp(before, after, sizeof(before)) == 0;
    }
    app_telemetry_get(&sample);
    ESP_LOGI(TAG, "CHECK subpage=game-2048 live_acceleration_ready=%u idle_board_unchanged=%u imu=%s fresh_frames_400ms=%u",
             ready, stayed, esp_err_to_name(sample.imu_error), samples);
    return ready && stayed && samples >= 24;
}

static bool test_game_controls(probe_t *probe, const route_t *route)
{
    if (strcmp(route->id, "game-dice") == 0) {
        if (!tap_game_button(probe, "1 枚") || !page_has_label(probe, "2 枚") ||
            !tap_game_button(probe, "掷骰") || !page_has_label(probe, "摇动中")) return false;
        pause_ms(800);
        return !page_has_label(probe, "摇动中") && page_has_label(probe, "2 枚");
    }
    if (strcmp(route->id, "game-fall") == 0) {
        return tap_game_button(probe, "开始") && tap_game_button(probe, "旋转") &&
            tap_game_button(probe, "快速落下") && !page_has_label(probe, "得分  0") &&
            tap_game_button(probe, LV_SYMBOL_PAUSE) && page_has_label(probe, "已暂停") &&
            tap_game_button(probe, "继续") && !page_has_label(probe, "已暂停");
    }
    const bool tiles = strcmp(route->id, "game-2048") == 0;
    const bool snake = strcmp(route->id, "game-snake") == 0;
    if (strcmp(route->id, "game-crates") == 0) return test_crates_touch(probe);
    if (!tiles && !snake) return true;

    const bool acceleration_selected = tiles && page_has_label(probe, "加速度");
    const char *initial_mode = tiles ? (acceleration_selected ? "加速度" : "滑动") : "模式 · 触摸";
    bool toggled;
    if (tiles) {
        const bool enabled = acceleration_selected || click_label(probe, "滑动");
        const bool idle_ok = enabled && test_tiles_acceleration_idle(probe);
        toggled = enabled && click_label(probe, "加速度") &&
                  (!acceleration_selected || click_label(probe, "滑动")) && idle_ok;
    } else toggled = snake_start_with_live_imu(probe);
    if (!toggled) return false;

    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *content = probe->ui->content;
    const bool verified = current_page(probe) && button_for(content, initial_mode) &&
        (tiles ? (acceleration_selected || find_label(content, "滑动棋盘", false) != NULL) :
            no_direction_buttons(content) && touch_surface(touch_board(content, 250)) &&
            find_label(content, "开始游戏", false) == NULL);
    lvgl_port_unlock();
    if (verified) ESP_LOGI(TAG, "CHECK subpage=%s mode_toggle=returned%s", route->id,
                           snake ? " large_board=1 full_width=1 dpad=0 start_button=activated" : "");
    return verified;
}

static bool header_return(probe_t *probe, void (*destination)(app_ui_t *), const route_t *parent)
{
    const uint64_t before = completed_regions();
    if (!lvgl_port_lock(1000)) return false;
    app_ui_t *ui = probe->ui;
    lv_obj_t *back = current_page(probe) && ui->page_back == destination ?
        button_for(ui->screen, LV_SYMBOL_LEFT) : NULL;
    if (back && lv_obj_get_parent(back) != ui->screen) back = NULL;
    if (back) {
        lv_obj_send_event(back, LV_EVENT_CLICKED, NULL);
        ++probe->generation;
    }
    lvgl_port_unlock();
    return back && wait_page(probe, parent, before, true);
}

static bool test_bluetooth_modes(probe_t *probe, const route_t *parent)
{
    const route_t modes[] = {
        {"bt-audio", "蓝牙音频", "音频输出"},
        {"bt-hid", "空中飞鼠", "蓝牙键鼠"},
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        const uint64_t before = completed_regions();
        if (!click_label(probe, modes[i].drawer_label)) return false;
        ++probe->generation;
        if (!wait_page(probe, &modes[i], before, true) ||
            !header_return(probe, app_open_bluetooth, parent)) return false;
    }
    ESP_LOGI(TAG, "CHECK bluetooth-modes audio=1 hid=1 back=overview");
    return true;
}

static bool test_countdown_controls(probe_t *probe)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *hours = current_page(probe) ? find_class(probe->ui->content, &lv_dropdown_class) : NULL;
    unsigned fields = 0;
    if (hours) {
        lv_obj_t *picker = lv_obj_get_parent(hours);
        for (uint32_t i = 0; i < lv_obj_get_child_count(picker); ++i) {
            lv_obj_t *field = lv_obj_get_child(picker, (int32_t)i);
            if (!lv_obj_check_type(field, &lv_dropdown_class)) continue;
            lv_dropdown_set_selected(field, fields++ == 2 ? 30 : 0);
            lv_obj_send_event(field, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }
    lvgl_port_unlock();
    if (fields != 3) return false;
    app_countdown_snapshot_t state;
    const bool started = tap_game_button(probe, "开始");
    const bool paused = started && tap_game_button(probe, "暂停");
    app_countdown_get(&state);
    const bool pause_state = state.state == APP_COUNTDOWN_PAUSED &&
        state.remaining_seconds > 0 && state.remaining_seconds <= 30;
    const bool resumed = paused && tap_game_button(probe, "继续");
    app_countdown_get(&state);
    const bool resume_state = state.state == APP_COUNTDOWN_RUNNING;
    const bool reset = resumed && tap_game_button(probe, "复位");
    app_countdown_get(&state);
    const bool reset_state = state.state == APP_COUNTDOWN_IDLE;
    if (!reset_state) app_countdown_reset();
    return started && paused && pause_state && resumed && resume_state && reset && reset_state;
}

static bool page_has_label(probe_t *probe, const char *label)
{
    if (!lvgl_port_lock(1000)) return false;
    const bool found = current_page(probe) && find_label(probe->ui->content, label, false);
    lvgl_port_unlock();
    return found;
}

static bool test_nested_routes(probe_t *probe, unsigned round)
{
    for (unsigned i = 0; i < sizeof(nested_routes) / sizeof(nested_routes[0]); i++) {
        const nested_route_t *test = &nested_routes[i];
        const heap_sample_t heap_before = psram_sample();
        if (!open_drawer_tile(probe, test->parent, true)) return false;
        bool level_zeroed = false;
        if (round == 1 && test->parent == MOTION) {
            for (unsigned attempt = 0; attempt < 15 && !level_zeroed; ++attempt) {
                app_telemetry_snapshot_t sample;
                app_telemetry_get(&sample);
                float reference[4];
                const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                if (sample.imu_error == ESP_OK && !sample.gyro_calibrating &&
                    sample.imu_updated_ms && now - sample.imu_updated_ms <= 250 &&
                    app_motion_level_reference(sample.imu.accel_mps2, reference)) {
                    level_zeroed = tap_game_button(probe, "姿态归零");
                }
                if (!level_zeroed) pause_ms(POLL_MS);
            }
        }
        const uint64_t before = completed_regions();
        if (!click_label(probe, test->button)) {
            ESP_LOGE(TAG, "FAIL nested=%s missing route button", test->page.id);
            return false;
        }
        ++probe->generation;
        if (!wait_page(probe, &test->page, before, true)) {
            ESP_LOGE(TAG, "FAIL nested=%s open/render", test->page.id);
            return false;
        }
        if (level_zeroed) {
            bool centered = false;
            for (unsigned attempt = 0; attempt < 10 && !centered; ++attempt) {
                centered = page_has_label(probe, "已水平");
                if (!centered) pause_ms(POLL_MS);
            }
            ESP_LOGI(TAG, "CHECK IMU shared pose/level zero=%s", centered ? "PASS" : "FAIL");
            if (!centered) return false;
        } else if (round == 1 && test->parent == MOTION) {
            ESP_LOGI(TAG, "SKIP IMU shared zero: stable face-up sample unavailable");
        }
        if (round == 1 && !test_game_controls(probe, &test->page)) {
            ESP_LOGE(TAG, "FAIL nested=%s input controls", test->page.id);
            return false;
        }
        if (!strcmp(test->page.id, "stopwatch")) {
            if (!tap_game_button(probe, "开始") || !tap_game_button(probe, "计次") ||
                !page_has_label(probe, "01") || !tap_game_button(probe, "暂停") ||
                !tap_game_button(probe, "复位") || !page_has_label(probe, "00:00:00") ||
                !page_has_label(probe, "开始")) {
                ESP_LOGE(TAG, "FAIL stopwatch start/lap/pause/reset");
                return false;
            }
        }
        if (!strcmp(test->page.id, "countdown") && !test_countdown_controls(probe)) {
            ESP_LOGE(TAG, "FAIL countdown start/pause/resume/reset");
            return false;
        }
        if (round == 1 && !strcmp(test->page.id, "wallpaper-picker") && !test_gallery(probe)) {
            ESP_LOGE(TAG, "FAIL wallpaper picker preview");
            return false;
        }
        const bool verify_bluetooth = !strcmp(test->page.id, "bluetooth") &&
            (round == 1 || round == ROUNDS / 2 || round == ROUNDS);
        if (verify_bluetooth && !test_bluetooth_modes(probe, &test->page)) {
            ESP_LOGE(TAG, "FAIL bluetooth modes/back");
            return false;
        }
        if ((round == 1 || verify_bluetooth) && test->parent == SETTINGS &&
            (!header_return(probe, app_open_settings, &routes[SETTINGS]) ||
             !test_settings_top(probe, test->page.id))) {
            ESP_LOGE(TAG, "FAIL nested=%s settings back/scroll", test->page.id);
            return false;
        }
        if (!exit_to_desktop(probe, false)) {
            ESP_LOGE(TAG, "FAIL nested=%s open/render/cleanup", test->page.id);
            return false;
        }
        ++probe->nested_passed;
        ESP_LOGI(TAG, "PASS round=%u subpage=%s lcd_regions=%" PRIu64 " cleanup=idle",
                 round, test->page.id, completed_regions() - before);
        report_memory(round, "subpage", test->page.id, heap_before);
    }
    return true;
}

static void start_weather_probe(void)
{
    aether_wifi_handle_t wifi = aether_wifi_get_default();
    aether_wifi_status_t network = {0};
    if (!wifi || aether_wifi_get_status(wifi, &network) != ESP_OK || !network.saved_network) {
        ESP_LOGI(TAG, "WEATHER saved network unavailable");
        return;
    }
    const esp_err_t error = network.enabled ? ESP_OK : aether_wifi_set_enabled(wifi, true);
    ESP_LOGI(TAG, "WEATHER saved network enable=%s", esp_err_to_name(error));
}

static void report_weather_probe(void)
{
    aether_wifi_handle_t wifi = aether_wifi_get_default();
    aether_wifi_status_t network = {0};
    if (wifi) (void)aether_wifi_get_status(wifi, &network);
    app_weather_snapshot_t weather;
    app_weather_get(&weather);
    ESP_LOGI(TAG, "WEATHER connected=%u forecast_valid=%u fetching=%u result=%s",
             network.connected, weather.valid, weather.fetching, esp_err_to_name(weather.error));
}

static bool keep_music_preferences(void *context)
{
    (void)context;
    return true;
}

typedef struct {
    TaskHandle_t caller;
    app_ui_t *ui;
    bool ok;
} mic_probe_t;

static void mic_probe_task(void *context)
{
    mic_probe_t *probe = context;
    probe->ok = selftest_microphone(probe->ui);
    xTaskNotifyGive(probe->caller);
    vTaskDelete(NULL);
}

static bool test_microphone_internal(app_ui_t *ui)
{
    /* The IRAM-safe bus requires internal I2C receive buffers. Use the same
     * internal stack class as the recorder rather than the UI probe's PSRAM. */
    mic_probe_t probe = {.caller = xTaskGetCurrentTaskHandle(), .ui = ui};
    if (xTaskCreate(mic_probe_task, "mic_probe", 4096, &probe, 2, NULL) != pdPASS)
        return false;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return probe.ok;
}

enum { TEST_WALLPAPER_WIDTH = 32, TEST_WALLPAPER_HEIGHT = 48, TEST_WALLPAPER_PIXEL = 0x47AF };

typedef struct {
    TaskHandle_t caller;
    uint16_t width, height;
    bool empty, ok;
} wallpaper_verify_t;

static void wallpaper_verify_task(void *context)
{
    wallpaper_verify_t *verify = context;
    app_wallpaper_store_t store;
    uint16_t *pixels, width, height;
    const esp_err_t result = app_wallpaper_store_open(&store, &pixels, &width, &height);
    bool ok = result == ESP_OK;
    if (verify->empty) ok = ok && !pixels;
    else {
        ok = ok && pixels && width == verify->width && height == verify->height;
        for (unsigned i = 0; ok && i < (unsigned)width * height; ++i)
            ok = pixels[i] == TEST_WALLPAPER_PIXEL;
    }
    free(pixels);
    verify->ok = ok;
    xTaskNotifyGive(verify->caller);
    vTaskDelete(NULL);
}

static bool verify_wallpaper_bank(uint16_t width, uint16_t height)
{
    wallpaper_verify_t verify = {
        .caller = xTaskGetCurrentTaskHandle(), .width = width, .height = height, .empty = !width,
    };
    if (xTaskCreate(wallpaper_verify_task, "verify_wallpaper", 4096, &verify, 2, NULL) != pdPASS)
        return false;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return verify.ok;
}

static esp_err_t wait_wallpaper_save(void)
{
    const int64_t deadline = esp_timer_get_time() + INT64_C(10000000);
    do {
        pause_ms(POLL_MS);
        if (!lvgl_port_lock(POLL_MS)) continue;
        const esp_err_t status = app_theme_write_status();
        lvgl_port_unlock();
        if (status != ESP_ERR_NOT_FINISHED) return status;
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

static bool load_wallpaper_fixture(app_ui_t *ui, app_media_image_t *image)
{
    static const char root[] = AETHER_STORAGE_MOUNT_POINT;
    enum { ROW_BYTES = TEST_WALLPAPER_WIDTH * 3, FILE_BYTES = 54 + ROW_BYTES * TEST_WALLPAPER_HEIGHT };
    _Static_assert(ROW_BYTES % 4 == 0, "BMP fixture row alignment");
    if (!aether_media_fs_path_ready(ui->storage, root) || app_io_usb_storage_blocked(ui->io)) {
        uint16_t *pixels = malloc(TEST_WALLPAPER_WIDTH * TEST_WALLPAPER_HEIGHT * sizeof(*pixels));
        if (!pixels) return false;
        for (unsigned i = 0; i < TEST_WALLPAPER_WIDTH * TEST_WALLPAPER_HEIGHT; ++i)
            pixels[i] = TEST_WALLPAPER_PIXEL;
        *image = (app_media_image_t){pixels, TEST_WALLPAPER_WIDTH, TEST_WALLPAPER_HEIGHT};
        ESP_LOGI(TAG, "SKIP wallpaper TF scaling: card unavailable");
        return true;
    }
    aether_media_fs_lease_t lease = {0};
    if (aether_media_fs_acquire(ui->storage, root, &lease) != ESP_OK) return false;
    char directory[64], name[92];
    snprintf(directory, sizeof(directory), "%s/.aether-wallpaper-%08" PRIx32,
             root, (uint32_t)esp_timer_get_time());
    snprintf(name, sizeof(name), "%s/fit.bmp", directory);
    const uint32_t generation = aether_media_fs_generation(ui->storage, name);
    const bool created = mkdir(directory, 0777) == 0;
    const int fd = created ? open(name, O_WRONLY | O_CREAT | O_EXCL, 0666) : -1;
    static const uint8_t header[54] = {
        [0] = 'B', [1] = 'M', [2] = FILE_BYTES & 255, [3] = FILE_BYTES >> 8,
        [10] = 54, [14] = 40, [18] = TEST_WALLPAPER_WIDTH, [22] = TEST_WALLPAPER_HEIGHT,
        [26] = 1, [28] = 24,
    };
    uint8_t row[ROW_BYTES];
    for (unsigned x = 0; x < ROW_BYTES; x += 3) {
        row[x] = 0x78;
        row[x + 1] = 0xF4;
        row[x + 2] = 0x40;
    }
    bool written = fd >= 0 && write(fd, header, sizeof(header)) == sizeof(header);
    for (unsigned y = 0; written && y < TEST_WALLPAPER_HEIGHT; ++y)
        written = write(fd, row, sizeof(row)) == sizeof(row);
    if (fd >= 0 && close(fd) != 0) written = false;
    const atomic_bool cancelled = false;
    const esp_err_t decoded = written ? app_media_image_read(ui->storage, name, generation, &cancelled,
        APP_WALLPAPER_SIZE, APP_WALLPAPER_SIZE, true, image) : ESP_FAIL;
    bool cleaned = false;
    if (created && aether_media_fs_available(&lease) &&
        aether_media_fs_generation(ui->storage, name) == generation) {
        const bool removed = fd < 0 || unlink(name) == 0;
        cleaned = removed && rmdir(directory) == 0;
    }
    aether_media_fs_release(&lease);
    const bool ok = decoded == ESP_OK && image->width == 320 && image->height == 480 && cleaned;
    ESP_LOGI(TAG, "%s wallpaper TF scaling %ux%u -> %ux%u source_removed=%u",
             ok ? "PASS" : "FAIL", TEST_WALLPAPER_WIDTH, TEST_WALLPAPER_HEIGHT,
             image->width, image->height, cleaned);
    return ok;
}

static bool test_photo_decoders(app_ui_t *ui)
{
    static const uint8_t png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
        0x9f, 0xe1, 0x3f, 0x43, 0x23, 0xbe, 0x87, 0xf5, 0x00, 0x00, 0x00, 0x0c,
        0x49, 0x44, 0x41, 0x54, 0x03, 0x03, 0x98, 0xfe, 0xff, 0xff, 0x3f, 0x00,
        0x3f, 0xd7, 0x08, 0x79, 0xa8, 0x57, 0xf3, 0x97, 0x00, 0x00, 0x00, 0x00,
        0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    static const uint8_t gif[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x02, 0x00, 0x81, 0x00,
        0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
        0xff, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45,
        0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x01,
        0x05, 0x00, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02,
        0x00, 0x00, 0x08, 0x07, 0x00, 0x01, 0x04, 0x10, 0x30, 0x20, 0x20, 0x00,
        0x3b,
    };
    static const struct {
        const char *suffix;
        const uint8_t *bytes;
        size_t size;
        uint16_t pixels[4];
    } cases[] = {{"png", png, sizeof(png), {0xf800, 0x0400, 0, 0xffff}},
                 {"gif", gif, sizeof(gif), {0xf800, 0x07e0, 0, 0xffff}}};
    const char *root = AETHER_STORAGE_MOUNT_POINT;
    if (!aether_media_fs_path_ready(ui->storage, root) || app_io_usb_storage_blocked(ui->io)) {
        ESP_LOGI(TAG, "SKIP PNG/GIF physical TF decode: card unavailable");
        return true;
    }
    aether_media_fs_lease_t lease = {0};
    if (aether_media_fs_acquire(ui->storage, root, &lease) != ESP_OK) return false;
    bool all_ok = true;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char path[88];
        snprintf(path, sizeof(path), "%s/.aether-image-%08" PRIx32 ".%s", root,
                 (uint32_t)esp_timer_get_time(), cases[i].suffix);
        const uint32_t generation = aether_media_fs_generation(ui->storage, path);
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        bool written = fd >= 0 && write(fd, cases[i].bytes, cases[i].size) == (ssize_t)cases[i].size;
        if (fd >= 0 && close(fd) != 0) written = false;
        const atomic_bool cancelled = false;
        app_media_image_t image = {0};
        const esp_err_t decoded = written ? app_media_image_read(ui->storage, path, generation,
            &cancelled, 408, 276, false, &image) : ESP_FAIL;
        const bool removed = fd >= 0 && aether_media_fs_available(&lease) &&
            aether_media_fs_generation(ui->storage, path) == generation && unlink(path) == 0;
        const bool ok = decoded == ESP_OK && removed && image.width == 2 && image.height == 2 &&
            memcmp(image.pixels, cases[i].pixels, sizeof(cases[i].pixels)) == 0;
        free(image.pixels);
        ESP_LOGI(TAG, "%s %s physical TF decode and RGB565 pixels", ok ? "PASS" : "FAIL", cases[i].suffix);
        all_ok = all_ok && ok;
    }
    aether_media_fs_release(&lease);
    return all_ok;
}

static bool test_wallpaper_store(app_ui_t *ui)
{
    if (!lvgl_port_lock(1000)) return false;
    const bool available = app_theme_wallpaper_available();
    const bool has_personal_wallpaper = app_theme_wallpaper() != NULL;
    lvgl_port_unlock();
    if (!available || has_personal_wallpaper) {
        ESP_LOGI(TAG, "SKIP wallpaper Flash round trip partition=%u existing=%u",
                 available, has_personal_wallpaper);
        return available;
    }
    if (!verify_wallpaper_bank(0, 0)) return false;
    app_media_image_t image = {0};
    if (!load_wallpaper_fixture(ui, &image)) { free(image.pixels); return false; }
    if (!lvgl_port_lock(1000)) { free(image.pixels); return false; }
    const esp_err_t submit = app_theme_write_wallpaper(image.pixels, image.width, image.height);
    lvgl_port_unlock();
    if (submit != ESP_OK) { free(image.pixels); return false; }
    const bool stored = wait_wallpaper_save() == ESP_OK;
    const bool readback = stored && verify_wallpaper_bank(image.width, image.height);
    if (!lvgl_port_lock(1000)) return false;
    const esp_err_t clear = app_theme_write_wallpaper(NULL, 0, 0);
    lvgl_port_unlock();
    const bool cleared = clear == ESP_OK && wait_wallpaper_save() == ESP_OK && verify_wallpaper_bank(0, 0);
    ESP_LOGI(TAG, "%s wallpaper Flash store=%u load=%u cleared=%u",
             readback && cleared ? "PASS" : "FAIL", stored, readback, cleared);
    return readback && cleared;
}

static void test_task(void *context)
{
    probe_t probe = {.ui = context, .route_count = app_home_app_count()};
    /* This diagnostic image will be replaced after the run. Test playback may
     * advance its in-memory bookmark without changing the user's NVS. */
    if (probe.ui->audio_player)
        aether_audio_player_set_persistence_guard(probe.ui->audio_player, keep_music_preferences, NULL);
    app_audio_system_set_persistence_guard(keep_music_preferences, NULL);
    pause_ms(1500);
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    app_display_metrics_t start_display, end_display;
    app_display_metrics_get(&start_display);
    bool ok = false;
    if (lvgl_port_lock(1000)) {
        probe.generation = probe.ui->page_generation;
        ok = current_page(&probe) && probe.ui->home_time &&
            (probe.route_count == INTERCOM || probe.route_count == sizeof(routes) / sizeof(routes[0]));
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "BEGIN rounds=%u routes=%u subpages=%u desktop_input=synthetic-pointer exit=queued",
             ROUNDS, (unsigned)probe.route_count,
             (unsigned)(sizeof(nested_routes) / sizeof(nested_routes[0])));
    ESP_LOGI(TAG, "SKIP physical touch/hold/tilt, accessory pairing, recorder writes and reader/ROM playback requiring writes");
    if (ok) start_weather_probe();
    if (ok) {
        ok = lvgl_port_lock(1000);
        if (ok) {
            ok = current_page(&probe) && desktop_drag(&probe, 0, -24, 1, false) &&
                desktop_drag(&probe, 24, 0, 3, false) && desktop_drag(&probe, 0, 24, 3, false);
            lvgl_port_unlock();
        }
        if (ok) ESP_LOGI(TAG, "CHECK desktop-gesture short-up/horizontal/down=unchanged");
        else ESP_LOGE(TAG, "FAIL desktop-gesture negative paths changed page");
    }
    const heap_sample_t baseline = psram_sample();
    pause_ms(1200);
    report_memory(0, "idle", "desktop", baseline);
    unsigned rounds_passed = 0;
    for (unsigned round = 1; ok && round <= ROUNDS; round++) {
        const heap_sample_t round_before = psram_sample();
        ok = test_routes(&probe, round) && test_nested_routes(&probe, round);
        const bool internal_ok = heap_caps_check_integrity(internal_caps, false);
        const bool external_ok = heap_caps_check_integrity(external_caps, false);
        ok = ok && internal_ok && external_ok;
        if (ok) ++rounds_passed;
        ESP_LOGI(TAG, "ROUND %u heap_internal_ok=%u heap_psram_ok=%u internal_free=%u psram_free=%u",
                 round, internal_ok, external_ok, (unsigned)heap_caps_get_free_size(internal_caps),
                 (unsigned)heap_caps_get_free_size(external_caps));
        report_memory(round, "round", "all", round_before);
    }
    if (ok) ok = test_idle_suspend(&probe);
    const heap_sample_t final_active = psram_sample();
    pause_ms(2000);
    report_memory(0, "idle", "desktop-after-cycles", final_active);
    report_weather_probe();
    if (ok) {
        const bool mic_ok = idle_media(probe.ui) && test_microphone_internal(probe.ui);
        const bool files_ok = selftest_files(probe.ui);
        const bool nes_ok = idle_media(probe.ui) && selftest_nes(probe.ui);
        const bool wallpaper_ok = (ROUNDS != 0 && ROUNDS < 100) || test_wallpaper_store(probe.ui);
        const bool photos_ok = test_photo_decoders(probe.ui);
        ok = mic_ok && files_ok && nes_ok && wallpaper_ok && photos_ok;
    }
    app_display_metrics_get(&end_display);
    const uint32_t drops = end_display.dropped_regions - start_display.dropped_regions;
    const uint32_t timeouts = end_display.flush_wait_timeouts - start_display.flush_wait_timeouts;
    ok = ok && (ROUNDS < DRAWER_BACK_ROUND ||
                (probe.drawer_home_clicked && probe.drawer_back_passed == probe.route_count)) &&
        !probe.failed_actions && !drops && !timeouts && !end_display.flush_stalled;
    ESP_LOGI(TAG, "SUMMARY result=%s rounds=%u routes=%u subpages=%u media_passed=%u media_skipped=%u actions_failed=%u lcd_regions=%" PRIu64
             " dropped_regions=%" PRIu32 " dma_timeouts=%" PRIu32 " stack_min=%u drawer_back_passed=%u drawer_home_clicked=%u",
             ok ? "PASS" : "FAIL", rounds_passed, probe.routes_passed, probe.nested_passed,
             probe.media_passed, probe.media_skipped, probe.failed_actions,
             end_display.completed_sequence - start_display.completed_sequence,
             drops, timeouts, (unsigned)uxTaskGetStackHighWaterMark(NULL), probe.drawer_back_passed,
             probe.drawer_home_clicked);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t app_ui_selftest_start(app_ui_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    /* Synthetic rendering needs a large stack. Keep internal RAM free for
     * the production codec stacks and DMA the diagnostic exercises. */
    return xTaskCreateWithCaps(test_task, "ui_selftest", 16384, ui, 2, NULL, external_caps) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}
