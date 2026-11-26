#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include "../../main/app_telemetry.c"

static pthread_t worker;
static struct { void (*fn)(void *); void *arg; } task;
static atomic_uint imu_reads, baro_reads, rtc_reads;
static atomic_uint display_diagnostic_polls;
static atomic_bool block_imu, imu_inside, unblock, quiesce_done, fail_imu;
static atomic_bool no_new_imu_frame;
static atomic_bool scripted_pressure;
static atomic_bool pressure_waiting_frame, pressure_allow_fault;
static atomic_uint pressure_attempts;
static atomic_bool measurement_requested;
static atomic_uint measurement_samples, measurement_pauses;
static atomic_uint background_imu_samples;
static atomic_int measurement_last_error;
static atomic_uint measurement_first_ms, measurement_last_ms;
static atomic_uint measurement_max_gap_ms, timed_rtc_reads;
static esp_err_t quiesce_result;
static atomic_bool finish_worker;
static _Thread_local bool is_worker;
static _Thread_local jmp_buf worker_exit;
static uint32_t fixture_ms;
static gyro_calibration_record_t stored_bias, pending_bias;
static float live_bias[3];
static bool record_exists, commit_fails, auto_bias_enabled = true;
static unsigned commit_attempts, bias_updates;

static void observe_background(const app_telemetry_snapshot_t *sample)
{
    if (sample && sample->imu_error == ESP_OK) atomic_fetch_add(&background_imu_samples, 1);
}
static int64_t monotonic_us(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * INT64_C(1000000) + ts.tv_nsec / 1000; }
int64_t esp_timer_get_time(void) { return fixture_ms ? INT64_C(1000) * fixture_ms : monotonic_us(); }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(monotonic_us() / 1000); }
void vTaskDelay(TickType_t ticks)
{ if (is_worker && atomic_load(&finish_worker)) longjmp(worker_exit, 1);
  struct timespec ts = {.tv_sec = ticks / 1000, .tv_nsec = (long)(ticks % 1000) * 1000000}; nanosleep(&ts, NULL); }
static void *start_task(void *arg)
{ (void)arg; is_worker = true; if (!setjmp(worker_exit)) task.fn(task.arg); return NULL; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *out)
{ (void)name; (void)stack; assert(priority==3); (void)out; task.fn = fn; task.arg = arg; assert(!pthread_create(&worker, NULL, start_task, NULL)); return pdPASS; }
unsigned uxTaskGetStackHighWaterMark(void *unused) { (void)unused; return 1000; }
size_t heap_caps_get_free_size(unsigned c) { (void)c; return 10000; }
size_t heap_caps_get_minimum_free_size(unsigned c) { return heap_caps_get_free_size(c); }
size_t heap_caps_get_largest_free_block(unsigned c) { return heap_caps_get_free_size(c); }
void app_display_metrics_get(app_display_metrics_t *m) { memset(m, 0, sizeof(*m)); }
void app_display_sync_poll_diagnostics(esp_lcd_panel_io_handle_t io)
{ assert(is_worker && io==(void *)1); atomic_fetch_add(&display_diagnostic_polls, 1); }
esp_err_t qmi8658a_read(void *h, qmi8658a_sample_t *s)
{
    (void)h; atomic_fetch_add(&imu_reads, 1); atomic_store(&imu_inside, true);
    while (atomic_load(&block_imu) && !atomic_load(&unblock)) vTaskDelay(1);
    memset(s, 0, sizeof(*s)); s->accel_mps2[2] = 9.8f;
    s->gyro_raw_dps[0] = 8.9f;
    atomic_store(&imu_inside, false); return atomic_load(&fail_imu) ? ESP_FAIL : ESP_OK;
}
esp_err_t qmi8658a_read_fresh(void *h, qmi8658a_sample_t *s)
{
    return atomic_load(&no_new_imu_frame) ? ESP_ERR_NOT_FINISHED : qmi8658a_read(h, s);
}
esp_err_t qmi8658a_set_sleep(void *handle, bool sleep)
{
    assert(handle);
    (void)sleep;
    return ESP_OK;
}
esp_err_t dps368_set_sleep(void *handle, bool sleep)
{
    assert(handle);
    (void)sleep;
    return ESP_OK;
}
bool qmi8658a_gyro_zero_add(qmi8658a_gyro_zero_t *window, const qmi8658a_sample_t *sample)
{
    assert(!auto_bias_enabled);
    if (sample->accel_mps2[2] < 8.5f) return false;
    window->samples++;
    memcpy(window->bias_dps, sample->gyro_raw_dps, sizeof(window->bias_dps));
    return true;
}
void qmi8658a_set_gyro_auto_bias(void *h, bool enabled)
{
    assert(h);
    auto_bias_enabled = enabled;
}
esp_err_t qmi8658a_set_gyro_bias(void *h, const float bias[3])
{
    assert(h);
    if (!isfinite(bias[0])) return ESP_ERR_INVALID_ARG;
    memcpy(live_bias, bias, sizeof(live_bias));
    bias_updates++;
    return ESP_OK;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    assert(!strcmp(ns, "gyro_cal"));
    if (mode == NVS_READONLY && !record_exists) return ESP_ERR_NVS_NOT_FOUND;
    *h = mode == NVS_READONLY ? 1 : 2;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 1 || h == 2); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *length)
{
    assert(h == 1 && record_exists && !strcmp(key, "bias_v1") && *length == sizeof(stored_bias));
    memcpy(data, &stored_bias, sizeof(stored_bias));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t length)
{
    assert(h == 2 && !strcmp(key, "bias_v1") && length == sizeof(pending_bias));
    memcpy(&pending_bias, data, length);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == 2);
    app_gyro_calibration_status_t status;
    app_telemetry_gyro_calibration_get(&status);
    assert(status.phase == APP_GYRO_CAL_SAVING);
    assert(app_telemetry_gyro_calibration_cancel() == ESP_ERR_INVALID_STATE);
    commit_attempts++;
    if (commit_fails) return ESP_FAIL;
    stored_bias = pending_bias;
    record_exists = true;
    return ESP_OK;
}
esp_err_t dps368_read(void *h, dps368_sample_t *s)
{
    (void)h;
    atomic_fetch_add(&baro_reads, 1);
    if (!atomic_load(&scripted_pressure)) {
        s->pressure_pa = 100000;
        return ESP_OK;
    }
    if (atomic_load(&pressure_attempts) == 3 && !atomic_load(&pressure_allow_fault)) {
        atomic_store(&pressure_waiting_frame, true);
        return ESP_ERR_NOT_FINISHED;
    }
    switch (atomic_fetch_add(&pressure_attempts, 1)) {
    case 0: s->pressure_pa = 100001; break;
    case 1: s->pressure_pa = 108000; break;
    case 2: s->pressure_pa = 100004; break;
    case 3: return ESP_FAIL;
    default: s->pressure_pa = 100020; break;
    }
    return ESP_OK;
}
esp_err_t pcf85063_get_time(void *h, pcf85063_time_t *s)
{ (void)h; atomic_fetch_add(&rtc_reads, 1); s->value = 123; return ESP_OK; }
esp_err_t pcf85063_get_time_with_timeout(void *h, pcf85063_time_t *s, int timeout_ms)
{
    assert(is_worker && timeout_ms == 8);
    vTaskDelay((TickType_t)timeout_ms);
    if (atomic_fetch_add(&timed_rtc_reads, 1) == 0) return ESP_ERR_TIMEOUT;
    return pcf85063_get_time(h, s);
}
static bool request_measurement(void)
{ assert(is_worker); return atomic_load(&measurement_requested); }
static void consume_measurement(const app_telemetry_snapshot_t *sample)
{
    assert(is_worker);
    if (!sample) {
        atomic_fetch_add(&measurement_pauses, 1);
        return;
    }
    atomic_store(&measurement_last_error, sample->imu_error);
    if (sample->imu_error != ESP_OK) return;
    assert(sample->imu.accel_mps2[2] == 9.8f);
    const unsigned previous = atomic_exchange(&measurement_last_ms, sample->imu_updated_ms);
    if (previous == sample->imu_updated_ms) return;
    if (!atomic_load(&measurement_samples)) atomic_store(&measurement_first_ms, sample->imu_updated_ms);
    else if (sample->imu_updated_ms - previous > atomic_load(&measurement_max_gap_ms))
        atomic_store(&measurement_max_gap_ms, sample->imu_updated_ms - previous);
    atomic_fetch_add(&measurement_samples, 1);
}
static void await_samples(unsigned count)
{
    for (unsigned n = 0; n < 2000 && atomic_load(&measurement_samples) < count; ++n) vTaskDelay(1);
    assert(atomic_load(&measurement_samples) >= count);
}
static void await(atomic_bool *flag)
{ for (unsigned n = 0; n < 2000 && !atomic_load(flag); ++n) vTaskDelay(1); assert(atomic_load(flag)); }
static void *quiesce_task(void *unused)
{ (void)unused; quiesce_result = app_telemetry_quiesce(500); atomic_store(&quiesce_done, true); return NULL; }
static app_telemetry_snapshot_t await_pressure(esp_err_t error, float pressure, unsigned delay_ms)
{
    app_telemetry_snapshot_t snap;
    const uint32_t began = (uint32_t)(monotonic_us() / 1000);
    do {
        app_telemetry_get(&snap);
        if (snap.barometer_error == error &&
            (error != ESP_OK || snap.barometer.pressure_pa == pressure)) return snap;
        vTaskDelay(1);
    } while ((uint32_t)(monotonic_us() / 1000) - began < delay_ms);
    assert(!"expected pressure sample was not published");
    return snap;
}

static void test_pressure_filter_recovery(void)
{
    atomic_store(&scripted_pressure, true);
    app_telemetry_set_interest(APP_TELEMETRY_BAROMETER);
    const app_telemetry_snapshot_t filtered = await_pressure(ESP_OK, 100004, 1500);
    assert(atomic_load(&pressure_attempts) == 3);
    await(&pressure_waiting_frame);
    app_telemetry_snapshot_t snap;
    app_telemetry_get(&snap);
    assert(snap.barometer_error == ESP_OK && snap.barometer.pressure_pa == 100004);
    assert(snap.barometer_updated_ms == filtered.barometer_updated_ms);
    atomic_store(&pressure_allow_fault, true);
    (void)await_pressure(ESP_FAIL, 0, 1000);
    assert(atomic_load(&pressure_attempts) == 4);
    const app_telemetry_snapshot_t recovered = await_pressure(ESP_OK, 100020, 3000);
    assert(recovered.barometer_updated_ms - filtered.barometer_updated_ms >= 2000);
    puts("PASS pressure spike median rejection, missing-frame cache retention, fault backoff and clean recovery");
}

static void assert_calibration(app_gyro_calibration_phase_t phase, uint32_t remaining,
                               uint32_t samples, esp_err_t error)
{
    app_gyro_calibration_status_t status;
    app_telemetry_gyro_calibration_get(&status);
    assert(status.phase == phase && status.remaining_ms == remaining && status.samples == samples);
    assert(status.error == error);
    app_telemetry_snapshot_t snap;
    app_telemetry_get(&snap);
    assert(snap.gyro_calibrating == gyro_calibration_active(phase));
}

static void test_calibration_worker(void)
{
    atomic_store(&fail_imu, false);
    app_telemetry_set_interest(0);
    vTaskDelay(50);
    const unsigned before = atomic_load(&imu_reads);
    const uint32_t began = uptime_ms();
    assert(app_telemetry_gyro_calibration_begin() == ESP_OK);
    vTaskDelay(250);
    assert(atomic_load(&imu_reads) == before);

    app_gyro_calibration_status_t status;
    do {
        vTaskDelay(20);
        app_telemetry_gyro_calibration_get(&status);
    } while (gyro_calibration_active(status.phase) && uptime_ms() - began < 8500);
    assert(status.phase == APP_GYRO_CAL_COMPLETE && status.error == ESP_OK);
    assert(uptime_ms() - began >= 7000 && status.samples >= 100);
    assert(commit_attempts == 1 && bias_updates == 1 && live_bias[0] == 8.9f);
    assert(atomic_load(&imu_reads) - before == status.samples);
    vTaskDelay(50);
    const unsigned completed_reads = atomic_load(&imu_reads);
    vTaskDelay(150);
    assert(atomic_load(&imu_reads) == completed_reads);
    puts("PASS real telemetry worker calibrates without an IMU subscription and releases the sampling request");
}

static void start_fixture_calibration(void *imu, gyro_calibration_job_t *job)
{
    bool sampling_started;
    assert(app_telemetry_gyro_calibration_begin() == ESP_OK);
    assert(app_telemetry_gyro_calibration_begin() == ESP_ERR_INVALID_STATE);
    assert_calibration(APP_GYRO_CAL_COUNTDOWN, 2000, 0, ESP_OK);
    assert(!prepare_gyro_calibration(imu, job, fixture_ms, false, &sampling_started));
    assert(!sampling_started && !auto_bias_enabled);
    fixture_ms += 1999;
    assert(!prepare_gyro_calibration(imu, job, fixture_ms, false, &sampling_started));
    assert_calibration(APP_GYRO_CAL_COUNTDOWN, 1, 0, ESP_OK);
    fixture_ms++;
    assert(prepare_gyro_calibration(imu, job, fixture_ms, false, &sampling_started));
    assert(sampling_started);
    assert_calibration(APP_GYRO_CAL_SAMPLING, 5000, 0, ESP_OK);
}

static void fill_calibration(void *imu, gyro_calibration_job_t *job,
                             const qmi8658a_sample_t *candidate)
{
    for (unsigned i = 0; i < 250; ++i) {
        assert(!collect_gyro_sample(imu, job, ESP_OK, candidate, fixture_ms));
        fixture_ms += 20;
    }
    assert_calibration(APP_GYRO_CAL_SAMPLING, 0, 250, ESP_OK);
}

static bool calibration_flash_busy(void *context)
{
    return *(bool *)context;
}

static void test_manual_calibration(void *imu)
{
    fixture_ms = 20000;
    s_quiesce_requested = false;
    s_imu_available = false;
    assert(app_telemetry_gyro_calibration_begin() == ESP_ERR_NOT_SUPPORTED);
    s_imu_available = true;
    commit_attempts = bias_updates = 0;
    record_exists = false;
    gyro_calibration_job_t job = {0};
    const qmi8658a_sample_t flat = {.accel_mps2 = {0, 0, 8.9f}, .gyro_raw_dps = {8.9f, 2.4f, 0}};
    const qmi8658a_sample_t moved = {.accel_mps2 = {0, 0, 5.5f}, .gyro_raw_dps = {9, 2.5f, 0}};
    const qmi8658a_sample_t update = {.accel_mps2 = {0, 0, 8.9f}, .gyro_raw_dps = {9.5f, 2.5f, 0}};

    start_fixture_calibration(imu, &job);
    assert(!collect_gyro_sample(imu, &job, ESP_ERR_NOT_FINISHED, NULL, fixture_ms));
    assert_calibration(APP_GYRO_CAL_SAMPLING, 5000, 0, ESP_OK);
    fixture_ms += 20;
    fill_calibration(imu, &job, &flat);
    assert(!commit_attempts && !bias_updates);
    assert(collect_gyro_sample(imu, &job, ESP_OK, &flat, fixture_ms));
    assert_calibration(APP_GYRO_CAL_COMPLETE, 0, 251, ESP_OK);
    assert(commit_attempts == 1 && bias_updates == 1 && live_bias[0] == 8.9f);
    bool sampling_started;
    assert(!prepare_gyro_calibration(imu, &job, fixture_ms, false, &sampling_started));
    assert(auto_bias_enabled);
    memset(live_bias, 0, sizeof(live_bias));
    restore_gyro_bias(imu);
    assert(bias_updates == 2 && live_bias[0] == 8.9f && live_bias[1] == 2.4f);
    puts("PASS manual calibration exclusive 2s countdown, 5s fresh frame window and per-device reboot restore");

    start_fixture_calibration(imu, &job);
    fill_calibration(imu, &job, &update);
    assert(app_telemetry_gyro_calibration_cancel() == ESP_OK);
    assert(!collect_gyro_sample(imu, &job, ESP_OK, &update, fixture_ms));
    assert_calibration(APP_GYRO_CAL_CANCELLED, 0, 250, ESP_OK);

    start_fixture_calibration(imu, &job);
    assert(!collect_gyro_sample(imu, &job, ESP_OK, &moved, fixture_ms));
    assert_calibration(APP_GYRO_CAL_FAILED, 0, 0, ESP_ERR_INVALID_RESPONSE);
    start_fixture_calibration(imu, &job);
    assert(!collect_gyro_sample(imu, &job, ESP_FAIL, NULL, fixture_ms));
    assert_calibration(APP_GYRO_CAL_FAILED, 0, 0, ESP_FAIL);
    start_fixture_calibration(imu, &job);
    fixture_ms += 101;
    assert(!collect_gyro_sample(imu, &job, ESP_ERR_NOT_FINISHED, NULL, fixture_ms));
    assert_calibration(APP_GYRO_CAL_FAILED, 0, 0, ESP_ERR_TIMEOUT);
    start_fixture_calibration(imu, &job);
    assert(!prepare_gyro_calibration(imu, &job, fixture_ms, true, &sampling_started));
    assert_calibration(APP_GYRO_CAL_FAILED, 0, 0, ESP_ERR_INVALID_STATE);
    assert(commit_attempts == 1 && bias_updates == 2 && live_bias[0] == 8.9f);

    bool flash_busy = true;
    app_telemetry_set_gyro_persistence_guard(calibration_flash_busy, &flash_busy);
    start_fixture_calibration(imu, &job);
    fill_calibration(imu, &job, &update);
    assert(!collect_gyro_sample(imu, &job, ESP_OK, &update, fixture_ms));
    assert_calibration(APP_GYRO_CAL_FAILED, 0, 251, ESP_ERR_INVALID_STATE);
    assert(commit_attempts == 1 && bias_updates == 2 && live_bias[0] == 8.9f);
    flash_busy = false;

    start_fixture_calibration(imu, &job);
    fill_calibration(imu, &job, &update);
    commit_fails = true;
    assert(!collect_gyro_sample(imu, &job, ESP_OK, &update, fixture_ms));
    assert_calibration(APP_GYRO_CAL_FAILED, 0, 251, ESP_FAIL);
    assert(commit_attempts == 2 && bias_updates == 2 && live_bias[0] == 8.9f);
    restore_gyro_bias(imu);
    assert(bias_updates == 3 && live_bias[0] == 8.9f);
    stored_bias.version++;
    restore_gyro_bias(imu);
    assert(bias_updates == 3);
    stored_bias.version--;
    stored_bias.bias_dps[0] = NAN;
    restore_gyro_bias(imu);
    assert(bias_updates == 3 && live_bias[0] == 8.9f);
    app_telemetry_set_gyro_persistence_guard(NULL, NULL);
    fixture_ms = 0;
    puts("PASS movement, bus error, stale frames, competing measurement, late cancellation, busy media and bad storage preserve last bias");
}

int main(void)
{
    assert(median_pressure((float[]){100001,109000,100000}) == 100001);
    assert(median_pressure((float[]){100001,96000,100000}) == 100000);
    assert(median_pressure((float[]){100001,100001,100001}) == 100001);
    aether_board_t board = {.imu_ready = true, .imu = (void *)1, .barometer_ready = true, .barometer = (void *)1,
                            .rtc_ready = true, .display_io=(void *)1};
    app_telemetry_set_imu_measurement(request_measurement, consume_measurement);
    assert(app_telemetry_start(&board) == ESP_OK);
    vTaskDelay(120); assert(!atomic_load(&imu_reads) && !atomic_load(&baro_reads) && atomic_load(&rtc_reads));
    assert(!atomic_load(&measurement_samples));
    assert(atomic_load(&display_diagnostic_polls)>=2);
    atomic_store(&block_imu, true); app_telemetry_set_interest(APP_TELEMETRY_IMU | APP_TELEMETRY_BAROMETER); await(&imu_inside);
    pthread_t waiter; assert(!pthread_create(&waiter, NULL, quiesce_task, NULL));
    vTaskDelay(60); assert(!atomic_load(&quiesce_done) && atomic_load(&imu_inside));
    atomic_store(&unblock, true); assert(!pthread_join(waiter, NULL));
    assert(quiesce_result == ESP_OK && !atomic_load(&imu_inside));
    const unsigned i = atomic_load(&imu_reads), b = atomic_load(&baro_reads), r = atomic_load(&rtc_reads);
    const unsigned diagnostic_polls = atomic_load(&display_diagnostic_polls);
    app_telemetry_set_interest(APP_TELEMETRY_IMU); vTaskDelay(150);
    assert(atomic_load(&imu_reads) == i && atomic_load(&baro_reads) == b && atomic_load(&rtc_reads) == r);
    assert(atomic_load(&display_diagnostic_polls)==diagnostic_polls);
    app_telemetry_snapshot_t snap; app_telemetry_get(&snap);
    assert(snap.imu_error == ESP_ERR_NOT_FINISHED && snap.rtc_error == ESP_ERR_NOT_FINISHED);
    puts("PASS quiesce waits for an actual in-flight I2C batch and no sensor/RTC read starts after acknowledgement");
    atomic_store(&block_imu, false); app_telemetry_resume();
    vTaskDelay(220); const unsigned fast_reads = atomic_load(&imu_reads) - i;
    assert(fast_reads >= 8 && fast_reads <= 14 && atomic_load(&baro_reads) == b);
    app_telemetry_get(&snap); assert(snap.imu_error == ESP_OK && snap.barometer_error == ESP_ERR_NOT_FINISHED);
    puts("PASS active IMU samples at 20ms cadence, uninterested sensors remain idle and resume invalidates old samples");

    atomic_store(&no_new_imu_frame, true);
    app_telemetry_set_consumer(observe_background);
    app_telemetry_set_interest(APP_TELEMETRY_IMU_GAME);
    vTaskDelay(50);
    app_telemetry_get(&snap);
    assert(snap.imu_error == ESP_ERR_NOT_FINISHED);
    atomic_store(&no_new_imu_frame, false);
    vTaskDelay(40);
    app_telemetry_get(&snap);
    assert(snap.imu_error == ESP_OK);
    const unsigned game_reads = atomic_load(&imu_reads);
    const unsigned callbacks = atomic_load(&background_imu_samples);
    vTaskDelay(240);
    const unsigned game_count = atomic_load(&imu_reads) - game_reads;
    assert(game_count >= 17 && game_count <= 27 && atomic_load(&measurement_samples) == 0);
    assert(atomic_load(&background_imu_samples) > callbacks);
    app_telemetry_set_screen_suspended(true);
    vTaskDelay(50);
    const unsigned suspended_game_reads = atomic_load(&imu_reads);
    vTaskDelay(70);
    assert(atomic_load(&imu_reads) == suspended_game_reads);
    app_telemetry_set_screen_suspended(false);
    vTaskDelay(150);
    assert(atomic_load(&imu_reads) > suspended_game_reads);
    app_telemetry_set_consumer(NULL);
    puts("PASS game IMU 10ms cadence, fresh frames on entry, background consumer and standby sampling gate");

    /* Measure real worker cadence between fresh timestamped samples rather
     * than guessing how many callbacks are delivered during a wall delay. */
    app_telemetry_set_interest(0);
    atomic_store(&measurement_requested, true);
    await_samples(16);
    unsigned measured = atomic_load(&measurement_samples);
    uint32_t span = atomic_load(&measurement_last_ms) - atomic_load(&measurement_first_ms);
    assert(span >= 4U * (measured - 1) && span <= 9U * (measured - 1));
    assert(atomic_load(&measurement_last_error) == ESP_OK);
    const unsigned measuring_diagnostics = atomic_load(&display_diagnostic_polls);
    for (unsigned elapsed = 0; elapsed < 1500 && atomic_load(&timed_rtc_reads) < 2; ++elapsed) vTaskDelay(1);
    assert(atomic_load(&timed_rtc_reads) >= 2);
    assert(atomic_load(&measurement_max_gap_ms) < 80);
    assert(atomic_load(&display_diagnostic_polls) == measuring_diagnostics);
    app_telemetry_get(&snap);
    assert(snap.rtc_error == ESP_OK);
    puts("PASS measurement keeps the IMU cadence through an 8ms RTC timeout/retry, leaves diagnostics deferred and preserves clock data");
    app_telemetry_get(&snap);
    assert(snap.imu_error == ESP_ERR_NOT_FINISHED);
    atomic_store(&no_new_imu_frame, true);
    for (unsigned n = 0; n < 500 && atomic_load(&measurement_last_error) != ESP_ERR_NOT_FINISHED; ++n) vTaskDelay(1);
    assert(atomic_load(&measurement_last_error) == ESP_ERR_NOT_FINISHED);
    measured = atomic_load(&measurement_samples);
    vTaskDelay(30);
    assert(atomic_load(&measurement_samples) == measured);
    atomic_store(&no_new_imu_frame, false);
    await_samples(measured + 2);

    /* An ordinary subscriber can read the cache while measuring. Removing the
     * fast request leaves the ordinary 20 ms cadence active. */
    app_telemetry_set_interest(APP_TELEMETRY_IMU);
    await_samples(measured + 4);
    app_telemetry_get(&snap);
    assert(snap.imu_error == ESP_OK);
    atomic_store(&measurement_requested, false);
    vTaskDelay(30);
    measured = atomic_load(&measurement_samples);
    unsigned ordinary_before = atomic_load(&imu_reads);
    vTaskDelay(200);
    unsigned ordinary_count = atomic_load(&imu_reads) - ordinary_before;
    assert(ordinary_count >= 7 && ordinary_count <= 12);
    assert(atomic_load(&measurement_samples) == measured);
    app_telemetry_set_interest(0);
    vTaskDelay(30);
    ordinary_before = atomic_load(&imu_reads);
    vTaskDelay(80);
    assert(atomic_load(&imu_reads) == ordinary_before);
    puts("PASS on-demand IMU targets 5ms cadence, ignores missing frames and returns to ordinary cadence or idle");

    atomic_store(&measurement_requested, true);
    await_samples(measured + 2);
    const unsigned pauses_before = atomic_load(&measurement_pauses);
    assert(app_telemetry_quiesce(500) == ESP_OK);
    const unsigned stopped_reads = atomic_load(&imu_reads);
    const unsigned stopped_samples = atomic_load(&measurement_samples);
    vTaskDelay(100);
    assert(atomic_load(&measurement_pauses) > pauses_before);
    assert(atomic_load(&imu_reads) == stopped_reads && atomic_load(&measurement_samples) == stopped_samples);
    app_telemetry_resume();
    await_samples(stopped_samples + 2);
    atomic_store(&measurement_requested, false);
    app_telemetry_set_interest(APP_TELEMETRY_IMU);
    puts("PASS active measurement receives NULL while quiesced, stops reads and resumes on demand");

    atomic_store(&block_imu, true); atomic_store(&unblock, false); await(&imu_inside);
    assert(app_telemetry_quiesce(30) == ESP_ERR_TIMEOUT && s_quiesce_requested);
    atomic_store(&unblock, true); assert(app_telemetry_quiesce(500) == ESP_OK);
    atomic_store(&block_imu, false); atomic_store(&fail_imu, true); app_telemetry_resume();
    vTaskDelay(80); const unsigned after_error = atomic_load(&imu_reads); vTaskDelay(180);
    assert(atomic_load(&imu_reads) == after_error);
    app_telemetry_get(&snap); assert(snap.imu_error == ESP_FAIL);
    test_pressure_filter_recovery();
    test_calibration_worker();
    assert(app_telemetry_gyro_calibration_begin() == ESP_OK);
    app_telemetry_set_interest(0); assert(app_telemetry_quiesce(500) == ESP_OK);
    assert_calibration(APP_GYRO_CAL_CANCELLED, 0, 0, ESP_OK);
    atomic_store(&finish_worker, true); assert(!pthread_join(worker, NULL));
    puts("PASS timeout retains the sampling gate, later acknowledgement recovers and I2C failure retains 2s backoff");
    auto_bias_enabled = true;
    test_manual_calibration(board.imu);
    return 0;
}
