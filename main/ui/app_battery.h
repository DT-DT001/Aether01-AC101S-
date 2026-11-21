#pragma once

#include <stddef.h>
#include "lvgl.h"
#include "app_tools.h"

/* Noninteractive badge. Its timer and context are owned by the returned object. */
lv_obj_t *app_battery_create(lv_obj_t *parent, int32_t width);
/* Uses a previously acquired background snapshot; performs no bus transaction. */
void app_battery_format_detail(const app_tools_snapshot_t *snapshot, char *text, size_t size);
