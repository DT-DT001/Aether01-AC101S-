#pragma once

#include <stdint.h>
uint32_t esp_crc32_le(uint32_t crc, const uint8_t *bytes, uint32_t length);
