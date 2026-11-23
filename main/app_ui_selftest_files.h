#pragma once

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    bool done, found, collision;
    esp_err_t error;
} file_probe_result_t;

static file_probe_result_t file_probe_result;

static void file_probe_complete(app_ui_t *ui, const app_io_result_t *result)
{
    (void)ui;
    file_probe_result = (file_probe_result_t){.done = true, .collision = result->target_exists,
        .error = result->error};
    for (size_t i = 0; i < result->count; ++i) {
        const char *name = strrchr(result->entries[i].path, '/');
        if (name && !strcmp(name + 1, "gesture-fixture.txt")) file_probe_result.found = true;
    }
}

static file_probe_result_t file_probe_run(app_ui_t *ui, app_io_request_t *request)
{
    const file_probe_result_t timeout = {.error = ESP_ERR_TIMEOUT};
    if (!lvgl_port_lock(1000)) return timeout;
    if (ui->io_complete) { lvgl_port_unlock(); return timeout; }
    file_probe_result = (file_probe_result_t){0};
    app_ui_request_io_ex(ui, request, file_probe_complete);
    lvgl_port_unlock();
    for (unsigned elapsed = 0; elapsed < 4000; elapsed += 40) {
        vTaskDelay(pdMS_TO_TICKS(40));
        if (!lvgl_port_lock(100)) continue;
        const file_probe_result_t result = file_probe_result;
        lvgl_port_unlock();
        if (result.done) return result;
    }
    if (lvgl_port_lock(1000)) {
        if (ui->io_complete == file_probe_complete) {
            ui->io_complete = NULL;
            app_io_cancel(ui->io, ui->page_generation);
        }
        lvgl_port_unlock();
    }
    return timeout;
}

/* This covers the real worker without deleting, copying, or renaming any
 * user file. All writes live in an exclusively created temporary folder. */
static bool selftest_files(app_ui_t *ui)
{
    if (!ui->io || !aether_media_fs_path_ready(ui->storage, AETHER_STORAGE_MOUNT_POINT) ||
        app_io_usb_storage_blocked(ui->io)) {
        ESP_LOGI("ui_selftest", "SKIP file-operations TF unavailable");
        return true;
    }
    aether_media_fs_lease_t lease = {0};
    if (aether_media_fs_acquire(ui->storage, AETHER_STORAGE_MOUNT_POINT, &lease) != ESP_OK) return false;
    app_io_request_t *request = calloc(1, sizeof(*request));
    if (!request) { aether_media_fs_release(&lease); return false; }
    char directory[56], original[88], copied[88], moved[88];
    snprintf(directory, sizeof(directory), AETHER_STORAGE_MOUNT_POINT "/.aether-test-%08" PRIx32,
             (uint32_t)esp_timer_get_time());
    snprintf(original, sizeof(original), "%s/gesture-fixture.txt", directory);
    snprintf(copied, sizeof(copied), "%s/copy.txt", directory);
    snprintf(moved, sizeof(moved), "%s/moved.txt", directory);
    const uint32_t generation = aether_media_fs_generation(ui->storage, directory);
    const bool created = mkdir(directory, 0777) == 0;
    bool ready = false, found = false, copied_ok = false, protected = false;
    bool moved_ok = false, deleted = false;
    static const char payload[] = "Aether01 file worker fixture\n";
    if (created) {
        const int fd = open(original, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) {
            ready = write(fd, payload, sizeof(payload)) == sizeof(payload);
            if (close(fd) != 0) ready = false;
        }
    }
    if (ready) {
        request->media_generation = generation;
        request->destination_generation = generation;
        request->operation = APP_IO_SEARCH_FILES;
        request->limit = 4;
        strlcpy(request->path, directory, sizeof(request->path));
        strlcpy(request->query, "FIXTURE", sizeof(request->query));
        const file_probe_result_t result = file_probe_run(ui, request);
        found = result.done && result.error == ESP_OK && result.found;
    }
    if (found) {
        request->operation = APP_IO_COPY_FILE;
        strlcpy(request->path, original, sizeof(request->path));
        strlcpy(request->destination, copied, sizeof(request->destination));
        if (file_probe_run(ui, request).error == ESP_OK) {
            char bytes[sizeof(payload)];
            const int fd = open(copied, O_RDONLY);
            if (fd >= 0) {
                copied_ok = read(fd, bytes, sizeof(bytes)) == sizeof(bytes) && !memcmp(bytes, payload, sizeof(bytes));
                close(fd);
            }
        }
    }
    if (copied_ok) {
        request->operation = APP_IO_MOVE_FILE;
        protected = file_probe_run(ui, request).collision;
        strlcpy(request->destination, moved, sizeof(request->destination));
        struct stat info;
        moved_ok = protected && file_probe_run(ui, request).error == ESP_OK &&
            stat(original, &info) != 0 && stat(moved, &info) == 0;
    }
    if (moved_ok) {
        request->operation = APP_IO_DELETE_FILE;
        strlcpy(request->path, moved, sizeof(request->path));
        struct stat info;
        deleted = file_probe_run(ui, request).error == ESP_OK && stat(moved, &info) != 0 && stat(copied, &info) == 0;
    }
    bool cleaned = false;
    if (created && aether_media_fs_available(&lease) && generation == aether_media_fs_generation(ui->storage, directory)) {
        unlink(original);
        unlink(copied);
        unlink(moved);
        cleaned = rmdir(directory) == 0;
    }
    aether_media_fs_release(&lease);
    free(request);
    const bool ok = found && copied_ok && protected && moved_ok && deleted && cleaned;
    ESP_LOGI("ui_selftest", "%s file-operations search=%u copy_bytes=%u conflict=%u move=%u delete=%u fixtures_cleaned=%u",
             ok ? "PASS" : "FAIL", found, copied_ok, protected, moved_ok, deleted, cleaned);
    return ok;
}
