#pragma once
#include <stdbool.h>
#include "aether_storage.h"
#include "aether_usb_storage.h"

/* One lease covers all FILE/DIR objects in the operation. Never copy a held
 * lease. Close every object before release, including on removal/error. */
typedef struct {
    aether_storage_handle_t tf;
    aether_usb_storage_lease_t usb;
    bool held;
} aether_media_fs_lease_t;

bool aether_media_fs_safe_path(const char *path);
bool aether_media_fs_path_ready(aether_storage_handle_t tf, const char *path);
/* USB attachment identity, also changes when replacement occurs between UI ticks. */
uint32_t aether_media_fs_generation(aether_storage_handle_t tf, const char *path);
const char *aether_media_fs_default_root(aether_storage_handle_t tf);
esp_err_t aether_media_fs_acquire(aether_storage_handle_t tf, const char *path,
                                  aether_media_fs_lease_t *lease);
bool aether_media_fs_available(const aether_media_fs_lease_t *lease);
void aether_media_fs_release(aether_media_fs_lease_t *lease);
