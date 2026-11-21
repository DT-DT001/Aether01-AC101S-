#include "app_air_mouse.h"

#include <math.h>

static float dot(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void normalize(float v[3])
{
    const float norm = sqrtf(dot(v, v));
    if (norm < 0.1f) {
        v[0] = v[1] = 0;
        v[2] = 1;
        return;
    }
    for (unsigned axis = 0; axis < 3; ++axis) v[axis] /= norm;
}

static int8_t pixels(app_air_mouse_t *mouse, unsigned axis, float rate, float dt)
{
    const float deadband = 0.65f;
    const float effective = copysignf(fmaxf(0, fabsf(rate) - deadband), rate);
    mouse->velocity[axis] += dt / (dt + 0.035f) * (effective - mouse->velocity[axis]);
    if (effective == 0 && fabsf(mouse->velocity[axis]) < deadband) {
        mouse->velocity[axis] = mouse->fraction[axis] = 0;
        return 0;
    }
    const float gain = 8.0f + fminf(4.0f, fabsf(effective) * 0.04f);
    const float total = mouse->fraction[axis] + mouse->velocity[axis] * dt * gain;
    const int8_t whole = (int8_t)fmaxf(-127, fminf(127, total));
    mouse->fraction[axis] = fabsf(total) < 127 ? total - whole : 0;
    return whole;
}

bool app_air_mouse_update(app_air_mouse_t *mouse, const float accel[3],
                          const float gyro[3], bool stationary, uint32_t stamp,
                          int8_t *dx, int8_t *dy)
{
    *dx = *dy = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!isfinite(accel[axis]) || !isfinite(gyro[axis]) ||
            fabsf(accel[axis]) > 50 || fabsf(gyro[axis]) > 520) {
            *mouse = (app_air_mouse_t){0};
            return false;
        }
    }
    const uint32_t elapsed = stamp - mouse->stamp;
    if (mouse->ready && !elapsed) return false;
    if (!mouse->ready || elapsed > 80) {
        *mouse = (app_air_mouse_t){.up = {accel[0], accel[1], accel[2]},
                                   .stamp = stamp, .ready = true};
        normalize(mouse->up);
        return false;
    }
    mouse->stamp = stamp;
    if (stationary) {
        mouse->velocity[0] = mouse->velocity[1] = 0;
        mouse->fraction[0] = mouse->fraction[1] = 0;
    }
    const float magnitude = sqrtf(dot(accel, accel));
    const float dt = elapsed * 0.001f;
    if (magnitude >= 8.3f && magnitude <= 11.3f) {
        const float blend = dt / (dt + 0.12f);
        for (unsigned axis = 0; axis < 3; ++axis)
            mouse->up[axis] += blend * (accel[axis] / magnitude - mouse->up[axis]);
        normalize(mouse->up);
    }
    if (stationary) return false;
    /* The right vector lies in the display plane. With the display flat,
     * its projection of gravity is degenerate, so use the board X axis. */
    const float horizontal = hypotf(mouse->up[0], mouse->up[1]);
    const float pitch = horizontal > 0.25f ?
        (gyro[0]*mouse->up[1] - gyro[1]*mouse->up[0]) / horizontal : gyro[0];
    *dx = pixels(mouse, 0, -dot(gyro, mouse->up), dt);
    *dy = pixels(mouse, 1, -pitch, dt);
    return *dx != 0 || *dy != 0;
}
