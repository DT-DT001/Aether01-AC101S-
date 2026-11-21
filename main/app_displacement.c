#include "app_displacement.h"
#include "app_telemetry.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define CALIBRATION_TIMEOUT_MS 12000U
#define READY_TIMEOUT_MS 20000U
#define STALE_SAMPLE_MS 100U
#define MOTION_LOG_INTERVAL_MS 750U

static const char *const TAG = "displacement";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_power_t *s_power;
static app_displacement_status_t s_status;
static uint32_t s_generation, s_started_at;
static bool s_finish, s_resume;

/* Only telemetry owns the estimator and its working state. */
static app_displacement_estimator_t s_estimator;
static uint32_t s_worker_generation, s_last_stamp, s_ready_at;
static uint32_t s_motion_logged_at;
static bool s_have_stamp;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool power_allows_measurement(void)
{
    app_power_status_t power;
    return s_power && app_power_get_status(s_power, &power) == ESP_OK &&
           !power.sleeping && !power.shutdown_recovering &&
           power.shutdown_state != APP_POWER_SHUTDOWN_PREPARING &&
           power.shutdown_state != APP_POWER_SHUTDOWN_REQUESTED;
}

static bool measurement_requested(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool active = s_status.active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

static void publish(uint32_t generation, app_displacement_state_t state,
                    app_displacement_result_t result)
{
    app_displacement_state_t previous = state;
    bool rest_changed = false;
    portENTER_CRITICAL(&s_lock);
    if (generation == s_generation && s_status.active) {
        previous = s_status.state;
        rest_changed = s_status.result.stationary != result.stationary;
        s_status.state = state;
        s_status.result = result;
        s_status.active = state != APP_DISPLACEMENT_COMPLETE && state != APP_DISPLACEMENT_INVALID;
    }
    portEXIT_CRITICAL(&s_lock);
    if (rest_changed && state == APP_DISPLACEMENT_MOVING) {
        const unsigned mean_hz = result.elapsed_ms ? (unsigned)(1000U * result.samples / result.elapsed_ms) : 0;
        ESP_LOGI(TAG, "session=%lu %s xyz_mm=%ld,%ld,%ld residual_mm_s=%ld imu_hz=%u corrected=%d low_confidence=%d",
                 (unsigned long)generation, result.stationary ? "held" : "resumed",
                 (long)(result.xyz_m[0] * 1000.0f), (long)(result.xyz_m[1] * 1000.0f),
                 (long)(result.xyz_m[2] * 1000.0f), (long)(result.end_speed_mps * 1000.0f),
                 mean_hz, (int)result.endpoint_corrected, (int)result.low_confidence);
    }
    if (previous == state) return;
    if (state == APP_DISPLACEMENT_INVALID) {
        ESP_LOGW(TAG, "session=%lu phase=%d fault=%d samples=%lu motion_ms=%lu max_gap_ms=%lu endpoint_mm_s2=%ld residual_mm_s=%ld",
                 (unsigned long)generation, (int)previous, (int)result.fault,
                 (unsigned long)result.samples, (unsigned long)result.elapsed_ms,
                 (unsigned long)result.max_gap_ms, (long)(result.endpoint_error_mps2 * 1000.0f),
                 (long)(result.end_speed_mps * 1000.0f));
    } else {
        ESP_LOGI(TAG, "session=%lu phase=%d->%d samples=%lu motion_ms=%lu limited=%d",
                 (unsigned long)generation, (int)previous, (int)state,
                 (unsigned long)result.samples, (unsigned long)result.elapsed_ms, (int)result.measurement_limited);
    }
}

static void fail(uint32_t generation, app_displacement_fault_t fault)
{
    app_displacement_result_t result = s_estimator.result;
    result.fault = fault;
    publish(generation, APP_DISPLACEMENT_INVALID, result);
}

static void consume_sample(const app_telemetry_snapshot_t *sample)
{
    portENTER_CRITICAL(&s_lock);
    const bool active = s_status.active;
    const bool finish = s_finish;
    const bool resume = s_resume;
    const uint32_t generation = s_generation;
    const uint32_t started_at = s_started_at;
    portEXIT_CRITICAL(&s_lock);
    if (!active) return;

    if (generation != s_worker_generation) {
        app_displacement_estimator_reset(&s_estimator);
        s_worker_generation = generation;
        s_have_stamp = false;
        s_motion_logged_at = started_at;
    }
    if (!sample || !power_allows_measurement()) {
        fail(generation, APP_DISPLACEMENT_FAULT_SENSOR);
        return;
    }

    const uint32_t now = now_ms();
    const uint32_t elapsed = now - started_at;
    if (sample->imu_error == ESP_ERR_NOT_FINISHED) {
        const uint32_t age = s_have_stamp ? now - s_last_stamp : elapsed;
        if (age >= STALE_SAMPLE_MS) fail(generation, APP_DISPLACEMENT_FAULT_GAP);
        return;
    }
    if (sample->imu_error != ESP_OK) {
        if (elapsed >= STALE_SAMPLE_MS || s_have_stamp) fail(generation, APP_DISPLACEMENT_FAULT_SENSOR);
        return;
    }
    const uint32_t stamp = sample->imu_updated_ms;
    if ((!s_have_stamp && (int32_t)(stamp - started_at) < 0) || now - stamp > STALE_SAMPLE_MS) {
        if (elapsed >= STALE_SAMPLE_MS) fail(generation, APP_DISPLACEMENT_FAULT_GAP);
        return;
    }
    if (s_estimator.phase == APP_DISPLACEMENT_WAITING_STILL && elapsed >= CALIBRATION_TIMEOUT_MS) {
        fail(generation, APP_DISPLACEMENT_FAULT_START_STILL);
        return;
    }
    if (s_estimator.phase == APP_DISPLACEMENT_READY && !finish && now - s_ready_at >= READY_TIMEOUT_MS) {
        fail(generation, APP_DISPLACEMENT_FAULT_WAIT_MOTION);
        return;
    }
    if (finish) (void)app_displacement_estimator_finish(&s_estimator);
    if (s_have_stamp && stamp == s_last_stamp) {
        if (finish) publish(generation, s_estimator.phase, app_displacement_estimator_preview(&s_estimator));
        return;
    }
    s_last_stamp = stamp;
    s_have_stamp = true;

    const qmi8658a_sample_t *imu = &sample->imu;
    /* The estimator learns its own zero rate while the operator holds still
     * at the start, then keeps that reference for the entire maneuver. */
    const app_displacement_state_t before = s_estimator.phase;
    if (resume && !finish) (void)app_displacement_estimator_resume(&s_estimator);
    const app_displacement_state_t after = app_displacement_estimator_push(
        &s_estimator, imu->accel_mps2, imu->gyro_raw_dps, stamp);
    if (after == APP_DISPLACEMENT_READY && before != after) s_ready_at = now;
    const app_displacement_result_t visible = app_displacement_estimator_preview(&s_estimator);
    publish(generation, after, visible);
    if (after == APP_DISPLACEMENT_MOVING && !visible.stationary && now - s_motion_logged_at >= MOTION_LOG_INTERVAL_MS) {
        const float *a = imu->accel_mps2, *w = s_estimator.last_rate, *v = s_estimator.velocity;
        const uint32_t quiet_ms = s_estimator.raw_endpoint.count ? stamp - s_estimator.raw_endpoint.since : 0;
        const unsigned mean_hz = visible.elapsed_ms ? (unsigned)(1000U * visible.samples / visible.elapsed_ms) : 0;
        ESP_LOGI(TAG, "motion quiet_ms=%lu preview_held=%d imu_hz=%u accel_mm_s2=%ld gyro_mdps=%ld speed_mm_s=%ld",
                 (unsigned long)quiet_ms, (int)visible.preview_held, mean_hz,
                 (long)(1000.0f * sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2])),
                 (long)(1000.0f * sqrtf(w[0]*w[0] + w[1]*w[1] + w[2]*w[2])),
                 (long)(1000.0f * sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])));
        s_motion_logged_at = now;
    }
    if (resume) {
        portENTER_CRITICAL(&s_lock);
        if (generation == s_generation) s_resume = false;
        portEXIT_CRITICAL(&s_lock);
    }
}

void app_displacement_init(app_power_t *power, bool imu_ready)
{
    s_power = power;
    portENTER_CRITICAL(&s_lock);
    s_status.available = imu_ready;
    portEXIT_CRITICAL(&s_lock);
    app_telemetry_set_imu_measurement(measurement_requested, consume_sample);
}

void app_displacement_get_status(app_displacement_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t app_displacement_start(void)
{
    if (!power_allows_measurement()) return ESP_ERR_INVALID_STATE;
    const uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (!s_status.available) error = ESP_ERR_NOT_FOUND;
    else if (s_status.active) error = ESP_ERR_INVALID_STATE;
    else {
        ++s_generation;
        s_started_at = now;
        s_finish = false;
        s_resume = false;
        s_status.active = true;
        s_status.state = APP_DISPLACEMENT_WAITING_STILL;
        s_status.result = (app_displacement_result_t){0};
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

esp_err_t app_displacement_finish(void)
{
    if (!power_allows_measurement()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    const bool allowed = s_status.active && (s_status.state == APP_DISPLACEMENT_READY ||
                                            s_status.state == APP_DISPLACEMENT_MOVING);
    if (allowed) s_finish = true;
    portEXIT_CRITICAL(&s_lock);
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t app_displacement_resume(void)
{
    if (!power_allows_measurement()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    const bool allowed = s_status.active && s_status.state == APP_DISPLACEMENT_MOVING &&
        s_status.result.stationary && !s_status.result.resume_pending && !s_resume && !s_finish;
    if (allowed) s_resume = true;
    portEXIT_CRITICAL(&s_lock);
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void app_displacement_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    ++s_generation;
    s_finish = false;
    s_resume = false;
    s_status.active = false;
    s_status.state = APP_DISPLACEMENT_IDLE;
    s_status.result = (app_displacement_result_t){0};
    portEXIT_CRITICAL(&s_lock);
}
