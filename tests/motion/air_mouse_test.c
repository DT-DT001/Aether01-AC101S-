#include "app_air_mouse.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    app_air_mouse_t mouse = {0};
    const float upright[] = {0, 9.80665f, 0};
    float gyro[] = {0.43f, -0.38f, 0.26f};
    int8_t dx, dy;
    for (uint32_t t = 0; t < 120000; t += 20) {
        assert(!app_air_mouse_update(&mouse, upright, gyro, false, t, &dx, &dy));
        assert(dx == 0 && dy == 0);
    }

    gyro[0] = -25;
    gyro[1] = 40;
    int x = 0, y = 0;
    for (uint32_t t = 120000; t < 121000; t += 20) {
        app_air_mouse_update(&mouse, upright, gyro, false, t, &dx, &dy);
        x += dx;
        y += dy;
    }
    assert(x < -300 && x > -410 && y > 180 && y < 240);
    assert(!app_air_mouse_update(&mouse, upright, gyro, false, 120980, &dx, &dy));
    assert(!app_air_mouse_update(&mouse, upright, gyro, true, 121000, &dx, &dy));
    assert(mouse.fraction[0] == 0 && mouse.velocity[1] == 0);
    assert(!app_air_mouse_update(&mouse, upright, gyro, false, 121200, &dx, &dy));
    assert(dx == 0 && dy == 0);

    const float flat[] = {0, 0, 9.80665f};
    gyro[0] = gyro[1] = 0;
    gyro[2] = -45;
    assert(!app_air_mouse_update(&mouse, flat, gyro, false, UINT32_MAX - 9, &dx, &dy));
    assert(app_air_mouse_update(&mouse, flat, gyro, false, 10, &dx, &dy) && dx > 0 && dy == 0);
    gyro[2] = NAN;
    assert(!app_air_mouse_update(&mouse, flat, gyro, false, 30, &dx, &dy) && !mouse.ready);
    puts("PASS air mouse suppresses idle drift, projects upright and flat motion, and resets on gaps or invalid data");
    return 0;
}
