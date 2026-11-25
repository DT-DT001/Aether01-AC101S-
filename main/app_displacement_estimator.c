#include "app_displacement_estimator.h"

#include <math.h>
#include <string.h>

#define GRAVITY_MPS2 9.80665f
#define RAD_PER_DEGREE 0.01745329252f
#define START_STILL_MS 650U
#define END_STILL_MS 400U
#define FALLBACK_STILL_MS 1800U
#define PREVIEW_HOLD_MS 250U
#define MIN_STILL_SAMPLES 12U
#define ONSET_MS 50U
#define RESUME_MS 100U
#define RESUME_REST_MS 300U
#define RESUME_WAIT_MS 12000U
#define DRIFT_WARNING_MS 3500U
#define MAX_CORRECTION_MS 8000U
#define END_CONFIRM_TIMEOUT_MS 4000U
#define MAX_GAP_MS 80U
#define PREFERRED_GAP_MS 30U
#define ONSET_ACCEL_MPS2 0.09f
#define RESUME_ACCEL_MPS2 0.14f
#define STILL_ACCEL_MPS2 0.22f
#define END_ACCEL_VARIATION_MPS2 0.10f
#define FALLBACK_ACCEL_VARIATION_MPS2 0.18f
#define FALLBACK_ACCEL_EXCURSION_MPS2 0.24f
#define STILL_GYRO_DPS 1.2f
#define FALLBACK_GYRO_LIMIT_DPS 2.5f
#define FALLBACK_GYRO_VARIATION_DPS 0.8f
#define FALLBACK_OUTLIER_MS 50U
#define FALLBACK_TOTAL_OUTLIER_MS 160U
#define START_RAW_GYRO_LIMIT_DPS 15.0f
#define START_GYRO_VARIATION_DPS 0.35f
#define START_GRAVITY_TOLERANCE 1.4f
#define END_GRAVITY_TOLERANCE 0.35f
#define END_LINEAR_LIMIT_MPS2 0.08f
#define FINISH_LINEAR_LIMIT_MPS2 0.30f
#define AUTO_STOP_SPEED_MPS 0.035f
#define BRAKING_STOP_SPEED_MPS 0.35f
#define BRAKING_STOP_LINEAR_MPS2 0.22f
#define BRAKING_EVIDENCE_MS 1200U
#define MAX_ENDPOINT_ERROR 0.95f
#define MAX_TILT_CORRECTION_MPS2 0.30f
#define MAX_PREDICTION_MISMATCH_MPS2 0.25f

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float norm(const float a[3])
{
    return sqrtf(dot(a, a));
}

static float distance_squared(const float a[3], const float b[3])
{
    const float delta[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return dot(delta, delta);
}

static void cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void app_displacement_estimator_reset(app_displacement_estimator_t *est)
{
    memset(est, 0, sizeof(*est));
    est->phase = APP_DISPLACEMENT_WAITING_STILL;
    est->attitude[0] = 1.0f;
}

bool app_displacement_estimator_finish(app_displacement_estimator_t *est)
{
    if (est->phase == APP_DISPLACEMENT_SETTLING) return true;
    if (est->phase != APP_DISPLACEMENT_READY && est->phase != APP_DISPLACEMENT_MOVING) return false;
    if (est->result.stationary) {
        est->result.resume_pending = false;
        est->phase = APP_DISPLACEMENT_COMPLETE;
        return true;
    }
    est->finish_estimate = app_displacement_estimator_preview(est);
    est->finish_estimate.preview_held = false;
    est->phase = APP_DISPLACEMENT_SETTLING;
    est->finish_since = est->last_stamp;
    return true;
}

bool app_displacement_estimator_resume(app_displacement_estimator_t *est)
{
    if (est->phase != APP_DISPLACEMENT_MOVING || !est->result.stationary || est->result.resume_pending) return false;
    est->result.resume_pending = true;
    est->resume_requested_at = est->last_stamp;
    est->resume_ready = false;
    est->resume_quiet_ms = 0;
    return true;
}

static app_displacement_state_t invalidate(app_displacement_estimator_t *est,
                                           app_displacement_fault_t fault)
{
    est->result.fault = fault;
    est->phase = APP_DISPLACEMENT_INVALID;
    return est->phase;
}

app_displacement_result_t app_displacement_estimator_preview(const app_displacement_estimator_t *est)
{
    app_displacement_result_t visible = est->result;
    if (est->phase == APP_DISPLACEMENT_SETTLING) {
        visible = est->finish_estimate;
        visible.preview_held = true;
    } else if (est->phase == APP_DISPLACEMENT_MOVING && !visible.stationary) {
        const app_displacement_endpoint_t *point = &est->raw_endpoint;
        if (point->count >= MIN_STILL_SAMPLES &&
            est->last_stamp - point->since >= PREVIEW_HOLD_MS + point->rejected_ms) {
            memcpy(visible.xyz_m, point->position, sizeof(visible.xyz_m));
            visible.distance_m = norm(visible.xyz_m);
            visible.preview_held = true;
        }
    }
    return visible;
}

/* Keep an uninterrupted quiet window against a fixed starting vector. */
static float add_still_sample(app_displacement_estimator_t *est, const float accel[3],
                              uint32_t stamp, float max_variation)
{
    if (!est->still_samples || distance_squared(accel, est->still_reference) > max_variation * max_variation) {
        est->still_since = stamp;
        est->still_samples = 0;
        memcpy(est->still_reference, accel, sizeof(est->still_reference));
    }
    return 1.0f / (float)++est->still_samples;
}

static void calibrate_start(app_displacement_estimator_t *est, const float accel[3],
                            const float gyro[3], uint32_t stamp)
{
    if (fabsf(norm(accel) - GRAVITY_MPS2) > START_GRAVITY_TOLERANCE ||
        dot(gyro, gyro) > START_RAW_GYRO_LIMIT_DPS * START_RAW_GYRO_LIMIT_DPS) {
        est->still_samples = 0;
        return;
    }
    if (est->still_samples && distance_squared(gyro, est->gyro_reference) >
        START_GYRO_VARIATION_DPS * START_GYRO_VARIATION_DPS) est->still_samples = 0;
    const float weight = add_still_sample(est, accel, stamp, STILL_ACCEL_MPS2);
    if (est->still_samples == 1) memcpy(est->gyro_reference, gyro, sizeof(est->gyro_reference));
    for (unsigned axis = 0; axis < 3; ++axis) {
        est->gravity[axis] += weight * (accel[axis] - est->gravity[axis]);
        est->gyro_zero[axis] += weight * (gyro[axis] - est->gyro_zero[axis]);
        est->last_rate[axis] = gyro[axis] - est->gyro_zero[axis];
        est->last_accel[axis] = accel[axis] - est->gravity[axis];
    }
    if (est->still_samples >= MIN_STILL_SAMPLES && stamp - est->still_since >= START_STILL_MS)
        est->phase = APP_DISPLACEMENT_READY;
}

/* Propagate the body-to-start attitude using the midpoint angular rate. */
static void update_attitude(app_displacement_estimator_t *est, const float rate[3], float dt)
{
    float step[3];
    for (unsigned axis = 0; axis < 3; ++axis)
        step[axis] = (est->last_rate[axis] + rate[axis]) * (RAD_PER_DEGREE * dt * 0.25f);
    const float half_angle = norm(step);
    const float scale = half_angle > 0.0001f ? sinf(half_angle) / half_angle : 1.0f;
    const float dw = cosf(half_angle);
    const float dx = step[0] * scale, dy = step[1] * scale, dz = step[2] * scale;
    const float w = est->attitude[0], x = est->attitude[1], y = est->attitude[2], z = est->attitude[3];
    float q[4] = {
        w * dw - x * dx - y * dy - z * dz,
        w * dx + x * dw + y * dz - z * dy,
        w * dy - x * dz + y * dw + z * dx,
        w * dz + x * dy - y * dx + z * dw,
    };
    const float inv_norm = 1.0f / sqrtf(q[0] * q[0] + dot(q + 1, q + 1));
    for (unsigned i = 0; i < 4; ++i) est->attitude[i] = q[i] * inv_norm;
}

static void rotate_vector(const float attitude[4], const float vector[3], float out[3])
{
    float first[3], second[3];
    cross(attitude + 1, vector, first);
    cross(attitude + 1, first, second);
    for (unsigned axis = 0; axis < 3; ++axis)
        out[axis] = vector[axis] + 2.0f * (attitude[0] * first[axis] + second[axis]);
}

static void integrate(app_displacement_estimator_t *est, const float accel[3],
                      const float rate[3], uint32_t elapsed)
{
    const float dt = elapsed * 0.001f;
    update_attitude(est, rate, dt);
    float transformed[3];
    rotate_vector(est->attitude, accel, transformed);
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float linear = transformed[axis] - est->gravity[axis] - est->rest_linear[axis];
        const float new_velocity = est->velocity[axis] + (est->last_accel[axis] + linear) * (0.5f * dt);
        est->position[axis] += (est->velocity[axis] + new_velocity) * (0.5f * dt);
        est->velocity[axis] = new_velocity;
        est->last_accel[axis] = linear;
        est->last_rate[axis] = rate[axis];
    }
}

static app_displacement_state_t end_without_endpoint(app_displacement_estimator_t *est,
                                                    app_displacement_fault_t fault)
{
    memset(est->velocity, 0, sizeof(est->velocity));
    if (!est->stopped_estimate.stationary) {
        memset(est->result.xyz_m, 0, sizeof(est->result.xyz_m));
        est->result.distance_m = 0.0f;
        est->result.low_confidence = true;
        est->result.endpoint_corrected = false;
        return invalidate(est, fault);
    }
    const float failed_end_speed = est->result.end_speed_mps;
    const float failed_endpoint_error = est->result.endpoint_error_mps2;
    est->result = est->stopped_estimate;
    if (fault == APP_DISPLACEMENT_FAULT_ENDPOINT) {
        est->result.end_speed_mps = failed_end_speed;
        est->result.endpoint_error_mps2 = failed_endpoint_error;
    }
    est->result.fault = fault;
    est->result.measurement_limited = true;
    est->result.low_confidence = true;
    return est->phase = APP_DISPLACEMENT_COMPLETE;
}

static bool apply_endpoint_correction(app_displacement_estimator_t *est)
{
    app_displacement_result_t *result = &est->result;
    const app_displacement_endpoint_t *point = &est->endpoint;
    float error[3];
    rotate_vector(point->attitude, point->accel_mean, error);
    for (unsigned axis = 0; axis < 3; ++axis) error[axis] -= est->gravity[axis] + est->rest_linear[axis];
    const uint32_t segment_ms = point->elapsed_ms - est->segment_started_ms;
    result->endpoint_error_mps2 = norm(error);
    result->end_speed_mps = norm(point->velocity);
    if (segment_ms > MAX_CORRECTION_MS || result->endpoint_error_mps2 > MAX_ENDPOINT_ERROR ||
        result->end_speed_mps > 2.0f) return false;

    result->elapsed_ms = point->elapsed_ms;
    result->samples = point->samples;
    result->max_gap_ms = point->max_gap_ms;
    const float dt = segment_ms * 0.001f;
    result->low_confidence = est->stopped_estimate.low_confidence || !est->recording || result->max_gap_ms > PREFERRED_GAP_MS ||
        result->endpoint_error_mps2 > MAX_TILT_CORRECTION_MPS2 || result->end_speed_mps > 0.45f ||
        result->elapsed_ms > DRIFT_WARNING_MS;

    /* Both endpoints have zero velocity. Use the residual velocity to remove
     * constant acceleration error. Tilt error developing at a constant rate
     * leaves an additional position term, perpendicular to gravity. */
    const float along_gravity = dot(error, est->gravity) / dot(est->gravity, est->gravity);
    float tilt[3];
    for (unsigned axis = 0; axis < 3; ++axis)
        tilt[axis] = error[axis] - est->gravity[axis] * along_gravity;
    bool use_tilt = result->endpoint_error_mps2 <= MAX_TILT_CORRECTION_MPS2;
    if (!use_tilt) {
        /* Use a larger correction only when the gyro residual measured at
         * rest independently predicts the observed gravity projection. */
        float rate[3], predicted[3];
        rotate_vector(point->attitude, point->gyro_mean, rate);
        cross(rate, est->gravity, predicted);
        for (unsigned axis = 0; axis < 3; ++axis) predicted[axis] *= RAD_PER_DEGREE * dt;
        const float tolerance = fminf(MAX_PREDICTION_MISMATCH_MPS2, norm(tilt) * 0.35f);
        use_tilt = distance_squared(tilt, predicted) <= tolerance * tolerance &&
            fabsf(along_gravity) * norm(est->gravity) <= MAX_PREDICTION_MISMATCH_MPS2;
    }
    const float tilt_weight = use_tilt ? dt * dt / 12.0f : 0.0f;
    for (unsigned axis = 0; axis < 3; ++axis) {
        result->xyz_m[axis] = point->position[axis] - point->velocity[axis] * (0.5f * dt) + tilt[axis] * tilt_weight;
    }
    result->distance_m = norm(result->xyz_m);
    result->endpoint_corrected = est->recording;
    return true;
}

static app_displacement_state_t publish_result(app_displacement_estimator_t *est)
{
    const bool low_confidence = est->result.low_confidence;
    if (!apply_endpoint_correction(est)) return end_without_endpoint(est, APP_DISPLACEMENT_FAULT_ENDPOINT);
    est->result.low_confidence |= low_confidence;
    est->result.stationary = false;
    est->phase = APP_DISPLACEMENT_COMPLETE;
    return est->phase;
}

static void discard_onset(app_displacement_estimator_t *est)
{
    est->onset_pending = false;
    est->attitude[0] = 1.0f;
    memset(est->attitude + 1, 0, 3 * sizeof(float));
    memset(est->position, 0, sizeof(est->position));
    memset(est->velocity, 0, sizeof(est->velocity));
    est->result = (app_displacement_result_t){0};
}

/* Keep a checkpoint at the start of a quiet window for the zero-speed update. */
static bool track_endpoint(const app_displacement_estimator_t *est, app_displacement_endpoint_t *point,
                           const float accel[3], const float rate[3], uint32_t stamp,
                           bool quiet, bool fast, bool tolerate_noise, uint32_t required_ms)
{
    const float tolerance = tolerate_noise ? FALLBACK_ACCEL_VARIATION_MPS2 : END_ACCEL_VARIATION_MPS2;
    const float *reference = tolerate_noise ? point->accel_mean : point->accel_first;
    const bool within_window = !point->count ||
        (distance_squared(accel, reference) <= tolerance * tolerance &&
         (fast || (distance_squared(accel, point->accel_first) <=
                        FALLBACK_ACCEL_EXCURSION_MPS2 * FALLBACK_ACCEL_EXCURSION_MPS2 &&
                   distance_squared(rate, point->gyro_mean) <=
                        FALLBACK_GYRO_VARIATION_DPS * FALLBACK_GYRO_VARIATION_DPS)));

    /* A sparse spike should not invalidate an otherwise steady interval.
     * Duration and budget are in milliseconds, so changing sample cadence
     * does not change the detector's tolerance to real movement. */
    if (!fast && point->count && (!quiet || !within_window)) {
        point->rejected_ms += stamp - point->last_stamp;
        point->last_stamp = stamp;
        if (stamp - point->last_good_stamp <= FALLBACK_OUTLIER_MS &&
            point->rejected_ms <= FALLBACK_TOTAL_OUTLIER_MS) return false;
        point->count = 0;
    }
    if (!quiet) {
        point->count = 0;
        return false;
    }

    if (!point->count || !within_window) {
        point->count = point->rejected_ms = 0;
        point->since = stamp;
        memcpy(point->accel_first, accel, sizeof(point->accel_first));
        memcpy(point->accel_mean, accel, sizeof(point->accel_mean));
        memcpy(point->gyro_mean, rate, sizeof(point->gyro_mean));
        memcpy(point->attitude, est->attitude, sizeof(point->attitude));
        memcpy(point->position, est->position, sizeof(point->position));
        memcpy(point->velocity, est->velocity, sizeof(point->velocity));
        point->elapsed_ms = est->result.elapsed_ms;
        point->samples = est->result.samples;
        point->max_gap_ms = est->result.max_gap_ms;
    }
    point->last_stamp = point->last_good_stamp = stamp;
    const float weight = 1.0f / (float)++point->count;
    for (unsigned axis = 0; axis < 3; ++axis) {
        point->accel_mean[axis] += weight * (accel[axis] - point->accel_mean[axis]);
        point->gyro_mean[axis] += weight * (rate[axis] - point->gyro_mean[axis]);
    }
    return point->count >= MIN_STILL_SAMPLES && stamp - point->since >= required_ms + point->rejected_ms;
}

static void restore_stationary_pose(app_displacement_estimator_t *est)
{
    memcpy(est->position, est->stopped_estimate.xyz_m, sizeof(est->position));
    memcpy(est->attitude, est->endpoint.attitude, sizeof(est->attitude));
    memset(est->velocity, 0, sizeof(est->velocity));
    memset(est->last_accel, 0, sizeof(est->last_accel));
    memset(est->last_rate, 0, sizeof(est->last_rate));
}

static app_displacement_state_t hold_position(app_displacement_estimator_t *est)
{
    const bool low_confidence = est->result.low_confidence;
    if (!apply_endpoint_correction(est)) return end_without_endpoint(est, APP_DISPLACEMENT_FAULT_ENDPOINT);
    est->result.low_confidence |= low_confidence;
    est->result.stationary = true;
    est->result.resume_pending = false;
    est->stopped_estimate = est->result;
    est->segment_started_ms = est->result.elapsed_ms;
    est->peak_speed = 0.0f;
    est->braking_seen = false;
    restore_stationary_pose(est);
    rotate_vector(est->attitude, est->endpoint.accel_mean, est->rest_linear);
    for (unsigned axis = 0; axis < 3; ++axis) est->rest_linear[axis] -= est->gravity[axis];
    return est->phase;
}

/* Keep small taps out of the visible estimate. The first samples of a real
 * move are integrated provisionally so resuming does not lose displacement. */
static bool resume_motion(app_displacement_estimator_t *est, const float accel[3],
                          const float gyro[3], const float rate[3], uint32_t stamp,
                          uint32_t elapsed, bool slow_rotation)
{
    const float deviation = distance_squared(accel, est->endpoint.accel_mean);
    if (!est->result.resume_pending) return false;
    if (stamp - est->resume_requested_at >= RESUME_WAIT_MS) {
        restore_stationary_pose(est);
        est->resume_ms = est->resume_samples = est->resume_gap_ms = 0;
        est->result.resume_pending = false;
        return false;
    }
    if (!est->resume_ready) {
        if (slow_rotation && deviation <= STILL_ACCEL_MPS2 * STILL_ACCEL_MPS2) est->resume_quiet_ms += elapsed;
        else est->resume_quiet_ms = 0;
        est->resume_ready = est->resume_quiet_ms >= RESUME_REST_MS;
        return false;
    }
    const float threshold = est->resume_ms ? ONSET_ACCEL_MPS2 : RESUME_ACCEL_MPS2;
    if (slow_rotation && deviation <= threshold * threshold) {
        if (est->resume_ms) restore_stationary_pose(est);
        est->resume_ms = est->resume_samples = est->resume_gap_ms = 0;
        /* Update the local gyro zero only after confirming rest. Keep the
         * original start axes and do not apply this to the shared IMU stream. */
        if (deviation <= ONSET_ACCEL_MPS2 * ONSET_ACCEL_MPS2) {
            const float weight = elapsed / (2000.0f + elapsed);
            for (unsigned axis = 0; axis < 3; ++axis)
                est->gyro_zero[axis] += weight * (gyro[axis] - est->gyro_zero[axis]);
        }
        return false;
    }
    integrate(est, accel, rate, elapsed);
    est->resume_ms += elapsed;
    ++est->resume_samples;
    if (elapsed > est->resume_gap_ms) est->resume_gap_ms = elapsed;
    return est->resume_ms >= RESUME_MS;
}

app_displacement_state_t app_displacement_estimator_push(app_displacement_estimator_t *est,
    const float accel[3], const float gyro[3], uint32_t stamp)
{
    if (est->phase == APP_DISPLACEMENT_IDLE || est->phase == APP_DISPLACEMENT_COMPLETE ||
        est->phase == APP_DISPLACEMENT_INVALID) return est->phase;
    uint32_t elapsed = est->have_stamp ? stamp - est->last_stamp : 0;
    if (est->have_stamp && !elapsed) return est->phase;
    if (elapsed > MAX_GAP_MS) {
        if (est->phase != APP_DISPLACEMENT_WAITING_STILL) {
            est->result.max_gap_ms = elapsed;
            return invalidate(est, APP_DISPLACEMENT_FAULT_GAP);
        }
        app_displacement_estimator_reset(est);
        elapsed = 0;
    }
    est->last_stamp = stamp;
    est->have_stamp = true;
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!isfinite(accel[axis]) || !isfinite(gyro[axis])) return invalidate(est, APP_DISPLACEMENT_FAULT_SENSOR);
        /* Near the configured QMI8658A per-axis rails (±4g and ±512 dps). */
        if (fabsf(accel[axis]) >= 39.0f || fabsf(gyro[axis]) >= 495.0f)
            return invalidate(est, APP_DISPLACEMENT_FAULT_SATURATION);
    }
    if (est->phase == APP_DISPLACEMENT_WAITING_STILL) {
        calibrate_start(est, accel, gyro, stamp);
        return est->phase;
    }

    float rate[3];
    for (unsigned axis = 0; axis < 3; ++axis) rate[axis] = gyro[axis] - est->gyro_zero[axis];
    const bool slow_rotation = dot(rate, rate) <= STILL_GYRO_DPS * STILL_GYRO_DPS;
    const bool settling = est->phase == APP_DISPLACEMENT_SETTLING;
    if (settling && stamp - est->finish_since > END_CONFIRM_TIMEOUT_MS) {
        if (est->recording) return end_without_endpoint(est, APP_DISPLACEMENT_FAULT_MOTION_TIMEOUT);
        return invalidate(est, APP_DISPLACEMENT_FAULT_STOP_STILL);
    }
    uint32_t new_samples = 1;
    uint32_t new_gap = elapsed;
    bool provisional_resume = false;
    if (est->result.stationary) {
        if (!resume_motion(est, accel, gyro, rate, stamp, elapsed, slow_rotation)) return est->phase;
        provisional_resume = true;
        elapsed = est->resume_ms;
        new_samples = est->resume_samples;
        new_gap = est->resume_gap_ms;
        est->resume_ms = est->resume_samples = est->resume_gap_ms = 0;
        est->result.stationary = false;
        est->result.resume_pending = false;
        est->result.endpoint_corrected = false;
        est->endpoint.count = est->raw_endpoint.count = 0;
    }
    if (!est->recording && !settling) {
        const bool above_onset = !slow_rotation ||
            distance_squared(accel, est->gravity) > ONSET_ACCEL_MPS2 * ONSET_ACCEL_MPS2;
        if (above_onset) {
            if (!est->onset_pending) {
                est->onset_pending = true;
                est->result.samples = 1;
            }
            if (est->result.elapsed_ms + elapsed >= ONSET_MS) {
                est->recording = true;
                est->onset_pending = false;
                est->phase = APP_DISPLACEMENT_MOVING;
            }
        } else if (est->onset_pending) discard_onset(est);
    } else if (settling && est->onset_pending) discard_onset(est);

    if (est->recording || est->onset_pending) {
        if (!provisional_resume) integrate(est, accel, rate, elapsed);
        est->result.elapsed_ms += elapsed > UINT32_MAX - est->result.elapsed_ms ?
            UINT32_MAX - est->result.elapsed_ms : elapsed;
        if (new_gap > est->result.max_gap_ms) est->result.max_gap_ms = new_gap;
        est->result.samples += new_samples > UINT32_MAX - est->result.samples ?
            UINT32_MAX - est->result.samples : new_samples;
        memcpy(est->result.xyz_m, est->position, sizeof(est->result.xyz_m));
        est->result.distance_m = norm(est->result.xyz_m);
        if (!isfinite(est->result.distance_m)) return invalidate(est, APP_DISPLACEMENT_FAULT_SENSOR);
        est->result.low_confidence = est->stopped_estimate.low_confidence || est->result.elapsed_ms > DRIFT_WARNING_MS ||
            est->result.max_gap_ms > PREFERRED_GAP_MS;
    } else if (!est->recording) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            est->last_rate[axis] = rate[axis];
            est->last_accel[axis] = accel[axis] - est->gravity[axis];
        }
    }

    if (!est->recording && !settling) return est->phase;
    const bool near_gravity = fabsf(norm(accel) - norm(est->gravity)) <= END_GRAVITY_TOLERANCE;
    const float linear_squared = dot(est->last_accel, est->last_accel);
    bool fast = true;
    bool coasting = false;
    if (est->recording) {
        const float speed = norm(est->velocity);
        if (speed > est->peak_speed) est->peak_speed = speed;
        if (speed > AUTO_STOP_SPEED_MPS && dot(est->velocity, est->last_accel) < -0.12f * speed) {
            est->last_brake_stamp = stamp;
            est->braking_seen = true;
        }
        const bool recent_braking = est->braking_seen && stamp - est->last_brake_stamp <= BRAKING_EVIDENCE_MS;
        const bool braking_stop = recent_braking &&
            speed <= fminf(BRAKING_STOP_SPEED_MPS, 0.40f * est->peak_speed);
        const float linear_limit = settling ? FINISH_LINEAR_LIMIT_MPS2 :
            (braking_stop ? BRAKING_STOP_LINEAR_MPS2 : END_LINEAR_LIMIT_MPS2);
        fast = linear_squared <= linear_limit * linear_limit &&
            (settling || braking_stop || speed <= AUTO_STOP_SPEED_MPS);
        coasting = !settling && !recent_braking && speed > BRAKING_STOP_SPEED_MPS &&
            linear_squared <= END_LINEAR_LIMIT_MPS2 * END_LINEAR_LIMIT_MPS2;
    }
    const bool tolerate_noise = linear_squared > BRAKING_STOP_LINEAR_MPS2 * BRAKING_STOP_LINEAR_MPS2;
    const bool fallback_rate = dot(rate, rate) <= FALLBACK_GYRO_LIMIT_DPS * FALLBACK_GYRO_LIMIT_DPS;
    /* A smooth glide can produce the same raw samples as rest. Clear its
     * candidate immediately so neither preview nor automatic lock rewinds it. */
    if (coasting) est->raw_endpoint.count = 0;
    /* An explicit Finish supplies the stop intent missing from automatic
     * detection during a glide. Keep all of the raw stability checks. */
    const bool raw_still = track_endpoint(est, &est->raw_endpoint, accel, rate, stamp,
        !coasting && near_gravity && fallback_rate, false, tolerate_noise, settling ? END_STILL_MS : FALLBACK_STILL_MS);
    const bool fast_still = track_endpoint(est, &est->endpoint, accel, rate, stamp,
        near_gravity && slow_rotation && fast, true, false, END_STILL_MS);
    if (fast_still || raw_still) {
        if (!fast_still) {
            est->endpoint = est->raw_endpoint;
            est->result.low_confidence = true;
        }
        if (settling) return publish_result(est);
        return hold_position(est);
    } else if (!settling && est->result.elapsed_ms - est->segment_started_ms >= MAX_CORRECTION_MS) {
        return end_without_endpoint(est, APP_DISPLACEMENT_FAULT_MOTION_TIMEOUT);
    }
    return est->phase;
}
