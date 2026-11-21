#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "aether_usb_host_class.h"
#include "aether_usb_input.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct aether_usb_host *aether_usb_host_handle_t;
typedef enum { AETHER_OTG_OFF, AETHER_OTG_WAIT_SINK, AETHER_OTG_STARTING,
    AETHER_OTG_ACTIVE, AETHER_OTG_STOPPING, AETHER_OTG_ERROR } aether_otg_state_t;
/* Bits intentionally match aether_nes_set_external_input(). */
enum { AETHER_HID_UP=1, AETHER_HID_DOWN=2, AETHER_HID_LEFT=4, AETHER_HID_RIGHT=8,
    AETHER_HID_A=16, AETHER_HID_B=32, AETHER_HID_SELECT=64, AETHER_HID_START=128,
    AETHER_HID_PAUSE=256, AETHER_HID_RESET=512, AETHER_HID_EXIT=1024 };
typedef struct {
    void *context;
    /* ESP_OK/true only for a verified attached external sink. Unknown is false. */
    esp_err_t (*query_sink)(void *context, bool *attached_sink);
    /* Must verify PMIC enable/disable and refuse external-source backfeed. */
    esp_err_t (*set_vbus)(void *context, bool enabled);
    /* Called from host worker, never LVGL. 0 releases every input on removal. */
    void (*input)(void *context, uint16_t buttons);
    /* Optional application keyboard mapping. NULL retains legacy defaults.
     * Receives the complete merged keyboard state; gamepad input is independent.
     * Must be bounded/cache-only: no NVS, GUI or USB calls. */
    uint16_t (*keyboard_map)(void *context, const uint8_t keys[32]);
    /* Optional nonblocking generic keyboard/mouse snapshot callback. */
    void (*hid_input)(void *context, const aether_usb_input_event_t *event);
    /* Immutable static-lifetime array, at most3 classes. */
    const aether_usb_host_class_ops_t *classes;
    uint8_t class_count;
} aether_usb_host_config_t;
typedef enum {
    AETHER_USB_HID_STAGE_NONE,
    AETHER_USB_HID_STAGE_DEVICE,
    AETHER_USB_HID_STAGE_INTERFACE,
    AETHER_USB_HID_STAGE_SET_PROTOCOL,
    AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR,
    AETHER_USB_HID_STAGE_REPORT_PARSE,
    AETHER_USB_HID_STAGE_INPUT,
    AETHER_USB_HID_STAGE_READY,
    AETHER_USB_HID_STAGE_IGNORED,
} aether_usb_hid_stage_t;

typedef struct {
    bool enabled, active, vbus_enabled, cleanup_pending;
    aether_otg_state_t state;
    esp_err_t last_error;
    uint8_t hid_interfaces;
    uint16_t buttons;
    /* One coherent last HID event. A real hid_error latches this tuple until
     * a new Host session; later healthy/ignored interfaces cannot mask it. */
    aether_usb_hid_stage_t hid_stage;
    esp_err_t hid_error;
    esp_err_t report_parse_error;
    uint16_t vid, pid, report_length, report_received;
    uint8_t last_interface, last_subclass, last_protocol;
    uint8_t ignored_interfaces; /* Valid unmapped interfaces in this session. */
    int32_t transfer_status; /* usb_transfer_status_t, -1 before a completion. */
} aether_usb_host_status_t;
/* Lazy default-OFF: initialization neither claims USB nor configures its PHY. */
esp_err_t aether_usb_host_new(const aether_usb_host_config_t *, aether_usb_host_handle_t *);
/* Nonblocking admission/command publication, safe from the GUI task. */
esp_err_t aether_usb_host_set_enabled(aether_usb_host_handle_t, bool enabled);
esp_err_t aether_usb_host_get_status(aether_usb_host_handle_t, aether_usb_host_status_t *);
/* Stop admission and confirm host teardown plus VBUS off. Outside LVGL. */
esp_err_t aether_usb_host_disable_wait(aether_usb_host_handle_t, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
