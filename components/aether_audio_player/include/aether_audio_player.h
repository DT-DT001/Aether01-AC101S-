#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aether_storage.h"
#include "ac101s_audio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aether_audio_player_t *aether_audio_player_handle_t;

typedef enum {
    AETHER_AUDIO_STOPPED = 0,
    AETHER_AUDIO_STARTING,
    AETHER_AUDIO_PLAYING,
    AETHER_AUDIO_ERROR,
    AETHER_AUDIO_PAUSED,
    AETHER_AUDIO_FINISHED,
} aether_audio_player_state_t;

typedef enum {
    AETHER_AUDIO_REPEAT_LIST = 0,
    AETHER_AUDIO_REPEAT_ONE,
    AETHER_AUDIO_SHUFFLE,
} aether_audio_repeat_t;

enum {
    AETHER_AUDIO_EQ_BANDS = 5,
    AETHER_AUDIO_EQ_MAX_DB = 6,
    AETHER_AUDIO_EQ_MAX_GAIN_DB = 6,
};

typedef struct {
    int8_t bands_db[AETHER_AUDIO_EQ_BANDS]; /* 100, 300, 1000, 3000 and 10000 Hz. */
    uint8_t gain_db;                     /* Music-only digital gain. */
    bool enabled;
} aether_audio_eq_t;

typedef struct {
    aether_audio_player_state_t state;
    esp_err_t last_error;
    char path[192];
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint16_t channels;
    uint8_t volume;
    aether_audio_repeat_t repeat;
    aether_audio_eq_t eq;
    bool active;
    bool closing; /* Session release remains pending until codec and playlist are free. */
    esp_err_t settings_error;
    bool preferences_pending; /* Actual worker-owned NVS dirty state, including resume. */
    uint16_t track_index;
    uint16_t track_count;
    bool mp3;
    bool aac;
    char detail[112];
    bool io_profile_valid; /* Split read/decode is measured for real MP3 only. */
    uint32_t pull_us,pull_max_us,read_us,read_max_us,decode_no_io_us,decode_max_us;
    uint32_t write_us,write_max_us,supply_gap_us,supply_gap_max_us;
    uint32_t refill_max_bytes,refill_max_us;

} aether_audio_player_status_t;

/**
 * Create a WAV/MP3 player with a persistent worker. Supports PCM16 WAV and
 * MP3 mono/stereo 8..48 kHz, linearly resampled to fixed 48 kHz stereo output.
 * Volume, repeat, EQ and the last track position are saved by the worker.
 */
esp_err_t aether_audio_player_new(ac101s_audio_handle_t audio,
                                  aether_storage_handle_t storage,
                                  aether_audio_player_handle_t *out_handle);
/** Defer preference commits while another media worker is running.
 * Callback is invoked outside player locks; explicit del still flushes. */
void aether_audio_player_set_persistence_guard(aether_audio_player_handle_t handle,
                                               bool (*busy)(void *), void *context);
esp_err_t aether_audio_player_play(aether_audio_player_handle_t handle,
                                   const char *path, int volume);
/** Request stop without blocking the caller (safe for LVGL event callbacks). */
esp_err_t aether_audio_player_stop(aether_audio_player_handle_t handle);
/** Exit the owning music page: stop and asynchronously discard its playlist,
 * paths and media state after codec/file/DMA cleanup. Safe for LVGL callbacks. */
esp_err_t aether_audio_player_release_session(aether_audio_player_handle_t handle);
/** All controls below only publish commands, and never perform codec/file I/O. */
esp_err_t aether_audio_player_set_paused(aether_audio_player_handle_t handle, bool paused);
esp_err_t aether_audio_player_seek(aether_audio_player_handle_t handle, uint32_t position_ms);
esp_err_t aether_audio_player_set_volume(aether_audio_player_handle_t handle, int volume);
esp_err_t aether_audio_player_set_repeat(aether_audio_player_handle_t handle, aether_audio_repeat_t repeat);
/** Apply music EQ at the next PCM block and schedule a background preference save. */
esp_err_t aether_audio_player_set_eq(aether_audio_player_handle_t handle, const aether_audio_eq_t *eq);
/** Copy a bounded playlist. Owning-page exit explicitly releases the session.
 * The worker applies repeat/shuffle on natural EOF, never after stop/error. */
esp_err_t aether_audio_player_set_playlist(aether_audio_player_handle_t handle,
                                           const char *const *paths, size_t count);
/** Copy the cached last music track path. Recordings are excluded from the
 * music bookmark. Does not access storage or NVS. */
esp_err_t aether_audio_player_get_resume_path(aether_audio_player_handle_t handle, char *path, size_t size);
/** Select the saved position if the prepared playlist still has that track.
 * Leaves playback idle until play is requested for this selection. */
esp_err_t aether_audio_player_restore(aether_audio_player_handle_t handle);
/** Previous/next wrap the playlist and replace the current stream asynchronously. */
esp_err_t aether_audio_player_next(aether_audio_player_handle_t handle, int direction);
/** Select a playlist entry without relying on a foreground UI timer. */
esp_err_t aether_audio_player_select(aether_audio_player_handle_t handle, size_t index);
/** Wait for codec, file, and mount lease cleanup. Call outside the LVGL task. */
esp_err_t aether_audio_player_stop_wait(aether_audio_player_handle_t handle,
                                        uint32_t timeout_ms);
esp_err_t aether_audio_player_get_status(aether_audio_player_handle_t handle,
                                         aether_audio_player_status_t *status);
/** Print cached supply timing after playback pauses/stops. Does not read the
 * card or touch the codec. Call from an idle housekeeping task, outside player
 * or media locks; console output may block. Ignored during active supply. */
void aether_audio_player_log_profile(aether_audio_player_handle_t handle);
esp_err_t aether_audio_player_del(aether_audio_player_handle_t handle);

#ifdef __cplusplus
}
#endif
