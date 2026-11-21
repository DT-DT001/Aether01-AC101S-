#pragma once

#include "aether_audio_player.h"
#include "aether_video_player.h"
#include "aether_nes.h"
#include "aether_storage.h"
#include "app_reader_file.h"

typedef struct app_io *app_io_handle_t;
typedef enum {
    APP_IO_MUSIC,
    APP_IO_VIDEO,
    APP_IO_NES,
    APP_IO_BOOK,
    APP_IO_FILES,
    APP_IO_PREPARE,
    APP_IO_CATALOG,
    APP_IO_READ_TEXT,
    APP_IO_READER_LOAD,
    APP_IO_READER_SAVE,
    APP_IO_RECORDER,
} app_io_operation_t;

typedef struct {
    char path[192];
    uint64_t size;
    bool directory;
} app_io_entry_t;

typedef struct {
    app_io_operation_t operation;
    uint32_t generation;
    char path[192];
    app_io_operation_t filter;
    uint64_t offset;
    uint32_t limit;
    uint32_t font_px;
    bool dark;
    bool source_boundary;
    unsigned encoding_hint;
} app_io_request_t;

typedef struct {
    uint32_t generation;
    app_io_operation_t operation;
    esp_err_t error;
    char path[192];
    char *text;
    uint64_t *source_offsets;
    app_reader_encoding_t encoding;
    uint32_t invalid_count;
    unsigned encoding_hint;
    app_io_entry_t *entries;
    size_t count;
    size_t total_count;
    bool truncated;
    bool playlist_prepared; /* Music list installed by the IO worker, not LVGL. */
    uint64_t offset;
    uint64_t next_offset;
    uint64_t file_size;
    uint64_t text_start_offset;
    uint32_t font_px;
    bool dark;
    uint32_t request_id;
} app_io_result_t;

esp_err_t app_io_start(aether_storage_handle_t storage,
                       aether_audio_player_handle_t audio,
                       aether_video_player_handle_t video,
                       aether_nes_handle_t nes, app_io_handle_t *out_handle);
/* One pending request: rapid navigation replaces work that has not started. */
void app_io_submit(app_io_handle_t handle, app_io_operation_t operation,
                    uint32_t generation);
/* Reader saves use a separate queue and survive navigation/cancellation. */
esp_err_t app_io_submit_request(app_io_handle_t handle, const app_io_request_t *request);
void app_io_cancel(app_io_handle_t handle, uint32_t generation);
/* Owning-page exit: immediate cancellation with background cleanup. New IO
 * requests can queue, but cannot run until prior media owners have drained. */
esp_err_t app_io_release_media(app_io_handle_t handle);
esp_err_t app_io_media_release_status(app_io_handle_t handle, bool *pending, esp_err_t *error);
bool app_io_poll(app_io_handle_t handle, app_io_result_t *result);
void app_io_result_free(app_io_result_t *result);
/* Latest reader persistence result; filesystem reading remains available on error. */
esp_err_t app_io_bookmark_status(app_io_handle_t handle);
/* Accepted saves can wait in RAM while music plays or a failed commit retries. */
bool app_io_bookmark_pending(app_io_handle_t handle);

typedef struct {
    uint32_t request_id; /* Reject a dialog from an older USB ownership change. */
    bool pending;
    bool requested_enabled;
    bool blocked; /* Includes preparation and incomplete USB teardown. */
    bool active;
    bool host_connected;
    bool host_ejected;
    bool media_present;
    uint8_t diagnostic_stage, selected_route;
    uint64_t capacity_bytes;
    uint16_t usb_speed_mbps;
    uint32_t usb_clock_khz;
    bool usb_high_speed_fallback;
    esp_err_t last_error;
} app_io_usb_status_t;

/* Nonblocking submission on a separate control queue. Navigation cannot cancel
 * USB ownership changes. Exit never needs a local TF filesystem lease. */
esp_err_t app_io_usb_set_enabled(app_io_handle_t handle, bool enabled);
/* Only the explicit "computer has safely ejected" dialog may call this.
 * The token must still match; confirmation is never reused for later sessions. */
esp_err_t app_io_usb_exit_confirmed(app_io_handle_t handle, uint32_t expected_request_id);
esp_err_t app_io_usb_get_status(app_io_handle_t handle, app_io_usb_status_t *status);
/* Safe from remote-control producer tasks; no card, filesystem or USB IO. */
bool app_io_usb_storage_blocked(app_io_handle_t handle);
/* BEGIN reserves storage atomically against USB and only posts nonblocking stop
 * requests. Submit the GUI's last bookmark before WAIT. WAIT runs off the GUI;
 * RESUME cancels a failed shutdown and releases the gate after the worker leaves
 * its old operation, without restarting playback. */
esp_err_t app_io_quiesce_begin(app_io_handle_t handle);
esp_err_t app_io_quiesce_wait(app_io_handle_t handle, uint32_t timeout_ms);
esp_err_t app_io_resume(app_io_handle_t handle);
bool app_io_is_quiescing(app_io_handle_t handle);
