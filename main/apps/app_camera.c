/* SPDX-License-Identifier: Apache-2.0 */

#include "app_ui_internal.h"
#include "app_catalog_ui.h"
#include "app_otg.h"
#include "aether_usb_camera.h"
#include "aether_camera_record.h"
#include "aether_image.h"
#include "camera_jpeg_defaults.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    VIEW_WIDTH = 448,
    VIEW_HEIGHT = 274,
    PREVIEW_PERIOD_US = 60000,
    FRAME_TIMEOUT_US = 2500000,
    MAX_FRAME_BYTES = 8 * 1024 * 1024,
};

typedef enum {
    CAPTURE_NONE, CAPTURE_PHOTO_SAVED, CAPTURE_VIDEO_SAVED, CAPTURE_VIDEO_INTERRUPTED, CAPTURE_TF_ERROR,
    CAPTURE_FRAME_ERROR, CAPTURE_MEMORY_ERROR,
} capture_notice_t;

typedef struct {
    app_ui_t *ui;
    lv_obj_t *image, *placeholder, *state, *detail, *notice, *modes, *photo, *record, *record_label;
    SemaphoreHandle_t preview_lock;
    atomic_uint refs;
    atomic_uint preview_generation, mode_epoch;
    atomic_bool closing, photo_pending, record_requested, recording, frame_ready;
    atomic_int notice_code;
    uint32_t mode_generation;
    bool listed, owns_otg, profile_requested, worker_started;
    esp_err_t otg_error;
} camera_page_t;

typedef struct {
    aether_camera_record_t avi;
    FILE *file;
    char temporary[112], final[112];
    uint32_t storage_generation, camera_generation, interval;
    int64_t first_us, last_us;
} camera_file_t;

typedef struct {
    jpeg_encoder_handle_t encoder;
    uint8_t *input, *output;
    size_t input_capacity, output_capacity;
} camera_encoder_t;

static atomic_bool s_worker_running;

static void page_release(camera_page_t *page)
{
    if (atomic_fetch_sub(&page->refs, 1) != 1) return;
    vSemaphoreDelete(page->preview_lock);
    free(page);
}

static void publish_notice(camera_page_t *page, capture_notice_t notice)
{
    atomic_store(&page->notice_code, notice);
}

static FILE *create_capture(const char *extension, char *temporary, char *final, size_t capacity)
{
    char directory[64];
    int n = snprintf(directory, sizeof(directory), "%s/DCIM", aether_storage_mount_point());
    if (n < 0 || (size_t)n >= sizeof(directory) || (mkdir(directory, 0777) && errno != EEXIST)) return NULL;
    const unsigned long long stamp = (unsigned long long)(esp_timer_get_time() / 1000);
    for (unsigned suffix = 0; suffix < 1000; ++suffix) {
        n = snprintf(final, capacity, "%s/CAM_%010llu_%03u.%s", directory, stamp, suffix, extension);
        if (n < 0 || (size_t)n >= capacity) return NULL;
        n = snprintf(temporary, capacity, "%s.part", final);
        if (n < 0 || (size_t)n >= capacity) return NULL;
        struct stat existing;
        if (stat(final, &existing) == 0) continue;
        if (errno != ENOENT) return NULL;
        const int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0 && errno == EEXIST) continue;
        if (fd < 0) return NULL;
        FILE *file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
            unlink(temporary);
        }
        return file;
    }
    return NULL;
}

/* Complete JPEG files need Huffman tables even when the UVC payload relies on
 * the standard implied tables. Strip any bytes after the end-of-image marker. */
static size_t normalize_jpeg(uint8_t *data, size_t size, size_t capacity)
{
    if (size < 6 || data[0] != 0xff || data[1] != 0xd8) return 0;
    size_t pos = 2;
    bool has_dht = false;
    while (pos + 4 <= size && data[pos] == 0xff) {
        const size_t marker = pos++;
        while (pos < size && data[pos] == 0xff) ++pos;
        if (pos + 3 > size) return 0;
        const uint8_t tag = data[pos++];
        const size_t length = ((size_t)data[pos] << 8) | data[pos + 1];
        if (length < 2 || length > size - pos) return 0;
        if (tag == 0xc4) has_dht = true;
        if (tag == 0xda) {
            const size_t scan_start = pos + length;
            while (size >= scan_start + 2 && (data[size - 2] != 0xff || data[size - 1] != 0xd9))
                --size;
            if (size < scan_start + 2) return 0;
            if (has_dht) return size;
            if (sizeof(camera_jpeg_dht) > capacity - size) return 0;
            memmove(data + marker + sizeof(camera_jpeg_dht), data + marker, size - marker);
            memcpy(data + marker, camera_jpeg_dht, sizeof(camera_jpeg_dht));
            return size + sizeof(camera_jpeg_dht);
        }
        pos += length;
    }
    return 0;
}

static esp_err_t prepare_encoder(camera_encoder_t *encoder, size_t bytes)
{
    if (!encoder->encoder) {
        const jpeg_encode_engine_cfg_t config = {.timeout_ms = 100};
        esp_err_t error = jpeg_new_encoder_engine(&config, &encoder->encoder);
        if (error != ESP_OK) return error;
    }
    if (encoder->input_capacity < bytes) {
        free(encoder->input);
        const jpeg_encode_memory_alloc_cfg_t in = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
        encoder->input = jpeg_alloc_encoder_mem(bytes, &in, &encoder->input_capacity);
        if (!encoder->input) encoder->input_capacity = 0;
    }
    if (encoder->output_capacity < bytes) {
        free(encoder->output);
        const jpeg_encode_memory_alloc_cfg_t out = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
        encoder->output = jpeg_alloc_encoder_mem(bytes, &out, &encoder->output_capacity);
        if (!encoder->output) encoder->output_capacity = 0;
    }
    return encoder->input && encoder->output ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t encode_raw(camera_encoder_t *encoder, const uint8_t *data,
                            const aether_usb_camera_mode_t *mode, size_t bytes, uint32_t *length)
{
    esp_err_t error = prepare_encoder(encoder, bytes);
    if (error != ESP_OK) return error;
    /* UVC YUY2 is Y0 U Y1 V. The S31 encoder expects U Y0 V Y1. */
    for (size_t i = 0; i < bytes; i += 4) {
        encoder->input[i] = data[i + 1];
        encoder->input[i + 1] = data[i];
        encoder->input[i + 2] = data[i + 3];
        encoder->input[i + 3] = data[i + 2];
    }
    const jpeg_encode_cfg_t config = {
        .width = mode->width, .height = mode->height,
        .src_type = JPEG_ENCODE_IN_FORMAT_YUV422, .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = 78,
    };
    return jpeg_encoder_process(encoder->encoder, &config, encoder->input, bytes,
                                encoder->output, encoder->output_capacity, length);
}

static uint16_t rgb565(const uint8_t *group, unsigned odd)
{
    const int y = 298 * (group[2 * odd] - 16);
    const int u = group[1] - 128, v = group[3] - 128;
    int r = (y + 409 * v + 128) >> 8;
    int g = (y - 100 * u - 208 * v + 128) >> 8;
    int b = (y + 516 * u + 128) >> 8;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void preview_raw(const uint8_t *data, uint16_t width, uint16_t height,
                        uint16_t *output, uint16_t *out_w, uint16_t *out_h)
{
    uint32_t w = width < VIEW_WIDTH ? width : VIEW_WIDTH;
    uint32_t h = w * height / width;
    if (h > VIEW_HEIGHT) {
        h = VIEW_HEIGHT;
        w = h * width / height;
    }
    if (!w) w = 1;
    if (!h) h = 1;
    const uint32_t step_x = ((uint32_t)width << 16) / w;
    const uint32_t step_y = ((uint32_t)height << 16) / h;
    for (uint32_t y = 0, sy = 0; y < h; ++y, sy += step_y) {
        const uint8_t *row = data + (size_t)(sy >> 16) * width * 2;
        for (uint32_t x = 0, sx = 0; x < w; ++x, sx += step_x) {
            const unsigned col = sx >> 16;
            *output++ = rgb565(row + (col & ~1U) * 2, col & 1U);
        }
    }
    *out_w = (uint16_t)w;
    *out_h = (uint16_t)h;
}

static bool capture_card_present(aether_storage_handle_t storage, uint32_t generation)
{
    aether_storage_status_t status;
    /* The active lease keeps VFS mounted while shutdown closes admission.
     * Physical removal invalidates the generation before another card mounts. */
    return aether_storage_get_status(storage, &status) == ESP_OK && status.card_inserted &&
        aether_storage_generation(storage) == generation;
}

static void save_photo(camera_page_t *page, const uint8_t *jpeg, size_t size)
{
    aether_storage_handle_t storage = page->ui->storage;
    if (aether_storage_acquire(storage, 0) != ESP_OK) {
        publish_notice(page, CAPTURE_TF_ERROR);
        return;
    }
    const uint32_t generation = aether_storage_generation(storage);
    char temporary[112], final[112];
    FILE *file = create_capture("jpg", temporary, final, sizeof(final));
    bool saved = file && fwrite(jpeg, 1, size, file) == size;
    if (file && fclose(file) != 0) saved = false;
    if (file && capture_card_present(storage, generation)) {
        if (saved) saved = rename(temporary, final) == 0;
        else unlink(temporary);
    } else saved = false;
    aether_storage_release(storage);
    publish_notice(page, saved ? CAPTURE_PHOTO_SAVED : CAPTURE_TF_ERROR);
}

static bool start_recording(camera_page_t *page, camera_file_t *video,
                            const aether_usb_camera_mode_t *mode, uint32_t generation)
{
    aether_storage_handle_t storage = page->ui->storage;
    if (aether_storage_acquire(storage, 0) != ESP_OK) return false;
    video->storage_generation = aether_storage_generation(storage);
    video->camera_generation = generation;
    video->file = create_capture("avi", video->temporary, video->final, sizeof(video->final));
    video->interval = mode->interval_100ns;
    video->first_us = video->last_us = 0;
    if (video->file && aether_camera_record_start(&video->avi, video->file, mode->width, mode->height,
                                                 mode->interval_100ns) == ESP_OK) {
        atomic_store(&page->recording, true);
        return true;
    }
    if (video->file) {
        fclose(video->file);
        video->file = NULL;
        unlink(video->temporary);
    }
    aether_storage_release(storage);
    return false;
}

static void finish_recording(camera_page_t *page, camera_file_t *video, bool interrupted)
{
    if (!video->file) return;
    aether_storage_handle_t storage = page->ui->storage;
    bool saved = false;
    if (capture_card_present(storage, video->storage_generation)) {
        const uint64_t duration = (video->last_us >= video->first_us ?
            (uint64_t)(video->last_us - video->first_us) : 0) + video->interval / 10U;
        saved = aether_camera_record_finish(&video->avi, duration) == ESP_OK;
    } else aether_camera_record_abort(&video->avi);
    if (fclose(video->file) != 0) saved = false;
    video->file = NULL;
    if (capture_card_present(storage, video->storage_generation)) {
        if (saved) saved = rename(video->temporary, video->final) == 0;
        else unlink(video->temporary);
    } else saved = false;
    aether_storage_release(storage);
    publish_notice(page, !saved ? CAPTURE_TF_ERROR :
                   interrupted ? CAPTURE_VIDEO_INTERRUPTED : CAPTURE_VIDEO_SAVED);
    atomic_store(&page->recording, false);
}

static void camera_worker(void *arg)
{
    camera_page_t *page = arg;
    aether_image_decoder_t *decoder = NULL;
    camera_encoder_t encoder = {0};
    camera_file_t video = {0};
    size_t capacity = 0;
    uint8_t *frame_copy = NULL;
    uint16_t *pixels = heap_caps_malloc(VIEW_WIDTH * VIEW_HEIGHT * sizeof(*pixels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int64_t preview_at = 0, last_frame_at = esp_timer_get_time();
    if (!pixels) publish_notice(page, CAPTURE_MEMORY_ERROR);

    while (!atomic_load(&page->closing)) {
        if (app_io_is_quiescing(page->ui->io)) {
            atomic_store(&page->photo_pending, false);
            atomic_store(&page->record_requested, false);
            atomic_store(&page->frame_ready, false);
            finish_recording(page, &video, false);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (video.file && (!atomic_load(&page->record_requested) || !aether_storage_card_inserted(page->ui->storage))) {
            atomic_store(&page->record_requested, false);
            finish_recording(page, &video, false);
        }
        aether_usb_camera_frame_t frame;
        const unsigned mode_epoch = atomic_load(&page->mode_epoch);
        esp_err_t error = aether_usb_camera_frame_acquire(&frame, 25);
        const int64_t now = esp_timer_get_time();
        if (error != ESP_OK) {
            if (now - last_frame_at >= FRAME_TIMEOUT_US) {
                atomic_store(&page->frame_ready, false);
                if (atomic_exchange(&page->photo_pending, false)) publish_notice(page, CAPTURE_FRAME_ERROR);
                if (atomic_exchange(&page->record_requested, false)) {
                    if (video.file) finish_recording(page, &video, true);
                    else publish_notice(page, CAPTURE_FRAME_ERROR);
                }
            }
            vTaskDelay(1);
            continue;
        }

        const aether_usb_camera_mode_t mode = frame.mode;
        const uint32_t generation = frame.generation;
        const int64_t frame_time = frame.time_us > 0 ? frame.time_us : now;
        const size_t size = frame.size;
        const bool raw = mode.format == AETHER_USB_CAMERA_YUY2;
        const size_t raw_size = (size_t)mode.width * mode.height * 2;
        const bool valid = frame.data && mode.width && mode.height && size && size <= MAX_FRAME_BYTES &&
            (!raw || (!(mode.width & 1U) && size == raw_size));
        const bool want_photo = atomic_load(&page->photo_pending);
        const bool replaced = video.file && video.camera_generation != generation;
        const bool want_record = !replaced && atomic_load(&page->record_requested);
        const bool want_preview = pixels && now >= preview_at && !app_power_is_sleeping(page->ui->power);
        const bool needed = valid && (want_photo || want_record || want_preview);
        const size_t required = valid ? size + sizeof(camera_jpeg_dht) : 0;
        if (needed && capacity < required) {
            const size_t rounded = (required + 65535U) & ~(size_t)65535U;
            uint8_t *next = heap_caps_realloc(frame_copy, rounded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (next) {
                frame_copy = next;
                capacity = rounded;
            }
        }
        if (needed && capacity >= required) memcpy(frame_copy, frame.data, size);
        aether_usb_camera_frame_release(&frame);
        if (atomic_load(&page->closing) || app_io_is_quiescing(page->ui->io)) continue;
        if (replaced) {
            atomic_store(&page->record_requested, false);
            finish_recording(page, &video, true);
        }
        if (!valid) continue;
        last_frame_at = now;
        if (!needed) continue;
        if (capacity < required) {
            atomic_store(&page->photo_pending, false);
            atomic_store(&page->record_requested, false);
            publish_notice(page, CAPTURE_MEMORY_ERROR);
            continue;
        }

        size_t preview_jpeg_size = size;
        if (want_photo || want_record) {
            const uint8_t *jpeg = frame_copy;
            size_t jpeg_size;
            if (raw) {
                uint32_t encoded = 0;
                error = encode_raw(&encoder, frame_copy, &mode, size, &encoded);
                jpeg = encoder.output;
                jpeg_size = error == ESP_OK ? encoded : 0;
            } else {
                jpeg_size = normalize_jpeg(frame_copy, size, capacity);
                preview_jpeg_size = jpeg_size;
            }

            if (jpeg_size) {
                if (want_record && !video.file && !start_recording(page, &video, &mode, generation)) {
                    atomic_store(&page->record_requested, false);
                    publish_notice(page, CAPTURE_TF_ERROR);
                }
                if (video.file && want_record) {
                    error = aether_camera_record_write(&video.avi, jpeg, jpeg_size);
                    if (error == ESP_OK) {
                        if (!video.first_us) video.first_us = frame_time;
                        video.last_us = frame_time;
                    } else {
                        atomic_store(&page->record_requested, false);
                        finish_recording(page, &video, true);
                    }
                }
                if (want_photo && atomic_exchange(&page->photo_pending, false)) save_photo(page, jpeg, jpeg_size);
            } else if (raw || !video.file) {
                atomic_store(&page->photo_pending, false);
                atomic_store(&page->record_requested, false);
                publish_notice(page, CAPTURE_FRAME_ERROR);
            }
        }

        if (want_preview) {
            preview_at = now + PREVIEW_PERIOD_US;
            uint16_t w = 0, h = 0;
            if (raw) preview_raw(frame_copy, mode.width, mode.height, pixels, &w, &h);
            else {
                if (!decoder) (void)aether_image_decoder_new(&decoder);
                if (decoder && preview_jpeg_size &&
                    aether_image_decode_jpeg(decoder, frame_copy, preview_jpeg_size, pixels, VIEW_WIDTH, VIEW_HEIGHT,
                                            &w, &h) != ESP_OK) w = 0;
            }
            if (w && h && xSemaphoreTake(page->preview_lock, 0) == pdTRUE) {
                if (!atomic_load(&page->closing) && !app_io_is_quiescing(page->ui->io) &&
                    atomic_load(&page->mode_epoch) == mode_epoch) {
                    app_frames_push(page->ui->frames, pixels, w, h);
                    atomic_store(&page->preview_generation, generation);
                    atomic_store(&page->frame_ready, true);
                }
                xSemaphoreGive(page->preview_lock);
            }
        }
    }
    finish_recording(page, &video, false);
    if (encoder.encoder) jpeg_del_encoder_engine(encoder.encoder);
    free(encoder.input);
    free(encoder.output);
    if (decoder) aether_image_decoder_del(decoder);
    free(pixels);
    free(frame_copy);
    page_release(page);
    atomic_store(&s_worker_running, false);
    vTaskDelete(NULL);
}

static void camera_cleanup(app_ui_t *ui)
{
    camera_page_t *page = ui->page_context;
    atomic_store(&page->closing, true);
    if (page->worker_started) {
        aether_usb_camera_stop();
        /* Wait at most for one checked PSRAM copy. File and USB stop on their workers. */
        xSemaphoreTake(page->preview_lock, portMAX_DELAY);
        app_frames_set_active(ui->frames, false);
        xSemaphoreGive(page->preview_lock);
    }
    if (page->owns_otg) (void)app_otg_set_enabled(false);
    if (page->profile_requested) (void)app_otg_set_camera_profile(false);
    page_release(page);
}

static unsigned default_mode(const aether_usb_camera_status_t *camera)
{
    unsigned best = 0;
    uint32_t best_score = UINT32_MAX;
    for (unsigned i = 0; i < camera->mode_count; ++i) {
        const aether_usb_camera_mode_t *mode = &camera->modes[i];
        const unsigned fps = mode->interval_100ns ? 10000000U / mode->interval_100ns : 0;
        const uint32_t score = abs((int)mode->width - 640) + abs((int)mode->height - 480) +
            30 * abs((int)fps - 25) + (mode->format == AETHER_USB_CAMERA_YUY2 ? 2500U : 0);
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static void update_modes(camera_page_t *page, const aether_usb_camera_status_t *camera)
{
    char options[AETHER_USB_CAMERA_MAX_MODES * 32 + 1];
    size_t used = 0;
    for (unsigned i = 0; i < camera->mode_count; ++i) {
        const aether_usb_camera_mode_t *mode = &camera->modes[i];
        const uint32_t fps = mode->interval_100ns ? (10000000U + mode->interval_100ns / 2) / mode->interval_100ns : 0;
        const int n = snprintf(options + used, sizeof(options) - used, "%s%ux%u  %ufps  %s",
                               i ? "\n" : "", mode->width, mode->height, (unsigned)fps,
                               mode->format == AETHER_USB_CAMERA_MJPEG ? "MJPEG" : "YUY2");
        if (n <= 0 || (size_t)n >= sizeof(options) - used) return;
        used += n;
    }
    const unsigned choice = default_mode(camera);
    lv_dropdown_set_options(page->modes, options);
    lv_dropdown_set_selected(page->modes, choice);
    page->listed = true;
    page->mode_generation = camera->generation;
    atomic_store(&page->frame_ready, false);
    if (aether_usb_camera_select_mode(camera->generation, choice) == ESP_OK)
        atomic_fetch_add(&page->mode_epoch, 1);
    else page->listed = false;
}

static void enable(lv_obj_t *object, bool enabled)
{
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static const char *failure_stage(aether_usb_camera_failure_t failure)
{
    switch (failure) {
    case AETHER_USB_CAMERA_FAILURE_OPEN: return "视频协商";
    case AETHER_USB_CAMERA_FAILURE_FORMAT: return "视频格式";
    case AETHER_USB_CAMERA_FAILURE_START: return "视频启动";
    case AETHER_USB_CAMERA_FAILURE_FRAMES: return "接收画面";
    default: return "摄像头";
    }
}

static void camera_tick(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    camera_page_t *page = ui->page_context;
    if (!page || app_ui_input_blocked(ui)) return;
    if (!page->worker_started) {
        bool expected = false;
        if (!atomic_compare_exchange_strong(&s_worker_running, &expected, true)) return;
        atomic_fetch_add(&page->refs, 1);
        if (xTaskCreate(camera_worker, "usb_camera_app", 6144, page, 4, NULL) != pdPASS) {
            page_release(page);
            atomic_store(&s_worker_running, false);
            app_ui_label_text(page->state, "摄像头任务启动失败");
            return;
        }
        page->worker_started = true;
        app_frames_set_viewport(ui->frames, VIEW_WIDTH, VIEW_HEIGHT);
        app_frames_set_active(ui->frames, true);
    }

    aether_usb_host_status_t host = {0};
    app_otg_power_status_t power = {0};
    (void)app_otg_get_power_status(&power);
    esp_err_t otg = app_otg_get_status(&host);
    aether_usb_camera_status_t camera = {0};
    (void)aether_usb_camera_get_status(&camera);
    /* Keep an attached regular OTG device running until the camera is present. */
    const bool camera_session = !host.active || host.camera_profile || camera.connected;
    if (otg == ESP_OK && !power.manual_source_requested && !host.cleanup_pending &&
        camera_session && !app_io_usb_storage_blocked(ui->io) &&
        (host.enabled || host.state == AETHER_OTG_OFF)) {
        if (!page->profile_requested) {
            page->otg_error = app_otg_set_camera_profile(true);
            page->profile_requested = page->otg_error == ESP_OK;
        }
        if (page->profile_requested && !host.enabled) {
            page->otg_error = app_otg_set_enabled(true);
            if (page->otg_error == ESP_OK) page->owns_otg = true;
            else {
                (void)app_otg_set_camera_profile(false);
                page->profile_requested = false;
            }
        }
    }

    const bool camera_host = page->profile_requested && host.active && host.camera_profile;
    if (camera_host && camera.connected && camera.mode_count) {
        if (!page->listed || page->mode_generation != camera.generation) update_modes(page, &camera);
    } else if (page->listed) {
        page->listed = false;
        atomic_store(&page->frame_ready, false);
        lv_dropdown_set_options(page->modes, "分辨率");
    }
    const bool streaming = camera_host && camera.streaming && page->listed &&
        camera.active_mode == lv_dropdown_get_selected(page->modes);
    const bool requested = atomic_load(&page->record_requested);
    const bool recording = atomic_load(&page->recording);
    const bool ready = streaming && atomic_load(&page->frame_ready) &&
        atomic_load(&page->preview_generation) == camera.generation;
    enable(page->modes, page->listed && !requested && !recording && !atomic_load(&page->photo_pending));
    const bool tf_ready = aether_storage_is_mounted(ui->storage) && !app_io_usb_storage_blocked(ui->io);
    enable(page->photo, ready && tf_ready && !atomic_load(&page->photo_pending));
    enable(page->record, requested || (ready && tf_ready && !recording));
    app_ui_label_text(page->record_label, requested ? "停止录像" : recording ? "正在保存…" : "录像");
    if (page->listed && camera.error != ESP_OK && !requested && !recording)
        lv_obj_add_flag(page->state, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(page->state, LV_OBJ_FLAG_CLICKABLE);
    if (ready) {
        lv_obj_remove_flag(page->image, LV_OBJ_FLAG_HIDDEN);
        app_frames_present(ui->frames, page->image);
    } else lv_obj_add_flag(page->image, LV_OBJ_FLAG_HIDDEN);

    const esp_err_t power_error = otg != ESP_OK ? otg : page->otg_error != ESP_OK ?
        page->otg_error : host.last_error != ESP_OK ? host.last_error : power.last_error;
    if (power.manual_source_requested) app_ui_label_text(page->state, "请先关闭手动 OTG 供电");
    else if (app_io_usb_storage_blocked(ui->io)) app_ui_label_text(page->state, "请先退出 USB 文件传输");
    else if (!host.active && power_error != ESP_OK)
        app_ui_label_fmt(page->state, "OTG 启动 · %s", esp_err_to_name(power_error));
    else if (!host.active) app_ui_label_text(page->state, "请接入 USB 摄像头");
    else if (!camera_session) app_ui_label_text(page->state, "请接入 USB 摄像头");
    else if (!camera_host) app_ui_label_text(page->state, "正在切换摄像头模式…");
    else if (camera.installed && !camera.usb_enumerated && !camera.connected)
        app_ui_label_text(page->state, "OTG 已开启 · 等待 USB 枚举");
    else if (camera.connected && camera.error == ESP_ERR_NOT_SUPPORTED && !camera.mode_count)
        app_ui_label_text(page->state, "摄像头没有可用的 MJPEG / YUY2 模式");
    else if (camera.error != ESP_OK) app_ui_label_fmt(page->state, "%s · %s",
        failure_stage(camera.failure), esp_err_to_name(camera.error));
    else if (!camera.connected) app_ui_label_fmt(page->state, "USB %04X:%04X · 等待 UVC",
        (unsigned)host.vid, (unsigned)host.pid);
    else if (!camera.mode_count) app_ui_label_text(page->state, "摄像头没有可用的 MJPEG / YUY2 模式");
    else if (!ready) app_ui_label_text(page->state, "正在等待预览画面…");
    else if (recording) app_ui_label_text(page->state, "● 正在录像");
    else if (!tf_ready) app_ui_label_text(page->state, "插入 TF 卡后可拍摄");
    else app_ui_label_text(page->state, "");

    const bool show_detail = host.active && camera.error != ESP_OK && camera.failure != AETHER_USB_CAMERA_FAILURE_NONE;
    if (ready || show_detail) lv_obj_add_flag(page->placeholder, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(page->placeholder, LV_OBJ_FLAG_HIDDEN);
    if (show_detail) {
        lv_obj_remove_flag(page->detail, LV_OBJ_FLAG_HIDDEN);
        if (camera.failure == AETHER_USB_CAMERA_FAILURE_FRAMES) {
            const aether_usb_camera_rx_status_t *rx = &camera.rx;
            app_ui_label_fmt(page->detail,
                             "USB %lu  数据 %lu  空包 %lu\n跳过 %lu  错误 %lu  最大 %lu\n"
                             "包头异常 %lu  起止 %lu/%lu  SOI %lu\n"
                             "首包 %02X%02X%02X%02X  重提交 0x%X\n传输 %u  帧 %lu/%lu/%lu",
                             (unsigned long)rx->transfers, (unsigned long)rx->packets,
                             (unsigned long)rx->empty, (unsigned long)rx->skipped,
                             (unsigned long)rx->errors, (unsigned long)rx->max_packet_bytes,
                             (unsigned long)rx->invalid_headers, (unsigned long)rx->frame_ids,
                             (unsigned long)rx->frame_ends, (unsigned long)rx->invalid_soi,
                             (unsigned)rx->first_header[0], (unsigned)rx->first_header[1],
                             (unsigned)rx->first_header[2], (unsigned)rx->first_header[3],
                             (unsigned)rx->submit_error, rx->transfer_status, (unsigned long)camera.valid_frames,
                             (unsigned long)camera.rejected_frames, (unsigned long)camera.buffer_errors);
        } else app_ui_label_text(page->detail, "点下方错误提示重试");
    } else lv_obj_add_flag(page->detail, LV_OBJ_FLAG_HIDDEN);

    switch ((capture_notice_t)atomic_exchange(&page->notice_code, CAPTURE_NONE)) {
    case CAPTURE_PHOTO_SAVED: app_ui_label_text(page->notice, "照片已保存 · DCIM"); break;
    case CAPTURE_VIDEO_SAVED: app_ui_label_text(page->notice, "录像已保存 · DCIM"); break;
    case CAPTURE_VIDEO_INTERRUPTED: app_ui_label_text(page->notice, "录像中断，已保存录到的片段"); break;
    case CAPTURE_TF_ERROR: app_ui_label_text(page->notice, "保存失败，请检查 TF 卡"); break;
    case CAPTURE_FRAME_ERROR: app_ui_label_text(page->notice, "拍摄中断，未收到可保存的画面"); break;
    case CAPTURE_MEMORY_ERROR: app_ui_label_text(page->notice, "可用内存不足，请调低分辨率"); break;
    default: break;
    }
}

static void mode_changed(lv_event_t *event)
{
    camera_page_t *page = lv_event_get_user_data(event);
    if (!page->listed || atomic_load(&page->record_requested) || atomic_load(&page->recording) ||
        atomic_load(&page->photo_pending)) return;
    const uint16_t mode = lv_dropdown_get_selected(page->modes);
    if (mode < AETHER_USB_CAMERA_MAX_MODES &&
        aether_usb_camera_select_mode(page->mode_generation, mode) == ESP_OK) {
        atomic_fetch_add(&page->mode_epoch, 1);
        atomic_store(&page->frame_ready, false);
    }
}

static void take_photo(lv_event_t *event)
{
    camera_page_t *page = lv_event_get_user_data(event);
    atomic_store(&page->photo_pending, true);
    app_ui_label_text(page->notice, "正在拍照…");
}

static void toggle_recording(lv_event_t *event)
{
    camera_page_t *page = lv_event_get_user_data(event);
    const bool requested = atomic_load(&page->record_requested);
    atomic_store(&page->record_requested, !requested);
    app_ui_label_text(page->notice, requested ? "正在结束录像…" : "正在开始录像…");
}

static lv_obj_t *button(camera_page_t *page, lv_obj_t *parent, const char *name, int x,
                        uint32_t color, lv_event_cb_t callback, lv_obj_t **label)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_size(obj, 106, 44);
    lv_obj_set_pos(obj, x, 312);
    app_catalog_control(obj, color);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, page);
    lv_obj_t *text = app_ui_add_text(obj, name, LV_ALIGN_CENTER, 0, 0, 0xf3f7ff, NULL);
    if (label) *label = text;
    enable(obj, false);
    return obj;
}

void app_open_camera(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "USB 摄像头");
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(content, 0, 0);
    camera_page_t *page = calloc(1, sizeof(*page));
    if (page) page->preview_lock = xSemaphoreCreateMutex();
    if (!page || !page->preview_lock) {
        free(page);
        app_ui_add_text(content, "摄像头内存不足", LV_ALIGN_CENTER, 0, 0, 0xeaf2ff, NULL);
        return;
    }
    page->ui = ui;
    atomic_init(&page->refs, 1);
    atomic_init(&page->preview_generation, 0);
    atomic_init(&page->mode_epoch, 0);
    atomic_init(&page->closing, false);
    atomic_init(&page->photo_pending, false);
    atomic_init(&page->record_requested, false);
    atomic_init(&page->recording, false);
    atomic_init(&page->frame_ready, false);
    atomic_init(&page->notice_code, CAPTURE_NONE);
    ui->page_context = page;
    ui->page_cleanup = camera_cleanup;

    lv_obj_t *preview = lv_obj_create(content);
    lv_obj_remove_style_all(preview);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(preview, VIEW_WIDTH, VIEW_HEIGHT);
    lv_obj_set_style_bg_color(preview, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    page->image = lv_image_create(preview);
    lv_image_set_inner_align(page->image, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(page->image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(page->image);
    page->placeholder = app_ui_add_text(preview, LV_SYMBOL_VIDEO, LV_ALIGN_CENTER, 0, 0,
                                       0x467990, &lv_font_montserrat_48);
    page->detail = app_ui_add_text(preview, "", LV_ALIGN_CENTER, 0, 0, 0xbfcee6, NULL);
    lv_obj_set_width(page->detail, 416);
    lv_obj_set_style_text_align(page->detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(page->detail, LV_OBJ_FLAG_HIDDEN);
    page->state = app_ui_add_text(content, "正在准备摄像头…", LV_ALIGN_TOP_MID, 0, 284, 0xbfcee6, NULL);
    lv_obj_set_size(page->state, 418, 22);
    lv_label_set_long_mode(page->state, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(page->state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_ext_click_area(page->state, 8);
    lv_obj_add_event_cb(page->state, mode_changed, LV_EVENT_CLICKED, page);
    page->notice = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 368, 0xacbfd7, NULL);
    page->modes = lv_dropdown_create(content);
    lv_dropdown_set_options(page->modes, "分辨率");
    lv_obj_set_size(page->modes, 192, 44);
    lv_obj_set_pos(page->modes, 12, 312);
    app_catalog_control(page->modes, 0x20344C);
    lv_obj_set_style_pad_top(page->modes, 11, 0);
    lv_obj_set_style_text_font(page->modes, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(page->modes), app_ui_chinese_font(), 0);
    lv_obj_add_event_cb(page->modes, mode_changed, LV_EVENT_VALUE_CHANGED, page);
    enable(page->modes, false);
    page->photo = button(page, content, "拍照", 212, 0x2C576C, take_photo, NULL);
    page->record = button(page, content, "录像", 326, 0x71394D, toggle_recording, &page->record_label);
    app_ui_set_timer(ui, camera_tick, 33);
    camera_tick(ui->timer);
}
