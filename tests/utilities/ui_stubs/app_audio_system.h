#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ready;
    uint8_t master_volume;
} app_audio_system_snapshot_t;

void app_audio_system_get_snapshot(app_audio_system_snapshot_t *snapshot);
