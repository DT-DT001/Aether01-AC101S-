#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ac101s_audio.h"
#include "aether_bluetooth.h"

typedef enum {
    APP_AUDIO_OUTPUT_SPEAKER = 0,
    APP_AUDIO_OUTPUT_BLUETOOTH = 1,
    APP_AUDIO_OUTPUT_USB = 2,
} app_audio_output_t;

typedef enum { APP_AUDIO_INPUT_BOARD = 0, APP_AUDIO_INPUT_USB = 1 } app_audio_input_t;

typedef struct {
    bool ready;
    uint8_t master_volume;       /* Requested 0..100; zero is mute. */
    uint8_t applied_volume;      /* Target accepted by the PCM writer. */
    app_audio_output_t output;
    app_audio_output_t applied_output;
    app_audio_input_t input, applied_input;
    bool usb_playback_connected, usb_capture_connected;
    bool pending;
    bool controls_quiesced; /* Service acknowledged no hardware apply in flight. */
    bool persistence_pending;
    bool persistence_deferred;
    esp_err_t error;
    esp_err_t persistence_error;
} app_audio_system_snapshot_t;

/* Transport changes and NVS commits happen on the background service.
 * Volume updates publish a PCM target immediately. */
esp_err_t app_audio_system_init(ac101s_audio_handle_t audio);
void app_audio_system_set_persistence_guard(bool (*busy)(void *), void *context);
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *snapshot);
esp_err_t app_audio_system_set_volume(uint8_t volume, bool persist);
esp_err_t app_audio_system_toggle_mute(bool persist);
/* A Bluetooth route is applied only when the external sink is connected.
 * Output selection is session-local; a new boot starts with the speaker. */
esp_err_t app_audio_system_set_output(app_audio_output_t output);
/* Input changes apply on the service task, never during an active capture.
 * USB removal stops that recording and preserves USB selection as unavailable;
 * explicitly select board MIC or reconnect before starting another recording. */
esp_err_t app_audio_system_set_input(app_audio_input_t input);
/* Register USB transports once, after the class manager is initialized and
 * before starting media. Connection snapshots are refreshed by the service. */
esp_err_t app_audio_system_set_usb_audio(bool initialized);
/* One application-owned manager; UI commands use this handle directly. */
void app_audio_system_set_bluetooth(aether_bluetooth_handle_t bluetooth);
aether_bluetooth_handle_t app_audio_system_bluetooth(void);
/* Runs on the audio service task, outside BT callbacks and GUI locks. */
void app_audio_system_set_media_control(void (*control)(aether_bluetooth_remote_action_t, void *),
                                        void *context);

/* Freeze new controls for shutdown; pending preferences still commit. */
void app_audio_system_quiesce_controls(bool quiesce);
