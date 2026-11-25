#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "../../main/app_power.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %u: %s\n", __LINE__, #c); return 1; } } while (0)

static uint32_t clock_ms;
static unsigned tick_jitter;
esp_err_t tg29_get_cc_events(tg29_handle_t handle, uint8_t *events)
{ (void)handle; *events=0; return ESP_OK; }
static unsigned sleeps, wakes, idle_audio_calls;
static uint8_t applied_brightness;
static esp_err_t sleep_error, wake_error;
static esp_err_t brightness_error, nvs_open_error, nvs_commit_error;
static unsigned commits;
static bool replace_during_commit;
static bool load_zero_brightness;
static app_power_t manager;
static aether_board_t board;
static lv_display_t display;
static jmp_buf stop_loop;
static unsigned loop_number, loop_limit;
static void (*iteration_hook)(unsigned);
static void (*lock_hook)(void);
static bool lock_available, gui_touch_enabled;
static int boot_level, touch_level, pmic_level;
static uint8_t key_events;
static unsigned key_reads, key_configures, consumed_gestures;
static esp_err_t key_setup_error, key_read_error;
static bool key_ack_sticky;
static bool pending_before_unlock;
static bool media_busy, deferred_without_commit;
static uint8_t key_script[40];
static unsigned key_error_until, key_unlock_at, key_clock_jump_at;
static uint32_t key_clock_jump_ms;
static bool expect_pending_sleep;
static unsigned power_off_calls, quiesces, resumes, qmi_sleeps, dps_sleeps;
static unsigned qmi_restores, dps_restores;
static esp_err_t quiesce_error, qmi_sleep_error, dps_sleep_error, power_off_error, restore_error;
static esp_err_t hook_begin_error, hook_finish_error, hook_cancel_error;
static unsigned hook_begin_calls, hook_finish_calls, hook_cancel_calls;
static uint32_t hook_finish_delay, cancel_budget_max;
static char shutdown_order[64];
static unsigned shutdown_order_length;
static esp_pm_config_t pm_config;
static bool awake_clock_locked, clock_at_panel_wake;

esp_err_t esp_pm_lock_create(int type, int argument, const char *name, esp_pm_lock_handle_t *lock)
{
    (void)argument;
    (void)name;
    if (type != ESP_PM_CPU_FREQ_MAX) return ESP_ERR_INVALID_ARG;
    *lock = &awake_clock_locked;
    return ESP_OK;
}
esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t lock) { (void)lock; return ESP_OK; }
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock) { *(bool *)lock = true; return ESP_OK; }
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock) { *(bool *)lock = false; return ESP_OK; }
esp_err_t esp_pm_configure(const esp_pm_config_t *config) { pm_config = *config; return ESP_OK; }
esp_err_t lvgl_port_stop(void) { return ESP_OK; }
esp_err_t lvgl_port_resume(void) { return ESP_OK; }
esp_err_t gpio_intr_disable(int pin) { return pin == AETHER_GPIO_BOOT_KEY ? ESP_OK : ESP_FAIL; }
esp_err_t ac101s_audio_idle_clocks(void *audio) { (void)audio; ++idle_audio_calls; return ESP_OK; }
uint32_t __real_lv_timer_handler(void) { return 7; }

static void step(char c) { shutdown_order[shutdown_order_length++] = c; }

esp_err_t app_telemetry_quiesce(uint32_t timeout)
{ (void)timeout; ++quiesces; step('Q'); return quiesce_error; }
void app_telemetry_resume(void) { ++resumes; step('R'); }
esp_err_t qmi8658a_set_sleep(qmi8658a_handle_t handle, bool sleep)
{ (void)handle; if (sleep) { ++qmi_sleeps; step('I'); return qmi_sleep_error; }
  ++qmi_restores; step('i'); return restore_error; }
esp_err_t dps368_set_sleep(dps368_handle_t handle, bool sleep)
{ (void)handle; if (sleep) { ++dps_sleeps; step('D'); return dps_sleep_error; }
  ++dps_restores; step('d'); return restore_error; }
esp_err_t tg29_request_power_off(tg29_handle_t handle)
{ (void)handle; ++power_off_calls; step('O'); return power_off_error; }
static esp_err_t shutdown_hook(void *ctx, app_power_shutdown_stage_t stage, uint32_t timeout)
{
    (void)ctx;
    if (stage == APP_POWER_SHUTDOWN_BEGIN) { ++hook_begin_calls; step('B'); return hook_begin_error; }
    if (stage == APP_POWER_SHUTDOWN_FINALIZE) {
        ++hook_finish_calls; step('F');
        if (timeout > POWER_CLEANUP_WAIT_MS) return ESP_FAIL;
        clock_ms += hook_finish_delay;
        return hook_finish_error;
    }
    if (stage == APP_POWER_SHUTDOWN_POWER_OFF) { step('P'); return ESP_OK; }
    ++hook_cancel_calls; if(timeout>cancel_budget_max)cancel_budget_max=timeout; step('C'); return stage == APP_POWER_SHUTDOWN_CANCEL ? hook_cancel_error : ESP_FAIL;
}

esp_err_t aether_board_set_display_sleep(aether_board_t *target, bool sleeping)
{
    if (sleeping) { ++sleeps; target->display_sleeping = true; return sleep_error; }
    ++wakes;
    clock_at_panel_wake = awake_clock_locked;
    target->display_sleeping = wake_error != ESP_OK;
    return wake_error;
}
esp_err_t aether_board_set_display_brightness(aether_board_t *target, uint8_t value)
{ (void)target; applied_brightness = value; return brightness_error; }
void lv_display_enable_invalidation(lv_display_t *target, bool enabled)
{ target->invalidation_count += enabled ? 1 : -1; target->invalidation = target->invalidation_count > 0; }
lv_timer_t *lv_display_get_refr_timer(lv_display_t *target) { return &target->refresh; }
void lv_timer_pause(lv_timer_t *timer) { timer->paused = true; }
void lv_timer_resume(lv_timer_t *timer) { timer->paused = false; }
void lv_timer_ready(lv_timer_t *timer) { ++timer->ready_calls; }
bool app_touch_wake_enabled(void) { return gui_touch_enabled; }
void app_touch_consume_wake_gesture(void) { ++consumed_gestures; }
esp_err_t tg29_configure_key_wakeup(tg29_handle_t pmic)
{ (void)pmic; ++key_configures; return key_setup_error; }
esp_err_t tg29_get_key_events(tg29_handle_t pmic, uint8_t *events)
{
    (void)pmic;
    ++key_reads;
    *events = key_events;
    /* A real TG29 acknowledgement error returns the observed bits while
     * their RW1C latches remain asserted for a later retry. */
    if (!key_ack_sticky || key_read_error == ESP_OK) key_events = 0;
    return key_read_error;
}
void lv_obj_invalidate(lv_obj_t *object) { ++object->invalidations; }
lv_obj_t *lv_display_get_screen_active(lv_display_t *target) { return &target->screen; }
void lv_display_trigger_activity(lv_display_t *target) { target->last_activity = clock_ms; }
uint32_t lv_display_get_inactive_time(lv_display_t *target) { return clock_ms - target->last_activity; }
uint32_t lv_tick_get(void) { const uint32_t tick = clock_ms; if (tick_jitter) { --tick_jitter; ++clock_ms; } return tick; }
bool lvgl_port_lock(uint32_t timeout) { (void)timeout; if (lock_hook) { void (*hook)(void) = lock_hook; lock_hook = NULL; hook(); } return lock_available; }
void lvgl_port_unlock(void) {}
int gpio_get_level(int gpio) { return gpio == AETHER_GPIO_BOOT_KEY ? boot_level : gpio == AETHER_GPIO_TOUCH_INT ? touch_level : pmic_level; }
TickType_t xTaskGetTickCount(void) { return clock_ms; }
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear;
    if (loop_number == loop_limit) longjmp(stop_loop, 1);
    clock_ms += wait;
    ++loop_number;
    if (iteration_hook) iteration_hook(loop_number);
    return 0;
}
BaseType_t xTaskCreate(void (*function)(void *), const char *name, unsigned stack, void *arg,
                       unsigned priority, TaskHandle_t *task)
{ (void)function; (void)name; (void)stack; (void)arg; (void)priority; if (task) *task = &manager; return pdPASS; }
void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)name; (void)mode; *handle = nvs_open_error == ESP_OK ? 1 : 0; return nvs_open_error; }
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value)
{
    (void)handle;
    if (load_zero_brightness && strcmp(key, POWER_NVS_BRIGHT) == 0) {
        *value = 0;
        return ESP_OK;
    }
    return ESP_FAIL;
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{ (void)handle; (void)key; (void)value; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    ++commits;
    if (replace_during_commit) {
        replace_during_commit = false;
        app_power_set_brightness(&manager, 60, true);
    }
    return nvs_commit_error;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }

static void reset(void)
{
    memset(&manager, 0, sizeof(manager));
    memset(&board, 0, sizeof(board));
    memset(&display, 0, sizeof(display));
    manager.board = &board;
    manager.display = &display;
    manager.task = &manager;
    manager.brightness = 80;
    manager.applied_brightness = UINT8_MAX;
    manager.timeout_seconds = 30;
    display.invalidation_count = 1;
    display.invalidation = true;
    lock_available = gui_touch_enabled = true;
    boot_level = touch_level = pmic_level = 1;
    key_events = key_reads = key_configures = consumed_gestures = 0;
    key_setup_error = key_read_error = ESP_OK;
    key_ack_sticky = false;
    pending_before_unlock = false;
    media_busy = deferred_without_commit = false;
    clock_ms = 0;
    tick_jitter = sleeps = wakes = idle_audio_calls = loop_number = 0;
    applied_brightness = 0;
    sleep_error = wake_error = ESP_OK;
    brightness_error = nvs_open_error = nvs_commit_error = ESP_OK;
    commits = 0;
    replace_during_commit = false;
    load_zero_brightness = false;
    iteration_hook = NULL;
    lock_hook = NULL;
    memset(key_script, 0, sizeof(key_script));
    key_error_until = key_unlock_at = key_clock_jump_at = 0;
    key_clock_jump_ms = 0;
    expect_pending_sleep = false;
    power_off_calls = quiesces = resumes = qmi_sleeps = dps_sleeps = 0;
    qmi_restores = dps_restores = 0;
    quiesce_error = qmi_sleep_error = dps_sleep_error = power_off_error = restore_error = ESP_OK;
    hook_begin_error = hook_finish_error = hook_cancel_error = ESP_OK;
    hook_begin_calls = hook_finish_calls = hook_cancel_calls = 0;
    hook_finish_delay = cancel_budget_max = 0;
    memset(shutdown_order, 0, sizeof(shutdown_order)); shutdown_order_length = 0;
    pm_config = (esp_pm_config_t){.light_sleep_enable = true};
    awake_clock_locked = clock_at_panel_wake = false;
    atomic_store(&s_gui_asleep, false);
}

static int test_standby_clock_policy(void)
{
    reset();
    CHECK(app_power_init(&manager, &board, &display) == ESP_OK);
    CHECK(awake_clock_locked && pm_config.min_freq_mhz == CONFIG_XTAL_FREQ);
    CHECK(pm_config.max_freq_mhz == CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ && !pm_config.light_sleep_enable);
    CHECK(apply_sleep(&manager) == ESP_OK && __wrap_lv_timer_handler() == LV_NO_TIMER_READY);
    set_screen_clock(&manager, false);
    CHECK(!awake_clock_locked && !pm_config.light_sleep_enable);
    wake_error = ESP_FAIL;
    CHECK(apply_wake(&manager) == ESP_FAIL && clock_at_panel_wake && manager.apps_suspended);
    set_screen_clock(&manager, false);
    wake_error = ESP_OK;
    CHECK(apply_wake(&manager) == ESP_OK && clock_at_panel_wake && awake_clock_locked);
    CHECK(!manager.apps_suspended && __wrap_lv_timer_handler() == 7);
    puts("PASS production standby keeps automatic MCU sleep disabled and raises the CPU clock before every panel wake attempt");
    return 0;
}

static int test_software_shutdown(void)
{
    reset();
    power_key_t key = {0};
    handle_key_events(&manager, &key, TG29_KEY_EVENT_LONG_PRESS, ESP_OK);
    clock_ms = 4000;
    handle_key_events(&manager, &key, 0, ESP_OK);
    CHECK(!key.shutdown_due && !key.hold_active);
    key = (power_key_t){0}; clock_ms = 0;
    handle_key_events(&manager, &key, TG29_KEY_EVENT_PRESS, ESP_OK);
    clock_ms = 2999;
    handle_key_events(&manager, &key, TG29_KEY_EVENT_LONG_PRESS, ESP_OK);
    CHECK(!key.shutdown_due);
    clock_ms = 3000;
    handle_key_events(&manager, &key, 0, ESP_OK);
    CHECK(key.shutdown_due && !key.hold_active && key.consume_short);
    handle_key_events(&manager, &key, TG29_KEY_EVENT_SHORT_PRESS | TG29_KEY_EVENT_RELEASE, ESP_OK);
    CHECK(!key.shutdown_due && !manager.sleep_requested);
    puts("PASS software hold requires a confirmed PRESS for 3000ms; LONG alone and release tails cannot trigger/toggle");

    const uint8_t cancellations[] = {TG29_KEY_EVENT_RELEASE, TG29_KEY_EVENT_SHORT_PRESS,
        TG29_KEY_EVENT_PRESS | TG29_KEY_EVENT_RELEASE};
    for (unsigned i = 0; i < sizeof(cancellations); ++i) {
        reset(); key = (power_key_t){0};
        handle_key_events(&manager, &key, TG29_KEY_EVENT_PRESS, ESP_OK);
        clock_ms = 2900;
        handle_key_events(&manager, &key, cancellations[i], ESP_OK);
        clock_ms = 5000;
        handle_key_events(&manager, &key, 0, ESP_OK);
        CHECK(!key.shutdown_due && !key.hold_active);
    }
    for (unsigned error = 0; error < 2; ++error) {
        reset(); key = (power_key_t){0};
        handle_key_events(&manager, &key, TG29_KEY_EVENT_PRESS, error ? ESP_FAIL : ESP_OK);
        clock_ms = 2900;
        handle_key_events(&manager, &key, error ? TG29_KEY_EVENT_PRESS : 0, ESP_FAIL);
        clock_ms = 5000;
        handle_key_events(&manager, &key, 0, ESP_OK);
        CHECK(!key.shutdown_due && !key.hold_active);
    }
    reset(); key = (power_key_t){0}; clock_ms = UINT32_MAX - 2000;
    handle_key_events(&manager, &key, TG29_KEY_EVENT_PRESS, ESP_OK);
    clock_ms += 3000;
    handle_key_events(&manager, &key, 0, ESP_OK);
    CHECK(key.shutdown_due);
    puts("PASS release, coalesced edges, I2C/ACK errors abort hold; tick wrap preserves 3s threshold");
    reset(); key = (power_key_t){0};
    handle_key_events(&manager, &key, TG29_KEY_EVENT_PRESS | TG29_KEY_EVENT_LONG_PRESS, ESP_OK);
    clock_ms = 4000; handle_key_events(&manager, &key, 0, ESP_OK);
    CHECK(!key.hold_active && !key.shutdown_due);
    reset(); key = (power_key_t){0};
    handle_key_events(&manager, &key, TG29_KEY_EVENT_PRESS, ESP_OK);
    clock_ms = 999; handle_key_events(&manager, &key, TG29_KEY_EVENT_LONG_PRESS, ESP_OK);
    clock_ms = 4000; handle_key_events(&manager, &key, 0, ESP_OK);
    CHECK(!key.hold_active && !key.shutdown_due);
    puts("PASS aged PRESS|LONG and abnormally early LONG retain hardware fallback without unsafe cleanup-time assumptions");

    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    prepare_shutdown(&manager);
    CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_REQUESTED && manager.shutdown_error == ESP_OK);
    CHECK(!strcmp(shutdown_order, "BQIDFPO") && power_off_calls == 1 && !resumes && !hook_cancel_calls);
    CHECK(app_power_set_shutdown_hook(&manager, NULL, NULL) == ESP_ERR_INVALID_STATE);
    puts("PASS shutdown order gates media, drains sampler, verifies both sensors, closes files then commands PMIC");

    for (unsigned failure = 0; failure < 7; ++failure) {
        reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
        CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
        if (failure == 0) hook_begin_error = ESP_ERR_INVALID_STATE; /* USB owns TF. */
        if (failure == 1) quiesce_error = ESP_ERR_TIMEOUT;
        if (failure == 2) qmi_sleep_error = ESP_FAIL;
        if (failure == 3) dps_sleep_error = ESP_FAIL;
        if (failure == 4) hook_finish_error = ESP_FAIL;
        if (failure == 5) hook_finish_delay = POWER_SHUTDOWN_BUDGET_MS;
        if (failure == 6) power_off_error = ESP_FAIL;
        prepare_shutdown(&manager);
        CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_FAILED && manager.shutdown_error != ESP_OK);
        CHECK(power_off_calls == (failure == 6 ? 1U : 0U) && hook_cancel_calls == 1);
        CHECK(resumes == (failure == 0 ? 0U : 1U));
        CHECK(qmi_restores == (failure >= 2 ? 1U : 0U));
    }
    reset(); prepare_shutdown(&manager);
    CHECK(manager.shutdown_error == ESP_ERR_INVALID_STATE && !power_off_calls);
    reset(); board.pmic = &board; board.imu_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    prepare_shutdown(&manager);
    CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_FAILED && !qmi_sleeps && !power_off_calls && resumes == 1);
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    hook_finish_error = ESP_FAIL; restore_error = ESP_FAIL;
    prepare_shutdown(&manager);
    CHECK(manager.sensor_restore_error == ESP_FAIL && !resumes && !power_off_calls && hook_cancel_calls == 1);
    app_power_status_t status;
    CHECK(app_power_get_status(&manager, &status) == ESP_OK && status.shutdown_state == APP_POWER_SHUTDOWN_FAILED &&
          status.shutdown_stage == APP_POWER_SHUTDOWN_FINALIZE && status.shutdown_error == ESP_FAIL && status.sensor_restore_error == ESP_FAIL);
    puts("PASS critical preparation/USB/sensor/PMIC failures and absolute-budget overrun retain power with rollback and cached errors");
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    hook_finish_error = ESP_ERR_TIMEOUT; hook_finish_delay = 500;
    app_power_set_timeout(&manager, 60, true); app_power_set_brightness(&manager, 50, true);
    prepare_shutdown(&manager);
    CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_REQUESTED && manager.cleanup_timed_out && power_off_calls == 1);
    CHECK(!resumes && !qmi_restores && !dps_restores && !hook_cancel_calls && clock_ms == 500);
    CHECK(qmi_sleeps == 1 && dps_sleeps == 1 && !strcmp(shutdown_order, "BQIDFPO"));
    CHECK(commits == 0 && manager.persist_pending == (PERSIST_TIMEOUT | PERSIST_BRIGHTNESS));
    for (unsigned sensor = 0; sensor < 2; ++sensor) {
        reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
        CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
        if (sensor) dps_sleep_error = ESP_ERR_TIMEOUT; else qmi_sleep_error = ESP_ERR_TIMEOUT;
        prepare_shutdown(&manager);
        CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_FAILED && !power_off_calls && resumes == 1 && hook_cancel_calls == 1);
    }
    puts("PASS noncritical 500ms cleanup timeout proceeds to power-off without waking sensors; sensor timeouts still retain power");
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    hook_begin_error = ESP_ERR_TIMEOUT; hook_finish_error = ESP_ERR_INVALID_STATE;
    app_power_set_timeout(&manager, 60, true);
    prepare_shutdown(&manager);
    CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_REQUESTED && manager.cleanup_timed_out && power_off_calls == 1);
    CHECK(!resumes && !qmi_restores && !dps_restores && !hook_cancel_calls && qmi_sleeps == 1 && dps_sleeps == 1);
    CHECK(!strcmp(shutdown_order, "BQIDFPO"));
    CHECK(commits == 0 && manager.persist_pending == PERSIST_TIMEOUT);
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    hook_begin_error = ESP_ERR_TIMEOUT; quiesce_error = ESP_ERR_TIMEOUT;
    prepare_shutdown(&manager);
    CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_FAILED && !power_off_calls && hook_cancel_calls == 1);
    puts("PASS general BEGIN timeout cannot skip mandatory sensor quiesce/sleep; an unestablished cleanup gate never wakes verified sleeping sensors");
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    app_power_set_timeout(&manager, 60, true); app_power_set_brightness(&manager, 50, true);
    prepare_shutdown(&manager);
    CHECK(commits == 2 && !manager.persist_pending && power_off_calls == 1);
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    app_power_set_timeout(&manager, 60, true); nvs_commit_error = ESP_FAIL;
    prepare_shutdown(&manager);
    CHECK(manager.shutdown_stage == APP_POWER_SHUTDOWN_PERSIST && manager.persist_pending && !power_off_calls && resumes == 1);
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    app_power_set_timeout(&manager, 60, true); nvs_commit_error = ESP_ERR_TIMEOUT;
    prepare_shutdown(&manager);
    CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_REQUESTED && manager.persist_pending && power_off_calls == 1);
    CHECK(manager.cleanup_timed_out && manager.persistence_error == ESP_ERR_TIMEOUT && !resumes && !hook_cancel_calls);
    puts("PASS own NVS runs after file cleanup: real failure retains power, timeout remains pending and skips without sensor resume");
    reset();board.pmic=&board;board.imu_ready=board.barometer_ready=true;
    CHECK(app_power_set_shutdown_hook(&manager,shutdown_hook,NULL)==ESP_OK);
    hook_finish_error=ESP_FAIL;hook_cancel_error=ESP_ERR_INVALID_STATE;prepare_shutdown(&manager);
    CHECK(manager.shutdown_cancel_error==ESP_ERR_INVALID_STATE&&hook_cancel_calls==1&&!power_off_calls);
    CHECK(app_power_get_status(&manager,&status)==ESP_OK&&status.shutdown_recovering&&status.shutdown_cancel_error==ESP_ERR_INVALID_STATE);
    CHECK(app_power_set_shutdown_hook(&manager,shutdown_hook,NULL)==ESP_ERR_INVALID_STATE);
    const unsigned begins=hook_begin_calls;prepare_shutdown(&manager);CHECK(hook_begin_calls==begins);
    retry_shutdown_cancel(&manager);CHECK(hook_cancel_calls==2&&manager.shutdown_cancel_error==ESP_ERR_INVALID_STATE);
    hook_cancel_error=ESP_ERR_NOT_FINISHED;retry_shutdown_cancel(&manager);CHECK(manager.shutdown_cancel_error==ESP_ERR_NOT_FINISHED);
    hook_cancel_error=ESP_OK;retry_shutdown_cancel(&manager);CHECK(manager.shutdown_cancel_error==ESP_OK);
    CHECK(app_power_get_status(&manager,&status)==ESP_OK&&!status.shutdown_recovering);
    const unsigned cancels=hook_cancel_calls;retry_shutdown_cancel(&manager);CHECK(hook_cancel_calls==cancels);
    puts("PASS aborted shutdown retains failed CANCEL for bounded retry, blocks replacing its context or a second shutdown, and publishes recovery until acknowledgment");
    return 0;
}

static void run_worker(unsigned iterations)
{
    loop_limit = iterations;
    if (!setjmp(stop_loop)) power_task(&manager);
}

static void unlock_second_iteration(unsigned iteration)
{
    if (iteration == 2) {
        pending_before_unlock = manager.wake_requested && wakes == 0;
        lock_available = true;
    }
}

static void scripted_key_events(unsigned iteration)
{
    if (iteration <= sizeof(key_script)) key_events |= key_script[iteration - 1];
    key_read_error = iteration <= key_error_until ? ESP_FAIL : ESP_OK;
    if (iteration == key_clock_jump_at) clock_ms += key_clock_jump_ms;
    if (iteration == key_unlock_at) {
        pending_before_unlock = (expect_pending_sleep ? manager.sleep_requested : manager.wake_requested) &&
                                (expect_pending_sleep ? sleeps == 0 : wakes == 0);
        lock_available = true;
    }
    pmic_level = key_events ? 0 : 1;
}

static void quick_touch(unsigned iteration)
{
    if (iteration == 1) display.last_activity = 95; /* Manual sleep at inactivity 5ms. */
    if (iteration == 2) display.last_activity = 150; /* Next sample is inactivity 50ms. */
}

static void change_settings_while_locking(void)
{
    app_power_set_timeout(&manager, 0, false);
    app_power_set_brightness(&manager, 40, false);
}

static unsigned sleeps_before_retry;
static void retry_automatic_sleep(unsigned iteration)
{
    if (iteration == 20) sleeps_before_retry = sleeps;
    if (iteration == 21) sleep_error = ESP_OK;
}

static void cancel_sleep_retry_with_wake(unsigned iteration)
{
    if (iteration == 2) {
        app_power_set_timeout(&manager, 0, false);
        app_power_wake(&manager);
    }
}

static void supersede_failed_touch_wake(unsigned iteration)
{
    if (iteration == 2) display.last_activity = 150;
    if (iteration == 3) {
        display.last_activity = 250; /* The click that explicitly requests sleep is not a wake. */
        wake_error = ESP_OK;
        app_power_sleep_now(&manager);
    }
}

static void recover_persistence(unsigned iteration)
{
    if (iteration == 5) nvs_commit_error = ESP_OK;
}

static void recover_brightness(unsigned iteration)
{
    if (iteration == 5) brightness_error = ESP_OK;
}

static bool media_persistence_busy(void *context)
{
    return *(bool *)context;
}

static int test_standby_audio_guard(void)
{
    for (unsigned manual = 0; manual < 2; ++manual) {
        reset();
        CHECK(app_power_init(&manager, &board, &display) == ESP_OK);
        board.audio_ready = true;
        board.audio = &board;
        media_busy = true;
        app_power_set_standby_audio_guard(&manager, media_persistence_busy, &media_busy);
        if (manual) CHECK(app_power_sleep_now(&manager) == ESP_OK);
        else CHECK(app_power_set_timeout(&manager, 1, false) == ESP_OK);
        run_worker(25);
        CHECK(sleeps == 1 && manager.sleeping && awake_clock_locked && !idle_audio_calls);
        media_busy = false;
        run_worker(loop_number + 2);
        CHECK(manager.sleeping && !awake_clock_locked && idle_audio_calls > 0);
        CHECK(app_power_wake(&manager) == ESP_OK);
        run_worker(loop_number + 2);
        CHECK(wakes == 1 && !manager.sleeping && awake_clock_locked);
    }
    puts("PASS manual and timed standby preserve the clock and skip idle I2S until background playback ends");
    return 0;
}

static void pause_media(unsigned iteration)
{
    if (iteration == 2) {
        app_power_status_t status;
        deferred_without_commit = app_power_get_status(&manager, &status) == ESP_OK &&
            status.persistence_pending && status.persistence_deferred && commits == 0;
        media_busy = false;
    }
}

static void key_fixture(bool sleeping)
{
    reset();
    board.pmic = &board;
    manager.key_irq_enabled = true;
    manager.timeout_seconds = 0;
    if (sleeping) {
        manager.sleeping = board.display_sleeping = true;
        set_render_enabled(&manager, false);
    }
    iteration_hook = scripted_key_events;
}

static int test_power_key_gestures(void)
{
    key_fixture(false);
    key_script[0] = TG29_KEY_EVENT_PRESS;
    run_worker(1);
    CHECK(!sleeps && !wakes && !manager.sleeping);
    CHECK(manager.request_revision == 1 && display.last_activity == clock_ms);
    key_fixture(false);
    manager.timeout_seconds = 1;
    clock_ms = 2000;
    key_script[0] = TG29_KEY_EVENT_PRESS;
    run_worker(1);
    CHECK(!sleeps && !wakes && !manager.sleeping && display.last_activity == clock_ms);
    key_fixture(false);
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_script[1] = TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(3);
    CHECK(sleeps == 1 && !wakes && manager.sleeping && display.invalidation_count == 0);
    CHECK(manager.request_revision == 2); /* Awake PRESS activity plus terminal sleep. */
    /* An isolated SHORT is a valid fallback when its negative edge was lost.
     * Allow an observed clear-poll between two independent short gestures. */
    key_fixture(false);
    key_script[0] = TG29_KEY_EVENT_SHORT_PRESS;
    key_script[7] = TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(9);
    CHECK(sleeps == 1 && wakes == 1 && !manager.sleeping && display.invalidation_count == 1);
    CHECK(manager.request_revision == 2);
    puts("PASS awake PWRON short sleeps, a second independent short wakes, and awake PRESS alone waits for gesture completion");

    key_fixture(true);
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_script[1] = TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(4);
    CHECK(!sleeps && wakes == 1 && !manager.sleeping && manager.request_revision == 1);
    CHECK(display.invalidation_count == 1 && !display.refresh.paused);
    key_fixture(true);
    key_script[0] = TG29_KEY_EVENT_PRESS;
    run_worker(1);
    CHECK(wakes == 1 && !manager.sleeping); /* Do not wait for release to wake. */
    puts("PASS sleeping PWRON press wakes immediately; its later RELEASE|SHORT cannot re-sleep the screen");

    key_fixture(false);
    key_script[0] = TG29_KEY_EVENT_PRESS | TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(2);
    CHECK(sleeps == 1 && !wakes && manager.sleeping && manager.request_revision == 2);
    key_fixture(true);
    key_script[0] = TG29_KEY_EVENT_PRESS | TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(2);
    CHECK(!sleeps && wakes == 1 && !manager.sleeping && manager.request_revision == 1);
    puts("PASS coalesced PRESS|RELEASE|SHORT performs exactly one transition from awake or asleep");

    key_fixture(false);
    key_ack_sticky = true;
    key_error_until = 6;
    key_script[0] = TG29_KEY_EVENT_SHORT_PRESS;
    key_script[10] = TG29_KEY_EVENT_PRESS;
    key_script[11] = TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(14);
    CHECK(key_reads >= 9 && sleeps == 1 && wakes == 1 && !manager.sleeping);
    CHECK(manager.request_revision == 2 && !key_events);
    puts("PASS sticky SHORT across repeated failed ACKs toggles once; successful recovery permits the next genuine gesture");

    key_fixture(false);
    manager.timeout_seconds = 1;
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_clock_jump_at = 2;
    key_clock_jump_ms = 1500;
    key_script[2] = TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(4);
    CHECK(sleeps >= 1 && !wakes && manager.sleeping && display.invalidation_count == 0);
    puts("PASS a press begun awake cannot reverse an intervening idle-timeout sleep when its release/short arrives");

    key_fixture(false);
    lock_available = false;
    expect_pending_sleep = true;
    key_unlock_at = 3;
    key_script[0] = TG29_KEY_EVENT_SHORT_PRESS;
    key_script[1] = TG29_KEY_EVENT_RELEASE;
    run_worker(4);
    CHECK(pending_before_unlock && sleeps == 1 && !wakes && manager.sleeping);
    CHECK(!manager.sleep_requested && manager.request_revision == 1);
    key_fixture(true);
    lock_available = false;
    key_unlock_at = 3;
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_script[1] = TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(4);
    CHECK(pending_before_unlock && !sleeps && wakes == 1 && !manager.sleeping);
    CHECK(!manager.wake_requested && manager.request_revision == 1);
    puts("PASS PWRON sleep/wake requests survive unavailable LVGL mutex without a same-gesture duplicate transition");

    key_fixture(true);
    CHECK(app_power_wake(&manager) == ESP_OK); /* Touch/UI wake has not reached the panel. */
    lock_available = false;
    key_unlock_at = 4;
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_script[1] = TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(5);
    CHECK(pending_before_unlock && !sleeps && wakes == 1 && !manager.sleeping);
    CHECK(manager.request_revision == 2 && display.invalidation_count == 1);
    key_fixture(true);
    CHECK(app_power_wake(&manager) == ESP_OK);
    lock_available = false;
    key_unlock_at = 3;
    key_script[0] = TG29_KEY_EVENT_SHORT_PRESS; /* A missing PRESS must also keep this wake. */
    run_worker(4);
    CHECK(pending_before_unlock && !sleeps && wakes == 1 && !manager.sleeping);
    key_fixture(false);
    CHECK(app_power_sleep_now(&manager) == ESP_OK); /* Pending sleep, physical panel still awake. */
    lock_available = false;
    key_unlock_at = 3;
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_script[1] = TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(4);
    CHECK(pending_before_unlock && !sleeps && !wakes && !manager.sleeping);
    CHECK(!manager.sleep_requested && !manager.wake_requested && manager.request_revision == 2);
    puts("PASS PMIC press/short cannot cancel a queued touch/UI wake while physically asleep; press safely cancels an unapplied explicit sleep");

    key_fixture(false);
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_script[1] = TG29_KEY_EVENT_LONG_PRESS;
    key_script[2] = TG29_KEY_EVENT_RELEASE | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(4);
    CHECK(!sleeps && !wakes && manager.request_revision == 1 && !manager.sleeping);
    key_fixture(false);
    key_script[0] = TG29_KEY_EVENT_PRESS | TG29_KEY_EVENT_LONG_PRESS | TG29_KEY_EVENT_SHORT_PRESS;
    run_worker(2);
    CHECK(!sleeps && !wakes && manager.request_revision == 1 && !manager.sleeping);
    puts("PASS LONG remains a hardware PMIC operation and suppresses a contradictory SHORT in the same gesture");
    return 0;
}

static void recover_cancel_on_fourth(unsigned iteration)
{
    if(iteration==4) hook_cancel_error=ESP_OK;
}

static int test_worker_shutdown(void)
{
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    manager.key_irq_enabled = true; lock_available = false;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_script[15] = TG29_KEY_EVENT_LONG_PRESS;
    iteration_hook = scripted_key_events;
    run_worker(60);
    CHECK(hook_begin_calls == 1 && hook_finish_calls == 1 && power_off_calls == 1 && clock_ms == 6000);
    CHECK(manager.shutdown_state == APP_POWER_SHUTDOWN_REQUESTED && !sleeps && !wakes);
    puts("PASS full power worker performs one 3s shutdown without LVGL mutex and keeps gates closed after command ACK");
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    manager.key_irq_enabled = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    key_script[0] = TG29_KEY_EVENT_PRESS;
    key_script[15] = TG29_KEY_EVENT_LONG_PRESS;
    key_script[30] = TG29_KEY_EVENT_RELEASE; /* Exactly the 3s poll must cancel. */
    iteration_hook = scripted_key_events; run_worker(40);
    CHECK(!power_off_calls && !hook_begin_calls);
    reset(); board.pmic = &board; board.imu_ready = board.barometer_ready = true;
    manager.key_irq_enabled = true;
    CHECK(app_power_set_shutdown_hook(&manager, shutdown_hook, NULL) == ESP_OK);
    key_script[0] = TG29_KEY_EVENT_LONG_PRESS; iteration_hook = scripted_key_events; run_worker(40);
    CHECK(!power_off_calls && !hook_begin_calls);
    puts("PASS full worker cancels on threshold RELEASE and never substitutes the hardware LONG IRQ for held time");
    reset();CHECK(app_power_set_shutdown_hook(&manager,shutdown_hook,NULL)==ESP_OK);
    manager.shutdown_state=APP_POWER_SHUTDOWN_FAILED;manager.shutdown_cancel_error=ESP_ERR_INVALID_STATE;
    hook_cancel_error=ESP_ERR_INVALID_STATE;iteration_hook=recover_cancel_on_fourth;run_worker(6);
    CHECK(hook_cancel_calls==3&&cancel_budget_max==100&&manager.shutdown_cancel_error==ESP_OK&&!hook_begin_calls&&!power_off_calls);
    puts("PASS full power worker retries aborted rollback at bounded200ms cadence with100ms hook budgets and stops immediately after recovery acknowledgment");
    return 0;
}

static unsigned user_actions, user_presses, user_irq_notifies;
static int64_t user_pressed_at, user_down_at;
static void user_action(void *context,int64_t at_us){(void)context;user_pressed_at=at_us; ++user_actions;}
static void user_press(void *context,int64_t at_us){(void)context;user_down_at=at_us;++user_presses;}
TickType_t xTaskGetTickCountFromISR(void){return clock_ms;}
int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
void vTaskNotifyGiveFromISR(TaskHandle_t task,BaseType_t *higher){(void)task;*higher=pdFALSE;++user_irq_notifies;}
esp_err_t gpio_isr_handler_add(int pin,void (*fn)(void*),void *context){(void)fn;(void)context;return pin==61?ESP_OK:ESP_FAIL;}
esp_err_t gpio_install_isr_service(int flags){(void)flags;return ESP_OK;}
esp_err_t gpio_set_intr_type(int pin,int type){return pin==61&&type==GPIO_INTR_NEGEDGE?ESP_OK:ESP_FAIL;}
esp_err_t gpio_intr_enable(int pin){return pin==61?ESP_OK:ESP_FAIL;}
static int test_user_key_dispatch(void)
{
    reset();user_actions=user_presses=user_irq_notifies=0;
    CHECK(app_power_set_user_key_handler(&manager,user_action,&manager)==ESP_OK);
    CHECK(app_power_set_user_key_press_handler(&manager,user_press)==ESP_OK);
    user_key_t key={0};
    clock_ms=100;user_key_interrupt(&manager);clock_ms=105;user_key_interrupt(&manager);
    dispatch_user_key(&manager,false,&key);CHECK(user_actions==0&&user_presses==0&&user_irq_notifies==1);
    clock_ms=200;user_key_interrupt(&manager);clock_ms=203;
    dispatch_user_key(&manager,true,&key);
    CHECK(key.down&&user_presses==1&&user_down_at==200000);
    clock_ms=2199;dispatch_user_key(&manager,true,&key);CHECK(user_actions==0&&user_presses==1);
    clock_ms=2200;dispatch_user_key(&manager,true,&key);CHECK(user_actions==1&&user_pressed_at==200000);
    clock_ms=3200;dispatch_user_key(&manager,true,&key);CHECK(user_actions==1);
    dispatch_user_key(&manager,false,&key);CHECK(user_presses==1);
    manager.sleeping=true;clock_ms=4000;user_key_interrupt(&manager);
    dispatch_user_key(&manager,true,&key);CHECK(user_actions==1&&!manager.wake_requested);
    manager.sleeping=false;manager.wake_requested=false;
    clock_ms=6500;dispatch_user_key(&manager,true,&key);CHECK(user_actions==1&&user_presses==1);
    dispatch_user_key(&manager,false,&key);
    manager.shutdown_cancel_error=ESP_ERR_TIMEOUT;clock_ms=7000;
    dispatch_user_key(&manager,true,&key);
    manager.shutdown_cancel_error=ESP_OK;clock_ms=9500;
    dispatch_user_key(&manager,true,&key);CHECK(user_actions==1&&user_presses==1);
    dispatch_user_key(&manager,false,&key);
    clock_ms=10000;dispatch_user_key(&manager,true,&key);
    CHECK(user_presses==2&&user_down_at==10000000);
    clock_ms=11999;dispatch_user_key(&manager,false,&key);CHECK(user_actions==1&&user_presses==2);
    clock_ms=13000;dispatch_user_key(&manager,true,&key);
    clock_ms=15000;dispatch_user_key(&manager,true,&key);CHECK(user_actions==2&&user_pressed_at==13000000);
    dispatch_user_key(&manager,false,&key);
    CHECK(user_presses==3&&user_down_at==13000000);
    CHECK(app_power_set_user_key_handler(NULL,user_action,NULL)==ESP_ERR_INVALID_ARG);
    puts("PASS awake intercom press edges and two-second exit; a sleeping hold is consumed through wake");
    return 0;
}
int main(void)
{
    CHECK(test_standby_clock_policy() == 0);
    CHECK(test_standby_audio_guard() == 0);
    reset();
    sleep_error = ESP_FAIL;
    CHECK(apply_sleep(&manager) == ESP_FAIL);
    CHECK(manager.sleeping && !display.invalidation);
    CHECK(app_power_is_sleeping(&manager));
    wake_error = ESP_FAIL;
    CHECK(apply_wake(&manager) == ESP_FAIL);
    CHECK(manager.sleeping && !display.invalidation);
    wake_error = ESP_OK;
    clock_ms = 150;
    CHECK(apply_wake(&manager) == ESP_OK);
    CHECK(!manager.sleeping && display.invalidation && display.last_activity == 150);
    clock_ms = 300;
    CHECK(apply_wake(&manager) == ESP_OK); /* Explicit wake while already awake. */
    CHECK(display.last_activity == 300);
    puts("PASS sleep/wake partial-failure state and explicit wake activity");

    reset();
    CHECK(app_power_sleep_now(&manager) == ESP_OK && app_power_is_sleeping(&manager));
    iteration_hook = quick_touch;
    run_worker(2);
    CHECK(sleeps == 1 && wakes == 0 && manager.sleeping);
    puts("PASS quick touch/activity after manual sleep cannot wake the screen");

    reset();
    manager.timeout_seconds = 1;
    clock_ms = 2000;
    lock_hook = change_settings_while_locking;
    run_worker(1);
    CHECK(!sleeps && applied_brightness == 40);
    puts("PASS latest settings after waiting for the LVGL lock");

    reset();
    uint32_t inactive = 0, activity = 0;
    clock_ms = 3;
    display.last_activity = UINT32_MAX - 3;
    CHECK(read_activity_tick(&display, &inactive, &activity));
    CHECK(inactive == 7 && activity == UINT32_MAX - 3);
    tick_jitter = 6;
    CHECK(!read_activity_tick(&display, &inactive, &activity));
    puts("PASS activity tick wrap and unstable sample rejection");

    reset();
    clock_ms = 1000;
    manager.timeout_seconds = 1;
    sleep_error = ESP_FAIL;
    iteration_hook = retry_automatic_sleep;
    sleeps_before_retry = 0;
    run_worker(21);
    CHECK(sleeps_before_retry == 1 && sleeps == 2 && manager.sleeping);
    CHECK(!manager.sleep_requested && !display.invalidation);
    puts("PASS automatic sleep retries failed transition after 2-second backoff");

    reset();
    clock_ms = 1000;
    manager.timeout_seconds = 1;
    sleep_error = ESP_FAIL;
    iteration_hook = cancel_sleep_retry_with_wake;
    run_worker(25);
    CHECK(sleeps == 1 && wakes == 1 && !manager.sleeping);
    CHECK(display.invalidation && !manager.wake_requested);
    puts("PASS explicit wake cancels failed automatic-sleep cleanup retry");

    reset();
    manager.sleeping = board.display_sleeping = true;
    wake_error = ESP_FAIL;
    CHECK(app_power_wake(&manager) == ESP_OK);
    iteration_hook = supersede_failed_touch_wake;
    run_worker(25);
    CHECK(wakes == 1 && sleeps == 1 && manager.sleeping);
    CHECK(!manager.sleep_requested && !display.invalidation);
    puts("PASS newer explicit sleep cancels old failed explicit-wake intent");

    reset();
    app_power_status_t status;
    CHECK(app_power_get_status(NULL, &status) == ESP_ERR_INVALID_ARG);
    CHECK(app_power_set_brightness(&manager, 40, true) == ESP_OK);
    CHECK(app_power_get_status(&manager, &status) == ESP_OK);
    CHECK(status.persistence_pending && status.persistence_error == ESP_ERR_NOT_FINISHED);
    run_worker(1);
    CHECK(app_power_get_status(&manager, &status) == ESP_OK);
    CHECK(!status.persistence_pending && status.persistence_error == ESP_OK && commits == 1);
    puts("PASS UI snapshot distinguishes queued settings from committed settings");

    reset();
    media_busy = true;
    app_power_set_persistence_guard(&manager, media_persistence_busy, &media_busy);
    CHECK(app_power_set_brightness(&manager, 40, true) == ESP_OK);
    iteration_hook = pause_media;
    run_worker(2);
    CHECK(deferred_without_commit && commits == 1);
    CHECK(app_power_get_status(&manager, &status) == ESP_OK);
    CHECK(!status.persistence_pending && !status.persistence_deferred &&
          status.persistence_error == ESP_OK);
    puts("PASS media activity defers Flash writes while applying brightness, then commits after pause");

    reset();
    nvs_commit_error = ESP_FAIL;
    app_power_set_timeout(&manager, 60, true);
    run_worker(1);
    CHECK(app_power_get_status(&manager, &status) == ESP_OK);
    CHECK(status.persistence_pending && status.persistence_error == ESP_FAIL);
    reset();
    nvs_commit_error = ESP_FAIL;
    app_power_set_timeout(&manager, 60, true);
    iteration_hook = recover_persistence;
    run_worker(22);
    CHECK(commits == 2);
    CHECK(app_power_get_status(&manager, &status) == ESP_OK);
    CHECK(!status.persistence_pending && status.persistence_error == ESP_OK);
    puts("PASS failed NVS commit remains visible and pending until delayed retry succeeds");

    reset();
    app_power_set_brightness(&manager, 40, true);
    replace_during_commit = true;
    run_worker(1);
    CHECK(app_power_get_status(&manager, &status) == ESP_OK);
    CHECK(status.persistence_pending && app_power_get_brightness(&manager) == 60);
    puts("PASS completion of an old commit cannot clear a newer pending setting");

    reset();
    brightness_error = ESP_FAIL;
    run_worker(1);
    CHECK(app_power_get_status(&manager, &status) == ESP_OK && status.display_error == ESP_FAIL);
    reset();
    brightness_error = ESP_FAIL;
    iteration_hook = recover_brightness;
    run_worker(22);
    CHECK(app_power_get_status(&manager, &status) == ESP_OK && status.display_error == ESP_OK);
    CHECK(applied_brightness == 80);
    puts("PASS failed brightness is reported and clears only after successful panel retry");

    reset();
    load_zero_brightness = true;
    load_settings(&manager);
    CHECK(manager.brightness == 5);
    puts("PASS stale zero-brightness NVS cannot hide the UI after cold boot");

    reset();
    for (unsigned cycle = 0; cycle < 1000; ++cycle) {
        CHECK(apply_sleep(&manager) == ESP_OK);
        CHECK(manager.sleeping && manager.render_paused && display.invalidation_count == 0 && display.refresh.paused);
        CHECK(apply_sleep(&manager) == ESP_OK); /* Repeated sleep owns no extra disable reference. */
        CHECK(display.invalidation_count == 0);
        wake_error = ESP_FAIL;
        CHECK(apply_wake(&manager) == ESP_FAIL);
        CHECK(manager.render_paused && display.invalidation_count == 0 && display.refresh.paused);
        wake_error = ESP_OK;
        CHECK(apply_wake(&manager) == ESP_OK);
        CHECK(!manager.sleeping && !manager.render_paused && display.invalidation_count == 1 && !display.refresh.paused);
        CHECK(apply_wake(&manager) == ESP_OK);
        CHECK(display.invalidation_count == 1);
    }
    puts("PASS 1000 sleep/retry/wake cycles balance LVGL invalidation references and refresh timer");

    reset();
    CHECK(apply_sleep(&manager) == ESP_OK);
    board.pmic = &board;
    manager.key_irq_enabled = true;
    key_events = TG29_KEY_EVENT_PRESS;
    lock_available = false;
    iteration_hook = unlock_second_iteration;
    run_worker(2);
    CHECK(pending_before_unlock && key_reads == 1 && wakes == 1 && !manager.sleeping &&
          !manager.wake_requested && display.invalidation_count == 1);
    puts("PASS TG29 physical power-key wake is recorded before acquiring the LVGL lock");

    reset();
    CHECK(apply_sleep(&manager) == ESP_OK);
    board.pmic = &board;
    key_events = TG29_KEY_EVENT_SHORT_PRESS;
    key_read_error = ESP_FAIL; /* A failed RW1C acknowledgment must not discard the observed press. */
    run_worker(1);
    CHECK(key_configures == 1 && wakes == 1 && !manager.sleeping);
    puts("PASS sticky PMIC key events wake with IRQ high and an acknowledgment error");

    reset();
    CHECK(apply_sleep(&manager) == ESP_OK);
    board.touch_ready = true;
    touch_level = 0;
    lock_available = false;
    iteration_hook = unlock_second_iteration;
    run_worker(2);
    CHECK(!pending_before_unlock && wakes == 0 && manager.sleeping && consumed_gestures == 0);
    puts("PASS raw touch IRQ cannot wake the panel even after LVGL lock recovery");

    reset();
    CHECK(apply_sleep(&manager) == ESP_OK);
    board.touch_ready = true;
    gui_touch_enabled = false; /* NES owns raw touch; its input gate handles screen wake. */
    touch_level = 0;
    run_worker(1);
    CHECK(wakes == 0 && consumed_gestures == 0 && manager.sleeping);
    boot_level = 0;
    run_worker(2);
    CHECK(wakes == 0 && manager.sleeping);
    puts("PASS touch and GPIO61 cannot wake a paused NES or the sleeping panel");
    CHECK(test_user_key_dispatch() == 0);
    CHECK(test_power_key_gestures() == 0);
    CHECK(test_software_shutdown() == 0);
    CHECK(test_worker_shutdown() == 0);
    return 0;
}
