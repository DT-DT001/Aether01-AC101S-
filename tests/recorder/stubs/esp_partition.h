#pragma once

#include <stddef.h>
#include "esp_err.h"

#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 255

typedef struct { size_t size; } esp_partition_t;

const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
