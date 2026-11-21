#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef void *aether_storage_handle_t;
typedef struct { bool mounted, card_inserted; } aether_storage_status_t;
esp_err_t aether_storage_get_status(aether_storage_handle_t, aether_storage_status_t *);
