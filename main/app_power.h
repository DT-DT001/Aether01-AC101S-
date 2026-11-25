#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "aether_board.h"
#include "esp_err.h"
#include "lvgl.h"

typedef enum {
    APP_POWER_SHUTDOWN_IDLE,
    APP_POWER_SHUTDOWN_PREPARING,
    APP_POWER_SHUTDOWN_FAILED,
    APP_POWER_SHUTDOWN_REQUESTED,
} app_power_shutdown_state_t;

typedef enum {
    APP_POWER_SHUTDOWN_BEGIN,
    APP_POWER_SHUTDOWN_QUIESCE,
    APP_POWER_SHUTDOWN_SENSORS,
    APP_POWER_SHUTDOWN_FINALIZE,
    APP_POWER_SHUTDOWN_PERSIST,
    APP_POWER_SHUTDOWN_POWER_OFF,
    APP_POWER_SHUTDOWN_CANCEL,
} app_power_shutdown_stage_t;

/** Called by the power task without the LVGL lock. BEGIN must gate new work
 * and request cancellation promptly; FINALIZE must finish file/storage cleanup
 * within timeout_ms. CANCEL restores the gate/storage after an aborted attempt.
 * POWER_OFF runs after persistence and before requesting PMIC power-off. No stage may
 * wait indefinitely. A missing hook deliberately prevents software power-off. */
typedef esp_err_t (*app_power_shutdown_hook_t)(void *context,
    app_power_shutdown_stage_t stage, uint32_t timeout_ms);

typedef struct {
    aether_board_t *board;
    lv_display_t *display;
    void *task;
    bool sleeping;
    bool dimmed;
    bool apps_suspended;
    bool render_paused; /* Own exactly one LVGL invalidation-disable reference. */
    bool key_irq_enabled;
    bool user_key_irq_enabled, power_key_irq_enabled;
    bool user_key_irq_pending, user_key_irq_seen;
    uint32_t user_key_irq_tick;
    int64_t user_key_irq_at_us;
    void (*user_key_handler)(void *, int64_t);
    void (*user_key_press_handler)(void *, int64_t);
    void *user_key_context;
    void (*screen_hook)(void *, bool);
    void *screen_hook_context;
    void *awake_pm_lock;
    bool awake_pm_lock_held;
    bool (*standby_audio_busy)(void *);
    void *standby_audio_context;
    uint32_t timeout_seconds;
    uint8_t brightness;
    uint8_t applied_brightness;
    bool sleep_requested;
    bool wake_requested;
    uint8_t persist_pending;
    uint32_t settings_revision;
    uint32_t request_revision;
    esp_err_t persistence_error;
    esp_err_t display_error;
    bool persistence_deferred;
    bool (*persistence_busy)(void *);
    void *persistence_busy_context;
    app_power_shutdown_hook_t shutdown_hook;
    void *shutdown_context;
    app_power_shutdown_state_t shutdown_state;
    app_power_shutdown_stage_t shutdown_stage;
    esp_err_t shutdown_error;
    esp_err_t sensor_restore_error;
    esp_err_t shutdown_cancel_error; /* Non-OK retains the hook context for bounded retry. */
    bool cleanup_timed_out; /* Noncritical cleanup/persistence skipped by policy. */
} app_power_t;

typedef struct {
    bool persistence_pending;
    bool sleeping;
    esp_err_t persistence_error;
    esp_err_t display_error;
    bool persistence_deferred;
    app_power_shutdown_state_t shutdown_state;
    app_power_shutdown_stage_t shutdown_stage; /* Failed/current preparation step. */
    esp_err_t shutdown_error;
    esp_err_t sensor_restore_error;
    bool shutdown_recovering;
    esp_err_t shutdown_cancel_error;
    bool cleanup_timed_out;
} app_power_status_t;

esp_err_t app_power_init(app_power_t *manager, aether_board_t *board,
                         lv_display_t *display);
esp_err_t app_power_set_timeout(app_power_t *manager, uint32_t seconds,
                                bool persist);
uint32_t app_power_get_timeout(const app_power_t *manager);
esp_err_t app_power_set_brightness(app_power_t *manager, uint8_t percent,
                                   bool persist);
uint8_t app_power_get_brightness(const app_power_t *manager);
/** Snapshot of the actual asynchronous persistence/display result. */
esp_err_t app_power_get_status(const app_power_t *manager, app_power_status_t *status);
void app_power_set_persistence_guard(app_power_t *manager, bool (*busy)(void *), void *context);
/** Keep CPU and codec clocks available for a live player while the panel is off. */
void app_power_set_standby_audio_guard(app_power_t *manager, bool (*busy)(void *), void *context);
/** Install once during initialization; replacing an active hook is rejected. */
esp_err_t app_power_set_shutdown_hook(app_power_t *manager,
    app_power_shutdown_hook_t hook, void *context);
/** Called under the LVGL lock once per display transition. Callback must
 * return promptly and leave asynchronous cleanup to the relevant owners. */
void app_power_set_screen_hook(app_power_t *manager, void (*hook)(void *, bool), void *context);
/** True while asleep or a sleep request is pending; safe from the input task. */
bool app_power_is_sleeping(const app_power_t *manager);
/** Enqueue a transition; panel I/O runs in the power task. */
esp_err_t app_power_sleep_now(app_power_t *manager);
esp_err_t app_power_wake(app_power_t *manager);

/* BOOT/user-key held for two seconds while awake. The power task passes the
 * original press timestamp so a new page cannot inherit an earlier hold.
 * Presses while asleep are consumed. Only the TG29 key handles button wake. */
esp_err_t app_power_set_user_key_handler(app_power_t *manager, void (*handler)(void *, int64_t), void *context);
/* Optional down-edge action shares the user-key handler context. An awake,
 * unblocked press invokes it once while the key is still down. The regular
 * two-second hold still dispatches independently. */
esp_err_t app_power_set_user_key_press_handler(app_power_t *manager, void (*handler)(void *, int64_t));
