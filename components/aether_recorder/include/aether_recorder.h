#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ac101s_audio.h"
#include "aether_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aether_recorder *aether_recorder_handle_t;
typedef enum {
    AETHER_RECORDER_IDLE = 0,
    AETHER_RECORDER_STARTING,
    AETHER_RECORDER_RECORDING,
    AETHER_RECORDER_PAUSED,
    AETHER_RECORDER_SAVING,
    AETHER_RECORDER_FINISHED,
    AETHER_RECORDER_ERROR,
} aether_recorder_state_t;
typedef struct {
    aether_recorder_state_t state;
    bool active; /* Includes STARTING/PAUSED/SAVING until codec/file/lease cleanup. */
    uint64_t duration_ms; /* Recorded sample time; paused samples are excluded. */
    uint64_t data_bytes;
    uint16_t peak; /* Absolute slot-0 PCM peak, 0..32768; zero while paused. */
    esp_err_t last_error;
    char detail[96];
    char path[192]; /* Latest created WAV; retained on recoverable partial-save error. */
} aether_recorder_status_t;

/* One application-wide recorder; construction creates no task or PCM buffer. */
esp_err_t aether_recorder_new(ac101s_audio_handle_t audio,
    aether_storage_handle_t storage, aether_recorder_handle_t *out_handle);
aether_recorder_handle_t aether_recorder_get_default(void);
/* Async acceptance. Creates a unique WAV in /sdcard/Recordings, PCM16 mono 48kHz. */
esp_err_t aether_recorder_start(aether_recorder_handle_t handle);
esp_err_t aether_recorder_set_paused(aether_recorder_handle_t handle, bool paused);
/** Park the microphone while preserving the current WAV and manual pause. */
void aether_recorder_set_screen_paused(aether_recorder_handle_t handle, bool paused);
esp_err_t aether_recorder_stop(aether_recorder_handle_t handle);
/* Requests stop; zero only polls. Timeout preserves the active owner/resources. */
esp_err_t aether_recorder_stop_wait(aether_recorder_handle_t handle, uint32_t timeout_ms);
/* Returns ESP_ERR_INVALID_ARG for NULL arguments; a valid snapshot is atomic. */
esp_err_t aether_recorder_get_status(aether_recorder_handle_t handle,
    aether_recorder_status_t *status);
/* Terminal cleanup. Caller must first quiesce other references to this handle. */
esp_err_t aether_recorder_del(aether_recorder_handle_t handle);

#ifdef __cplusplus
}
#endif
