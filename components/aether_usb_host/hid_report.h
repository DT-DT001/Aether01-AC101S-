#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define AETHER_HID_FIELDS 96
#define AETHER_HID_REPORTS 16
typedef enum { HID_KEY_ARRAY,HID_KEY_BITMAP,HID_KEY,HID_BUTTON,HID_X,HID_Y,HID_HAT,HID_DPAD,HID_EXIT,
 HID_MOUSE_X,HID_MOUSE_Y,HID_MOUSE_WHEEL,HID_MOUSE_BUTTON } hid_field_kind_t;
typedef struct { uint16_t offset,count,usage; uint8_t size,id,kind; int32_t min,max; } aether_hid_field_t;
typedef struct { aether_hid_field_t fields[AETHER_HID_FIELDS]; unsigned count; bool ids;
    uint8_t report_ids[AETHER_HID_REPORTS]; uint16_t states[AETHER_HID_REPORTS],button_max[AETHER_HID_REPORTS],non_keyboard_states[AETHER_HID_REPORTS]; unsigned reports;
    uint8_t keyboard_states[AETHER_HID_REPORTS][32],mouse_states[AETHER_HID_REPORTS];
    bool has_keyboard,has_mouse;
} aether_hid_report_t;
typedef enum {
    AETHER_HID_PARSE_MAPPED,
    AETHER_HID_PARSE_UNMAPPED, /* Structurally valid, no NES input usages. */
    AETHER_HID_PARSE_MALFORMED,
    AETHER_HID_PARSE_LIMIT,
} aether_hid_parse_result_t;
aether_hid_parse_result_t aether_hid_parse_ex(aether_hid_report_t *,const uint8_t *,size_t);
bool aether_hid_parse(aether_hid_report_t *,const uint8_t *,size_t);
/* Invalid/truncated relevant report releases this interface's state. Unknown IDs
 * do not erase a keyboard state when the same HID interface also reports mouse. */
typedef struct {
    uint8_t keys[32];
    int16_t dx,dy,wheel;
    uint8_t buttons;
    uint16_t non_keyboard_buttons; /* Gamepad/Home only; excludes all keyboard shortcuts. */
    bool keyboard,mouse;
} aether_hid_input_t;
bool aether_hid_decode_input(aether_hid_report_t *,const uint8_t *,size_t,uint16_t *,aether_hid_input_t *);
bool aether_hid_decode(aether_hid_report_t *,const uint8_t *,size_t,uint16_t *);
void aether_hid_release(aether_hid_report_t *);
