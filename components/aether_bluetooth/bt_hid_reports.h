#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BT_HID_KEYBOARD = 1, BT_HID_MOUSE = 2, BT_HID_TEXT_MAX = 48, BT_HID_QUEUE_MAX = 64 };
typedef struct {
    uint8_t id;
    uint8_t bytes[8];
    /* Optional minimum down time for deliberate taps; zero uses the normal
     * report cadence. This metadata is never transmitted to the host. */
    uint8_t hold_ms;
} bt_hid_report_t;

extern uint8_t bt_hid_descriptor[];
extern const size_t bt_hid_descriptor_size;
size_t bt_hid_report_size(uint8_t id);
bool bt_hid_ascii(const char *text, bt_hid_report_t out[BT_HID_TEXT_MAX], size_t *count);
bt_hid_report_t bt_hid_keyboard(uint8_t modifiers, uint8_t key);
bt_hid_report_t bt_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy);
bool bt_hid_needs_release(const bt_hid_report_t *report);
