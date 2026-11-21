#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef enum { APP_AUDIO_INPUT_BOARD, APP_AUDIO_INPUT_USB } app_audio_input_t;
typedef struct {
    bool ready, usb_capture_connected;
    app_audio_input_t input, applied_input;
} app_audio_system_snapshot_t;
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *);
esp_err_t app_audio_system_set_input(app_audio_input_t);
