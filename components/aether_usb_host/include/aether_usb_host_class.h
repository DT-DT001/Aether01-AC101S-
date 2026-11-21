#pragma once
#include "esp_err.h"

/* Static-lifetime class callbacks. The core alone owns usb_host_install(),
 * usb_host_lib_handle_events(), root-port power and usb_host_uninstall().
 * Each class may own its driver/client task and handle its own client events. */
typedef struct {
    void *context;
    /* Called once after core install with root port unpowered. A failed start
     * must roll back its own partial work; only successful starts are retained. */
    esp_err_t (*start)(void *context);
    /* Idempotent, nonblocking: revoke new work and request cancellation. */
    void (*request_stop)(void *context);
    /* Nonblocking cache/poll only. ESP_OK means every client/transfer and the
     * class driver has drained/uninstalled; NOT_FINISHED means still pending.
     * Never free a pending URB, sleep/join here, or touch core/VBUS power. */
    esp_err_t (*poll_stopped)(void *context);
} aether_usb_host_class_ops_t;
