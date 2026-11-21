#pragma once
#include <stdint.h>

typedef enum {
    AETHER_USB_INPUT_RESET,
    AETHER_USB_INPUT_KEYBOARD,
    AETHER_USB_INPUT_MOUSE,
} aether_usb_input_type_t;
/* Full keyboard/button state plus relative mouse deltas. The callback receives
 * a temporary value: copy it into a bounded nonblocking queue. No USB/LVGL/file
 * calls from the callback. HID usages0..255 include modifiers0xE0..0xE7. */
typedef struct {
    aether_usb_input_type_t type;
    uint8_t keys[32];
    int16_t dx, dy, wheel;
    uint8_t buttons;
} aether_usb_input_event_t;
