#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum {APP_AUDIO_OUTPUT_SPEAKER, APP_AUDIO_OUTPUT_BLUETOOTH, APP_AUDIO_OUTPUT_USB} app_audio_output_t;
typedef struct {
    bool ready;
    bool usb_playback_connected,usb_capture_connected;
    uint8_t master_volume, applied_volume;
    app_audio_output_t output, applied_output;
    bool pending, persistence_pending, persistence_deferred;
    esp_err_t error, persistence_error;
} app_audio_system_snapshot_t;
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *);
esp_err_t app_audio_system_set_volume(uint8_t, bool);
esp_err_t app_audio_system_toggle_mute(bool);
esp_err_t app_audio_system_set_output(app_audio_output_t);
struct aether_bluetooth *app_audio_system_bluetooth(void);
