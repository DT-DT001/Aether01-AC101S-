/* Link against the actual project LVGL 9 objects, not the power-test bool stub.
 * lv_display_enable_invalidation() is a balanced counter API, not a setter. */
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    assert(display);
    assert(lv_display_is_invalidation_enabled(display));

    lv_display_enable_invalidation(display, false); /* Counter: 1 -> 0. */
    assert(!lv_display_is_invalidation_enabled(display));
    lv_display_enable_invalidation(display, false); /* Counter: 0 -> -1. */
    assert(!lv_display_is_invalidation_enabled(display));
    lv_display_enable_invalidation(display, true);  /* Counter: -1 -> 0. */
    assert(!lv_display_is_invalidation_enabled(display));
    puts("PASS real LVGL: disable twice + enable once remains disabled");

    lv_display_enable_invalidation(display, true);  /* Counter: 0 -> 1. */
    assert(lv_display_is_invalidation_enabled(display));
    puts("PASS real LVGL: second balancing enable restores invalidation");

    for (unsigned i = 0; i < 1000; ++i) {
        lv_display_enable_invalidation(display, false);
        assert(!lv_display_is_invalidation_enabled(display));
        lv_display_enable_invalidation(display, true);
        assert(lv_display_is_invalidation_enabled(display));
    }
    lv_display_t *other = lv_display_create(480, 480);
    assert(other && lv_display_is_invalidation_enabled(other));
    lv_display_enable_invalidation(display, false);
    assert(!lv_display_is_invalidation_enabled(display));
    assert(lv_display_is_invalidation_enabled(other));
    lv_display_enable_invalidation(display, true);
    assert(lv_display_is_invalidation_enabled(display));
    lv_display_delete(other);
    lv_display_delete(display);
    lv_deinit();
    puts("PASS real LVGL: 1000 balanced cycles and independent display counters");
    return 0;
}
