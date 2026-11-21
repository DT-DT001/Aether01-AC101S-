#pragma once

/* Consume held touch across an explicit power/BOOT-key wake. No display wake
 * is requested by touch input, and this function performs no I2C or LVGL IO. */
void app_touch_consume_wake_gesture(void);
