#pragma once

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "co5300_display.h"

static bool nes_fixture_write(const char *path)
{
    enum { HEADER = 16, PRG = 0x4000, CHR = 0x2000, SIZE = HEADER + PRG + CHR };
    /* Original NROM test program. Generate a pulse and change the PPU color
     * each vblank so the real codec, renderer and frame transfer all run. */
    static const uint8_t program[] = {
        0x78, 0xd8, 0xa2, 0xff, 0x9a,
        0xa9, 0x00, 0x8d, 0x00, 0x20, 0x8d, 0x01, 0x20,
        0xa9, 0x01, 0x8d, 0x15, 0x40,
        0xa9, 0x9f, 0x8d, 0x00, 0x40,
        0xa9, 0xfd, 0x8d, 0x02, 0x40,
        0xa9, 0x08, 0x8d, 0x03, 0x40,
        0xa9, 0x08, 0x8d, 0x01, 0x20,
        /* $8026: wait for vblank, advance color, then repeat. */
        0xad, 0x02, 0x20, 0x10, 0xfb, 0xe6, 0x00,
        0xa9, 0x3f, 0x8d, 0x06, 0x20,
        0xa9, 0x00, 0x8d, 0x06, 0x20,
        0xa5, 0x00, 0x29, 0x3f, 0x8d, 0x07, 0x20, 0x4c, 0x26, 0x80,
    };
    uint8_t *rom = heap_caps_calloc(1, SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rom) return false;
    static const uint8_t header[] = {'N', 'E', 'S', 0x1a, 1, 1};
    memcpy(rom, header, sizeof(header));
    memcpy(rom + HEADER, program, sizeof(program));
    for (unsigned vector = 0x3ffb; vector <= 0x3fff; vector += 2) rom[HEADER + vector] = 0x80;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    bool ok = false;
    if (fd >= 0) {
        ok = write(fd, rom, SIZE) == SIZE;
        if (close(fd) != 0) ok = false;
        if (!ok) unlink(path);
    }
    free(rom);
    return ok;
}

static bool nes_wait_phase(app_ui_t *ui, bool sleeping, aether_nes_state_t state)
{
    for (unsigned elapsed = 0; elapsed < 4000; elapsed += 40) {
        aether_nes_status_t nes = {0};
        app_power_status_t power = {0};
        if (aether_nes_get_status(ui->nes, &nes) != ESP_OK ||
            app_power_get_status(ui->power, &power) != ESP_OK) return false;
        if (nes.state == AETHER_NES_ERROR || power.display_error != ESP_OK) return false;
        if (power.sleeping == sleeping && app_power_is_sleeping(ui->power) == sleeping &&
            atomic_load(&ui->screen_suspended) == sleeping && nes.state == state) return true;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return false;
}

static bool nes_take_frames(app_frames_handle_t frames, app_frames_metrics_t *metrics)
{
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const esp_err_t error = app_frames_get_metrics(frames, metrics);
        if (error != ESP_ERR_TIMEOUT) return error == ESP_OK;
        vTaskDelay(1);
    }
    return false;
}

static bool nes_measure_playing(app_ui_t *ui, const char *phase)
{
    aether_nes_status_t first = {0}, last = {0};
    app_frames_metrics_t frames_first = {0}, frames_last = {0};
    app_display_metrics_t display_first = {0}, display_last = {0};
    ac101s_audio_write_metrics_t audio_first = {0}, audio_last = {0};
    co5300_te_metrics_t te_first = {0}, te_last = {0};
    ac101s_audio_handle_t codec = ui->board->audio;
    bool ok = aether_nes_get_status(ui->nes, &first) == ESP_OK &&
        nes_take_frames(ui->frames, &frames_first) &&
        ac101s_audio_get_write_metrics(codec, &audio_first) == ESP_OK;
    if (!ok) return false;
    app_display_metrics_set_probe_active(false);
    app_display_metrics_set_probe_active(true);
    app_display_metrics_get(&display_first);
    (void)co5300_display_get_te_metrics(ui->board->display_io, &te_first);
    const int64_t started = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(2000));
    const unsigned wall_ms = (unsigned)((esp_timer_get_time() - started) / 1000);
    app_display_metrics_get(&display_last);
    (void)co5300_display_get_te_metrics(ui->board->display_io, &te_last);
    app_display_metrics_set_probe_active(false);
    ok = aether_nes_get_status(ui->nes, &last) == ESP_OK &&
        nes_take_frames(ui->frames, &frames_last) &&
        ac101s_audio_get_write_metrics(codec, &audio_last) == ESP_OK;
    const uint32_t emulated = last.emulated_frames - first.emulated_frames;
    const uint32_t presented = frames_last.presented_frames - frames_first.presented_frames;
    const uint32_t refreshes = display_last.completed_refreshes - display_first.completed_refreshes;
    const uint32_t drops = display_last.dropped_regions - display_first.dropped_regions;
    const uint32_t audio_writes = audio_last.write_calls - audio_first.write_calls;
    const uint32_t direct = display_last.nes.direct_frames - display_first.nes.direct_frames;
    const uint32_t direct_timed = display_last.nes.direct_timed_frames - display_first.nes.direct_timed_frames;
    const uint32_t direct_late = display_last.nes.direct_late_frames - display_first.nes.direct_late_frames;
    ok = ok && last.state == AETHER_NES_RUNNING && display_last.available &&
        emulated * 1000U >= wall_ms * 48U && emulated * 1000U <= wall_ms * 65U &&
        (presented + 1U) * 1000U >= wall_ms * 30U && (refreshes + 1U) * 1000U >= wall_ms * 30U &&
        direct >= 30U && direct_timed + 1U >= direct && !direct_late &&
        display_last.max_refresh_gap_us <= 85000U && !drops &&
        display_last.flush_wait_timeouts == display_first.flush_wait_timeouts &&
        audio_writes > 0 && last.audio_errors == first.audio_errors &&
        audio_last.output_errors == audio_first.output_errors;
    ESP_LOGI("ui_selftest", "%s NES-%s wall_ms=%u emulated=%" PRIu32
        " presented=%" PRIu32 " lcd_refreshes=%" PRIu32 " lcd_max_gap_us=%" PRIu32
        " lcd_drops=%" PRIu32 " audio_writes=%" PRIu32 " te_synced=%" PRIu32 " te_fallback=%" PRIu32
        " rendered=%" PRIu32 " replaced=%" PRIu32
        " nes_timed=%" PRIu32 " estimated_late=%" PRIu32 " te_to_submit_max_us=%" PRIu32
        " te_to_dma_max_us=%" PRIu32 " estimated_scan_margin_min_us=%" PRId32,
        ok ? "PASS" : "FAIL", phase, wall_ms, emulated, presented, refreshes,
        display_last.max_refresh_gap_us, drops, audio_writes, te_last.synced - te_first.synced,
        te_last.fallback_frames - te_first.fallback_frames, last.frame_number - first.frame_number,
        frames_last.replaced_frames - frames_first.replaced_frames,
        display_last.nes.timed_frames - display_first.nes.timed_frames,
        display_last.nes.late_frames - display_first.nes.late_frames,
        display_last.nes.max_te_to_submit_us, display_last.nes.max_te_to_done_us,
        display_last.nes.min_scan_margin_us);
    ESP_LOGI("ui_selftest", "NES-phase %s us/frame core=%" PRIu32 " audio_out=%" PRIu32
        " convert=%" PRIu32 " callback=%" PRIu32 " input=%" PRIu32 " yield=%" PRIu32,
        phase, last.core_us_per_frame, last.audio_output_us_per_frame,
        last.video_convert_us_per_frame, last.frame_callback_us_per_frame,
        last.input_us_per_frame, last.yield_us_per_frame);
    ESP_LOGI("ui_selftest", "NES-direct %s frames=%" PRIu32 " copy_max_us=%" PRIu32
        " submit_max_us=%" PRIu32 " wait_max_us=%" PRIu32 " timed=%" PRIu32 " late=%" PRIu32
        " minimum_margin_us=%" PRId32,
        phase, direct,
        display_last.nes.direct_max_copy_us, display_last.nes.direct_max_submit_us,
        display_last.nes.direct_max_wait_us, direct_timed, direct_late,
        display_last.nes.direct_min_scan_margin_us);
    return ok;
}

static bool nes_measure_frozen(app_ui_t *ui, const char *phase, bool screen_off)
{
    aether_nes_status_t first = {0}, last = {0};
    app_frames_metrics_t frames_first = {0}, frames_last = {0};
    app_display_metrics_t display_first = {0}, display_last = {0};
    ac101s_audio_write_metrics_t audio_first = {0}, audio_last = {0};
    ac101s_audio_handle_t codec = ui->board->audio;
    vTaskDelay(pdMS_TO_TICKS(80)); /* Allow the last in-flight frame/transfer to settle. */
    app_display_metrics_get(&display_first);
    bool ok = aether_nes_get_status(ui->nes, &first) == ESP_OK &&
        app_frames_get_metrics(ui->frames, &frames_first) == ESP_OK &&
        ac101s_audio_get_write_metrics(codec, &audio_first) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(430));
    app_display_metrics_get(&display_last);
    ok = ok && aether_nes_get_status(ui->nes, &last) == ESP_OK &&
        app_frames_get_metrics(ui->frames, &frames_last) == ESP_OK &&
        ac101s_audio_get_write_metrics(codec, &audio_last) == ESP_OK &&
        first.state == AETHER_NES_PAUSED && last.state == AETHER_NES_PAUSED &&
        last.emulated_frames == first.emulated_frames && last.frame_number == first.frame_number &&
        frames_last.presented_frames == frames_first.presented_frames &&
        audio_last.write_calls == audio_first.write_calls &&
        (!screen_off || display_last.submitted_sequence == display_first.submitted_sequence);
    ESP_LOGI("ui_selftest", "%s NES-%s frame_before=%" PRIu32 " frame_after=%" PRIu32,
             ok ? "PASS" : "FAIL", phase, first.emulated_frames, last.emulated_frames);
    return ok;
}

static esp_err_t nes_wait_media_released(app_ui_t *ui)
{
    const int64_t deadline = esp_timer_get_time() + INT64_C(2000000);
    esp_err_t release_error;
    do {
        bool pending;
        const esp_err_t status = app_io_media_release_status(ui->io, &pending, &release_error);
        if (status != ESP_OK) return status;
        if (!pending) return release_error;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (esp_timer_get_time() < deadline);
    return release_error == ESP_OK ? ESP_ERR_TIMEOUT : release_error;
}

static void nes_remove_prior_fixture(app_ui_t *ui, const char *current, uint32_t generation,
                                     const aether_media_fs_lease_t *lease)
{
    static const char directory[] = AETHER_STORAGE_MOUNT_POINT "/.aether-nes-00d3687f";
    static const char previous[] = AETHER_STORAGE_MOUNT_POINT "/.aether-nes-00d3687f/pacing.nes";
    if (!strcmp(current, previous)) return;
    struct stat old_info, new_info;
    if (stat(previous, &old_info) != 0) {
        if (errno != ENOENT) ESP_LOGW("ui_selftest", "NES prior fixture could not be inspected");
        return;
    }
    bool matched = stat(current, &new_info) == 0 && S_ISREG(old_info.st_mode) && S_ISREG(new_info.st_mode) &&
                   old_info.st_size > 0 && old_info.st_size == new_info.st_size;
    if (matched) {
        FILE *old = fopen(previous, "rb");
        FILE *fresh = fopen(current, "rb");
        matched = old && fresh;
        uint8_t old_bytes[512], new_bytes[512];
        for (size_t left = (size_t)old_info.st_size; matched && left;) {
            const size_t chunk = left < sizeof(old_bytes) ? left : sizeof(old_bytes);
            matched = aether_media_fs_available(lease) && fread(old_bytes, 1, chunk, old) == chunk &&
                      fread(new_bytes, 1, chunk, fresh) == chunk && !memcmp(old_bytes, new_bytes, chunk);
            left -= chunk;
        }
        if (old && fclose(old) != 0) matched = false;
        if (fresh && fclose(fresh) != 0) matched = false;
    }
    if (matched && aether_media_fs_available(lease) && generation == aether_media_fs_generation(ui->storage, previous) &&
        unlink(previous) == 0) {
        if (rmdir(directory) == 0) ESP_LOGI("ui_selftest", "NES prior fixture cleaned");
        else ESP_LOGW("ui_selftest", "NES prior fixture directory retained: %s", directory);
    } else ESP_LOGW("ui_selftest", "NES prior fixture retained: %s", previous);
}

static bool selftest_nes(app_ui_t *ui)
{
    if (!ui->nes || !ui->board->audio || app_io_usb_storage_blocked(ui->io) ||
        !aether_media_fs_path_ready(ui->storage, AETHER_STORAGE_MOUNT_POINT)) {
        ESP_LOGI("ui_selftest", "SKIP NES playback: backend, audio or TF unavailable");
        return true;
    }
    char directory[64], path[96];
    snprintf(directory, sizeof(directory), AETHER_STORAGE_MOUNT_POINT "/.aether-nes-%08" PRIx32,
             (uint32_t)esp_timer_get_time());
    snprintf(path, sizeof(path), "%s/pacing.nes", directory);
    aether_media_fs_lease_t lease = {0};
    if (aether_media_fs_acquire(ui->storage, path, &lease) != ESP_OK) return false;
    const uint32_t generation = aether_media_fs_generation(ui->storage, path);
    const bool created = mkdir(directory, 0777) == 0;
    const bool ready = created && nes_fixture_write(path);
    aether_media_fs_release(&lease);

    bool opened = false;
    if (ready && lvgl_port_lock(1000)) {
        app_open_nes_path(ui, path);
        lvgl_port_unlock();
        opened = true;
    }
    const bool running = opened && nes_wait_phase(ui, false, AETHER_NES_RUNNING);
    const bool initial_ok = running && nes_measure_playing(ui, "initial");
    const bool sleep_requested = running && app_power_sleep_now(ui->power) == ESP_OK;
    const bool frozen_ok = sleep_requested && nes_wait_phase(ui, true, AETHER_NES_PAUSED) &&
        nes_measure_frozen(ui, "screen-off", true);
    const bool resumed = sleep_requested && app_power_wake(ui->power) == ESP_OK &&
        nes_wait_phase(ui, false, AETHER_NES_RUNNING);
    const bool resume_ok = resumed && nes_measure_playing(ui, "resumed");

    bool manual_ok = false;
    if (resumed && aether_nes_set_paused(ui->nes, true) == ESP_OK &&
        nes_wait_phase(ui, false, AETHER_NES_PAUSED)) {
        const bool asleep = app_power_sleep_now(ui->power) == ESP_OK &&
            nes_wait_phase(ui, true, AETHER_NES_PAUSED);
        const bool awake_and_paused = app_power_wake(ui->power) == ESP_OK &&
            nes_wait_phase(ui, false, AETHER_NES_PAUSED);
        const bool stays_paused = awake_and_paused && nes_measure_frozen(ui, "manual-pause-after-wake", false);
        const bool unpaused = aether_nes_set_paused(ui->nes, false) == ESP_OK &&
            nes_wait_phase(ui, false, AETHER_NES_RUNNING);
        manual_ok = asleep && stays_paused && unpaused;
    }

    bool returned = false;
    if (opened && lvgl_port_lock(1000)) {
        app_ui_show_home(ui);
        lvgl_port_unlock();
        returned = true;
    }
    /* Navigation already queued a close. The IO worker owns the NES stop
     * barrier until it acknowledges release of the codec and files. */
    const esp_err_t stop_error = !opened ? ESP_OK : returned ? nes_wait_media_released(ui) :
                                 aether_nes_stop_wait(ui->nes, 2000);
    const bool stopped = stop_error == ESP_OK;
    bool cleaned = false;
    if (created && stopped && aether_media_fs_acquire(ui->storage, path, &lease) == ESP_OK) {
        if (generation == aether_media_fs_generation(ui->storage, path)) {
            if (ready) nes_remove_prior_fixture(ui, path, generation, &lease);
            if (aether_media_fs_available(&lease)) {
                const bool removed = !ready || unlink(path) == 0;
                cleaned = removed && rmdir(directory) == 0;
            }
        }
        aether_media_fs_release(&lease);
    }
    const bool ok = initial_ok && frozen_ok && resume_ok && manual_ok && returned && stopped && cleaned;
    ESP_LOGI("ui_selftest", "%s NES fixture=%u initial=%u frozen=%u resumed=%u manual=%u home=%u stop=%u cleaned=%u stop_error=%s",
             ok ? "PASS" : "FAIL", ready, initial_ok, frozen_ok, resume_ok, manual_ok, returned, stopped, cleaned,
             esp_err_to_name(stop_error));
    if (created && !cleaned) ESP_LOGW("ui_selftest", "NES fixture retained: %s", directory);
    return ok;
}
