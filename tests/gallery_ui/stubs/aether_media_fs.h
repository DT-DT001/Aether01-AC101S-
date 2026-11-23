#pragma once
#include "aether_storage.h"
#include "esp_err.h"

#define AETHER_USB_STORAGE_MOUNT_POINT "usbphotos"
typedef struct { bool held; unsigned source; } aether_media_fs_lease_t;
bool aether_media_fs_path_ready(aether_storage_handle_t, const char *);
uint32_t aether_media_fs_generation(aether_storage_handle_t, const char *);
esp_err_t aether_media_fs_acquire(aether_storage_handle_t, const char *, aether_media_fs_lease_t *);
bool aether_media_fs_available(const aether_media_fs_lease_t *);
void aether_media_fs_release(aether_media_fs_lease_t *);
