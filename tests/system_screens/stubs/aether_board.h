#pragma once

#include "dps368.h"
#include "pcf85063.h"
#include "qmi8658a.h"
#include "tg29.h"

typedef struct {
    void *display_io;
    bool display_ready;
    bool touch_ready;
    bool audio_ready;
    bool imu_ready;
    bool barometer_ready;
    bool rtc_ready;
} aether_board_t;
