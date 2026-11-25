#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

enum { ESP_PARTITION_TYPE_DATA = 1 };
typedef struct {
    uint32_t size, erase_size;
    bool encrypted, readonly;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *, size_t offset, void *output, size_t length);
esp_err_t esp_partition_write(const esp_partition_t *, size_t offset, const void *input, size_t length);
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t offset, size_t length);
