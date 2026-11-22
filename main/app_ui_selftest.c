#include "app_ui_selftest.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "aether_recorder.h"
#include "aether_wifi.h"
#include "app_display_metrics.h"
#include "app_home.h"
#include "app_telemetry.h"
#include "app_ui_internal.h"
#include "app_utilities.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/task.h"

enum { ROUNDS = 8, DRAWER_BACK_ROUND = 2, PAGE_TIMEOUT_MS = 5000, POLL_MS = 40 };
static const char *const TAG = "ui_selftest";
static const uint32_t external_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

typedef enum {
    CLOCK, WEATHER, CALENDAR, NOTES, MUSIC, READER, VIDEO, FILES, GAMES, NES,
    RECORDER, SETTINGS, WIFI, BLUETOOTH, STATUS, MOTION, PRESSURE, DISPLACEMENT,
    REMOTE, OBS, INTERCOM, GPS, CAMERA, GALLERY,
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
    [GAMES] = {"games", "游戏", "游戏"},
    [NES] = {"nes-picker", "选择 NES 游戏", "NES 游戏"},
    [RECORDER] = {"recorder", "录音机", "录音"},
    [SETTINGS] = {"settings", "设置", "设置"},
    [WIFI] = {"wifi", "Wi-Fi 网络", "无线网络"},
    [BLUETOOTH] = {"bluetooth", "蓝牙耳机", "蓝牙耳机"},
    [STATUS] = {"status", "设备状态", "设备状态"},
    [MOTION] = {"motion", "运动与姿态", "运动"},
    [PRESSURE] = {"barometer", "气压与高度", "气压高度"},
    [DISPLACEMENT] = {"displacement", "短程位移", "位移测量"},
    [REMOTE] = {"pc-remote", "电脑键鼠", "电脑遥控"},
    [OBS] = {"obs-switcher", "OBS 切换台", "OBS 切换"},
    [INTERCOM] = {"intercom", "无线对讲", "无线对讲"},
    [GPS] = {"gps", "卫星定位", "定位"},
    [CAMERA] = {"usb-camera", "USB 摄像头", "USB 摄像头"},
    [GALLERY] = {"gallery", "图库", "图库"},
};

typedef struct {
    route_id_t parent;
    const char *button;
    route_t page;
} nested_route_t;

static const nested_route_t nested_routes[] = {
    {CLOCK, LV_SYMBOL_BELL, {"alarm", "闹钟", NULL}},
    {SETTINGS, "NES 按键映射", {"nes-keymap", "NES 键盘映射", NULL}},
    {SETTINGS, "传输详情", {"usb-transfer", "USB 文件传输", NULL}},
    {GAMES, "2048", {"game-2048", "2048", NULL}},
    {GAMES, "推箱子", {"game-crates", "推箱子", NULL}},
    {GAMES, "贪吃蛇", {"game-snake", "贪吃蛇", NULL}},
    {GAMES, "跳一跳", {"game-jump", "跳一跳", NULL}},
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
    /* Some storage workers acknowledge cancellation asynchronously. Always
     * sample back at the desktop after a short, fixed settling window. */
    pause_ms(100);
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
    lv_indev_delete(pointer);

    const uint32_t expected = probe->generation + (opens_drawer ? 1U : 0U);
    return before_release == expected && ui->page_generation == expected;
}

static bool open_drawer_tile(probe_t *probe, route_id_t id, bool await_io)
{
    if (!lvgl_port_lock(1000)) return false;
    app_ui_t *ui = probe->ui;
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

    pause_ms(120);
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
                lv_indev_delete(pointer);
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

static bool test_notes_keyboard(probe_t *probe)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *field = current_page(probe) ?
        find_class(probe->ui->content, &lv_textarea_class) : NULL;
    const bool can_edit = field && !lv_obj_has_state(field, LV_STATE_DISABLED);
    if (can_edit) lv_obj_send_event(field, LV_EVENT_CLICKED, NULL);
    const bool soft_keyboard = can_edit &&
        find_class(probe->ui->screen, &lv_keyboard_class);
    lvgl_port_unlock();
    if (!can_edit) return false;
    ESP_LOGI(TAG, "CHECK notes-input opened keyboard=%u (existing text left untouched)",
             soft_keyboard);
    return true;
}

static bool test_music(probe_t *probe)
{
    if (!click_label(probe, "曲库")) return false;
    pause_ms(180);
    if (!click_label(probe, "返回")) return false;
    aether_audio_player_status_t audio;
    if (!probe->ui->audio_player ||
        aether_audio_player_get_status(probe->ui->audio_player, &audio) != ESP_OK ||
        !audio.track_count) {
        ++probe->media_skipped;
        ESP_LOGI(TAG, "SKIP music-playback no indexed track or audio backend");
        return true;
    }
    if (!click_label(probe, "播放")) return false;
    const int64_t deadline = esp_timer_get_time() + PAGE_TIMEOUT_MS * INT64_C(1000);
    do {
        pause_ms(POLL_MS);
        if (aether_audio_player_get_status(probe->ui->audio_player, &audio) != ESP_OK ||
            audio.state == AETHER_AUDIO_ERROR) break;
        if (audio.position_ms >= 100 && audio.sample_rate) {
            ++probe->media_passed;
            ESP_LOGI(TAG, "PASS music-playback position=%" PRIu32 "ms source=%" PRIu32 "Hz",
                     audio.position_ms, audio.sample_rate);
            return true;
        }
    } while (esp_timer_get_time() < deadline);
    ESP_LOGE(TAG, "FAIL music-playback state=%d error=%s", audio.state,
             esp_err_to_name(audio.last_error));
    return false;
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
            return true;
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

static bool test_routes(probe_t *probe, unsigned round)
{
    for (unsigned i = 0; i < probe->route_count; i++) {
        const route_t *route = &routes[i];
        const heap_sample_t heap_before = psram_sample();
        const uint64_t before = completed_regions();
        const bool settle = round != ROUNDS;
        if (!open_drawer_tile(probe, (route_id_t)i, settle)) {
            ESP_LOGE(TAG, "FAIL round=%u route=%s open/render/catalog", round, route->id);
            return false;
        }
        if (round == 1) {
            pause_ms(160);
            report_sensors((route_id_t)i);
        }
        const bool actions_ok = round != 1 ||
            ((i != NOTES || test_notes_keyboard(probe)) &&
             (i != MUSIC || test_music(probe)) &&
             (i != VIDEO || test_video(probe)));
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
    lv_indev_delete(pointer);
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

static bool test_game_controls(probe_t *probe, const route_t *route)
{
    const bool tiles = strcmp(route->id, "game-2048") == 0;
    const bool snake = strcmp(route->id, "game-snake") == 0;
    if (strcmp(route->id, "game-crates") == 0) return test_crates_touch(probe);
    if (!tiles && !snake) return true;

    const char *default_mode = tiles ? "滑动" : "模式 · 触摸";
    const bool toggled = tiles ? (click_label(probe, default_mode) && click_label(probe, "倾斜")) :
        snake_start_with_live_imu(probe);
    if (!toggled) return false;

    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *content = probe->ui->content;
    const bool verified = current_page(probe) && button_for(content, default_mode) &&
        (tiles ? find_label(content, "滑动棋盘", false) != NULL :
            no_direction_buttons(content) && touch_surface(touch_board(content, 250)) &&
            find_label(content, "开始游戏", false) == NULL);
    lvgl_port_unlock();
    if (verified) ESP_LOGI(TAG, "CHECK subpage=%s mode_toggle=returned%s", route->id,
                           snake ? " large_board=1 full_width=1 dpad=0 start_button=activated" : "");
    return verified;
}

static bool test_nested_routes(probe_t *probe, unsigned round)
{
    for (unsigned i = 0; i < sizeof(nested_routes) / sizeof(nested_routes[0]); i++) {
        const nested_route_t *test = &nested_routes[i];
        const heap_sample_t heap_before = psram_sample();
        if (!open_drawer_tile(probe, test->parent, true)) return false;
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
        if (round == 1 && !test_game_controls(probe, &test->page)) {
            ESP_LOGE(TAG, "FAIL nested=%s input controls", test->page.id);
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

static void test_task(void *context)
{
    probe_t probe = {.ui = context, .route_count = app_home_app_count()};
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
    for (unsigned round = 1; ok && round <= ROUNDS; round++) {
        const heap_sample_t round_before = psram_sample();
        ok = test_routes(&probe, round) && test_nested_routes(&probe, round);
        const bool internal_ok = heap_caps_check_integrity(internal_caps, false);
        const bool external_ok = heap_caps_check_integrity(external_caps, false);
        ok = ok && internal_ok && external_ok;
        ESP_LOGI(TAG, "ROUND %u heap_internal_ok=%u heap_psram_ok=%u internal_free=%u psram_free=%u",
                 round, internal_ok, external_ok, (unsigned)heap_caps_get_free_size(internal_caps),
                 (unsigned)heap_caps_get_free_size(external_caps));
        report_memory(round, "round", "all", round_before);
    }
    const heap_sample_t final_active = psram_sample();
    pause_ms(2000);
    report_memory(0, "idle", "desktop-after-cycles", final_active);
    report_weather_probe();
    app_display_metrics_get(&end_display);
    const uint32_t drops = end_display.dropped_regions - start_display.dropped_regions;
    const uint32_t timeouts = end_display.flush_wait_timeouts - start_display.flush_wait_timeouts;
    ok = ok && probe.drawer_home_clicked && probe.drawer_back_passed == probe.route_count &&
        !probe.failed_actions && !drops && !timeouts && !end_display.flush_stalled;
    ESP_LOGI(TAG, "SUMMARY result=%s routes=%u subpages=%u media_passed=%u media_skipped=%u actions_failed=%u lcd_regions=%" PRIu64
             " dropped_regions=%" PRIu32 " dma_timeouts=%" PRIu32 " stack_min=%u drawer_back_passed=%u drawer_home_clicked=%u",
             ok ? "PASS" : "FAIL", probe.routes_passed, probe.nested_passed,
             probe.media_passed, probe.media_skipped, probe.failed_actions,
             end_display.completed_sequence - start_display.completed_sequence,
             drops, timeouts, (unsigned)uxTaskGetStackHighWaterMark(NULL), probe.drawer_back_passed,
             probe.drawer_home_clicked);
    vTaskDelete(NULL);
}

esp_err_t app_ui_selftest_start(app_ui_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    return xTaskCreate(test_task, "ui_selftest", 6656, ui, 2, NULL) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}
