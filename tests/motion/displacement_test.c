#include "app_displacement_estimator.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static const float GRAVITY = 9.80665f;
static const float radians = 0.01745329252f;
static const float zero_gyro[3];

static app_displacement_state_t feed(app_displacement_estimator_t *est, uint32_t *stamp,
                                     uint32_t elapsed, const float accel[3], const float gyro[3])
{
    *stamp += elapsed;
    return app_displacement_estimator_push(est, accel, gyro, *stamp);
}

static void ready(app_displacement_estimator_t *est, uint32_t *stamp,
                  const float accel[3], const float gyro[3])
{
    app_displacement_estimator_reset(est);
    assert(!app_displacement_estimator_finish(est));
    for (unsigned i = 0; i < 66; ++i)
        assert(feed(est, stamp, 10, accel, gyro) == (i < 65 ? APP_DISPLACEMENT_WAITING_STILL : APP_DISPLACEMENT_READY));
}

static void settle(app_displacement_estimator_t *est, uint32_t *stamp,
                   const float accel[3], const float gyro[3])
{
    assert(app_displacement_estimator_finish(est));
    assert(app_displacement_estimator_finish(est));
    const app_displacement_result_t preview = app_displacement_estimator_preview(est);
    for (unsigned i = 0; i < 190 && est->phase == APP_DISPLACEMENT_SETTLING; ++i) {
        feed(est, stamp, 10, accel, gyro);
        if (est->phase != APP_DISPLACEMENT_SETTLING) break;
        const app_displacement_result_t current = app_displacement_estimator_preview(est);
        assert(current.preview_held && current.distance_m == preview.distance_m);
        for (unsigned axis = 0; axis < 3; ++axis) assert(current.xyz_m[axis] == preview.xyz_m[axis]);
    }
    assert(est->phase == APP_DISPLACEMENT_COMPLETE);
}

static void rearm(app_displacement_estimator_t *est, uint32_t *stamp,
                  const float accel[3], const float gyro[3])
{
    assert(app_displacement_estimator_resume(est));
    assert(!app_displacement_estimator_resume(est));
    for (unsigned i = 0; i < 30; ++i) {
        assert(feed(est, stamp, 10, accel, gyro) == APP_DISPLACEMENT_MOVING);
        assert(est->result.stationary && est->result.resume_pending);
    }
}

static void stationary(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = UINT32_MAX - 200;
    const float accel[3] = {0.3603f, 0.6476f, 9.4978f};
    const float gyro[3] = {0.15f, -0.06f, 7.3f};
    ready(&est, &stamp, accel, gyro);
    /* A user can wait beyond the motion time bound before moving. */
    const float tap[3] = {1.3603f, 0.6476f, 9.4978f};
    for (unsigned i = 0; i < 3; ++i) assert(feed(&est, &stamp, 10, tap, gyro) == APP_DISPLACEMENT_READY);
    for (unsigned i = 0; i < 900; ++i)
        assert(feed(&est, &stamp, 10, accel, gyro) == APP_DISPLACEMENT_READY);
    settle(&est, &stamp, accel, gyro);
    assert(est.result.distance_m == 0.0f && est.result.end_speed_mps == 0.0f);
    assert(est.result.low_confidence && est.result.elapsed_ms == 0 && est.result.fault == APP_DISPLACEMENT_FAULT_NONE);
    puts("PASS displacement stationary acquisition with 7 dps offset, isolated tap, timestamp wrap and explicit zero check");
}

static float pulse(unsigned i)
{
    if (i <= 10) return i * 0.2f;
    if (i <= 50) return 2.0f;
    return (60 - i) * 0.2f;
}

static void translation(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float gravity[3] = {0.15f, 0.02f, GRAVITY - 0.2f};
    const float gyro[3] = {0.15f, -0.25f, 0.07f};
    ready(&est, &stamp, gravity, gyro);
    for (unsigned i = 1; i <= 60; ++i) {
        const float a[3] = {gravity[0] + pulse(i), gravity[1], gravity[2]};
        feed(&est, &stamp, 10, a, gyro);
    }
    /* Coast for one second at 1 m/s. Acceleration and gyro both look still. */
    for (unsigned i = 0; i < 100; ++i) {
        assert(feed(&est, &stamp, 10, gravity, gyro) == APP_DISPLACEMENT_MOVING);
        assert(!est.result.stationary);
    }
    for (unsigned i = 1; i <= 60; ++i) {
        const float a[3] = {gravity[0] - pulse(i), gravity[1], gravity[2]};
        assert(feed(&est, &stamp, 10, a, gyro) == APP_DISPLACEMENT_MOVING);
    }
    settle(&est, &stamp, gravity, gyro);
    assert(fabsf(est.result.xyz_m[0] - 1.6f) < 0.015f);
    assert(fabsf(est.result.xyz_m[1]) < 0.001f && fabsf(est.result.xyz_m[2]) < 0.001f);
    assert(fabsf(est.result.distance_m - 1.6f) < 0.015f && est.result.end_speed_mps < 0.01f);
    assert(est.result.elapsed_ms >= 2140 && est.result.elapsed_ms <= 2210);
    printf("PASS displacement 1.600 m synthetic translation measured %.5f m, stop tail excluded\n", est.result.distance_m);

    ready(&est, &stamp, gravity, gyro);
    for (unsigned i = 1; i <= 60; ++i) {
        const float a[3] = {gravity[0] + 0.06f * pulse(i), gravity[1], gravity[2]};
        feed(&est, &stamp, 10, a, gyro);
    }
    assert(est.phase == APP_DISPLACEMENT_MOVING);
    for (unsigned i = 0; i < 100; ++i) {
        assert(feed(&est, &stamp, 10, gravity, gyro) == APP_DISPLACEMENT_MOVING);
        assert(!est.result.stationary);
    }
    for (unsigned i = 1; i <= 60; ++i) {
        const float a[3] = {gravity[0] - 0.06f * pulse(i), gravity[1], gravity[2]};
        feed(&est, &stamp, 10, a, gyro);
    }
    for (unsigned i = 0; i < 80; ++i)
        assert(feed(&est, &stamp, 10, gravity, gyro) == APP_DISPLACEMENT_MOVING);
    assert(est.result.stationary);
    assert(app_displacement_estimator_finish(&est) && est.phase == APP_DISPLACEMENT_COMPLETE);
    assert(fabsf(est.result.distance_m - 0.096f) < 0.015f);
    printf("PASS displacement 0.12 m/s² peak smooth translation, 0.096 m actual, %.5f m estimated\n", est.result.distance_m);
}

static void hold_and_resume(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float level[3] = {0, 0, GRAVITY};
    const float stop[3] = {0.10f, 0, GRAVITY};
    const float bump[3] = {2.2f, 0, GRAVITY};
    ready(&est, &stamp, level, zero_gyro);
    for (unsigned i = 1; i <= 120; ++i) {
        const float accel[3] = {stop[0] + (i <= 60 ? pulse(i) : -pulse(i - 60)), 0, GRAVITY};
        feed(&est, &stamp, 10, accel, zero_gyro);
    }
    assert(!est.result.stationary && est.velocity[0] > 0.10f);
    for (unsigned i = 0; i < 70; ++i)
        assert(feed(&est, &stamp, 10, stop, zero_gyro) == APP_DISPLACEMENT_MOVING);
    assert(est.result.stationary && est.result.endpoint_corrected);
    assert(fabsf(est.result.xyz_m[0] - 0.60f) < 0.025f && est.velocity[0] == 0.0f);
    const app_displacement_result_t first_stop = est.result;
    for (unsigned i = 0; i < 30; ++i) feed(&est, &stamp, 10, bump, zero_gyro);
    assert(est.result.stationary && !est.result.resume_pending && est.result.distance_m == first_stop.distance_m);
    for (unsigned i = 0; i < 1000; ++i) {
        const float accel[3] = {stop[0] + (i & 1 ? 0.015f : -0.015f), 0, GRAVITY};
        feed(&est, &stamp, 10, accel, zero_gyro);
        assert(est.result.stationary && est.result.distance_m == first_stop.distance_m);
        assert(est.result.elapsed_ms == first_stop.elapsed_ms);
    }
    rearm(&est, &stamp, stop, zero_gyro);
    for (unsigned i = 0; i < 1200; ++i) feed(&est, &stamp, 10, stop, zero_gyro);
    assert(est.result.stationary && !est.result.resume_pending && est.result.distance_m == first_stop.distance_m);
    rearm(&est, &stamp, stop, zero_gyro);
    for (unsigned i = 1; i <= 120; ++i) {
        const float accel[3] = {stop[0], i <= 60 ? pulse(i) : -pulse(i - 60), GRAVITY};
        feed(&est, &stamp, 10, accel, zero_gyro);
    }
    assert(!est.result.stationary && est.result.xyz_m[1] > 0.55f);
    for (unsigned i = 0; i < 70; ++i) feed(&est, &stamp, 10, stop, zero_gyro);
    assert(est.result.stationary && fabsf(est.result.xyz_m[1] - 0.60f) < 0.015f);
    assert(fabsf(est.result.xyz_m[0] - first_stop.xyz_m[0]) < 0.015f);
    const float final_distance = est.result.distance_m;
    assert(app_displacement_estimator_finish(&est) && est.phase == APP_DISPLACEMENT_COMPLETE);
    assert(est.result.stationary && est.result.endpoint_corrected && est.result.distance_m == final_distance);
    puts("PASS displacement with residual speed holds for 10s despite sensor noise and a tap, then resumes on the original XYZ axes");
}

static void late_finish(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float rest[3] = {0, 0, GRAVITY};
    const float tap[3] = {2.2f, 0, GRAVITY};
    const float tap_gyro[3] = {0, 6.0f, 0};
    ready(&est, &stamp, rest, zero_gyro);
    for (unsigned i = 1; i <= 60; ++i) {
        const float accel[3] = {pulse(i), 0, GRAVITY};
        feed(&est, &stamp, 10, accel, zero_gyro);
    }
    for (unsigned i = 1; i <= 60; ++i) {
        const float accel[3] = {-pulse(i), 0, GRAVITY};
        feed(&est, &stamp, 10, accel, zero_gyro);
    }
    for (unsigned i = 0; i < 850; ++i)
        assert(feed(&est, &stamp, 10, rest, zero_gyro) == APP_DISPLACEMENT_MOVING);
    for (unsigned i = 0; i < 9; ++i)
        assert(feed(&est, &stamp, 10, tap, tap_gyro) == APP_DISPLACEMENT_MOVING);
    assert(app_displacement_estimator_finish(&est) && est.phase == APP_DISPLACEMENT_COMPLETE);
    assert(feed(&est, &stamp, 10, tap, tap_gyro) == APP_DISPLACEMENT_COMPLETE);
    assert(est.result.elapsed_ms < 1250);
    assert(fabsf(est.result.distance_m - 0.60f) < 0.015f);
    puts("PASS displacement can confirm a settled endpoint after the integration limit and ignore the Finish tap");
}

static void rotate(bool rate_error)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float level[3] = {0, 0, GRAVITY};
    ready(&est, &stamp, level, zero_gyro);
    float angle = 0.0f;
    float previous = 0.0f;
    for (unsigned i = 1; i <= 200; ++i) {
        const float speed = i < 20 ? i * 2.5f : i <= 180 ? 50.0f : (200 - i) * 2.5f;
        angle += (previous + speed) * (0.005f * radians);
        previous = speed;
        const float gyro[3] = {0, speed + (rate_error ? 0.4f : 0.0f), 0};
        const float accel[3] = {-sinf(angle) * GRAVITY, 0, cosf(angle) * GRAVITY};
        feed(&est, &stamp, 10, accel, gyro);
    }
    const float stopped[3] = {-sinf(angle) * GRAVITY, 0, cosf(angle) * GRAVITY};
    const float rest_gyro[3] = {0, rate_error ? 0.4f : 0.0f, 0};
    settle(&est, &stamp, stopped, rest_gyro);
    assert(est.result.distance_m < (rate_error ? 0.025f : 0.001f));
    assert(est.result.endpoint_error_mps2 < (rate_error ? 0.20f : 0.001f));
    printf("PASS displacement rotation in place%s, synthetic residual %.5f m\n",
           rate_error ? " with 0.4 deg/s drift" : "", est.result.distance_m);
}

static void high_residual_stop_and_reverse(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = UINT32_MAX - 1200;
    const float rest[3] = {0, 0, GRAVITY};
    const float biased_rest[3] = {0.45f, -0.12f, GRAVITY + 0.04f};
    ready(&est, &stamp, rest, zero_gyro);
    for (unsigned i = 1; i <= 120; ++i) {
        const float accel[3] = {biased_rest[0] + (i <= 60 ? pulse(i) : -pulse(i - 60)),
                                biased_rest[1], biased_rest[2]};
        feed(&est, &stamp, 10, accel, zero_gyro);
    }
    assert(est.velocity[0] > 0.5f && !est.result.stationary);
    for (unsigned i = 0; i < 195; ++i) {
        const float jitter[3] = {biased_rest[0] + (i & 1 ? 0.08f : -0.08f),
                                 biased_rest[1], biased_rest[2]};
        feed(&est, &stamp, 10, jitter, zero_gyro);
    }
    assert(est.result.stationary && est.result.low_confidence && est.result.endpoint_corrected);
    assert(fabsf(est.result.xyz_m[0] - 0.60f) < 0.04f);
    const app_displacement_result_t checkpoint = est.result;
    for (unsigned i = 0; i < 3000; ++i) {
        const float accel[3] = {biased_rest[0] + (i < 200 ? 0.25f : i & 1 ? 0.12f : -0.12f),
                                biased_rest[1], biased_rest[2]};
        feed(&est, &stamp, 10, accel, zero_gyro);
        assert(est.result.stationary && est.result.elapsed_ms == checkpoint.elapsed_ms);
        assert(est.result.distance_m == checkpoint.distance_m);
        for (unsigned axis = 0; axis < 3; ++axis)
            assert(est.result.xyz_m[axis] == checkpoint.xyz_m[axis] && est.velocity[axis] == 0.0f);
    }
    rearm(&est, &stamp, biased_rest, zero_gyro);
    for (unsigned i = 1; i <= 120; ++i) {
        const float accel[3] = {biased_rest[0] + 2.0f * (i <= 60 ? -pulse(i) : pulse(i - 60)),
                                biased_rest[1], biased_rest[2]};
        feed(&est, &stamp, 10, accel, zero_gyro);
    }
    for (unsigned i = 0; i < 80; ++i) feed(&est, &stamp, 10, biased_rest, zero_gyro);
    assert(est.result.stationary && fabsf(est.result.xyz_m[0] + 0.60f) < 0.05f);
    assert(fabsf(est.result.xyz_m[1]) < 0.02f && fabsf(est.result.xyz_m[2]) < 0.02f);
    puts("PASS displacement holds all axes for 30 s despite large residual speed and resumes with signed reversal");
}

static void unconfirmed_movement_limit(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float rest[3] = {0, 0, GRAVITY};
    const float motion[3] = {4.0f, 0, GRAVITY};
    ready(&est, &stamp, rest, zero_gyro);
    for (unsigned i = 0; i < 4; ++i)
        assert(feed(&est, &stamp, 10, motion, zero_gyro) == APP_DISPLACEMENT_READY);
    for (unsigned i = 4; i < 799; ++i)
        assert(feed(&est, &stamp, 10, motion, zero_gyro) == APP_DISPLACEMENT_MOVING);
    assert(feed(&est, &stamp, 10, motion, zero_gyro) == APP_DISPLACEMENT_INVALID);
    assert(est.result.elapsed_ms == 8000 && est.result.fault == APP_DISPLACEMENT_FAULT_MOTION_TIMEOUT);
    for (unsigned i = 0; i < 100; ++i)
        assert(feed(&est, &stamp, 10, motion, zero_gyro) == APP_DISPLACEMENT_INVALID);
    puts("PASS continuous motion without any stop evidence is invalidated at the deadline");
}

static void freeze_preview_with_gravity_leakage(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = UINT32_MAX - 1500;
    const float level[3] = {0, 0, GRAVITY};
    const float tilt = 5.0f * radians;
    const float rest[3] = {GRAVITY * sinf(tilt), 0, GRAVITY * cosf(tilt)};
    ready(&est, &stamp, level, zero_gyro);
    for (unsigned i = 1; i <= 120; ++i) {
        const float accel[3] = {rest[0] + (i <= 60 ? pulse(i) : -pulse(i - 60)), 0, rest[2]};
        feed(&est, &stamp, 10, accel, zero_gyro);
    }
    for (unsigned i = 0; i < 38; ++i) feed(&est, &stamp, 10, rest, zero_gyro);
    assert(est.last_accel[0] > 0.75f && !est.result.stationary);
    const app_displacement_result_t preview = app_displacement_estimator_preview(&est);
    assert(preview.preview_held);
    for (unsigned i = 0; i < 100; ++i) {
        const float jitter[3] = {rest[0] + (i & 1 ? 0.055f : -0.055f), 0, rest[2]};
        feed(&est, &stamp, 10, jitter, zero_gyro);
        const app_displacement_result_t held = app_displacement_estimator_preview(&est);
        assert(held.preview_held && held.distance_m == preview.distance_m);
        for (unsigned axis = 0; axis < 3; ++axis) assert(held.xyz_m[axis] == preview.xyz_m[axis]);
    }
    for (unsigned i = 0; i < 75; ++i) feed(&est, &stamp, 10, rest, zero_gyro);
    assert(est.result.stationary && est.result.endpoint_corrected && est.result.low_confidence);
    assert(fabsf(est.result.distance_m - 0.60f) < 0.12f);
    puts("PASS provisional readings stay fixed at a raw IMU stop with 5 degrees of lost tilt compensation");
}

static void biased_gyro_at_stop(uint32_t dt_ms)
{
    app_displacement_estimator_t est;
    uint32_t stamp = UINT32_MAX - 1000;
    const float level[3] = {0, 0, 9.51f};
    const float bias[3] = {0, 0, 7.30f};
    const float parked[3] = {0.34f, 0, 9.50f};
    ready(&est, &stamp, level, bias);
    for (unsigned i = 1; i <= 120; ++i) {
        const float a[3] = {parked[0] + (i <= 60 ? pulse(i) : -pulse(i - 60)), 0, parked[2]};
        feed(&est, &stamp, 10, a, bias);
    }
    const uint32_t parked_at = stamp;
    app_displacement_result_t frozen = {0};
    for (unsigned i = 0; i < 2150U / dt_ms; ++i) {
        const uint32_t elapsed = i * dt_ms;
        const bool spike = elapsed >= 620U && elapsed < 640U;
        const float a[3] = {parked[0] + (spike ? 0.31f : (i & 1 ? 0.055f : -0.055f)), 0, parked[2]};
        const float w[3] = {0, 0, bias[2] + (spike ? 3.4f : (i & 1 ? 1.15f : 1.75f))};
        assert(feed(&est, &stamp, dt_ms, a, w) == APP_DISPLACEMENT_MOVING);
        if (stamp - parked_at < 350U || est.result.stationary) continue;
        const app_displacement_result_t preview = app_displacement_estimator_preview(&est);
        assert(preview.preview_held);
        if (!frozen.preview_held) frozen = preview;
        assert(preview.distance_m == frozen.distance_m);
    }
    assert(frozen.preview_held && est.result.stationary && est.result.endpoint_corrected);
    assert(est.result.low_confidence && fabsf(est.result.xyz_m[0] - 0.60f) < 0.08f);
    printf("PASS %lu ms samples tolerate biased gyro and a 20 ms spike while stopping\n", (unsigned long)dt_ms);
}

static void slow_tilt_is_not_rest(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float level[3] = {0, 0, GRAVITY};
    const float gyro[3] = {0, 2.0f, 0};
    ready(&est, &stamp, level, zero_gyro);
    for (unsigned i = 1; i <= 350; ++i) {
        const float angle = i * 0.02f * radians;
        const float accel[3] = {-sinf(angle) * GRAVITY, 0, cosf(angle) * GRAVITY};
        feed(&est, &stamp, 10, accel, gyro);
        assert(!est.result.stationary);
    }
    assert(est.phase == APP_DISPLACEMENT_MOVING);
    puts("PASS gradual tilt cannot satisfy a fixed gravity-vector rest window");
}

static void rejected_endpoint_keeps_last_stop(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float level[3] = {0, 0, GRAVITY};
    const float hard_motion[3] = {8, 0, GRAVITY};
    ready(&est, &stamp, level, zero_gyro);
    for (unsigned i = 1; i <= 120; ++i) {
        const float accel[3] = {i <= 60 ? pulse(i) : -pulse(i - 60), 0, GRAVITY};
        feed(&est, &stamp, 10, accel, zero_gyro);
    }
    for (unsigned i = 0; i < 100 && !est.result.stationary; ++i)
        feed(&est, &stamp, 10, level, zero_gyro);
    assert(est.result.stationary && est.result.endpoint_corrected);
    const app_displacement_result_t previous = est.result;

    rearm(&est, &stamp, level, zero_gyro);
    for (unsigned i = 0; i < 50; ++i) feed(&est, &stamp, 10, hard_motion, zero_gyro);
    assert(!est.result.stationary && est.result.distance_m > previous.distance_m);
    settle(&est, &stamp, level, zero_gyro);
    assert(est.result.fault == APP_DISPLACEMENT_FAULT_ENDPOINT);
    assert(est.result.measurement_limited && est.result.low_confidence && est.result.stationary);
    assert(est.result.end_speed_mps > 2.0f);
    assert(est.result.distance_m == previous.distance_m && est.result.elapsed_ms == previous.elapsed_ms);
    for (unsigned axis = 0; axis < 3; ++axis) assert(est.result.xyz_m[axis] == previous.xyz_m[axis]);
    puts("PASS rejected endpoint discards current segment and keeps the previous confirmed three-axis reading");
}

static void faults(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float level[3] = {0, 0, GRAVITY};
    const float motion[3] = {1, 0, GRAVITY};
    ready(&est, &stamp, level, zero_gyro);
    assert(feed(&est, &stamp, 90, level, zero_gyro) == APP_DISPLACEMENT_INVALID);
    assert(est.result.fault == APP_DISPLACEMENT_FAULT_GAP);
    ready(&est, &stamp, level, zero_gyro);
    for (unsigned i = 0; i < 5; ++i) feed(&est, &stamp, 10, motion, zero_gyro);
    assert(est.phase == APP_DISPLACEMENT_MOVING);
    assert(feed(&est, &stamp, 90, motion, zero_gyro) == APP_DISPLACEMENT_INVALID);
    assert(est.result.fault == APP_DISPLACEMENT_FAULT_GAP);

    ready(&est, &stamp, level, zero_gyro);
    const float saturation[3] = {0, 0, 39.1f};
    assert(feed(&est, &stamp, 10, saturation, zero_gyro) == APP_DISPLACEMENT_INVALID);
    assert(est.result.fault == APP_DISPLACEMENT_FAULT_SATURATION);

    ready(&est, &stamp, level, zero_gyro);
    assert(feed(&est, &stamp, 50, motion, zero_gyro) == APP_DISPLACEMENT_MOVING);
    settle(&est, &stamp, level, zero_gyro);
    assert(est.result.low_confidence && est.result.max_gap_ms == 50);

    ready(&est, &stamp, level, zero_gyro);
    const float sustained_motion[3] = {8, 0, GRAVITY};
    for (unsigned i = 0; i < 40; ++i) feed(&est, &stamp, 10, sustained_motion, zero_gyro);
    assert(est.phase == APP_DISPLACEMENT_MOVING);
    assert(est.result.distance_m > 0.5f);
    assert(app_displacement_estimator_finish(&est));
    for (unsigned i = 0; i < 200 && est.phase == APP_DISPLACEMENT_SETTLING; ++i)
        feed(&est, &stamp, 10, level, zero_gyro);
    assert(est.phase == APP_DISPLACEMENT_INVALID);
    assert(est.result.fault == APP_DISPLACEMENT_FAULT_ENDPOINT && est.result.end_speed_mps > 2.0f);
    assert(est.result.low_confidence && !est.result.endpoint_corrected && est.result.distance_m == 0.0f);
    for (unsigned axis = 0; axis < 3; ++axis) assert(est.result.xyz_m[axis] == 0.0f);

    ready(&est, &stamp, level, zero_gyro);
    assert(app_displacement_estimator_finish(&est));
    const float turning[3] = {0, 8, 0};
    for (unsigned i = 0; i < 400; ++i)
        assert(feed(&est, &stamp, 10, level, turning) == APP_DISPLACEMENT_SETTLING);
    assert(feed(&est, &stamp, 10, level, turning) == APP_DISPLACEMENT_INVALID);
    assert(est.result.fault == APP_DISPLACEMENT_FAULT_STOP_STILL);

    ready(&est, &stamp, level, zero_gyro);
    for (unsigned i = 0; i < 10; ++i) feed(&est, &stamp, 10, motion, zero_gyro);
    assert(app_displacement_estimator_finish(&est));
    for (unsigned i = 0; i < 400; ++i)
        assert(feed(&est, &stamp, 10, sustained_motion, zero_gyro) == APP_DISPLACEMENT_SETTLING);
    assert(feed(&est, &stamp, 10, sustained_motion, zero_gyro) == APP_DISPLACEMENT_INVALID);
    assert(est.result.fault == APP_DISPLACEMENT_FAULT_MOTION_TIMEOUT);
    puts("PASS displacement dropout, saturation and sampling quality; failed first endpoint discards the raw estimate");
}

static void correct_constant_gyro_drift_on_finish(void)
{
    app_displacement_estimator_t est;
    uint32_t stamp = 0;
    const float level[3] = {0, 0, GRAVITY};
    const float rest_rate[3] = {0, 1.0f, 0};
    const float accel_y = 0.30f;
    const float expected_y = accel_y * 1.7f * 1.7f;
    ready(&est, &stamp, level, zero_gyro);
    /* The board never rotates. The gyro acquires a constant pitch offset
     * during a signed Y translation, creating false X travel over time. */
    for (unsigned i = 0; i < 680; ++i) {
        const float accel[3] = {0, i < 340 ? -accel_y : accel_y, GRAVITY};
        feed(&est, &stamp, 5, accel, rest_rate);
    }
    assert(est.phase == APP_DISPLACEMENT_MOVING && !est.result.stationary);
    for (unsigned i = 0; i < 36; ++i) feed(&est, &stamp, 5, level, rest_rate);
    assert(est.phase == APP_DISPLACEMENT_MOVING && !est.result.stationary);
    assert(est.result.xyz_m[0] > 1.0f);
    assert(app_displacement_estimator_finish(&est));
    const uint32_t finish_stamp = stamp;
    for (unsigned i = 0; i < 70 && est.phase == APP_DISPLACEMENT_SETTLING; ++i)
        feed(&est, &stamp, 5, level, rest_rate);
    assert(est.phase == APP_DISPLACEMENT_COMPLETE && stamp - finish_stamp < 400U);
    assert(est.result.endpoint_corrected && est.result.low_confidence);
    assert(est.result.endpoint_error_mps2 > 0.30f);
    assert(fabsf(est.result.xyz_m[0]) < 0.05f && fabsf(est.result.xyz_m[2]) < 0.05f);
    assert(fabsf(est.result.xyz_m[1] + expected_y) < 0.025f);
    puts("PASS Finish reuses the quiet window and removes large gyro-driven tilt drift from signed translation");
}

int main(void)
{
    stationary();
    translation();
    hold_and_resume();
    late_finish();
    rotate(false);
    rotate(true);
    high_residual_stop_and_reverse();
    unconfirmed_movement_limit();
    freeze_preview_with_gravity_leakage();
    biased_gyro_at_stop(10);
    biased_gyro_at_stop(5);
    slow_tilt_is_not_rest();
    rejected_endpoint_keeps_last_stop();
    correct_constant_gyro_drift_on_finish();
    faults();
    return 0;
}
