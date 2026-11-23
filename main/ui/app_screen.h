#pragma once

#include "lvgl.h"

enum { APP_HEADER_SIDE_INSET = 36, APP_HEADER_TOP_INSET = 16 };

static inline void app_ui_load_screen(lv_obj_t *screen)
{
    lv_obj_t *previous = lv_screen_active();
    lv_screen_load(screen);
    /* A click can load a screen from inside the previous screen's callback.
     * Keep that screen's event lists alive until LVGL returns to its timers. */
    if (previous && previous != screen) lv_obj_delete_async(previous);
}
