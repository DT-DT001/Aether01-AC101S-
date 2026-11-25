#include "app_displacement.h"
#include "app_telemetry.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static app_power_t power;
static app_power_status_t power_status;
static uint32_t now = 1000;
static const float raw_gyro_bias = 7.0f;
static float shared_gyro_bias;
static bool shared_bias_valid;
static bool (*requested)(void);
static void (*consume)(const app_telemetry_snapshot_t *);

int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }

esp_err_t app_power_get_status(const app_power_t *handle, app_power_status_t *status)
{
    assert(handle == &power);
    *status = power_status;
    return ESP_OK;
}

void app_telemetry_set_imu_measurement(bool (*want)(void), void (*read)(const app_telemetry_snapshot_t *))
{
    requested = want;
    consume = read;
}

static app_displacement_status_t status(void)
{
    app_displacement_status_t result;
    app_displacement_get_status(&result);
    assert(result.active == requested());
    return result;
}

static void sample_with_rate(float accel_x, float gyro_z, uint32_t age_ms)
{
    now += 10;
    const app_telemetry_snapshot_t reading = {
        .imu = {
            .accel_mps2 = {accel_x, 0, 9.80665f},
            .gyro_raw_dps = {0, 0, gyro_z},
            .gyro_dps = {0, 0, gyro_z - shared_gyro_bias},
            .gyro_bias_dps = {0, 0, shared_gyro_bias},
            .gyro_bias_valid = shared_bias_valid,
        },
        .imu_error = ESP_OK,
        .imu_updated_ms = now - age_ms,
    };
    consume(&reading);
}

static void sample(float accel_x, uint32_t age_ms)
{
    sample_with_rate(accel_x, raw_gyro_bias, age_ms);
}

static void start_ready(void)
{
    assert(app_displacement_start() == ESP_OK);
    assert(app_displacement_finish() == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 150 && status().state == APP_DISPLACEMENT_WAITING_STILL; ++i) sample(0, 0);
    assert(status().state == APP_DISPLACEMENT_READY);
}

static void stage_timeouts(void)
{
    assert(app_displacement_start() == ESP_OK);
    for (unsigned i = 0; i < 1199; ++i) sample_with_rate(0, 50.0f, 0);
    assert(status().state == APP_DISPLACEMENT_WAITING_STILL && requested());
    sample_with_rate(0, 50.0f, 0);
    assert(status().state == APP_DISPLACEMENT_INVALID && !requested());
    assert(status().result.fault == APP_DISPLACEMENT_FAULT_START_STILL);

    start_ready();
    for (unsigned i = 0; i < 1999; ++i) sample(0, 0);
    assert(status().state == APP_DISPLACEMENT_READY && requested());
    sample(0, 0);
    assert(status().state == APP_DISPLACEMENT_INVALID && !requested());
    assert(status().result.fault == APP_DISPLACEMENT_FAULT_WAIT_MOTION);
}

static void pending_frames_do_not_integrate(void)
{
    const app_telemetry_snapshot_t no_new_frame = {.imu_error = ESP_ERR_NOT_FINISHED};
    assert(app_displacement_start() == ESP_OK);
    now += 99;
    consume(&no_new_frame);
    assert(status().state == APP_DISPLACEMENT_WAITING_STILL);
    now += 1;
    consume(&no_new_frame);
    assert(status().state == APP_DISPLACEMENT_INVALID && status().result.fault == APP_DISPLACEMENT_FAULT_GAP);
    assert(status().result.max_gap_ms == 100 && status().result.samples == 0);

    start_ready();
    unsigned samples = status().result.samples;
    for (unsigned i = 0; i < 3; ++i) {
        now += 2;
        consume(&no_new_frame);
    }
    assert(status().state == APP_DISPLACEMENT_READY && status().result.samples == samples);
    sample(2.0f, 0);
    samples = status().result.samples;
    now += 99;
    consume(&no_new_frame);
    assert(requested() && status().result.samples == samples);
    now += 1;
    consume(&no_new_frame);
    assert(status().state == APP_DISPLACEMENT_INVALID && status().result.fault == APP_DISPLACEMENT_FAULT_GAP);
    assert(status().result.max_gap_ms == 100);
}

int main(void)
{
    app_displacement_init(&power, false);
    assert(!status().available && app_displacement_start() == ESP_ERR_NOT_FOUND);
    app_displacement_init(&power, true);
    start_ready();
    for (unsigned i = 0; i < 50; ++i) sample(2, 0);
    assert(status().state == APP_DISPLACEMENT_MOVING);
    /* Shared bias is unavailable at start and changes midway. Neither
     * condition may affect the measurement's frozen stationary reference. */
    shared_bias_valid = true;
    shared_gyro_bias = raw_gyro_bias + 0.5f;
    for (unsigned i = 0; i < 50; ++i) sample(-2, 0);
    sample(0, 0);
    assert(app_displacement_finish() == ESP_OK);
    for (unsigned i = 0; i < 150 && requested(); ++i) sample(0, 0);
    assert(status().state == APP_DISPLACEMENT_COMPLETE && !requested());
    assert(fabsf(status().result.distance_m - 0.50f) < 0.02f);
    assert(fabsf(status().result.xyz_m[1]) < 0.001f);
    shared_bias_valid = false;
    shared_gyro_bias = 0.0f;

    start_ready();
    assert(app_displacement_resume() == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 50; ++i) sample(2, 0);
    for (unsigned i = 0; i < 50; ++i) sample(-2, 0);
    for (unsigned i = 0; i < 80; ++i) sample(0, 0);
    const app_displacement_result_t locked = status().result;
    assert(locked.stationary && !locked.resume_pending && requested());
    for (unsigned i = 0; i < 300; ++i) {
        sample_with_rate(0.28f, raw_gyro_bias + 1.8f, 0);
        assert(status().result.stationary && status().result.distance_m == locked.distance_m);
    }
    assert(app_displacement_resume() == ESP_OK);
    assert(app_displacement_resume() == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 31; ++i) sample(0, 0);
    assert(status().result.resume_pending && status().result.stationary);
    for (unsigned i = 0; i < 20; ++i) sample(-2, 0);
    assert(!status().result.resume_pending && !status().result.stationary);
    assert(status().result.xyz_m[0] < locked.xyz_m[0]);
    app_displacement_cancel();

    stage_timeouts();
    pending_frames_do_not_integrate();

    start_ready();
    app_displacement_cancel();
    sample(2, 0);
    assert(status().state == APP_DISPLACEMENT_IDLE && !requested());
    assert(app_displacement_start() == ESP_OK);
    for (unsigned i = 0; i < 9; ++i) sample(0, 120);
    assert(status().state == APP_DISPLACEMENT_WAITING_STILL);
    sample(0, 120);
    assert(status().state == APP_DISPLACEMENT_INVALID && status().result.fault == APP_DISPLACEMENT_FAULT_GAP);
    assert(!requested() && status().result.max_gap_ms == 120 && status().result.samples == 0);

    start_ready();
    consume(NULL);
    assert(status().state == APP_DISPLACEMENT_INVALID && !requested());
    start_ready();
    power_status.sleeping = true;
    sample(0, 0);
    assert(status().state == APP_DISPLACEMENT_INVALID && !requested());
    assert(app_displacement_start() == ESP_ERR_INVALID_STATE);
    puts("PASS displacement session holds until explicit resume, owns gyro calibration and releases sampling on exit");
    return 0;
}
