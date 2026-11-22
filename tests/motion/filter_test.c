#include "app_motion_filter.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#define G 9.80665f
static void unit(const app_motion_filter_t *f) {
    float n=0;for(unsigned i=0;i<4;++i) {assert(isfinite(f->q[i]));n+=f->q[i]*f->q[i];}
    assert(fabsf(n-1)<.0002f);
    assert(isfinite(f->roll) && isfinite(f->pitch) && isfinite(f->yaw));
}
static void uncalibrated_game_tilt(void)
{
    app_motion_filter_t filter;
    app_motion_filter_init(&filter);
    const float raw_gyro[3] = {8.05f, 1.92f, -.09f};
    const float neutral[3] = {0, 0, 8.9f};
    assert(app_motion_filter_update_tilt(&filter, neutral, raw_gyro, 0, false));
    app_motion_filter_zero(&filter);
    uint32_t time = 0;
    for (unsigned i = 0; i < 200; ++i) {
        time += 20;
        assert(app_motion_filter_update_tilt(&filter, neutral, raw_gyro, time, false));
    }
    assert(fabsf(filter.roll) < .1f && fabsf(filter.pitch) < .1f);

    const float poses[][3] = {{-2.453f, 0, 8.555f}, {2.453f, 0, 8.555f},
                              {0, -2.453f, 8.555f}, {0, 2.453f, 8.555f}};
    for (unsigned axis = 0; axis < 4; ++axis) {
        for (unsigned i = 0; i < 5; ++i) {
            time += 20;
            assert(app_motion_filter_update_tilt(&filter, poses[axis], raw_gyro, time, false));
        }
        const float angle = axis < 2 ? filter.pitch : -filter.roll;
        assert(axis % 2 ? angle < -12.0f : angle > 12.0f);
        for (unsigned i = 0; i < 6; ++i) {
            time += 20;
            assert(app_motion_filter_update_tilt(&filter, neutral, raw_gyro, time, false));
        }
        assert(fabsf(filter.roll) < 1.0f && fabsf(filter.pitch) < 1.0f);
        unit(&filter);
    }
    const float freefall[3] = {0, 0, 0};
    assert(!app_motion_filter_update_tilt(&filter, freefall, raw_gyro, time + 20, false));
    assert(!app_motion_filter_update_tilt(&filter, neutral, raw_gyro, time, false));
    puts("PASS unqualified IMU game filter: raw-bias rejection, four-way response, neutral recovery and invalid frames");
}
int main(void) {
    app_motion_filter_t f;app_motion_filter_init(&f);
    float a[3]={0,0,G},w[3]={0,0,0};
    for(unsigned i=0;i<300;++i) assert(app_motion_filter_update(&f,a,w,i*20));
    unit(&f);assert(f.action==APP_MOTION_STILL && fabsf(f.roll)<.01 && fabsf(f.pitch)<.01);
    assert(!app_motion_filter_update(&f,a,w,f.stamp));
    puts("PASS stationary fusion and duplicate timestamp do not invent motion");
    app_motion_filter_init(&f);assert(app_motion_filter_update(&f,a,w,0));w[2]=90;
    for(unsigned i=1;i<=50;++i) assert(app_motion_filter_update(&f,a,w,i*20));
    unit(&f);assert(fabsf(f.yaw-90)<.1 && f.action==APP_MOTION_TURNING);
    app_motion_filter_zero(&f);assert(fabsf(f.roll)<.01 && fabsf(f.pitch)<.01 && fabsf(f.yaw)<.01);
    puts("PASS measured gyro rotates quaternion; reference-zero resets relative pose");
    app_motion_filter_init(&f);w[2]=0;a[1]=G*.5f;a[2]=G*.8660254f;
    assert(app_motion_filter_update(&f,a,w,20));assert(fabsf(f.roll-30)<.01 && f.action==APP_MOTION_TILTED);
    for(unsigned i=2;i<200;++i) assert(app_motion_filter_update(&f,a,w,i*20));
    assert(fabsf(f.roll-30)<.1);unit(&f);
    puts("PASS acceleration initializes and stabilizes actual tilt");
    app_motion_filter_init(&f);a[1]=0;a[2]=21;
    assert(app_motion_filter_update(&f,a,w,0));assert(f.action==APP_MOTION_IMPACT);
    a[2]=0;assert(app_motion_filter_update(&f,a,w,20));assert(f.action==APP_MOTION_FREEFALL);
    a[2]=G;for(unsigned i=2;i<40;++i) assert(app_motion_filter_update(&f,a,w,i*20));
    assert(f.action==APP_MOTION_STILL);
    app_motion_filter_init(&f);assert(app_motion_filter_update(&f,a,w,0));
    for(unsigned i=1;i<=8;++i) {a[0]=(i%2)?6:-6;assert(app_motion_filter_update(&f,a,w,i*60));}
    assert(f.action==APP_MOTION_SHAKING);
    puts("PASS impact at timestamp-zero, freefall, expiry and measured alternating shake");
    float q[4];for(unsigned i=0;i<4;++i) q[i]=f.q[i];
    a[0]=NAN;assert(!app_motion_filter_update(&f,a,w,600));
    for(unsigned i=0;i<4;++i) assert(f.q[i]==q[i]);
    a[0]=0;app_motion_filter_init(&f);assert(app_motion_filter_update(&f,a,w,0xfffffff0u));
    assert(app_motion_filter_update(&f,a,w,4));unit(&f);
    w[0]=140;w[1]=-80;w[2]=25;
    for(unsigned i=2;i<20000;++i) {assert(app_motion_filter_update(&f,a,w,i*20));unit(&f);}
    puts("PASS invalid samples preserve orientation; tick wrap and20k updates remain normalized");
    uncalibrated_game_tilt();
    return 0;
}
