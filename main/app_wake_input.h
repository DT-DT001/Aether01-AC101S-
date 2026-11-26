#pragma once

#include <stdbool.h>

/* Consume held touch across a display wake. The gesture never reaches LVGL. */
void app_touch_consume_wake_gesture(void);

/* Enable touch IRQ dispatch only while the display is awake. Called with the LVGL lock held. */
void app_touch_set_screen_awake(bool awake);
