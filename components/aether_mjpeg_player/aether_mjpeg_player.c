#include "aether_media_fs.h"
#include "aether_mjpeg_player.h"
#include "mjpeg_stream.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MJPEG_MAX_WIDTH       480U
#define MJPEG_MAX_HEIGHT      480U
#define MJPEG_MAX_FRAME_BYTES (512U * 1024U)
#define MJPEG_TASK_STACK      6144U

static const char *TAG = "aether_mjpeg";

struct aether_mjpeg_player_t {
    aether_storage_handle_t storage;
    aether_media_fs_lease_t media_lease;
    portMUX_TYPE lock;
    bool running;
    atomic_bool stop_requested;
    atomic_bool pause_requested;
    bool user_paused, screen_paused;
    aether_mjpeg_state_t state;
    esp_err_t last_error;
    uint32_t frame_number;
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint8_t requested_fps;
    uint32_t actual_fps_x100, source_fps_x100;
    uint32_t read_us, decode_us, callback_us, late_frames, elapsed_ms;
    char path[192];
    aether_mjpeg_frame_cb_t frame_cb;
    void *user_ctx;
    uint8_t *output_buffer;
    size_t output_buffer_size;
    bool release_output_requested;
};

static bool reader_cancelled(void *context)
{
    aether_mjpeg_player_handle_t handle = context;
    return atomic_load(&handle->stop_requested) || !aether_media_fs_available(&handle->media_lease);
}

static void reader_yield(void *context)
{
    (void)context;
    vTaskDelay(1);
}

static void player_task(void *argument)
{
    aether_mjpeg_player_handle_t handle = argument;
    FILE *file = NULL;
    jpeg_decoder_handle_t decoder = NULL;
    uint8_t *input = NULL;
    uint8_t *read_buffer = NULL;
    uint8_t *output = NULL;
    bool storage_locked = false;
    bool finished = false;
    uint32_t produced_frames = 0;
    esp_err_t result = ESP_OK;

    if (aether_media_fs_acquire(handle->storage, handle->path, &handle->media_lease) != ESP_OK) {
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }
    storage_locked = true;
    if (atomic_load(&handle->stop_requested)) {
        goto done;
    }
    file = fopen(handle->path, "rb");
    if (!file) {
        result = ESP_ERR_NOT_FOUND;
        goto done;
    }

    jpeg_decode_memory_alloc_cfg_t input_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    jpeg_decode_memory_alloc_cfg_t output_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t input_allocated = 0;
    portENTER_CRITICAL(&handle->lock);
    output = handle->output_buffer;
    size_t output_allocated = handle->output_buffer_size;
    portEXIT_CRITICAL(&handle->lock);
    input = jpeg_alloc_decoder_mem(MJPEG_MAX_FRAME_BYTES, &input_cfg,
                                   &input_allocated);
    read_buffer = malloc(AETHER_MJPEG_READ_BYTES);
    if (!output) {
        output = jpeg_alloc_decoder_mem(MJPEG_MAX_WIDTH * MJPEG_MAX_HEIGHT * 2U,
                                        &output_cfg, &output_allocated);
        portENTER_CRITICAL(&handle->lock);
        handle->output_buffer = output;
        handle->output_buffer_size = output_allocated;
        portEXIT_CRITICAL(&handle->lock);
    }
    if (!input || !output || !read_buffer || input_allocated < MJPEG_MAX_FRAME_BYTES ||
        output_allocated < MJPEG_MAX_WIDTH * MJPEG_MAX_HEIGHT * 2U) {
        result = ESP_ERR_NO_MEM;
        goto done;
    }

    const jpeg_decode_engine_cfg_t engine_cfg = {
        .timeout_ms = 80,
    };
    result = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (result != ESP_OK) {
        goto done;
    }
    const jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    aether_mjpeg_reader_t reader = {
        .file = file, .buffer = read_buffer, .context = handle,
        .cancelled = reader_cancelled, .yield = reader_yield,
    };
    const int64_t started_us = esp_timer_get_time();
    int64_t paused_us = 0;
    int64_t next_frame_us = started_us;
    int64_t first_frame_us = 0;
    int64_t first_frame_paused_us = 0;

    while (!atomic_load(&handle->stop_requested) &&
           aether_media_fs_available(&handle->media_lease)) {
        const bool paused = atomic_load(&handle->pause_requested);
        portENTER_CRITICAL(&handle->lock);
        handle->state = paused ? AETHER_MJPEG_PAUSED : AETHER_MJPEG_PLAYING;
        portEXIT_CRITICAL(&handle->lock);
        if (paused) {
            const int64_t pause_start = esp_timer_get_time();
            while (atomic_load(&handle->pause_requested) && !reader_cancelled(handle))
                vTaskDelay(pdMS_TO_TICKS(10));
            const int64_t now = esp_timer_get_time();
            paused_us += now - pause_start;
            next_frame_us = now;
            continue;
        }

        const int64_t read_start = esp_timer_get_time();
        size_t frame_size = 0;
        result = aether_mjpeg_read_frame(&reader, input, input_allocated, &frame_size);
        if (result == ESP_ERR_NOT_FOUND) {
            finished = produced_frames != 0;
            result = finished ? ESP_OK : ESP_ERR_INVALID_SIZE;
            break;
        }
        if (result != ESP_OK) {
            goto done;
        }
        const int64_t read_done = esp_timer_get_time();
        if (reader_cancelled(handle)) { result = ESP_ERR_INVALID_STATE; goto done; }
        portENTER_CRITICAL(&handle->lock);
        const uint8_t requested_fps = handle->requested_fps;
        portEXIT_CRITICAL(&handle->lock);
        /* Keep AVI's fractional native cadence instead of rounding 29.97 to
         * 30, or playing every 25/30 fps file at the raw-MJPEG default. */
        int64_t frame_interval_us = requested_fps ? 1000000LL / requested_fps :
                                    reader.frame_period_us ? reader.frame_period_us : 1000000LL / 24;
        if (!requested_fps && reader.frame_period_us &&
            (reader.frame_period_us < 33333U || reader.frame_period_us > 1000000U)) {
            result = ESP_ERR_NOT_SUPPORTED;
            goto done;
        }
        portENTER_CRITICAL(&handle->lock);
        handle->fps = (uint8_t)((1000000LL + frame_interval_us / 2) / frame_interval_us);
        handle->source_fps_x100 = reader.frame_period_us ? 100000000U / reader.frame_period_us : 0;
        portEXIT_CRITICAL(&handle->lock);

        jpeg_decode_picture_info_t info;
        result = jpeg_decoder_get_info(input, (uint32_t)frame_size, &info);
        if (result != ESP_OK) {
            goto done;
        }
        if (!info.width || !info.height || info.width > MJPEG_MAX_WIDTH ||
            info.height > MJPEG_MAX_HEIGHT || (info.width & 15U) ||
            (info.height & 15U)) {
            ESP_LOGE(TAG, "unsupported frame %lux%lu; require <=480 and /16",
                     (unsigned long)info.width, (unsigned long)info.height);
            result = ESP_ERR_NOT_SUPPORTED;
            goto done;
        }

        uint32_t decoded_size = 0;
        result = jpeg_decoder_process(decoder, &decode_cfg, input,
                                      (uint32_t)frame_size, output,
                                      (uint32_t)output_allocated,
                                      &decoded_size);
        if (result != ESP_OK) {
            goto done;
        }
        if (decoded_size < info.width * info.height * 2U) {
            result = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        const int64_t decode_done = esp_timer_get_time();
        if (reader_cancelled(handle)) { result = ESP_ERR_INVALID_STATE; goto done; }
        portENTER_CRITICAL(&handle->lock);
        handle->width = (uint16_t)info.width;
        handle->height = (uint16_t)info.height;
        const uint32_t frame_number = ++handle->frame_number;
        ++produced_frames;
        portEXIT_CRITICAL(&handle->lock);
        if (!atomic_load(&handle->stop_requested)) {
            handle->frame_cb((const uint16_t *)output, (uint16_t)info.width,
                             (uint16_t)info.height, frame_number,
                             handle->user_ctx);
        }

        const int64_t callback_done = esp_timer_get_time();
        if (frame_number == 1) { first_frame_us = callback_done; first_frame_paused_us = paused_us; }
        const int64_t active_us = callback_done - started_us - paused_us;
        next_frame_us += frame_interval_us;
        const bool late = callback_done > next_frame_us;
        portENTER_CRITICAL(&handle->lock);
        handle->read_us = (uint32_t)(read_done - read_start);
        handle->decode_us = (uint32_t)(decode_done - read_done);
        handle->callback_us = (uint32_t)(callback_done - decode_done);
        handle->elapsed_ms = (uint32_t)(active_us / 1000);
        const int64_t interval_us = callback_done - first_frame_us - (paused_us - first_frame_paused_us);
        handle->actual_fps_x100 = frame_number > 1 && interval_us > 0 ?
            (uint32_t)((uint64_t)(frame_number - 1U) * 100000000ULL / (uint64_t)interval_us) : 0;
        if (late) ++handle->late_frames;
        portEXIT_CRITICAL(&handle->lock);
        if (late) next_frame_us = callback_done + frame_interval_us;
        while (!reader_cancelled(handle) && !atomic_load(&handle->pause_requested)) {
            portENTER_CRITICAL(&handle->lock);
            const bool rate_changed = handle->requested_fps != requested_fps;
            portEXIT_CRITICAL(&handle->lock);
            if (rate_changed) { next_frame_us = esp_timer_get_time(); break; }
            const int64_t remaining_us = next_frame_us - esp_timer_get_time();
            if (remaining_us <= 0) {
                break;
            }
            const uint32_t delay_ms = (uint32_t)((remaining_us + 999) / 1000);
            vTaskDelay(pdMS_TO_TICKS(delay_ms < 10 ? delay_ms : 10));
        }
    }
    if (!atomic_load(&handle->stop_requested) && !aether_media_fs_available(&handle->media_lease))
        result = ESP_ERR_INVALID_STATE;

done:
    if (decoder) {
        jpeg_del_decoder_engine(decoder);
    }
    free(input);
    free(read_buffer);
    if (file) {
        fclose(file);
    }
    if (storage_locked) {
        aether_media_fs_release(&handle->media_lease);
    }
    const bool stopped = atomic_load(&handle->stop_requested);
    if (result != ESP_OK && !stopped) {
        ESP_LOGE(TAG, "playback failed: %s", esp_err_to_name(result));
    }
    uint8_t *release = NULL;
    portENTER_CRITICAL(&handle->lock);
    if (handle->release_output_requested) {
        release = handle->output_buffer;
        handle->output_buffer = NULL;
        handle->output_buffer_size = 0;
        handle->release_output_requested = false;
    }
    handle->last_error = stopped ? ESP_OK : result;
    handle->state = result != ESP_OK && !stopped ? AETHER_MJPEG_ERROR :
                    finished && !stopped ? AETHER_MJPEG_FINISHED : AETHER_MJPEG_STOPPED;
    /* All callback, decoder, file and hardware accesses have ended. Never
     * access handle after publishing completion under this lock. */
    handle->running = false;
    portEXIT_CRITICAL(&handle->lock);
    free(release);
    vTaskDelete(NULL);
}

esp_err_t aether_mjpeg_player_new(aether_storage_handle_t storage,
                                  aether_mjpeg_player_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(storage && out_handle, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");
    aether_mjpeg_player_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");
    handle->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    atomic_init(&handle->stop_requested, false);
    atomic_init(&handle->pause_requested, false);
    handle->storage = storage;
    handle->state = AETHER_MJPEG_STOPPED;
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t aether_mjpeg_player_play(aether_mjpeg_player_handle_t handle,
                                   const char *path, uint8_t fps,
                                   aether_mjpeg_frame_cb_t frame_cb,
                                   void *user_ctx)
{
    ESP_RETURN_ON_FALSE(handle && path && path[0] && frame_cb,
                        ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(fps <= 30, ESP_ERR_INVALID_ARG, TAG,
                        "fps must be 0 (automatic) or 1..30");
    ESP_RETURN_ON_FALSE(strlen(path) < sizeof(handle->path),
                        ESP_ERR_INVALID_SIZE, TAG, "path too long");
    portENTER_CRITICAL(&handle->lock);
    if (handle->running) {
        portEXIT_CRITICAL(&handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(handle->path, path, sizeof(handle->path));
    handle->requested_fps = fps;
    handle->fps = fps ? fps : 24;
    handle->frame_cb = frame_cb;
    handle->user_ctx = user_ctx;
    handle->stop_requested = false;
    handle->user_paused = false;
    handle->pause_requested = handle->screen_paused;
    handle->frame_number = 0;
    handle->width = 0;
    handle->height = 0;
    handle->actual_fps_x100 = handle->source_fps_x100 = 0;
    handle->read_us = handle->decode_us = handle->callback_us = handle->late_frames = handle->elapsed_ms = 0;
    handle->last_error = ESP_OK;
    handle->release_output_requested = false;
    handle->state = AETHER_MJPEG_STARTING;
    handle->running = true;
    portEXIT_CRITICAL(&handle->lock);
    if (xTaskCreate(player_task, "mjpeg", MJPEG_TASK_STACK,
                    handle, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&handle->lock);
        handle->last_error = ESP_ERR_NO_MEM;
        handle->state = AETHER_MJPEG_ERROR;
        handle->running = false;
        portEXIT_CRITICAL(&handle->lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t aether_mjpeg_player_set_paused(aether_mjpeg_player_handle_t handle,
                                         bool paused)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    portENTER_CRITICAL(&handle->lock);
    const bool running = handle->running;
    if (running) {
        handle->user_paused = paused;
        atomic_store(&handle->pause_requested, paused || handle->screen_paused);
    }
    portEXIT_CRITICAL(&handle->lock);
    return running ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void aether_mjpeg_player_set_screen_paused(aether_mjpeg_player_handle_t handle, bool paused)
{
    if (!handle) return;
    portENTER_CRITICAL(&handle->lock);
    handle->screen_paused = paused;
    atomic_store(&handle->pause_requested, paused || handle->user_paused);
    portEXIT_CRITICAL(&handle->lock);
}

esp_err_t aether_mjpeg_player_set_fps(aether_mjpeg_player_handle_t handle, uint8_t fps)
{
    if (!handle || fps > 30) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&handle->lock);
    handle->requested_fps = fps;
    if (fps) handle->fps = fps;
    portEXIT_CRITICAL(&handle->lock);
    return ESP_OK;
}

esp_err_t aether_mjpeg_player_stop(aether_mjpeg_player_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    atomic_store(&handle->stop_requested, true);
    return ESP_OK;
}

esp_err_t aether_mjpeg_player_stop_wait(aether_mjpeg_player_handle_t handle,
                                        uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(aether_mjpeg_player_stop(handle), TAG, "stop player");
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        portENTER_CRITICAL(&handle->lock);
        const bool running = handle->running;
        portEXIT_CRITICAL(&handle->lock);
        if (!running) {
            return ESP_OK;
        }
        if (xTaskGetTickCount() - start >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

void aether_mjpeg_player_release_frame(aether_mjpeg_player_handle_t handle)
{
    if (!handle) {
        return;
    }
    uint8_t *release = NULL;
    portENTER_CRITICAL(&handle->lock);
    if (handle->running) {
        handle->release_output_requested = true;
    } else {
        release = handle->output_buffer;
        handle->output_buffer = NULL;
        handle->output_buffer_size = 0;
    }
    portEXIT_CRITICAL(&handle->lock);
    free(release);
}

esp_err_t aether_mjpeg_player_get_status(aether_mjpeg_player_handle_t handle,
                                         aether_mjpeg_status_t *status)
{
    ESP_RETURN_ON_FALSE(handle && status, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");
    portENTER_CRITICAL(&handle->lock);
    status->state = handle->state;
    status->last_error = handle->last_error;
    status->frame_number = handle->frame_number;
    status->width = handle->width;
    status->height = handle->height;
    status->fps = handle->fps;
    status->actual_fps_x100 = handle->actual_fps_x100;
    status->source_fps_x100 = handle->source_fps_x100;
    status->read_us = handle->read_us;
    status->decode_us = handle->decode_us;
    status->callback_us = handle->callback_us;
    status->late_frames = handle->late_frames;
    status->elapsed_ms = handle->elapsed_ms;
    strlcpy(status->path, handle->path, sizeof(status->path));
    portEXIT_CRITICAL(&handle->lock);
    return ESP_OK;
}

esp_err_t aether_mjpeg_player_del(aether_mjpeg_player_handle_t handle)
{
    ESP_RETURN_ON_ERROR(aether_mjpeg_player_stop_wait(handle, 3000), TAG,
                        "player task did not stop");
    aether_mjpeg_player_release_frame(handle);
    free(handle);
    return ESP_OK;
}
