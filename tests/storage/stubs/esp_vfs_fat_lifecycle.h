#pragma once
#include <stdint.h>
#include "esp_err.h"
esp_err_t esp_vfs_fat_lifecycle_acquire(uint32_t timeout_ms);
void esp_vfs_fat_lifecycle_release(void);
