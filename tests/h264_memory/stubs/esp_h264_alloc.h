#pragma once
#include <stdint.h>
void *esp_h264_calloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size,
                           uint32_t caps1, uint32_t caps2);
