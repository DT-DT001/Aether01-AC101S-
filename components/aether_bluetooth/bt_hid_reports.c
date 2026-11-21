#include "bt_hid_reports.h"

/* One SDP descriptor with report ID 1 (8-byte keyboard) and 2 (3-byte mouse).
 * IDs/widths also match Bluetooth HID boot keyboard and mouse framing. */
uint8_t bt_hid_descriptor[] = {
    0x05,0x01, 0x09,0x06, 0xa1,0x01, 0x85,BT_HID_KEYBOARD,
    0x05,0x07, 0x19,0xe0, 0x29,0xe7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x75,0x08, 0x95,0x01, 0x81,0x01,
    0x05,0x07, 0x19,0x00, 0x29,0x73, 0x15,0x00, 0x25,0x73,
    0x75,0x08, 0x95,0x06, 0x81,0x00,
    0x05,0x08, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x05, 0x91,0x02, 0x75,0x03, 0x95,0x01, 0x91,0x01, 0xc0,
    0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x85,BT_HID_MOUSE,
    0x09,0x01, 0xa1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x03, 0x81,0x02, 0x75,0x05, 0x95,0x01, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7f,
    0x75,0x08, 0x95,0x02, 0x81,0x06, 0xc0, 0xc0,
};
const size_t bt_hid_descriptor_size = sizeof(bt_hid_descriptor);

size_t bt_hid_report_size(uint8_t id)
{
    if (id == BT_HID_KEYBOARD) return 8;
    if (id == BT_HID_MOUSE) return 3;
    return 0;
}

bt_hid_report_t bt_hid_keyboard(uint8_t modifiers, uint8_t key)
{
    return (bt_hid_report_t){.id = BT_HID_KEYBOARD, .bytes = {modifiers, 0, key}};
}

bt_hid_report_t bt_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy)
{
    return (bt_hid_report_t){.id = BT_HID_MOUSE, .bytes = {
        buttons, (uint8_t)(dx == INT8_MIN ? -127 : dx), (uint8_t)(dy == INT8_MIN ? -127 : dy),
    }};
}

bool bt_hid_needs_release(const bt_hid_report_t *report)
{
    if (report->id == BT_HID_KEYBOARD) return report->bytes[0] || report->bytes[2];
    return report->id == BT_HID_MOUSE && report->bytes[0];
}

static bool ascii_key(unsigned char ch, bt_hid_report_t *out)
{
    if (ch >= 'a' && ch <= 'z') *out = bt_hid_keyboard(0, ch - 'a' + 4);
    else if (ch >= 'A' && ch <= 'Z') *out = bt_hid_keyboard(2, ch - 'A' + 4);
    else if (ch >= '1' && ch <= '9') *out = bt_hid_keyboard(0, ch - '1' + 0x1e);
    else if (ch == '0') *out = bt_hid_keyboard(0, 0x27);
    else {
        static const char plain[] = " -=[]\\;'`,./";
        static const char shift[] = " _+{}|:\"~<>?";
        static const uint8_t usage[] = {0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x33,0x34,0x35,0x36,0x37,0x38};
        static const char digit_shift[] = "!@#$%^&*()";
        for (size_t i = 0; i < sizeof(usage); ++i) {
            if (ch == (unsigned char)plain[i] || ch == (unsigned char)shift[i]) {
                *out = bt_hid_keyboard(ch == (unsigned char)plain[i] ? 0 : 2, usage[i]);
                return true;
            }
        }
        for (size_t i = 0; i < sizeof(digit_shift) - 1; ++i) {
            if (ch == (unsigned char)digit_shift[i]) {
                *out = bt_hid_keyboard(2, 0x1e + i);
                return true;
            }
        }
        return false;
    }
    return true;
}

bool bt_hid_ascii(const char *text, bt_hid_report_t out[BT_HID_TEXT_MAX], size_t *count)
{
    *count = 0;
    if (!text) return false;
    size_t len = 0;
    while (text[len]) {
        if (len == BT_HID_TEXT_MAX || !ascii_key((unsigned char)text[len], &out[len])) return false;
        ++len;
    }
    *count = len;
    return len != 0;
}
