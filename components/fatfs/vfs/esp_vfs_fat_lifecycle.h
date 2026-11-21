#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Serialize whole mount/unmount transactions across TF and USB workers.
 * Ordinary file I/O uses FatFs' own per-volume locks and never takes this gate.
 * Not recursive. Never acquire from an ISR or while holding a GUI lock. */
esp_err_t esp_vfs_fat_lifecycle_acquire(uint32_t timeout_ms);
void esp_vfs_fat_lifecycle_release(void);
