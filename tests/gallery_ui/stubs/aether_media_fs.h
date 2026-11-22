#pragma once
#include "aether_storage.h"
#include "esp_err.h"

typedef struct { bool held; } aether_media_fs_lease_t;
esp_err_t aether_media_fs_acquire(aether_storage_handle_t, const char *, aether_media_fs_lease_t *);
bool aether_media_fs_available(const aether_media_fs_lease_t *);
void aether_media_fs_release(aether_media_fs_lease_t *);
