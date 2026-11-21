#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "aether_storage.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AETHER_USB_MSC_OFF = 0,
    AETHER_USB_MSC_STARTING,
    AETHER_USB_MSC_ACTIVE,
    AETHER_USB_MSC_STOPPING,
    AETHER_USB_MSC_ERROR,
} aether_usb_msc_state_t;

typedef enum {
    AETHER_USB_MSC_STAGE_IDLE = 0,
    AETHER_USB_MSC_STAGE_STORAGE = 1,
    AETHER_USB_MSC_STAGE_ROUTE = 2,
    AETHER_USB_MSC_STAGE_STACK_START = 3,
    AETHER_USB_MSC_STAGE_WAIT_HOST = 4,
    AETHER_USB_MSC_STAGE_HOST = 5,
    AETHER_USB_MSC_STAGE_EJECTED = 6,
    AETHER_USB_MSC_STAGE_DRAIN = 7,
    AETHER_USB_MSC_STAGE_STACK_STOP = 8,
    AETHER_USB_MSC_STAGE_RESTORE_TF = 9,
    AETHER_USB_MSC_STAGE_OFF = 10,
} aether_usb_msc_stage_t;

typedef struct {
    aether_usb_msc_state_t state;
    aether_usb_msc_stage_t diagnostic_stage;
    uint8_t selected_route;
    uint16_t usb_speed_mbps;  /* Cached negotiated link rate: 480, 12, or 0 unknown. */
    uint32_t usb_clock_khz;   /* Actual raw SD host clock, copied from storage cache. */
    bool usb_high_speed_fallback;
    bool active;              /* Local TF use must stay blocked while true. */
    bool host_connected;
    bool host_ejected;
    bool media_present;
    uint64_t capacity_bytes;
    esp_err_t last_error;
} aether_usb_msc_status_t;

/* Lazy singleton: initialization does not install USB or take the TF card. */
esp_err_t aether_usb_msc_init(aether_storage_handle_t storage);
/* Optional board VBUS query, invoked only by the IO worker during exit.
 * Required for reliable cable-removal recognition on a battery-powered board
 * without a TinyUSB VBUS monitor GPIO. Return ESP_OK/false only with actual
 * physical-absence evidence; VBUSGOOD=false alone is not sufficient. Return an
 * error for unknown/unsafe supply states. Never called by get_status(). */
typedef esp_err_t (*aether_usb_msc_vbus_query_t)(bool *present, void *context);
void aether_usb_msc_set_vbus_query(aether_usb_msc_vbus_query_t query, void *context);
/* Call only from the serialized IO worker after stopping all TF media users.
 * Neither function formats the card. Timeout never releases live USB ownership. */
esp_err_t aether_usb_msc_enter(uint32_t timeout_ms);
esp_err_t aether_usb_msc_exit(uint32_t timeout_ms);
/* Call only after the user explicitly confirms safe removal on the computer.
 * Some hosts remove the disk without START_STOP(eject). This single call may
 * bypass the host-ejected check; callback drain and teardown still apply.
 * No confirmation is retained for a later USB session. */
esp_err_t aether_usb_msc_exit_confirmed(uint32_t timeout_ms);
/* Cached status; no USB, card, filesystem IO or waiting in the GUI. */
esp_err_t aether_usb_msc_get_status(aether_usb_msc_status_t *status);
bool aether_usb_msc_is_active(void);

#ifdef __cplusplus
}
#endif
