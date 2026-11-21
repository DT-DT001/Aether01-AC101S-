#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct ac101s_audio *ac101s_audio_handle_t;
typedef struct {
    void *context;
    esp_err_t (*start)(void *);
    esp_err_t (*stop)(void *);
    esp_err_t (*write)(void *, const void *, size_t, uint32_t);
    bool (*connected)(void *);
} ac101s_audio_sink_t;
