#pragma once
#include <stdbool.h>
#include "app_power.h"
#include "aether_usb_input.h"

/* Call under the LVGL lock before creating home/widgets. The default group
 * automatically collects focusable controls created afterwards. */
esp_err_t app_usb_input_init(app_power_t *power, lv_display_t *display);
/* USB worker callback: bounded queue/cache publication only, no LVGL calls. */
void app_usb_input_publish(void *context, const aether_usb_input_event_t *event);
/* Optional additional page gate (e.g. NES active), queried only on LVGL reads.
 * Configure under LVGL; callback must be cache-only and must not call LVGL. */
void app_usb_input_set_gate(bool (*blocked)(void *), void *context);
/* Thread-safe admission. Disabled/held inputs are consumed until release. */
void app_usb_input_set_enabled(bool enabled);
/* LVGL-only modal keyboard scope. Call after the dialog controls exist.
 * DELETE automatically restores the previous scope/focus; close is needed for
 * hidden/reused panels. Pointer/touch still work. Failure blocks keypad input
 * until that modal is closed/deleted, rather than exposing background controls.
 * Reopening the same scope imports newly created controls without consuming
 * the active gesture or changing focus (unless preferred_focus is supplied).
 * Safe no-op when optional USB UI input initialization did not succeed. */
esp_err_t app_usb_input_modal_open(lv_obj_t *modal, lv_obj_t *preferred_focus);
void app_usb_input_modal_close(lv_obj_t *modal);
