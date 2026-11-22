#include "app_power.h"
#include "app_wake_input.h"
#include "app_telemetry.h"

#include "aether_board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define POWER_NVS_NAMESPACE "power"
#define POWER_NVS_TIMEOUT   "timeout_s"
#define POWER_NVS_BRIGHT    "brightness"
#define POWER_TASK_PERIOD_MS 100
#define PERSIST_TIMEOUT (1U << 0)
#define PERSIST_BRIGHTNESS (1U << 1)
#define POWER_SOFTWARE_HOLD_MS 3000
#define POWER_SHUTDOWN_BUDGET_MS 3500
#define POWER_OFF_RESERVE_MS 400
#define POWER_CLEANUP_WAIT_MS 500

static const char *TAG = "app_power";
static portMUX_TYPE s_settings_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_err_t save_u32(const char *key, uint32_t value);

typedef struct {
    uint8_t unacked;
    bool saw_press;
    bool consume_short;
    bool short_seen;
    bool hold_active;
    bool shutdown_due;
    TickType_t hold_started;
} power_key_t;

typedef struct {
    bool down;
    bool consumed;
    int64_t started_at_us;
} user_key_t;

#define USER_KEY_HOLD_US INT64_C(2000000)

/* The falling-edge latch preserves taps for screen wake between worker polls.
 * The shared GPIO ISR service is never uninstalled by this application. */
static void user_key_interrupt(void *context)
{
    app_power_t *manager = context;
    const TickType_t now = xTaskGetTickCountFromISR();
    const int64_t pressed_at_us = esp_timer_get_time();
    bool notify = false;
    portENTER_CRITICAL_ISR(&s_settings_lock);
    if (!manager->user_key_irq_seen || (TickType_t)(now-manager->user_key_irq_tick) >= pdMS_TO_TICKS(30)) {
        manager->user_key_irq_seen = true; manager->user_key_irq_tick = now;
        manager->user_key_irq_pending = true; manager->user_key_irq_at_us=pressed_at_us; notify = true;
    }
    portEXIT_CRITICAL_ISR(&s_settings_lock);
    if (notify && manager->task) {
        BaseType_t higher = pdFALSE;
        vTaskNotifyGiveFromISR((TaskHandle_t)manager->task, &higher);
        if (higher) portYIELD_FROM_ISR();
    }
}

static void dispatch_user_key(app_power_t *manager, bool pressed, user_key_t *key)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_settings_lock);
    const bool edge = manager->user_key_irq_pending;
    const int64_t pressed_at_us = edge ? manager->user_key_irq_at_us : now;
    manager->user_key_irq_pending = false;
    const bool sleeping = manager->sleeping || manager->sleep_requested;
    const bool blocked = manager->shutdown_state == APP_POWER_SHUTDOWN_PREPARING ||
        manager->shutdown_state == APP_POWER_SHUTDOWN_REQUESTED ||
        manager->shutdown_cancel_error != ESP_OK || manager->sensor_restore_error != ESP_OK;
    void (*handler)(void *, int64_t) = manager->user_key_handler;
    void (*tap_handler)(void *, int64_t) = manager->user_key_tap_handler;
    void *context = manager->user_key_context;
    portEXIT_CRITICAL(&s_settings_lock);
    const bool wake = !key->down && (pressed || edge) && sleeping && !blocked;
    if (!pressed) {
        const int64_t started = key->down ? key->started_at_us : pressed_at_us;
        const bool tap = (key->down || edge) && !key->consumed && !sleeping && !blocked &&
                         now - started < USER_KEY_HOLD_US;
        *key = (user_key_t){0};
        if (tap && tap_handler) tap_handler(context, started);
    } else {
        if (!key->down || (edge && !key->consumed))
            key->started_at_us = pressed_at_us;
        key->down = true;
        if (sleeping || blocked) key->consumed = true;
        if (!key->consumed && now - key->started_at_us >= USER_KEY_HOLD_US) {
            key->consumed = true;
            if (handler) handler(context, key->started_at_us);
        }
    }
    if (wake) (void)app_power_wake(manager);
}

/* A physically black screen keeps wake priority, even if another wake request
 * is waiting for the LVGL lock. Decide and publish under the same lock. */
static bool key_request_wake(app_power_t *manager)
{
    portENTER_CRITICAL(&s_settings_lock);
    const bool was_asleep = manager->sleep_requested || manager->sleeping;
    manager->wake_requested = true;
    manager->sleep_requested = false;
    ++manager->request_revision;
    portEXIT_CRITICAL(&s_settings_lock);
    return was_asleep;
}

static void key_request_short(app_power_t *manager, bool pressed_awake)
{
    portENTER_CRITICAL(&s_settings_lock);
    const bool destination_asleep = manager->sleep_requested || manager->sleeping;
    /* A press that began awake must not wake the panel if idle sleep happened
     * while the key was held. SHORT-only fallback prioritizes physical sleep. */
    const bool sleep = pressed_awake || !destination_asleep;
    manager->sleep_requested = sleep;
    manager->wake_requested = !sleep;
    ++manager->request_revision;
    portEXIT_CRITICAL(&s_settings_lock);
    ESP_LOGI(TAG, "TG29 short press: request screen %s", sleep ? "sleep" : "wake");
}

static void handle_key_events(app_power_t *manager, power_key_t *key,
                              uint8_t events, esp_err_t acknowledge_error)
{
    const uint8_t fresh = events & (uint8_t)~key->unacked;
    key->shutdown_due = false;
    /* REG20 is only a boot-source latch, NOT a current PWRON level. Track
     * confirmed edge history instead. Uncertainty must cancel, never infer a
     * hold from the 1.5s LONG IRQ or from a stale unacknowledged PRESS. */
    if (acknowledge_error != ESP_OK ||
        (events & (TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS)))
        key->hold_active = false;
    /* get_key_events preserves a read snapshot when its RW1C ACK fails. Such
     * sticky bits represent the same gesture until a successful ACK drains it. */
    if (acknowledge_error == ESP_OK) key->unacked = 0;
    else key->unacked |= events;
    if (!events && acknowledge_error == ESP_OK && key->short_seen) {
        key->saw_press = key->consume_short = key->short_seen = false;
    }
    if (fresh & TG29_KEY_EVENT_PRESS) {
        key->saw_press = true;
        key->short_seen = false;
        /* Awake presses are activity too: this cancels an expired idle timer
         * or failed automatic-sleep retry before the GUI lock is acquired. */
        key->consume_short = key_request_wake(manager);
        if (acknowledge_error == ESP_OK &&
            !(events & (TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS | TG29_KEY_EVENT_LONG_PRESS))) {
            key->hold_active = true;
            key->hold_started = xTaskGetTickCount();
        } else {
            key->hold_active = false;
        }
        ESP_LOGI(TAG, "TG29 key down: %s", key->consume_short ? "request wake; consume release" : "await short press");
    }
    if (fresh & TG29_KEY_EVENT_LONG_PRESS) {
        key->consume_short = true;
        /* TG29 has no edge timestamps. A coalesced/abnormally early LONG is
         * evidence of an aged PRESS; counting three more seconds from that
         * read cannot promise the cleanup margin before hardware long-off.
         * This does not change the PMIC's IRQLEVEL/OFFLEVEL configuration. */
        if (key->hold_active &&
            (TickType_t)(xTaskGetTickCount() - key->hold_started) < pdMS_TO_TICKS(1000))
            key->hold_active = false;
    }
    if (fresh & TG29_KEY_EVENT_SHORT_PRESS) {
        if (!key->consume_short) key_request_short(manager, key->saw_press);
        key->consume_short = true;
        key->short_seen = true;
    }
    if (key->hold_active && acknowledge_error == ESP_OK &&
        (TickType_t)(xTaskGetTickCount() - key->hold_started) >= pdMS_TO_TICKS(POWER_SOFTWARE_HOLD_MS)) {
        key->hold_active = false; /* One attempt per physical press, even on failure. */
        key->consume_short = true;
        key->shutdown_due = true;
    }
    /* RELEASE cannot clear the gesture: SHORT can arrive before or after it.
     * A fresh PRESS begins another gesture; drained SHORT-only gestures can
     * also recover when the falling edge was not observed. */
}

static uint32_t shutdown_remaining(TickType_t started)
{
    const TickType_t elapsed = xTaskGetTickCount() - started;
    const TickType_t budget = pdMS_TO_TICKS(POWER_SHUTDOWN_BUDGET_MS);
    if (elapsed >= budget) return 0;
    return (uint32_t)((budget - elapsed) * portTICK_PERIOD_MS);
}

static void shutdown_stage(app_power_t *manager, app_power_shutdown_stage_t stage)
{
    portENTER_CRITICAL(&s_settings_lock);
    manager->shutdown_stage = stage;
    portEXIT_CRITICAL(&s_settings_lock);
}

static esp_err_t restore_sensors(app_power_t *manager)
{
    esp_err_t first = ESP_OK;
    if (manager->board->imu_ready) first = qmi8658a_set_sleep(manager->board->imu, false);
    if (manager->board->barometer_ready) {
        const esp_err_t err = dps368_set_sleep(manager->board->barometer, false);
        if (first == ESP_OK) first = err;
    }
    /* Do not restart the sampler against a partially restored sensor. A later
     * recovery retry can restore it safely while ordinary UI remains usable. */
    if (first == ESP_OK) app_telemetry_resume();
    return first;
}

static void prepare_shutdown(app_power_t *manager)
{
    const TickType_t started = xTaskGetTickCount();
    portENTER_CRITICAL(&s_settings_lock);
    if (manager->shutdown_cancel_error != ESP_OK) {
        portEXIT_CRITICAL(&s_settings_lock);
        return; /* Preserve the pending aborted-attempt context. */
    }
    app_power_shutdown_hook_t hook = manager->shutdown_hook;
    void *context = manager->shutdown_context;
    manager->shutdown_state = APP_POWER_SHUTDOWN_PREPARING;
    manager->shutdown_stage = APP_POWER_SHUTDOWN_BEGIN;
    manager->shutdown_error = ESP_ERR_NOT_FINISHED;
    manager->sensor_restore_error = ESP_OK;
    manager->cleanup_timed_out = false;
    portEXIT_CRITICAL(&s_settings_lock);
    ESP_LOGI(TAG, "3s power hold: prepare shutdown (hardware long-off unchanged)");
    bool began = false, sampling_gated = false, sensors_touched = false;
    esp_err_t err = hook ? hook(context, APP_POWER_SHUTDOWN_BEGIN,
                               shutdown_remaining(started)) : ESP_ERR_INVALID_STATE;
    /* Even a failed BEGIN may have installed gates or sent cancellation. */
    began = hook != NULL;
    const bool begin_timed_out = err == ESP_ERR_TIMEOUT;
    if (begin_timed_out) {
        portENTER_CRITICAL(&s_settings_lock);
        manager->cleanup_timed_out = true;
        portEXIT_CRITICAL(&s_settings_lock);
        ESP_LOGW(TAG, "noncritical shutdown gate timed out; still requiring verified sensor sleep");
        err = ESP_OK;
    }
    if (err != ESP_OK) goto failed;
    if (shutdown_remaining(started) <= POWER_OFF_RESERVE_MS) { err = ESP_ERR_TIMEOUT; goto failed; }

    shutdown_stage(manager, APP_POWER_SHUTDOWN_QUIESCE);
    sampling_gated = true;
    uint32_t remaining = shutdown_remaining(started);
    err = app_telemetry_quiesce(remaining < 500 ? remaining : 500);
    if (err != ESP_OK) goto failed;
    shutdown_stage(manager, APP_POWER_SHUTDOWN_SENSORS);
    /* Missing initialization is not evidence that an always-powered part is
     * asleep. Fail closed and retain the independent hardware-off fallback. */
    if (!manager->board->imu_ready || !manager->board->barometer_ready) {
        err = ESP_ERR_INVALID_STATE;
        goto failed;
    }
    sensors_touched = true;
    err = qmi8658a_set_sleep(manager->board->imu, true);
    if (err != ESP_OK) goto failed;
    if (shutdown_remaining(started) <= POWER_OFF_RESERVE_MS) { err = ESP_ERR_TIMEOUT; goto failed; }
    err = dps368_set_sleep(manager->board->barometer, true);
    if (err != ESP_OK) goto failed;
    ESP_LOGI(TAG, "sensor low-power readback verified: QMI8658A power-down, DPS368 standby");
    remaining = shutdown_remaining(started);
    if (remaining <= POWER_OFF_RESERVE_MS) { err = ESP_ERR_TIMEOUT; goto failed; }

    shutdown_stage(manager, APP_POWER_SHUTDOWN_FINALIZE);
    uint32_t cleanup_budget = remaining - POWER_OFF_RESERVE_MS;
    if (cleanup_budget > POWER_CLEANUP_WAIT_MS) cleanup_budget = POWER_CLEANUP_WAIT_MS;
    err = hook(context, APP_POWER_SHUTDOWN_FINALIZE, cleanup_budget);
    if (err == ESP_ERR_TIMEOUT || (begin_timed_out && err == ESP_ERR_INVALID_STATE)) {
        portENTER_CRITICAL(&s_settings_lock);
        manager->cleanup_timed_out = true;
        portEXIT_CRITICAL(&s_settings_lock);
        ESP_LOGW(TAG, "noncritical shutdown cleanup incomplete after timeout; continuing with sensors asleep");
        err = ESP_OK;
    }
    if (err != ESP_OK) goto failed;
    if (shutdown_remaining(started) <= POWER_OFF_RESERVE_MS) { err = ESP_ERR_TIMEOUT; goto failed; }

    shutdown_stage(manager, APP_POWER_SHUTDOWN_PERSIST);
    /* Only confirmed cleanup permits another Flash write. After a skipped
     * cleanup timeout, media may still own DMA/PSRAM; retain our dirty settings
     * and proceed to power-off without starting a new NVS transaction. */
    portENTER_CRITICAL(&s_settings_lock);
    const uint8_t persist = manager->persist_pending;
    const uint32_t timeout = manager->timeout_seconds;
    const uint8_t brightness = manager->brightness;
    const uint32_t revision = manager->settings_revision;
    const bool cleanup_timed_out = manager->cleanup_timed_out;
    portEXIT_CRITICAL(&s_settings_lock);
    bool skipped_preferences = false;
    if (persist && (cleanup_timed_out || shutdown_remaining(started) <= POWER_OFF_RESERVE_MS + 200)) {
        skipped_preferences = true;
    } else {
        if (persist & PERSIST_TIMEOUT) err = save_u32(POWER_NVS_TIMEOUT, timeout);
        if (err == ESP_OK && (persist & PERSIST_BRIGHTNESS)) {
            if (shutdown_remaining(started) <= POWER_OFF_RESERVE_MS + 200) skipped_preferences = true;
            else err = save_u32(POWER_NVS_BRIGHT, brightness);
        }
        if (err == ESP_ERR_TIMEOUT) skipped_preferences = true;
    }
    portENTER_CRITICAL(&s_settings_lock);
    if (!skipped_preferences && err == ESP_OK && revision == manager->settings_revision)
        manager->persist_pending &= (uint8_t)~persist;
    manager->persistence_error = skipped_preferences ? ESP_ERR_TIMEOUT : err;
    const bool newer_settings = revision != manager->settings_revision;
    if (skipped_preferences || newer_settings) manager->cleanup_timed_out = true;
    portEXIT_CRITICAL(&s_settings_lock);
    if (err == ESP_ERR_TIMEOUT) err = ESP_OK;
    if (err != ESP_OK) goto failed;
    if (skipped_preferences || newer_settings)
        ESP_LOGW(TAG, "noncritical shutdown preferences not fully saved; continuing with sensors asleep");
    if (shutdown_remaining(started) <= POWER_OFF_RESERVE_MS) {
        err = ESP_ERR_TIMEOUT;
        goto failed;
    }
    shutdown_stage(manager, APP_POWER_SHUTDOWN_POWER_OFF);
    err = tg29_request_power_off(manager->board->pmic);
    if (err != ESP_OK) goto failed;
    portENTER_CRITICAL(&s_settings_lock);
    manager->shutdown_state = APP_POWER_SHUTDOWN_REQUESTED;
    manager->shutdown_error = ESP_OK;
    portEXIT_CRITICAL(&s_settings_lock);
    /* Keep every gate and sensor asleep after an ACK; power removal itself is
     * asynchronous. Do not revive DMA, storage or sampling on the next tick. */
    return;
failed:
    {
        const esp_err_t restore = sensors_touched ? restore_sensors(manager) : ESP_OK;
        if (sampling_gated && !sensors_touched) app_telemetry_resume();
        const esp_err_t cancel = began ? hook(context, APP_POWER_SHUTDOWN_CANCEL, 500) : ESP_OK;
        (void)cancel; /* Some native test log adapters omit format arguments. */
        portENTER_CRITICAL(&s_settings_lock);
        manager->shutdown_state = APP_POWER_SHUTDOWN_FAILED;
        manager->shutdown_error = err;
        manager->sensor_restore_error = restore;
        manager->shutdown_cancel_error = cancel;
        portEXIT_CRITICAL(&s_settings_lock);
        ESP_LOGW(TAG, "shutdown aborted stage=%u error=%s sensors=%s cancel=%s; retaining power",
                 (unsigned)manager->shutdown_stage, esp_err_to_name(err),
                 esp_err_to_name(restore), esp_err_to_name(cancel));
    }
}

static void retry_shutdown_cancel(app_power_t *manager)
{
    portENTER_CRITICAL(&s_settings_lock);
    const bool pending = manager->shutdown_cancel_error != ESP_OK;
    app_power_shutdown_hook_t hook = manager->shutdown_hook;
    void *context = manager->shutdown_context;
    portEXIT_CRITICAL(&s_settings_lock);
    if (!pending || !hook) return;
    const esp_err_t error = hook(context, APP_POWER_SHUTDOWN_CANCEL, 100);
    portENTER_CRITICAL(&s_settings_lock);
    manager->shutdown_cancel_error = error;
    portEXIT_CRITICAL(&s_settings_lock);
}

static void set_render_enabled(app_power_t *manager, bool enabled)
{
    /* LVGL 9 uses a reference count here, not a boolean assignment. Repeated
     * sleep/error retries must not accumulate disable references. */
    lv_timer_t *refresh = lv_display_get_refr_timer(manager->display);
    if (!enabled && !manager->render_paused) {
        if (refresh) lv_timer_pause(refresh);
        lv_display_enable_invalidation(manager->display, false);
        manager->render_paused = true;
    } else if (enabled && manager->render_paused) {
        lv_display_enable_invalidation(manager->display, true);
        manager->render_paused = false;
        if (refresh) { lv_timer_resume(refresh); lv_timer_ready(refresh); }
    }
}

static esp_err_t save_u32(const char *key, uint32_t value)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(POWER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, key, value);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs) {
        nvs_close(nvs);
    }
    return err;
}

static void load_settings(app_power_t *manager)
{
    manager->timeout_seconds = CONFIG_AETHER_SCREEN_TIMEOUT_SEC;
    manager->brightness = CONFIG_AETHER_SCREEN_BRIGHTNESS < 5 ? 5 : CONFIG_AETHER_SCREEN_BRIGHTNESS;
    nvs_handle_t nvs = 0;
    if (nvs_open(POWER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint32_t timeout = manager->timeout_seconds;
    uint32_t brightness = manager->brightness;
    if (nvs_get_u32(nvs, POWER_NVS_TIMEOUT, &timeout) == ESP_OK && timeout <= 3600) {
        manager->timeout_seconds = timeout;
    }
    if (nvs_get_u32(nvs, POWER_NVS_BRIGHT, &brightness) == ESP_OK && brightness <= 100) {
        /* A stale 0% setting must not turn a cold boot into an invisible UI. */
        manager->brightness = (uint8_t)(brightness < 5 ? 5 : brightness);
    }
    nvs_close(nvs);
}

static esp_err_t apply_sleep(app_power_t *manager)
{
    ESP_RETURN_ON_FALSE(manager && manager->display && manager->board,
                        ESP_ERR_INVALID_ARG, TAG, "invalid manager");
    set_render_enabled(manager, false);
    esp_err_t err = aether_board_set_display_sleep(manager->board, true);
    /* A partially completed command sequence can still leave the panel asleep.
     * Follow the board state as well as the function return code. */
    portENTER_CRITICAL(&s_settings_lock);
    manager->sleeping = manager->board->display_sleeping;
    manager->display_error = err;
    portEXIT_CRITICAL(&s_settings_lock);
    set_render_enabled(manager, !manager->sleeping);
    manager->dimmed = false;
    return err;
}

static esp_err_t apply_wake(app_power_t *manager)
{
    ESP_RETURN_ON_FALSE(manager && manager->display && manager->board,
                        ESP_ERR_INVALID_ARG, TAG, "invalid manager");
    esp_err_t err = ESP_OK;
    if (manager->sleeping || manager->board->display_sleeping) {
        app_touch_consume_wake_gesture();
        err = aether_board_set_display_sleep(manager->board, false);
    }
    portENTER_CRITICAL(&s_settings_lock);
    manager->sleeping = manager->board->display_sleeping;
    manager->display_error = err;
    portEXIT_CRITICAL(&s_settings_lock);
    set_render_enabled(manager, !manager->sleeping);
    if (err == ESP_OK && !manager->sleeping) {
        lv_obj_invalidate(lv_display_get_screen_active(manager->display));
        lv_display_trigger_activity(manager->display);
        manager->dimmed = false;
        manager->applied_brightness = UINT8_MAX;
        ESP_LOGI(TAG, "screen awake; rendering resumed");
    }
    return err;
}

/* Sample idle duration across a stable LVGL tick for timeout/dimming only.
 * Touch and generic GUI activity never request a screen wake. */
static bool read_activity_tick(lv_display_t *display, uint32_t *inactive,
                                uint32_t *activity)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const uint32_t before = lv_tick_get();
        *inactive = lv_display_get_inactive_time(display);
        if (before == lv_tick_get()) {
            *activity = before - *inactive;
            return true;
        }
    }
    return false;
}

static void power_task(void *argument)
{
    app_power_t *manager = argument;
    bool pending_sleep_retry = false;
    uint32_t observed_request_revision = 0;
    bool request_revision_valid = false;
    TickType_t persist_due = 0;
    bool persist_throttled = false;
    TickType_t display_retry_due = 0;
    bool display_backoff = false;
    uint32_t failed_request_revision = 0;
    TickType_t key_poll_due = 0;
    TickType_t cc_poll_due = 0;
    TickType_t key_retry_due = 0;
    TickType_t sensor_retry_due = 0;
    TickType_t cancel_retry_due = 0;
    user_key_t user_key = {0};
    power_key_t key = {0};
    esp_err_t previous_cc_error = ESP_OK;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POWER_TASK_PERIOD_MS));
        const TickType_t tick_now = xTaskGetTickCount();
        if (manager->shutdown_state == APP_POWER_SHUTDOWN_REQUESTED) continue;
        if (manager->shutdown_cancel_error != ESP_OK && (int32_t)(tick_now - cancel_retry_due) >= 0) {
            retry_shutdown_cancel(manager);
            cancel_retry_due = xTaskGetTickCount() + pdMS_TO_TICKS(200);
        }
        if (manager->sensor_restore_error != ESP_OK && (int32_t)(tick_now - sensor_retry_due) >= 0) {
            /* An I2C outage may outlive the initial rollback. Keep the sampler
             * gated until restoration is verified; retry without touching TF. */
            esp_err_t recovery = app_telemetry_quiesce(500);
            if (recovery == ESP_OK) recovery = restore_sensors(manager);
            portENTER_CRITICAL(&s_settings_lock);
            manager->sensor_restore_error = recovery;
            portEXIT_CRITICAL(&s_settings_lock);
            sensor_retry_due = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        }
        /* Wake sources must be serviced before taking LVGL's lock. The real
         * power button belongs to TG29; GPIO61 is only the separate BOOT key. */
        if (manager->board->pmic) {
            if (!manager->key_irq_enabled && (int32_t)(tick_now - key_retry_due) >= 0) {
                const esp_err_t key_error = tg29_configure_key_wakeup(manager->board->pmic);
                manager->key_irq_enabled = key_error == ESP_OK;
                key_retry_due = tick_now + pdMS_TO_TICKS(2000);
                if (key_error != ESP_OK) ESP_LOGW(TAG, "TG29 key setup: %s", esp_err_to_name(key_error));
            }
            if (manager->key_irq_enabled &&
                (gpio_get_level(AETHER_GPIO_TG29_INT) == 0 || (int32_t)(tick_now - key_poll_due) >= 0)) {
                uint8_t events = 0;
                const esp_err_t key_error = tg29_get_key_events(manager->board->pmic, &events);

                key_poll_due = tick_now + pdMS_TO_TICKS(key.hold_active ? 100 : 500);
                handle_key_events(manager, &key, events, key_error);
                if (key_error != ESP_OK) ESP_LOGW(TAG, "TG29 key events: %s", esp_err_to_name(key_error));
                if (key.shutdown_due) {
                    key.shutdown_due = false;
                    if (manager->sensor_restore_error == ESP_OK && manager->shutdown_cancel_error == ESP_OK) prepare_shutdown(manager);
                    sensor_retry_due = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
                    if (manager->shutdown_state == APP_POWER_SHUTDOWN_REQUESTED) continue;
                }
            }
        }
        /* CC shares GPIO5 with the power key. Drain only its own latched bits,
         * independently of key setup success and of the OTG user's preference. */
        if (manager->board->pmic &&
            (gpio_get_level(AETHER_GPIO_TG29_INT) == 0 || (int32_t)(tick_now - cc_poll_due) >= 0)) {
            uint8_t cc_events = 0;
            const esp_err_t cc_error = tg29_get_cc_events(manager->board->pmic, &cc_events);
            if (cc_error != ESP_OK && cc_error != previous_cc_error)
                ESP_LOGW(TAG, "TG29 CC interrupt acknowledgment: %s", esp_err_to_name(cc_error));
            previous_cc_error = cc_error;
            cc_poll_due = tick_now + pdMS_TO_TICKS(500);
        }
        const bool boot_pressed = gpio_get_level(AETHER_GPIO_BOOT_KEY) == 0;
        dispatch_user_key(manager, boot_pressed, &user_key);
        if (display_backoff && (int32_t)(tick_now - display_retry_due) >= 0) {
            display_backoff = false;
        }
        if (persist_throttled && (int32_t)(tick_now - persist_due) >= 0) {
            persist_throttled = false;
        }
        portENTER_CRITICAL(&s_settings_lock);
        const uint32_t persist_timeout = manager->timeout_seconds;
        const uint8_t persist_brightness = manager->brightness;
        bool sleep_requested = manager->sleep_requested;
        bool wake_requested = manager->wake_requested;
        const uint8_t persist = manager->persist_pending;
        const uint32_t revision = manager->settings_revision;
        uint32_t request_revision = manager->request_revision;
        bool (*persistence_busy)(void *) = manager->persistence_busy;
        void *persistence_context = manager->persistence_busy_context;
        portEXIT_CRITICAL(&s_settings_lock);

        /* Failed lock acquisition is not fresh touch activity. */
        if (lvgl_port_lock(50)) {
            /* A UI request may have changed while this task waited for LVGL. */
            portENTER_CRITICAL(&s_settings_lock);
            sleep_requested = manager->sleep_requested;
            wake_requested = manager->wake_requested;
            request_revision = manager->request_revision;
            const uint32_t timeout = manager->timeout_seconds;
            const uint8_t brightness = manager->brightness;
            portEXIT_CRITICAL(&s_settings_lock);
            if (!request_revision_valid || request_revision != observed_request_revision) {
                /* A newer explicit command supersedes earlier transition retries. */
                pending_sleep_retry = false;
                observed_request_revision = request_revision;
                request_revision_valid = true;
            }
            uint32_t inactive = 0, activity = 0;
            const bool sampled = read_activity_tick(manager->display, &inactive, &activity);
            esp_err_t err = ESP_OK;
            (void)sampled; /* Activity controls idle timeout, never screen wake. */
            if (boot_pressed && !manager->sleeping && !sleep_requested) {
                lv_display_trigger_activity(manager->display);
                inactive = 0;
            }
            const bool allow_panel_io = !display_backoff ||
                                       request_revision != failed_request_revision;
            bool attempted_sleep = false;
            if (wake_requested) pending_sleep_retry = false;
            if (allow_panel_io && (wake_requested)) {
                err = apply_wake(manager);
                if (err == ESP_OK) {
                    inactive = 0;
                }
            } else if (allow_panel_io && (sleep_requested || pending_sleep_retry || (!manager->sleeping && timeout &&
                                          inactive >= timeout * 1000U))) {
                attempted_sleep = true;
                err = apply_sleep(manager);
                if (err == ESP_OK) {
                    pending_sleep_retry = false;
                }
            }
            if (err == ESP_OK && allow_panel_io) {
                portENTER_CRITICAL(&s_settings_lock);
                if (manager->request_revision == request_revision) {
                    if (sleep_requested) manager->sleep_requested = false;
                    if (wake_requested) manager->wake_requested = false;
                }
                portEXIT_CRITICAL(&s_settings_lock);
            } else if (err != ESP_OK) {
                if (attempted_sleep) {
                    portENTER_CRITICAL(&s_settings_lock);
                    /* Automatic sleep has no public request flag. Retain its
                     * unfinished transition even if the panel is now asleep,
                     * but never overwrite a newer wake/sleep command. */
                    pending_sleep_retry = manager->request_revision == request_revision;
                    portEXIT_CRITICAL(&s_settings_lock);
                }
                ESP_LOGW(TAG, "display power transition: %s", esp_err_to_name(err));
                display_backoff = true;
                failed_request_revision = request_revision;
                display_retry_due = tick_now + pdMS_TO_TICKS(2000);
            }
            if (!manager->sleeping && allow_panel_io && err == ESP_OK) {
                const bool dimmed = timeout && inactive >= timeout * 500U;
                const uint8_t target = dimmed && brightness > 20 ? 20 : brightness;
                if (target != manager->applied_brightness) {
                    err = aether_board_set_display_brightness(manager->board, target);
                    portENTER_CRITICAL(&s_settings_lock);
                    manager->display_error = err;
                    portEXIT_CRITICAL(&s_settings_lock);
                    if (err == ESP_OK) {
                        manager->applied_brightness = target;
                    } else {
                        ESP_LOGW(TAG, "display brightness: %s", esp_err_to_name(err));
                        display_backoff = true;
                        failed_request_revision = request_revision;
                        display_retry_due = tick_now + pdMS_TO_TICKS(2000);
                    }
                }
                manager->dimmed = dimmed;
            }
            lvgl_port_unlock();
        }

        /* Flash commit runs outside both the UI callbacks and LVGL lock. */
        const TickType_t now = xTaskGetTickCount();
        const bool deferred = persist && persistence_busy && persistence_busy(persistence_context);
        portENTER_CRITICAL(&s_settings_lock);
        manager->persistence_deferred = deferred;
        portEXIT_CRITICAL(&s_settings_lock);
        if (persist && !persist_throttled && !deferred) {
            esp_err_t err = ESP_OK;
            if (persist & PERSIST_TIMEOUT) err = save_u32(POWER_NVS_TIMEOUT, persist_timeout);
            if (err == ESP_OK && (persist & PERSIST_BRIGHTNESS)) {
                err = save_u32(POWER_NVS_BRIGHT, persist_brightness);
            }
            if (err == ESP_OK) {
                portENTER_CRITICAL(&s_settings_lock);
                manager->persistence_error = ESP_OK;
                if (manager->settings_revision == revision) {
                    manager->persist_pending &= (uint8_t)~persist;
                }
                portEXIT_CRITICAL(&s_settings_lock);
            } else {
                portENTER_CRITICAL(&s_settings_lock);
                manager->persistence_error = err;
                portEXIT_CRITICAL(&s_settings_lock);
                ESP_LOGW(TAG, "persist settings: %s", esp_err_to_name(err));
            }
            persist_due = now + pdMS_TO_TICKS(err == ESP_OK ? 500 : 2000);
            persist_throttled = true;
        }
    }
}

esp_err_t app_power_init(app_power_t *manager, aether_board_t *board,
                         lv_display_t *display)
{
    ESP_RETURN_ON_FALSE(manager && board && display, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");
    *manager = (app_power_t) {
        .board = board,
        .display = display,
    };
    load_settings(manager);
    manager->applied_brightness = UINT8_MAX;
    TaskHandle_t task = NULL;
    ESP_RETURN_ON_FALSE(xTaskCreate(power_task, "display_idle", 4096, manager, 4,
                                    &task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "create power task");
    manager->task = task;
    esp_err_t key_irq = gpio_isr_handler_add(AETHER_GPIO_BOOT_KEY, user_key_interrupt, manager);
    if (key_irq == ESP_ERR_INVALID_STATE) {
        const esp_err_t installed = gpio_install_isr_service(0);
        if (installed == ESP_OK || installed == ESP_ERR_INVALID_STATE)
            key_irq = gpio_isr_handler_add(AETHER_GPIO_BOOT_KEY, user_key_interrupt, manager);
    }
    if (key_irq == ESP_OK) key_irq = gpio_set_intr_type(AETHER_GPIO_BOOT_KEY, GPIO_INTR_NEGEDGE);
    if (key_irq == ESP_OK) key_irq = gpio_intr_enable(AETHER_GPIO_BOOT_KEY);
    if (key_irq != ESP_OK) ESP_LOGW(TAG, "user key IRQ unavailable; polling fallback: %s", esp_err_to_name(key_irq));
    ESP_LOGI(TAG, "screen timeout=%lu s brightness=%u%%",
             (unsigned long)manager->timeout_seconds, manager->brightness);
    return ESP_OK;
}

esp_err_t app_power_set_timeout(app_power_t *manager, uint32_t seconds,
                                bool persist)
{
    ESP_RETURN_ON_FALSE(manager && seconds <= 3600, ESP_ERR_INVALID_ARG, TAG,
                        "timeout must be 0..3600 seconds");
    portENTER_CRITICAL(&s_settings_lock);
    manager->timeout_seconds = seconds;
    if (persist) {
        manager->persist_pending |= PERSIST_TIMEOUT;
        manager->persistence_error = ESP_ERR_NOT_FINISHED;
    }
    ++manager->settings_revision;
    portEXIT_CRITICAL(&s_settings_lock);
    if (manager->task) xTaskNotifyGive((TaskHandle_t)manager->task);
    return ESP_OK;
}

uint32_t app_power_get_timeout(const app_power_t *manager)
{
    portENTER_CRITICAL(&s_settings_lock);
    const uint32_t value = manager ? manager->timeout_seconds : 0;
    portEXIT_CRITICAL(&s_settings_lock);
    return value;
}

esp_err_t app_power_set_brightness(app_power_t *manager, uint8_t percent,
                                   bool persist)
{
    ESP_RETURN_ON_FALSE(manager && percent <= 100, ESP_ERR_INVALID_ARG, TAG,
                        "brightness must be 0..100");
    portENTER_CRITICAL(&s_settings_lock);
    manager->brightness = percent;
    if (persist) {
        manager->persist_pending |= PERSIST_BRIGHTNESS;
        manager->persistence_error = ESP_ERR_NOT_FINISHED;
    }
    ++manager->settings_revision;
    portEXIT_CRITICAL(&s_settings_lock);
    if (manager->task) xTaskNotifyGive((TaskHandle_t)manager->task);
    return ESP_OK;
}

uint8_t app_power_get_brightness(const app_power_t *manager)
{
    portENTER_CRITICAL(&s_settings_lock);
    const uint8_t value = manager ? manager->brightness : 0;
    portEXIT_CRITICAL(&s_settings_lock);
    return value;
}

esp_err_t app_power_get_status(const app_power_t *manager, app_power_status_t *status)
{
    if (!manager || !status) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_settings_lock);
    *status = (app_power_status_t) {
        .persistence_pending = manager->persist_pending != 0,
        .sleeping = manager->sleeping,
        .persistence_error = manager->persistence_error,
        .display_error = manager->display_error,
        .persistence_deferred = manager->persistence_deferred,
        .shutdown_state = manager->shutdown_state,
        .shutdown_stage = manager->shutdown_stage,
        .shutdown_error = manager->shutdown_error,
        .sensor_restore_error = manager->sensor_restore_error,
        .shutdown_recovering = manager->shutdown_cancel_error != ESP_OK || manager->sensor_restore_error != ESP_OK,
        .shutdown_cancel_error = manager->shutdown_cancel_error,
        .cleanup_timed_out = manager->cleanup_timed_out,
    };
    portEXIT_CRITICAL(&s_settings_lock);
    return ESP_OK;
}

esp_err_t app_power_set_shutdown_hook(app_power_t *manager,
    app_power_shutdown_hook_t hook, void *context)
{
    if (!manager) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_settings_lock);
    if (manager->shutdown_state == APP_POWER_SHUTDOWN_PREPARING ||
        manager->shutdown_state == APP_POWER_SHUTDOWN_REQUESTED || manager->shutdown_cancel_error != ESP_OK) {
        portEXIT_CRITICAL(&s_settings_lock);
        return ESP_ERR_INVALID_STATE;
    }
    manager->shutdown_hook = hook;
    manager->shutdown_context = context;
    portEXIT_CRITICAL(&s_settings_lock);
    return ESP_OK;
}

void app_power_set_persistence_guard(app_power_t *manager, bool (*busy)(void *), void *context)
{
    if (!manager) return;
    portENTER_CRITICAL(&s_settings_lock);
    manager->persistence_busy = busy;
    manager->persistence_busy_context = context;
    portEXIT_CRITICAL(&s_settings_lock);
    if (manager->task) xTaskNotifyGive((TaskHandle_t)manager->task);
}

bool app_power_is_sleeping(const app_power_t *manager)
{
    portENTER_CRITICAL(&s_settings_lock);
    const bool sleeping = manager && (manager->sleeping || manager->sleep_requested);
    portEXIT_CRITICAL(&s_settings_lock);
    return sleeping;
}

esp_err_t app_power_sleep_now(app_power_t *manager)
{
    ESP_RETURN_ON_FALSE(manager && manager->task, ESP_ERR_INVALID_ARG, TAG,
                        "invalid manager");
    portENTER_CRITICAL(&s_settings_lock);
    manager->sleep_requested = true;
    manager->wake_requested = false;
    ++manager->request_revision;
    portEXIT_CRITICAL(&s_settings_lock);
    xTaskNotifyGive((TaskHandle_t)manager->task);
    return ESP_OK;
}

esp_err_t app_power_wake(app_power_t *manager)
{
    ESP_RETURN_ON_FALSE(manager && manager->task, ESP_ERR_INVALID_ARG, TAG,
                        "invalid manager");
    portENTER_CRITICAL(&s_settings_lock);
    manager->wake_requested = true;
    manager->sleep_requested = false;
    ++manager->request_revision;
    portEXIT_CRITICAL(&s_settings_lock);
    xTaskNotifyGive((TaskHandle_t)manager->task);
    return ESP_OK;
}

esp_err_t app_power_set_user_key_handler(app_power_t *manager, void (*handler)(void *, int64_t), void *context)
{
    if (!manager || !manager->task) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_settings_lock);
    manager->user_key_handler = handler; manager->user_key_context = context;
    manager->user_key_irq_pending = false;
    portEXIT_CRITICAL(&s_settings_lock);
    return ESP_OK;
}

esp_err_t app_power_set_user_key_tap_handler(app_power_t *manager, void (*handler)(void *, int64_t))
{
    if (!manager || !manager->task) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_settings_lock);
    manager->user_key_tap_handler = handler;
    portEXIT_CRITICAL(&s_settings_lock);
    return ESP_OK;
}
