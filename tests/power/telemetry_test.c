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
static atomic_int measurement_last_error;
static atomic_uint measurement_first_ms, measurement_last_ms;
static esp_err_t quiesce_result;
static atomic_bool finish_worker;
static _Thread_local bool is_worker;
static _Thread_local jmp_buf worker_exit;
static int64_t monotonic_us(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * INT64_C(1000000) + ts.tv_nsec / 1000; }
int64_t esp_timer_get_time(void) { return monotonic_us(); }
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
    atomic_store(&imu_inside, false); return atomic_load(&fail_imu) ? ESP_FAIL : ESP_OK;
}
esp_err_t qmi8658a_read_fresh(void *h, qmi8658a_sample_t *s)
{
    return atomic_load(&no_new_imu_frame) ? ESP_ERR_NOT_FINISHED : qmi8658a_read(h, s);
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

int main(void)
{
    assert(median_pressure((float[]){100001,109000,100000}) == 100001);
    assert(median_pressure((float[]){100001,96000,100000}) == 100000);
    assert(median_pressure((float[]){100001,100001,100001}) == 100001);
    aether_board_t board = {.imu_ready = true, .barometer_ready = true, .rtc_ready = true, .display_io=(void *)1};
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

    /* Measure real worker cadence between fresh timestamped samples rather
     * than guessing how many callbacks are delivered during a wall delay. */
    app_telemetry_set_interest(0);
    atomic_store(&measurement_requested, true);
    await_samples(16);
    unsigned measured = atomic_load(&measurement_samples);
    uint32_t span = atomic_load(&measurement_last_ms) - atomic_load(&measurement_first_ms);
    assert(span >= 4U * (measured - 1) && span <= 9U * (measured - 1));
    assert(atomic_load(&measurement_last_error) == ESP_OK);
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
    app_telemetry_set_interest(0); assert(app_telemetry_quiesce(500) == ESP_OK);
    atomic_store(&finish_worker, true); assert(!pthread_join(worker, NULL));
    puts("PASS timeout retains the sampling gate, later acknowledgement recovers and I2C failure retains 2s backoff");
    return 0;
}
