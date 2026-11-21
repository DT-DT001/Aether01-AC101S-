#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AETHER_USB_BUS_NONE = 0,
    AETHER_USB_BUS_MSC,
    AETHER_USB_BUS_HOST,
} aether_usb_bus_owner_t;

/* Atomic, nonblocking physical-port reservation. Keep it across preparation,
 * CC waits and failed teardown; release only after hardware ownership ends. */
esp_err_t aether_usb_bus_claim(aether_usb_bus_owner_t owner);
esp_err_t aether_usb_bus_release(aether_usb_bus_owner_t owner);
aether_usb_bus_owner_t aether_usb_bus_get_owner(void);
#ifdef __cplusplus
}
#endif
