#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "aether_usb_host_class.h"

#define AETHER_USB_STORAGE_MOUNT_POINT "/usb"
typedef struct aether_usb_storage *aether_usb_storage_handle_t;
typedef struct {
    void *owner;
    uint32_t generation;
    uint32_t cookie;
    uint16_t slot;
    bool held;
} aether_usb_storage_lease_t;
typedef struct {
    bool driver_running, connected, mounted, draining;
    uint32_t generation, users, sector_size;
    uint64_t capacity_bytes;
    uint8_t device_address, ignored_devices;
    esp_err_t last_error;
} aether_usb_storage_status_t;

/* Lazy static singleton; no USB, PHY, task or power side effects. */
aether_usb_storage_handle_t aether_usb_storage_default(void);
/* Nonblocking admission. Zero-initialize a fresh lease; hold it until every
 * fopen/opendir/file operation has ended and fclose/closedir completed. */
esp_err_t aether_usb_storage_acquire(aether_usb_storage_handle_t handle,
                                     aether_usb_storage_lease_t *lease);
void aether_usb_storage_release(aether_usb_storage_lease_t *lease);
/* False immediately on detach/stop/generation change. Close existing files
 * promptly on false; the VFS/device is retained until all leases are released. */
bool aether_usb_storage_is_available(const aether_usb_storage_lease_t *lease);
esp_err_t aether_usb_storage_get_status(aether_usb_storage_handle_t handle,
                                       aether_usb_storage_status_t *status);
esp_err_t aether_usb_storage_class_start(void *context);
void aether_usb_storage_class_request_stop(void *context);
esp_err_t aether_usb_storage_class_poll_stopped(void *context);
