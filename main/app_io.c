#include "aether_media_fs.h"
#include "aether_usb_bus.h"
#include "app_io.h"
#include "app_intercom.h"
#include "app_text.h"
#include "app_reader_file.h"
#include "aether_recorder.h"
#include "aether_usb_msc.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CATALOG_MAX 4096U
#define CATALOG_PAGE 256U
#define TEXT_MAX 32768U

typedef struct { app_io_request_t request; uint32_t id; } io_request_t;
typedef struct { app_io_request_t request; uint32_t id; } io_save_t;
typedef struct { bool enabled, host_eject_confirmed; uint32_t id; } io_usb_t;
typedef struct { uint32_t id; TickType_t deadline; } io_quiesce_t;
struct app_io {
    aether_storage_handle_t storage;
    aether_media_fs_lease_t media_lease;
    aether_audio_player_handle_t audio;
    aether_video_player_handle_t video;
    aether_nes_handle_t nes;
    QueueHandle_t requests, results, saves, latest_save;
    QueueHandle_t usb_requests;
    SemaphoreHandle_t usb_control;
    atomic_uint usb_id;
    atomic_bool usb_pending, usb_requested, usb_gate;
    atomic_int usb_error;
    QueueHandle_t quiesce_requests;
    atomic_uint quiesce_id, quiesce_completed;
    atomic_bool quiesce_gate, quiesce_requested, resume_requested;
    atomic_int quiesce_error;
    atomic_uint media_release_requested, media_release_completed;
    atomic_int media_release_error;
    TickType_t media_release_retry_at;
    bool quiesce_recording;
    atomic_uint latest_generation, latest_id, save_id, bookmark_completed_id;
    atomic_int bookmark_error;
    esp_err_t bookmark_init_error;
    io_save_t *save_batch; /* Worker-owned PSRAM cache survives transient NVS failures. */
    size_t save_pending_count, save_capacity;
    TickType_t bookmark_attempt_at;
    bool bookmark_retry;
};

static bool request_current(app_io_handle_t io, const app_io_result_t *r)
{
    return atomic_load(&io->latest_generation) == r->generation &&
           atomic_load(&io->latest_id) == r->request_id;
}

static bool safe_path(const char *path)
{
    return aether_media_fs_safe_path(path);
}

static bool matches(const char *name, app_io_operation_t filter)
{
    if (filter == APP_IO_FILES) return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    switch (filter) {
    case APP_IO_MUSIC: return !strcasecmp(dot, ".wav") || !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".m4a");
    case APP_IO_VIDEO: return !strcasecmp(dot, ".mjpg") || !strcasecmp(dot, ".mjpeg") || !strcasecmp(dot, ".avi") || !strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v");
    case APP_IO_NES: return !strcasecmp(dot, ".nes");
    case APP_IO_BOOK: return !strcasecmp(dot, ".txt");
    default: return false;
    }
}

static int entry_compare(const void *a, const void *b)
{
    const app_io_entry_t *l = a, *r = b;
    if (l->directory != r->directory) return l->directory ? -1 : 1;
    return strcasecmp(l->path, r->path);
}

static esp_err_t scan_directory(app_io_handle_t io, const char *directory,
                                app_io_operation_t filter, app_io_result_t *r, size_t *capacity)
{
    DIR *dir = opendir(directory);
    if (!dir) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    esp_err_t error = ESP_OK;
    struct dirent *entry;
    unsigned scanned = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!request_current(io, r) || app_io_usb_storage_blocked(io) ||
            !aether_media_fs_available(&io->media_lease)) { error = ESP_ERR_INVALID_STATE; break; }
        if ((++scanned & 31U) == 0) vTaskDelay(1);
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        app_io_entry_t item = {0};
        int length = snprintf(item.path, sizeof(item.path), "%s/%s", directory, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(item.path)) { r->truncated = true; continue; }
        struct stat info;
        if (stat(item.path, &info)) continue;
        item.directory = S_ISDIR(info.st_mode);
        if (item.directory ? filter != APP_IO_FILES : !matches(entry->d_name, filter)) continue;
        item.size = item.directory ? 0 : (uint64_t)info.st_size;
        ++r->total_count;
        if (r->count >= CATALOG_MAX) { r->truncated = true; continue; }
        if (r->count == *capacity) {
            size_t next = *capacity ? *capacity * 2 : 32;
            if (next > CATALOG_MAX) next = CATALOG_MAX;
            void *memory = heap_caps_realloc(r->entries, next * sizeof(item), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!memory) { error = ESP_ERR_NO_MEM; break; }
            r->entries = memory; *capacity = next;
        }
        r->entries[r->count++] = item;
    }
    closedir(dir);
    return error;
}

static esp_err_t catalog(app_io_handle_t io, const app_io_request_t *q, app_io_result_t *r, const char *leased_root)
{
    app_io_operation_t filter = q->operation == APP_IO_CATALOG ? q->filter : q->operation;
    size_t capacity = 0;
    esp_err_t error;
    if (q->path[0]) {
        if (!safe_path(q->path)) return ESP_ERR_INVALID_ARG;
        strlcpy(r->path, q->path, sizeof(r->path));
        error = scan_directory(io, q->path, filter, r, &capacity);
    } else if (filter == APP_IO_FILES) {
        strlcpy(r->path, leased_root, sizeof(r->path));
        error = scan_directory(io, r->path, filter, r, &capacity);
    } else {
        static const char *const dirs[] = {"Music", "Video", "ROMs", "Books"};
        if (filter < APP_IO_MUSIC || filter > APP_IO_BOOK) return ESP_ERR_INVALID_ARG;
        char path[64];
        const char *root = leased_root;
        const int written = snprintf(path, sizeof(path), "%s/%s", root, dirs[filter]);
        if (written < 0 || (size_t)written >= sizeof(path)) return ESP_ERR_INVALID_ARG;
        error = scan_directory(io, path, filter, r, &capacity);
        if (error == ESP_OK || error == ESP_ERR_NOT_FOUND)
            error = scan_directory(io, root, filter, r, &capacity);
    }
    if (error != ESP_OK) return error;
    if (r->count) qsort(r->entries, r->count, sizeof(*r->entries), entry_compare);
    size_t start = q->offset > r->count ? r->count : (size_t)q->offset;
    size_t limit = q->limit ? q->limit : CATALOG_PAGE;
    size_t count = r->count - start;
    if (count > limit) count = limit;
    if (start && count) memmove(r->entries, r->entries + start, count * sizeof(*r->entries));
    r->offset = start; r->next_offset = start + count;
    if (start + count < r->count) r->truncated = true;
    r->count = count;
    if (q->operation != APP_IO_CATALOG && count) strlcpy(r->path, r->entries[0].path, sizeof(r->path));
    return ESP_OK;
}

typedef struct { app_io_handle_t io; const app_io_result_t *result; } text_cancel_t;
static bool text_cancelled(void *context)
{
    const text_cancel_t *cancel = context;
    return !request_current(cancel->io, cancel->result) || app_io_usb_storage_blocked(cancel->io) ||
           !aether_media_fs_available(&cancel->io->media_lease);
}

static esp_err_t read_text(app_io_handle_t io, const app_io_request_t *q, app_io_result_t *r)
{
    if (!safe_path(q->path)) return ESP_ERR_INVALID_ARG;
    strlcpy(r->path, q->path, sizeof(r->path));
    app_reader_chunk_t chunk;
    text_cancel_t cancel = {.io=io,.result=r};
    const esp_err_t error = app_reader_file_read_cancelable(q->path, q->offset, q->limit,
        q->source_boundary, q->encoding_hint, text_cancelled, &cancel, &chunk);
    r->text = chunk.text;
    r->source_offsets = chunk.source_offsets;
    r->encoding = chunk.encoding; r->invalid_count = chunk.invalid_count;
    r->offset = chunk.offset;
    r->next_offset = chunk.next_offset;
    r->file_size = chunk.file_size;
    r->text_start_offset = chunk.text_start_offset;
    return error;
}

typedef struct {
    uint32_t version, font_px;
    uint64_t offset;
    uint8_t dark;
    char path[192];
    uint32_t encoding_hint; /* Version 2 uses previous tail padding; version 1 defaults to auto. */
} reader_bookmark_t;

static esp_err_t bookmark(const app_io_request_t *q, app_io_result_t *r)
{
    if (!safe_path(q->path)) return ESP_ERR_INVALID_ARG;
    uint32_t h1 = 2166136261U, h2 = 5381;
    for (const unsigned char *p = (const unsigned char *)q->path; *p; ++p) {
        h1 = (h1 ^ *p) * 16777619U; h2 = h2 * 33U + *p;
    }
    char key[16];
    snprintf(key, sizeof(key), "%08lx%07lx", (unsigned long)h1, (unsigned long)(h2 & 0x0FFFFFFF));
    nvs_handle_t nvs;
    bool save = q->operation == APP_IO_READER_SAVE;
    esp_err_t error = nvs_open_from_partition("reader_nvs", "reader",
                                              save ? NVS_READWRITE : NVS_READONLY, &nvs);
    if (!save) {
        r->font_px = 24; r->dark = true; strlcpy(r->path, q->path, sizeof(r->path));
        if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    }
    if (error != ESP_OK) return error;
    reader_bookmark_t value = {.version = 2, .font_px = q->font_px, .offset = q->offset, .dark = q->dark,
                               .encoding_hint = q->encoding_hint <= 4 ? q->encoding_hint : 0};
    strlcpy(value.path, q->path, sizeof(value.path));
    if (save) {
        reader_bookmark_t old;
        size_t length = sizeof(old);
        if (nvs_get_blob(nvs, key, &old, &length) == ESP_OK && length == sizeof(old) &&
            old.version == value.version && old.font_px == value.font_px && old.offset == value.offset &&
            old.dark == value.dark && old.encoding_hint == value.encoding_hint && !strncmp(old.path, value.path, sizeof(old.path))) {
            error = nvs_commit(nvs); nvs_close(nvs); return error;
        }
        error = nvs_set_blob(nvs, key, &value, sizeof(value));
        if (error == ESP_OK) error = nvs_commit(nvs);
    } else {
        size_t length = sizeof(value);
        error = nvs_get_blob(nvs, key, &value, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
        else if (error == ESP_OK && length == sizeof(value) && (value.version == 1 || value.version == 2) &&
                 !strncmp(value.path, q->path, sizeof(value.path))) {
            r->offset = value.offset; r->font_px = value.font_px == 16 ? 16 : 24; r->dark = value.dark != 0;
            r->encoding_hint = value.version == 2 && value.encoding_hint <= 4 ? value.encoding_hint : 0;
        }
    }
    nvs_close(nvs);
    return error;
}

static bool save_newer(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

static void drain_bookmarks(app_io_handle_t io)
{
    const uint32_t accepted_before_drain = atomic_load(&io->save_id);
    size_t count = io->save_pending_count;
    io_save_t incoming;
    bool drained = false;
    for (;;) {
        if (count == io->save_capacity) {
            const size_t capacity = io->save_capacity * 2;
            void *memory = capacity <= CATALOG_MAX ? heap_caps_realloc(io->save_batch,
                capacity * sizeof(*io->save_batch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
            if (!memory) break; /* Leave unreceived saves in their queues. */
            io->save_batch = memory; io->save_capacity = capacity;
        }
        if (xQueueReceive(io->saves, &incoming, 0) != pdTRUE &&
            xQueueReceive(io->latest_save, &incoming, 0) != pdTRUE) { drained = true; break; }
        size_t i = 0;
        while (i < count && strcmp(io->save_batch[i].request.path, incoming.request.path)) ++i;
        if (i == count) io->save_batch[count++] = incoming;
        else if (save_newer(incoming.id, io->save_batch[i].id)) io->save_batch[i] = incoming;
    }
    io_save_t *pending = io->save_batch;
    io->save_pending_count = count;
    if (!count) {
        atomic_store(&io->bookmark_completed_id, accepted_before_drain);
        return;
    }
    aether_recorder_handle_t recorder = aether_recorder_get_default();
    aether_recorder_status_t recording;
    if (recorder && (aether_recorder_get_status(recorder, &recording) != ESP_OK || recording.active)) return;
    aether_audio_player_status_t audio = {0};
    if (io->audio && (aether_audio_player_get_status(io->audio, &audio) != ESP_OK ||
        audio.state == AETHER_AUDIO_STARTING ||
        (audio.active && audio.state != AETHER_AUDIO_PAUSED))) return;
    aether_video_status_t video = {0};
    if (io->video && (aether_video_player_get_status(io->video, &video) != ESP_OK ||
        video.state == AETHER_VIDEO_STARTING || video.state == AETHER_VIDEO_PLAYING)) return;
    aether_nes_status_t nes = {0};
    if (io->nes && (aether_nes_get_status(io->nes, &nes) != ESP_OK ||
        nes.state == AETHER_NES_STARTING || nes.state == AETHER_NES_RUNNING)) return;
    if (io->bookmark_retry &&
        xTaskGetTickCount() - io->bookmark_attempt_at < pdMS_TO_TICKS(2000)) return;
    /* Coalescing above includes both queues and retained retries, so an older
     * overflow slot can never overwrite a newer position for the same book. */
    size_t failed = 0;
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < count; ++i) {
        const esp_err_t error = io->bookmark_init_error == ESP_OK ?
                                  bookmark(&pending[i].request, NULL) : io->bookmark_init_error;
        if (error != ESP_OK) {
            if (first_error == ESP_OK) first_error = error;
            pending[failed++] = pending[i];
            ESP_LOGW("app_io", "bookmark save failed: %s", esp_err_to_name(error));
        }
    }
    io->save_pending_count = failed;
    io->bookmark_retry = failed != 0;
    io->bookmark_attempt_at = xTaskGetTickCount();
    atomic_store(&io->bookmark_error, first_error);
    if (!failed && drained)
        atomic_store(&io->bookmark_completed_id, accepted_before_drain);
}

static esp_err_t prepare_music_playlist(app_io_handle_t io, const app_io_request_t *q, app_io_result_t *r)
{
    const app_io_operation_t filter = q->operation == APP_IO_CATALOG ? q->filter : q->operation;
    if (filter != APP_IO_MUSIC || !io->audio) return ESP_OK;
    if (!request_current(io, r)) return ESP_ERR_INVALID_STATE;
    const char **paths = r->count ? heap_caps_malloc(r->count * sizeof(*paths), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (r->count && !paths) return ESP_ERR_NO_MEM;
    for (size_t i=0; i<r->count; ++i) paths[i] = r->entries[i].path;
    const esp_err_t error = aether_audio_player_set_playlist(io->audio, paths, r->count);
    free(paths);
    if (error == ESP_OK) r->playlist_prepared = true;
    return error;
}

static esp_err_t perform_request(app_io_handle_t io, const app_io_request_t *q, app_io_result_t *r)
{
    if (!request_current(io, r)) return ESP_ERR_INVALID_STATE;
    if (q->operation != APP_IO_READER_LOAD && app_io_usb_storage_blocked(io))
        return ESP_ERR_INVALID_STATE;
    if (q->operation == APP_IO_READER_LOAD) {
        if (!safe_path(q->path)) return ESP_ERR_INVALID_ARG;
        /* Closing and immediately reopening a book must observe the save
         * submitted by page cleanup, even if both queues became ready at once. */
        drain_bookmarks(io);
        if (io->bookmark_init_error != ESP_OK) return io->bookmark_init_error;
        /* Reopening a book during a failed commit still resumes its latest
         * accepted position. The worker retains ownership until retry succeeds. */
        for (size_t i = 0; i < io->save_pending_count; ++i) {
            const app_io_request_t *saved = &io->save_batch[i].request;
            if (!strcmp(saved->path, q->path)) {
                strlcpy(r->path, q->path, sizeof(r->path));
                r->offset = saved->offset;
                r->font_px = saved->font_px == 16 ? 16 : 24;
                r->dark = saved->dark;
                r->encoding_hint = saved->encoding_hint <= 4 ? saved->encoding_hint : 0;
                return ESP_OK;
            }
        }
        return bookmark(q, r);
    }
    app_io_operation_t media = (q->operation == APP_IO_CATALOG || q->operation == APP_IO_PREPARE) ? q->filter : q->operation;
    if (media == APP_IO_VIDEO || media == APP_IO_NES || media == APP_IO_MUSIC || media == APP_IO_RECORDER) {
        aether_recorder_handle_t recorder = aether_recorder_get_default();
        if (recorder) ESP_RETURN_ON_ERROR(aether_recorder_stop_wait(recorder, 5000), "app_io", "save recording");
        if (!request_current(io, r)) return ESP_ERR_INVALID_STATE;
        if (io->nes) ESP_RETURN_ON_ERROR(aether_nes_stop_wait(io->nes, 3000), "app_io", "stop NES");
        if (!request_current(io, r)) return ESP_ERR_INVALID_STATE;
        if (io->video) ESP_RETURN_ON_ERROR(aether_video_player_stop_wait(io->video, 2000), "app_io", "stop video");
        if (!request_current(io, r)) return ESP_ERR_INVALID_STATE;
        if (media != APP_IO_MUSIC && io->audio)
            ESP_RETURN_ON_ERROR(aether_audio_player_stop_wait(io->audio, 2000), "app_io", "stop audio");
    }
    if (!request_current(io, r)) return ESP_ERR_INVALID_STATE;
    if (app_io_usb_storage_blocked(io)) return ESP_ERR_INVALID_STATE;
    const char *source = q->operation == APP_IO_RECORDER ? AETHER_STORAGE_MOUNT_POINT :
        q->path[0] ? q->path : aether_media_fs_default_root(io->storage);
    ESP_RETURN_ON_ERROR(aether_media_fs_acquire(io->storage, source, &io->media_lease), "app_io", "storage unavailable");
    esp_err_t error;
    if (q->operation == APP_IO_RECORDER) error = ESP_OK;
    else if (q->operation == APP_IO_PREPARE) {
        struct stat info;
        error = !safe_path(q->path) ? ESP_ERR_INVALID_ARG :
                stat(q->path, &info) || !S_ISREG(info.st_mode) ? ESP_ERR_NOT_FOUND : ESP_OK;
        strlcpy(r->path, q->path, sizeof(r->path));
    } else error = q->operation == APP_IO_READ_TEXT ? read_text(io, q, r) : catalog(io, q, r, source);
    if (error == ESP_OK && q->operation == APP_IO_BOOK && r->count) {
        app_io_request_t text = {.operation = APP_IO_READ_TEXT, .limit = 8192};
        strlcpy(text.path, r->path, sizeof(text.path)); error = read_text(io, &text, r);
    }
    if (!aether_media_fs_available(&io->media_lease)) error = ESP_ERR_INVALID_STATE;
    aether_media_fs_release(&io->media_lease);
    if (error == ESP_OK && (q->operation == APP_IO_CATALOG || q->operation == APP_IO_MUSIC))
        error = prepare_music_playlist(io, q, r);
    return error;
}

void app_io_result_free(app_io_result_t *r)
{
    if (!r) return;
    free(r->text); r->text = NULL; free(r->source_offsets); r->source_offsets = NULL;
    free(r->entries); r->entries = NULL; r->count = 0;
}

static esp_err_t usb_stop_media(app_io_handle_t io)
{
    aether_recorder_handle_t recorder = aether_recorder_get_default();
    if (recorder) {
        aether_recorder_status_t before = {0}, after = {0};
        ESP_RETURN_ON_ERROR(aether_recorder_get_status(recorder, &before), "app_io", "read recording state");
        ESP_RETURN_ON_ERROR(aether_recorder_stop_wait(recorder, 5000), "app_io", "save recording for USB");
        ESP_RETURN_ON_ERROR(aether_recorder_get_status(recorder, &after), "app_io", "confirm saved recording");
        if (after.active) return ESP_ERR_INVALID_STATE;
        if (before.active && after.last_error != ESP_OK) return after.last_error;
    }
    if (io->nes) ESP_RETURN_ON_ERROR(aether_nes_stop_wait(io->nes, 3000), "app_io", "stop NES for USB");
    if (io->video) ESP_RETURN_ON_ERROR(aether_video_player_stop_wait(io->video, 2000), "app_io", "stop video for USB");
    if (io->audio) ESP_RETURN_ON_ERROR(aether_audio_player_stop_wait(io->audio, 2000), "app_io", "stop music for USB");
    return ESP_OK;
}

static void perform_usb_request(app_io_handle_t io, const io_usb_t *request)
{
    esp_err_t error = ESP_OK;
    if (request->enabled) {
        error = usb_stop_media(io);
        /* This worker has closed every FILE/DIR from its previous request.
         * Bookmarks live in reader_nvs, not on the TF card. Failed commits keep
         * their existing RAM retry ownership and do not block USB recovery. */
        if (error == ESP_OK) drain_bookmarks(io);
        if (error == ESP_OK) {
            xSemaphoreTake(io->usb_control, portMAX_DELAY);
            const bool current = atomic_load(&io->usb_id) == request->id;
            xSemaphoreGive(io->usb_control);
            if (current) error = aether_usb_msc_enter(10000);
        }
    } else {
        error = request->host_eject_confirmed ? aether_usb_msc_exit_confirmed(10000) :
                aether_usb_msc_exit(10000);
    }
    xSemaphoreTake(io->usb_control, portMAX_DELAY);
    if (atomic_load(&io->usb_id) == request->id) {
        atomic_store(&io->usb_error, error);
        atomic_store(&io->usb_pending, false);
        /* Core ownership remains an independent guard if teardown failed. */
        atomic_store(&io->usb_gate, false);
    }
    xSemaphoreGive(io->usb_control);
    if (error != ESP_OK) ESP_LOGW("app_io", "USB %s failed: %s",
        request->enabled ? "export" : "restore", esp_err_to_name(error));
}

static esp_err_t quiesce_media_poll(app_io_handle_t io)
{
    esp_err_t error = ESP_OK;
    bool waiting = false;
    aether_recorder_handle_t recorder = aether_recorder_get_default();
    const esp_err_t results[] = {
        recorder ? aether_recorder_stop_wait(recorder, 0) : ESP_OK,
        io->nes ? aether_nes_stop_wait(io->nes, 0) : ESP_OK,
        io->video ? aether_video_player_stop_wait(io->video, 0) : ESP_OK,
        io->audio ? aether_audio_player_stop_wait(io->audio, 0) : ESP_OK,
    };
    for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        if (results[i] == ESP_ERR_TIMEOUT) waiting = true;
        else if (results[i] != ESP_OK && error == ESP_OK) error = results[i];
    }
    if (error != ESP_OK) return error;
    if (waiting) return ESP_ERR_TIMEOUT;
    if (recorder && io->quiesce_recording) {
        aether_recorder_status_t status;
        ESP_RETURN_ON_ERROR(aether_recorder_get_status(recorder, &status), "app_io", "confirm shutdown recording");
        if (status.active) return ESP_ERR_TIMEOUT;
        if (status.last_error != ESP_OK) return status.last_error;
    }
    return ESP_OK;
}

static void perform_quiesce_request(app_io_handle_t io, const io_quiesce_t *request)
{
    esp_err_t error = ESP_ERR_INVALID_STATE;
    while (atomic_load(&io->quiesce_requested) &&
           atomic_load(&io->quiesce_id) == request->id) {
        if ((int32_t)(xTaskGetTickCount() - request->deadline) >= 0) { error = ESP_ERR_TIMEOUT; break; }
        error = quiesce_media_poll(io);
        if (error == ESP_OK) {
            io->bookmark_retry = false; /* One bounded final attempt, not a flash retry loop. */
            drain_bookmarks(io);
            if (app_io_bookmark_pending(io)) {
                error = app_io_bookmark_status(io);
                if (error == ESP_OK) error = ESP_ERR_NOT_FINISHED;
            }
            break;
        }
        if (error != ESP_ERR_TIMEOUT) break;
        vTaskDelay(1);
    }
    atomic_store(&io->quiesce_error, error);
    atomic_store(&io->quiesce_completed, request->id);
}

static bool perform_control_request(app_io_handle_t io)
{
    if (atomic_exchange(&io->resume_requested, false)) {
        io_quiesce_t old;
        while (xQueueReceive(io->quiesce_requests, &old, 0) == pdTRUE) {}
        atomic_store(&io->quiesce_gate, false);
        return true;
    }
    io_quiesce_t quiesce;
    if (xQueueReceive(io->quiesce_requests, &quiesce, 0) == pdTRUE) {
        perform_quiesce_request(io, &quiesce);
        return true;
    }
    io_usb_t usb;
    if (xQueueReceive(io->usb_requests, &usb, 0) == pdTRUE) {
        perform_usb_request(io, &usb);
        return true;
    }
    return false;
}

static bool service_media_release(app_io_handle_t io)
{
    const uint32_t request = atomic_load(&io->media_release_requested);
    if (request == atomic_load(&io->media_release_completed)) return false;
    if (app_intercom_closing()) return true;
    const TickType_t now = xTaskGetTickCount();
    if (io->media_release_retry_at && (int32_t)(now-io->media_release_retry_at)<0) return true;
    aether_recorder_handle_t recorder = aether_recorder_get_default();
    const esp_err_t stops[] = {
        recorder ? aether_recorder_stop_wait(recorder, 0) : ESP_OK,
        io->nes ? aether_nes_stop_wait(io->nes, 1200) : ESP_OK,
        io->video ? aether_video_player_stop_wait(io->video, 0) : ESP_OK,
        io->audio ? aether_audio_player_stop_wait(io->audio, 0) : ESP_OK,
    };
    esp_err_t error = ESP_OK;
    for (unsigned i=0; i<sizeof(stops)/sizeof(stops[0]); ++i)
        if (stops[i] != ESP_OK && (error == ESP_OK || error == ESP_ERR_TIMEOUT)) error = stops[i];
    if (error == ESP_OK && io->audio) {
        aether_audio_player_status_t audio = {0};
        error = aether_audio_player_get_status(io->audio, &audio);
        if (error == ESP_OK && (audio.active || audio.closing)) error = ESP_ERR_NOT_FINISHED;
    }
    atomic_store(&io->media_release_error, error);
    if (error != ESP_OK) {
        io->media_release_retry_at = now + pdMS_TO_TICKS(100);
        return true; /* Retain all owners; never kill a worker or drop a lease. */
    }
    if (io->nes) aether_nes_release_frame(io->nes);
    if (io->video) aether_video_player_release_frame(io->video);
    atomic_store(&io->media_release_completed, request);
    io->media_release_retry_at = 0;
    return request != atomic_load(&io->media_release_requested);
}

static void perform_queued_request(app_io_handle_t io, const io_request_t *request)
{
    app_io_result_t r = {.operation = request->request.operation,
        .generation = request->request.generation, .request_id = request->id};
    /* A close can arrive while xQueueReceive sleeps. Recheck after dequeue:
     * the new page's queued preparation must not pass the old-page close. */
    while (service_media_release(io)) {
        if (!request_current(io, &r)) return;
        (void)perform_control_request(io);
        drain_bookmarks(io);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!request_current(io, &r)) return;
    r.error = perform_request(io, &request->request, &r);
    if (request_current(io, &r)) {
        app_io_result_t old;
        if (xQueueReceive(io->results, &old, 0) == pdTRUE) app_io_result_free(&old);
        if (xQueueSend(io->results, &r, 0) != pdTRUE) app_io_result_free(&r);
    } else app_io_result_free(&r);
}

static void io_task(void *argument)
{
    app_io_handle_t io = argument;
    while (true) {
        if (perform_control_request(io)) continue;
        if (service_media_release(io)) {
            drain_bookmarks(io);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        drain_bookmarks(io);
        io_request_t request;
        if (xQueueReceive(io->requests, &request, pdMS_TO_TICKS(50)) == pdTRUE) {
            perform_queued_request(io, &request);
        }
        drain_bookmarks(io);
    }
}

esp_err_t app_io_start(aether_storage_handle_t storage, aether_audio_player_handle_t audio,
                       aether_video_player_handle_t video, aether_nes_handle_t nes, app_io_handle_t *out_handle)
{
    if (!storage || !out_handle) return ESP_ERR_INVALID_ARG;
    app_io_handle_t io = calloc(1, sizeof(*io));
    if (!io) return ESP_ERR_NO_MEM;
    io->save_capacity = 16;
    io->save_batch = heap_caps_calloc(io->save_capacity, sizeof(*io->save_batch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!io->save_batch) { free(io); return ESP_ERR_NO_MEM; }
    io->storage = storage; io->audio = audio; io->video = video; io->nes = nes;
    atomic_init(&io->latest_generation, 0); atomic_init(&io->latest_id, 0); atomic_init(&io->save_id, 0);
    atomic_init(&io->bookmark_completed_id, 0);
    atomic_init(&io->usb_id, 0);
    atomic_init(&io->usb_pending, false); atomic_init(&io->usb_requested, false);
    atomic_init(&io->usb_gate, false); atomic_init(&io->usb_error, ESP_OK);
    atomic_init(&io->quiesce_id, 0); atomic_init(&io->quiesce_completed, 0);
    atomic_init(&io->quiesce_gate, false); atomic_init(&io->quiesce_requested, false);
    atomic_init(&io->resume_requested, false); atomic_init(&io->quiesce_error, ESP_OK);
    atomic_init(&io->media_release_requested, 0); atomic_init(&io->media_release_completed, 0);
    atomic_init(&io->media_release_error, ESP_OK);
    io->bookmark_init_error = nvs_flash_init_partition("reader_nvs");
    atomic_init(&io->bookmark_error, io->bookmark_init_error);
    if (io->bookmark_init_error != ESP_OK)
        ESP_LOGW("app_io", "reader bookmarks unavailable: %s", esp_err_to_name(io->bookmark_init_error));
    io->requests = xQueueCreate(1, sizeof(io_request_t));
    io->results = xQueueCreate(1, sizeof(app_io_result_t));
    io->saves = xQueueCreate(8, sizeof(io_save_t));
    io->latest_save = xQueueCreate(1, sizeof(io_save_t));
    io->usb_requests = xQueueCreate(1, sizeof(io_usb_t));
    io->quiesce_requests = xQueueCreate(1, sizeof(io_quiesce_t));
    io->usb_control = xSemaphoreCreateMutex();
    if (!io->requests || !io->results || !io->saves || !io->latest_save ||
        !io->usb_requests || !io->quiesce_requests || !io->usb_control ||
        xTaskCreate(io_task, "app_io", 6144, io, 3, NULL) != pdPASS) {
        if (io->requests) vQueueDelete(io->requests);
        if (io->results) vQueueDelete(io->results);
        if (io->saves) vQueueDelete(io->saves);
        if (io->latest_save) vQueueDelete(io->latest_save);
        if (io->usb_requests) vQueueDelete(io->usb_requests);
        if (io->quiesce_requests) vQueueDelete(io->quiesce_requests);
        if (io->usb_control) vSemaphoreDelete(io->usb_control);
        free(io->save_batch); free(io); return ESP_ERR_NO_MEM;
    }
    *out_handle = io;
    return ESP_OK;
}

esp_err_t app_io_submit_request(app_io_handle_t io, const app_io_request_t *q)
{
    if (!io || !q || q->operation < APP_IO_MUSIC || q->operation > APP_IO_RECORDER ||
        !memchr(q->path, 0, sizeof(q->path))) return ESP_ERR_INVALID_ARG;
    if (q->operation == APP_IO_READER_SAVE) {
        if (!safe_path(q->path)) return ESP_ERR_INVALID_ARG;
        if (io->bookmark_init_error != ESP_OK) return io->bookmark_init_error;
        const io_save_t save = {.request = *q, .id = atomic_fetch_add(&io->save_id, 1) + 1U};
        if (xQueueSend(io->saves, &save, 0) == pdTRUE) return ESP_OK;
        /* The current page's most recent location remains owned by the
         * worker even when normal saves are temporarily backlogged. */
        return xQueueOverwrite(io->latest_save, &save) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (q->operation != APP_IO_READER_LOAD && app_io_usb_storage_blocked(io))
        return ESP_ERR_INVALID_STATE;
    atomic_store(&io->latest_generation, q->generation);
    uint32_t id = atomic_fetch_add(&io->latest_id, 1) + 1;
    const io_request_t queued = {.request = *q, .id = id};
    return xQueueOverwrite(io->requests, &queued) == pdTRUE ? ESP_OK : ESP_FAIL;
}

void app_io_submit(app_io_handle_t io, app_io_operation_t operation, uint32_t generation)
{
    const app_io_request_t q = {.operation = operation, .generation = generation};
    (void)app_io_submit_request(io, &q);
}
void app_io_cancel(app_io_handle_t io, uint32_t generation)
{
    if (io) { atomic_store(&io->latest_generation, generation); atomic_fetch_add(&io->latest_id, 1); }
}
esp_err_t app_io_release_media(app_io_handle_t io)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    /* All calls only publish cancellation. Publish the generation afterward,
     * so the worker cannot ACK a close before the final stop request exists. */
    aether_recorder_handle_t recorder = aether_recorder_get_default();
    if (recorder) (void)aether_recorder_stop(recorder);
    if (io->nes) (void)aether_nes_stop(io->nes);
    if (io->video) (void)aether_video_player_stop(io->video);
    if (io->audio) (void)aether_audio_player_release_session(io->audio);
    atomic_store(&io->media_release_error, ESP_OK);
    atomic_fetch_add(&io->media_release_requested, 1U);
    return ESP_OK;
}
esp_err_t app_io_media_release_status(app_io_handle_t io, bool *pending, esp_err_t *error)
{
    if (!io || !pending || !error) return ESP_ERR_INVALID_ARG;
    *pending = atomic_load(&io->media_release_requested) != atomic_load(&io->media_release_completed);
    *error = atomic_load(&io->media_release_error);
    return ESP_OK;
}

bool app_io_poll(app_io_handle_t io, app_io_result_t *r)
{
    if (!io || !r) return false;
    while (xQueueReceive(io->results, r, 0) == pdTRUE) {
        if (request_current(io, r)) return true;
        app_io_result_free(r);
    }
    return false;
}

esp_err_t app_io_bookmark_status(app_io_handle_t io)
{
    return io ? atomic_load(&io->bookmark_error) : ESP_ERR_INVALID_ARG;
}

bool app_io_bookmark_pending(app_io_handle_t io)
{
    return io && atomic_load(&io->save_id) != atomic_load(&io->bookmark_completed_id);
}

bool app_io_usb_storage_blocked(app_io_handle_t io)
{
    return (io && (atomic_load(&io->usb_gate) || atomic_load(&io->quiesce_gate))) || aether_usb_msc_is_active();
}

static esp_err_t usb_submit(app_io_handle_t io, bool enabled, bool confirmed, uint32_t expected_id)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(io->usb_control, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (atomic_load(&io->quiesce_gate) || (confirmed &&
        (enabled || atomic_load(&io->usb_pending) ||
         atomic_load(&io->usb_id) != expected_id || !aether_usb_msc_is_active()))) {
        xSemaphoreGive(io->usb_control);
        return ESP_ERR_INVALID_STATE;
    }
    const bool old_pending = atomic_load(&io->usb_pending);
    const bool old_requested = atomic_load(&io->usb_requested);
    const bool old_gate = atomic_load(&io->usb_gate);
    const esp_err_t old_error = atomic_load(&io->usb_error);
    const uint32_t id = atomic_fetch_add(&io->usb_id, 1) + 1U;
    /* Publish the exclusion before this command can run. Late AVRCP requests
     * and queued/prepared TF operations must not restart a stopped producer. */
    atomic_store(&io->usb_gate, true);
    atomic_store(&io->usb_pending, true);
    atomic_store(&io->usb_requested, enabled);
    atomic_store(&io->usb_error, ESP_OK);
    const io_usb_t request = {.enabled = enabled, .host_eject_confirmed = confirmed, .id = id};
    const bool accepted = xQueueOverwrite(io->usb_requests, &request) == pdTRUE;
    if (!accepted) {
        atomic_store(&io->usb_id, id - 1U);
        atomic_store(&io->usb_pending, old_pending);
        atomic_store(&io->usb_requested, old_requested);
        atomic_store(&io->usb_gate, old_gate);
        atomic_store(&io->usb_error, old_error);
    }
    xSemaphoreGive(io->usb_control);
    return accepted ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t app_io_usb_set_enabled(app_io_handle_t io, bool enabled)
{
    if (enabled && aether_usb_bus_get_owner() == AETHER_USB_BUS_HOST)
        return ESP_ERR_INVALID_STATE;
    return usb_submit(io, enabled, false, 0);
}

esp_err_t app_io_usb_exit_confirmed(app_io_handle_t io, uint32_t expected_request_id)
{
    return usb_submit(io, false, true, expected_request_id);
}

bool app_io_is_quiescing(app_io_handle_t io)
{
    return io && atomic_load(&io->quiesce_gate);
}

esp_err_t app_io_quiesce_begin(app_io_handle_t io)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(io->usb_control, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (atomic_load(&io->usb_gate) || aether_usb_msc_is_active() ||
        atomic_load(&io->quiesce_gate)) {
        xSemaphoreGive(io->usb_control);
        return ESP_ERR_INVALID_STATE;
    }
    io->quiesce_recording = false;
    aether_recorder_handle_t recorder = aether_recorder_get_default();
    if (recorder) {
        aether_recorder_status_t recording;
        const esp_err_t error = aether_recorder_get_status(recorder, &recording);
        if (error != ESP_OK) { xSemaphoreGive(io->usb_control); return error; }
        io->quiesce_recording = recording.active;
    }
    atomic_fetch_add(&io->quiesce_id, 1);
    atomic_store(&io->quiesce_gate, true);
    atomic_store(&io->quiesce_requested, true);
    atomic_store(&io->quiesce_error, ESP_ERR_NOT_FINISHED);
    xSemaphoreGive(io->usb_control);
    /* Zero-time calls request all stops before any bounded final wait. */
    const esp_err_t error = quiesce_media_poll(io);
    return error == ESP_ERR_TIMEOUT ? ESP_OK : error;
}

esp_err_t app_io_quiesce_wait(app_io_handle_t io, uint32_t timeout_ms)
{
    if (!io || !timeout_ms) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(io->usb_control, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (!atomic_load(&io->quiesce_requested)) {
        xSemaphoreGive(io->usb_control);
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t started = xTaskGetTickCount(), budget = pdMS_TO_TICKS(timeout_ms);
    const uint32_t id = atomic_load(&io->quiesce_id);
    if (atomic_load(&io->quiesce_completed) == id) {
        const esp_err_t error = atomic_load(&io->quiesce_error);
        xSemaphoreGive(io->usb_control);
        return error;
    }
    const io_quiesce_t request = {.id = id, .deadline = started + budget};
    const bool accepted = xQueueOverwrite(io->quiesce_requests, &request) == pdTRUE;
    xSemaphoreGive(io->usb_control);
    if (!accepted) return ESP_ERR_NO_MEM;
    for (;;) {
        if (!atomic_load(&io->quiesce_requested) || atomic_load(&io->quiesce_id) != id)
            return ESP_ERR_INVALID_STATE;
        if (atomic_load(&io->quiesce_completed) == id) return atomic_load(&io->quiesce_error);
        if (xTaskGetTickCount() - started >= budget) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}

esp_err_t app_io_resume(app_io_handle_t io)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(io->usb_control, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (!atomic_load(&io->quiesce_gate)) { xSemaphoreGive(io->usb_control); return ESP_OK; }
    atomic_store(&io->quiesce_requested, false);
    atomic_fetch_add(&io->quiesce_id, 1);
    /* The worker acknowledges cancellation after its current FILE/DIR or NVS
     * call has ended, then releases the gate. New media never races old stops. */
    atomic_store(&io->resume_requested, true);
    xSemaphoreGive(io->usb_control);
    return ESP_OK;
}

esp_err_t app_io_usb_get_status(app_io_handle_t io, app_io_usb_status_t *status)
{
    if (!io || !status) return ESP_ERR_INVALID_ARG;
    aether_usb_msc_status_t usb = {0};
    const esp_err_t error = aether_usb_msc_get_status(&usb);
    *status = (app_io_usb_status_t){
        .request_id = atomic_load(&io->usb_id),
        .pending = atomic_load(&io->usb_pending),
        .requested_enabled = atomic_load(&io->usb_requested),
        .blocked = app_io_usb_storage_blocked(io), .active = usb.active,
        .host_connected = usb.host_connected, .media_present = usb.media_present,
        .host_ejected = usb.host_ejected,
        .usb_speed_mbps = usb.usb_speed_mbps, .usb_clock_khz = usb.usb_clock_khz,
        .usb_high_speed_fallback = usb.usb_high_speed_fallback,
        .diagnostic_stage = (uint8_t)usb.diagnostic_stage, .selected_route = usb.selected_route,
        .capacity_bytes = usb.capacity_bytes, .last_error = atomic_load(&io->usb_error),
    };
    if (status->last_error == ESP_OK) status->last_error = error != ESP_OK ? error : usb.last_error;
    return ESP_OK;
}
