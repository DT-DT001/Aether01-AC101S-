#include "app_video_probe.h"
#include "ui/app_ui_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

#ifndef AETHER_VIDEO_PROBE_ZERO_SECONDS
#define AETHER_VIDEO_PROBE_ZERO_SECONDS 20
#endif

static const char *TAG = "video_probe";
static const char *PATH = "/sdcard/Video/《小雨》歌曲原唱黄龄完整版视频.s31.mp4";
static atomic_bool s_claimed;
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "Optional video probe requires external task-stack support"
#endif
/* The diagnostic controller uses a PSRAM stack and a legal HP SRAM TCB.
 * Its stack remains application-owned until reboot because the task self-deletes;
 * freeing a live/self-deleting stack would race the scheduler. Normal builds omit
 * this file, so they retain neither this allocation nor the automatic test. */
static StackType_t *s_probe_stack;
static StaticTask_t s_probe_task __attribute__((aligned(16)));
static struct {
    app_ui_t *ui;
    uint32_t home_generation;
    void (*radio_check)(void *);
    void *context;
} s_probe;

static bool allowed(app_ui_t *ui, uint32_t generation)
{
    app_power_status_t power = {0};
    return ui && ui->page_generation == generation && !app_ui_input_blocked(ui) &&
        !app_power_is_sleeping(ui->power) &&
        app_power_get_status(ui->power, &power) == ESP_OK && !power.sleeping &&
        !power.shutdown_recovering && power.shutdown_state != APP_POWER_SHUTDOWN_PREPARING &&
        power.shutdown_state != APP_POWER_SHUTDOWN_REQUESTED;
}

static bool return_home(app_ui_t *ui, uint32_t generation, uint32_t *home_generation)
{
    const int64_t deadline = esp_timer_get_time() + 3000000;
    while (esp_timer_get_time() < deadline) {
        if (lvgl_port_lock(20)) {
            const bool same = allowed(ui, generation);
            if (same) {
                app_ui_show_home(ui);
                *home_generation = ui->page_generation;
            }
            lvgl_port_unlock();
            if (same) ESP_LOGI(TAG, "returned to home through normal page cleanup");
            return same;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "cleanup GUI lock timed out; leaving page untouched");
    return false;
}

static void followup_when_idle(app_ui_t *ui, uint32_t generation)
{
    const int64_t deadline = esp_timer_get_time() + 5000000;
    while (esp_timer_get_time() < deadline) {
        if (lvgl_port_lock(20)) {
            const bool same = allowed(ui, generation) && ui->home_time && !ui->page_context;
            lvgl_port_unlock();
            if (!same) return; /* Respect navigation, sleep and shutdown. */
            bool pending = true;
            esp_err_t error = ESP_OK;
            if (app_io_media_release_status(ui->io, &pending, &error) == ESP_OK &&
                    !pending && error == ESP_OK) {
                s_probe.radio_check(s_probe.context);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "follow-up skipped: media release did not complete");
}

typedef enum {
    WAIT_BOOT, WAIT_PLAY, PLAY_FIRST, SEND_FORWARD, WAIT_FORWARD,
    PLAY_FORWARD, SEND_ZERO, WAIT_ZERO, PLAY_ZERO, COMPLETE
} probe_stage_t;

static const char *stage_name(probe_stage_t stage)
{
    static const char *const names[] = {"boot wait", "first picture", "play15s", "seek75%",
        "seek75% picture", "play30s", "seek0", "seek0 picture", "play after seek0", "complete"};
    return names[stage];
}

static void probe_task(void *unused)
{
    (void)unused;
    app_ui_t *ui = s_probe.ui;
    uint32_t generation = s_probe.home_generation, baseline_frame = 0, target = 0;
    probe_stage_t stage = WAIT_BOOT;
    int64_t deadline = esp_timer_get_time() + 10000000;
    int64_t lock_deadline = esp_timer_get_time() + 3000000;
    const char *reason = "complete";
    esp_err_t error = ESP_OK;
    bool own_page = false, cleanup = false;
    aether_video_status_t status = {0};
    ESP_LOGI(TAG, "armed initial_home_generation=%lu; waits10s then15s/seek75%%/30s/seek0/%us",
        (unsigned long)generation, (unsigned)AETHER_VIDEO_PROBE_ZERO_SECONDS);

    if (s_probe.radio_check) {
        /* First release app_main/self-test stacks and finish startup rendering.
         * The diagnostic controller is PSRAM; it never reserves radio HP budget. */
        vTaskDelay(pdMS_TO_TICKS(4000));
        bool same = false;
        if (lvgl_port_lock(1000)) {
            same = allowed(ui, generation) && ui->home_time && !ui->page_context;
            lvgl_port_unlock();
        }
        if (!same) { vTaskDelete(NULL); return; }
        ESP_LOGI(TAG, "cold radio qualification begins");
        s_probe.radio_check(s_probe.context);
        ESP_LOGI(TAG, "cold radio qualification returned; settling before video");
        deadline = esp_timer_get_time() + 1000000;
        lock_deadline = esp_timer_get_time() + 3000000;
    }

    while (stage != COMPLETE) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const int64_t now = esp_timer_get_time();
        if (!lvgl_port_lock(20)) {
            if (now < lock_deadline) continue;
            reason = "GUI lock timeout"; error = ESP_ERR_TIMEOUT; cleanup = own_page; break;
        }
        lock_deadline = now + 3000000;
        if (!allowed(ui, generation)) {
            lvgl_port_unlock();
            reason = "user changed page, sleeping, or shutdown gate";
            break;
        }
        lv_display_t *display = lv_display_get_default();
        if (display) lv_display_trigger_activity(display);

        const probe_stage_t previous = stage;
        if (stage == WAIT_BOOT) {
            if (now >= deadline) {
                app_open_video_path(ui, PATH);
                generation = ui->page_generation;
                own_page = true; stage = WAIT_PLAY; deadline = now + 30000000;
            }
        } else {
            error = aether_video_player_get_status(ui->video_player, &status);
            if (error != ESP_OK) {
                reason = "cached backend status failed"; cleanup = true;
            } else if (status.state == AETHER_VIDEO_ERROR || status.state == AETHER_VIDEO_STOPPED ||
                       status.state == AETHER_VIDEO_FINISHED) {
                /* Preparation initially has the previous stopped snapshot. */
                if (stage != WAIT_PLAY || ui->media_started || status.state == AETHER_VIDEO_ERROR) {
                    reason = "backend stopped, finished, or failed";
                    error = status.last_error == ESP_OK ? ESP_ERR_INVALID_STATE : status.last_error;
                    cleanup = true;
                }
            } else if (status.state == AETHER_VIDEO_PAUSED) {
                /* The probe never pauses: respect a user's same-page pause. */
                reason = "user paused video";
                lvgl_port_unlock();
                break;
            }
            if (error == ESP_OK && !cleanup) {
                if (stage == WAIT_PLAY && ui->media_started && status.state == AETHER_VIDEO_PLAYING &&
                    status.frame_number && status.seek_supported && status.duration_ms > 1) {
                    stage = PLAY_FIRST; deadline = now + 15000000;
                } else if ((stage == PLAY_FIRST || stage == PLAY_FORWARD) && now >= deadline) {
                    target = stage == PLAY_FIRST ? (uint32_t)((uint64_t)status.duration_ms * 3U / 4U) : 0;
                    baseline_frame = status.frame_number;
                    stage = stage == PLAY_FIRST ? SEND_FORWARD : SEND_ZERO;
                    deadline = now + 5000000;
                } else if (stage == SEND_FORWARD || stage == SEND_ZERO) {
                    const esp_err_t sent = app_ui_video_probe_seek(ui, target);
                    if (sent == ESP_OK) {
                        stage = stage == SEND_FORWARD ? WAIT_FORWARD : WAIT_ZERO;
                        deadline = now + 20000000;
                    } else if (sent != ESP_ERR_INVALID_STATE) {
                        error = sent; reason = "seek rejected"; cleanup = true;
                    }
                } else if ((stage == WAIT_FORWARD || stage == WAIT_ZERO) && !status.seeking &&
                           status.state == AETHER_VIDEO_PLAYING && status.frame_number > baseline_frame &&
                           status.seek_requested_ms == target) {
                    stage = stage == WAIT_FORWARD ? PLAY_FORWARD : PLAY_ZERO;
                    deadline = now + (stage == PLAY_FORWARD ? 30000000 :
                        (int64_t)AETHER_VIDEO_PROBE_ZERO_SECONDS * 1000000);
                } else if (stage == PLAY_ZERO && now >= deadline) {
                    stage = COMPLETE; cleanup = true;
                }
                if (stage == previous && now >= deadline &&
                    (stage == WAIT_PLAY || stage == WAIT_FORWARD || stage == WAIT_ZERO ||
                     stage == SEND_FORWARD || stage == SEND_ZERO)) {
                    error = ESP_ERR_TIMEOUT; reason = "stage deadline"; cleanup = true;
                }
            }
        }
        lvgl_port_unlock();
        if (stage != previous) ESP_LOGI(TAG, "stage=%s generation=%lu target_ms=%lu frames=%lu pos_ms=%lu elapsed_ms=%lu",
            stage_name(stage), (unsigned long)generation, (unsigned long)target,
            (unsigned long)status.frame_number, (unsigned long)status.position_ms, (unsigned long)status.elapsed_ms);
        if (cleanup) break;
    }
    ESP_LOGI(TAG, "end stage=%s reason=%s error=%s frames=%lu pos_ms=%lu dropped=%lu read_us=%lu decode_us=%lu",
        stage_name(stage), reason, esp_err_to_name(error), (unsigned long)status.frame_number,
        (unsigned long)status.position_ms, (unsigned long)status.dropped_frames,
        (unsigned long)status.read_us, (unsigned long)status.decode_us);
    uint32_t home_generation = 0;
    const bool home = cleanup && own_page && return_home(ui, generation, &home_generation);
    if (home && stage == COMPLETE && error == ESP_OK && s_probe.radio_check) {
        ESP_LOGI(TAG, "post-video radio qualification waits for media release");
        followup_when_idle(ui, home_generation);
    }
    ESP_LOGI(TAG, "PSRAM controller minimum_free=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

esp_err_t app_video_probe_start(app_ui_t *ui, void (*radio_check)(void *), void *context)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_claimed, &expected, true)) return ESP_ERR_INVALID_STATE;
    /* Initial font/layout work may hold the GUI longer than one input poll.
     * This diagnostic runs once at startup, before its bounded worker loop. */
    if (!lvgl_port_lock(1000)) { atomic_store(&s_claimed, false); return ESP_ERR_TIMEOUT; }
    /* The first home is generation1. Returning home after navigation must not
     * give this temporary diagnostic permission to open a video later. */
    const bool initial_home = ui->page_generation == 1 && ui->home_time && !ui->page_context &&
        !ui->io_complete && ui->video_player && ui->frames && allowed(ui, ui->page_generation);
    if (initial_home) {
        s_probe.ui = ui; s_probe.home_generation = ui->page_generation;
        s_probe.radio_check = radio_check; s_probe.context = context;
    }
    lvgl_port_unlock();
    if (!initial_home) { atomic_store(&s_claimed, false); return ESP_ERR_INVALID_STATE; }
    s_probe_stack = heap_caps_aligned_alloc(16, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_probe_stack) {
        atomic_store(&s_claimed, false); return ESP_ERR_NO_MEM;
    }
    if (!xTaskCreateStaticPinnedToCore(probe_task, "video_probe", 4096, NULL, 2,
            s_probe_stack, &s_probe_task, tskNO_AFFINITY)) {
        heap_caps_free(s_probe_stack); s_probe_stack = NULL;
        atomic_store(&s_claimed, false); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
