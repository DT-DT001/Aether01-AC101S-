#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_DISPLACEMENT_IDLE,
    APP_DISPLACEMENT_WAITING_STILL,
    APP_DISPLACEMENT_READY,
    APP_DISPLACEMENT_MOVING,
    APP_DISPLACEMENT_SETTLING,
    APP_DISPLACEMENT_COMPLETE,
    APP_DISPLACEMENT_INVALID,
} app_displacement_state_t;

typedef enum {
    APP_DISPLACEMENT_FAULT_NONE,
    APP_DISPLACEMENT_FAULT_SENSOR,
    APP_DISPLACEMENT_FAULT_TIMEOUT,
    APP_DISPLACEMENT_FAULT_GAP,
    APP_DISPLACEMENT_FAULT_SATURATION,
    APP_DISPLACEMENT_FAULT_ENDPOINT,
    APP_DISPLACEMENT_FAULT_START_STILL,
    APP_DISPLACEMENT_FAULT_WAIT_MOTION,
    APP_DISPLACEMENT_FAULT_MOTION_TIMEOUT,
    APP_DISPLACEMENT_FAULT_STOP_STILL,
} app_displacement_fault_t;

typedef struct {
    float xyz_m[3], distance_m;
    float end_speed_mps; /* Before the zero-velocity constraint. */
    float endpoint_error_mps2;
    uint32_t elapsed_ms, samples, max_gap_ms;
    app_displacement_fault_t fault;
    bool low_confidence, endpoint_corrected, stationary, measurement_limited, resume_pending, preview_held;
} app_displacement_result_t;

typedef struct {
    float accel_first[3], accel_mean[3], gyro_mean[3], attitude[4], position[3], velocity[3];
    uint32_t since, count, elapsed_ms, samples, max_gap_ms;
    uint32_t last_stamp, last_good_stamp, rejected_ms;
} app_displacement_endpoint_t;

/* One owner feeds real sample timestamps and reads the result. XYZ stays in
 * the sensor axes at the initial stationary point. */
typedef struct {
    app_displacement_state_t phase;
    app_displacement_result_t result;
    float attitude[4];
    float gravity[3], gyro_zero[3];
    float still_reference[3], gyro_reference[3];
    app_displacement_endpoint_t endpoint, raw_endpoint;
    app_displacement_result_t finish_estimate, stopped_estimate;
    float last_accel[3], last_rate[3], velocity[3], position[3];
    float rest_linear[3], peak_speed;
    uint32_t last_stamp, still_since, finish_since;
    uint32_t still_samples;
    uint32_t segment_started_ms, last_brake_stamp, resume_ms, resume_samples, resume_gap_ms;
    uint32_t resume_requested_at, resume_quiet_ms;
    bool have_stamp, onset_pending, recording, braking_seen, resume_ready;
} app_displacement_estimator_t;

void app_displacement_estimator_reset(app_displacement_estimator_t *est);

/* Request endpoint confirmation after the operator has stopped. */
bool app_displacement_estimator_finish(app_displacement_estimator_t *est);
bool app_displacement_estimator_resume(app_displacement_estimator_t *est);
app_displacement_result_t app_displacement_estimator_preview(const app_displacement_estimator_t *est);

app_displacement_state_t app_displacement_estimator_push(app_displacement_estimator_t *est,
    const float accel_mps2[3], const float gyro_raw_dps[3], uint32_t stamp_ms);
