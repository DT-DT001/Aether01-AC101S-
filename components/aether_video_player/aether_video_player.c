#include "aether_video_player.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct aether_video_player {
    aether_mjpeg_player_handle_t mjpeg;
    aether_mp4_player_handle_t mp4;
    SemaphoreHandle_t commands;
    portMUX_TYPE lock;
    bool use_mp4, stop_requested;
};
static bool mode(aether_video_player_handle_t h)
{ portENTER_CRITICAL(&h->lock); bool value = h->use_mp4; portEXIT_CRITICAL(&h->lock); return value; }
static bool active(aether_mjpeg_state_t state)
{ return state == AETHER_VIDEO_STARTING || state == AETHER_VIDEO_PLAYING || state == AETHER_VIDEO_PAUSED; }

esp_err_t aether_video_player_new(aether_storage_handle_t storage, ac101s_audio_handle_t audio,
                                  aether_video_player_handle_t *out)
{
    if (!storage || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    aether_video_player_handle_t h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    h->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    h->commands = xSemaphoreCreateMutex();
    esp_err_t error = h->commands ? aether_mjpeg_player_new(storage, &h->mjpeg) : ESP_ERR_NO_MEM;
    if (error == ESP_OK) error = aether_mp4_player_new(storage, audio, &h->mp4);
    if (error != ESP_OK) {
        if (h->mjpeg) aether_mjpeg_player_del(h->mjpeg);
        if (h->commands) vSemaphoreDelete(h->commands);
        free(h); return error;
    }
    *out = h; return ESP_OK;
}

esp_err_t aether_video_player_play(aether_video_player_handle_t h, const char *path, uint8_t fps,
                                   aether_video_frame_cb_t callback, void *context)
{
    if (!h || !path || !path[0] || !callback || fps > 30) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->commands, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    /* Publish this start before consulting either backend: an asynchronous
     * stop during those status checks must cancel the accepted command too. */
    portENTER_CRITICAL(&h->lock); h->stop_requested = false; portEXIT_CRITICAL(&h->lock);
    aether_mjpeg_status_t mjpeg = {0}; aether_mp4_status_t mp4 = {0};
    esp_err_t error = aether_mjpeg_player_get_status(h->mjpeg, &mjpeg);
    if (error == ESP_OK) error = aether_mp4_player_get_status(h->mp4, &mp4);
    if (error == ESP_OK && (active(mjpeg.state) || active(mp4.state))) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        const char *extension = strrchr(path, '.');
        const bool use_mp4 = extension && (!strcasecmp(extension, ".mp4") || !strcasecmp(extension, ".m4v") || !strcasecmp(extension, ".mov"));
        aether_video_player_release_frame(h);
        portENTER_CRITICAL(&h->lock); h->use_mp4 = use_mp4; portEXIT_CRITICAL(&h->lock);
        error = use_mp4 ? aether_mp4_player_play(h->mp4, path, callback, context) :
            aether_mjpeg_player_play(h->mjpeg, path, fps, callback, context);
        /* stop may run between mode publication and backend command acceptance. */
        portENTER_CRITICAL(&h->lock); bool stopped = h->stop_requested; portEXIT_CRITICAL(&h->lock);
        if (stopped) aether_video_player_stop(h);
    }
    xSemaphoreGive(h->commands);
    return error;
}

esp_err_t aether_video_player_set_paused(aether_video_player_handle_t h, bool paused)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    return mode(h) ? aether_mp4_player_set_paused(h->mp4, paused) : aether_mjpeg_player_set_paused(h->mjpeg, paused);
}
void aether_video_player_set_screen_paused(aether_video_player_handle_t h, bool paused)
{
    if (!h) return;
    aether_mjpeg_player_set_screen_paused(h->mjpeg, paused);
    aether_mp4_player_set_screen_paused(h->mp4, paused);
}
esp_err_t aether_video_player_set_fps(aether_video_player_handle_t h, uint8_t fps)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    return mode(h) ? ESP_ERR_NOT_SUPPORTED : aether_mjpeg_player_set_fps(h->mjpeg, fps);
}
esp_err_t aether_video_player_seek(aether_video_player_handle_t h,uint32_t position_ms)
{ return aether_video_player_seek_with_reset(h,position_ms,NULL,NULL); }
esp_err_t aether_video_player_seek_with_reset(aether_video_player_handle_t h,uint32_t position_ms,
    void (*reset)(void *),void *context)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->commands, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->lock);
    const bool mp4 = h->use_mp4, stopped = h->stop_requested;
    portEXIT_CRITICAL(&h->lock);
    const esp_err_t error = !mp4 ? ESP_ERR_NOT_SUPPORTED : stopped ? ESP_ERR_INVALID_STATE :
        aether_mp4_player_seek_with_reset(h->mp4,position_ms,reset,context);
    xSemaphoreGive(h->commands);
    return error;
}
esp_err_t aether_video_player_set_volume(aether_video_player_handle_t h, uint8_t volume)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    return aether_mp4_player_set_volume(h->mp4, volume);
}
esp_err_t aether_video_player_stop(aether_video_player_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock); h->stop_requested = true; portEXIT_CRITICAL(&h->lock);
    const esp_err_t first = h->mjpeg ? aether_mjpeg_player_stop(h->mjpeg) : ESP_OK;
    const esp_err_t second = h->mp4 ? aether_mp4_player_stop(h->mp4) : ESP_OK;
    return first != ESP_OK ? first : second;
}
esp_err_t aether_video_player_stop_wait(aether_video_player_handle_t h, uint32_t timeout_ms)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    const int64_t started = esp_timer_get_time();
    aether_video_player_stop(h);
    if (xSemaphoreTake(h->commands, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    aether_video_player_stop(h);
    uint64_t elapsed = (uint64_t)(esp_timer_get_time() - started) / 1000;
    const esp_err_t first = h->mjpeg ? aether_mjpeg_player_stop_wait(h->mjpeg,
        elapsed < timeout_ms ? timeout_ms - (uint32_t)elapsed : 0) : ESP_OK;
    elapsed = (uint64_t)(esp_timer_get_time() - started) / 1000;
    const esp_err_t second = h->mp4 ? aether_mp4_player_stop_wait(h->mp4,
        elapsed < timeout_ms ? timeout_ms - (uint32_t)elapsed : 0) : ESP_OK;
    xSemaphoreGive(h->commands);
    return first != ESP_OK ? first : second;
}
void aether_video_player_release_frame(aether_video_player_handle_t h)
{
    if (!h) return;
    aether_mjpeg_player_release_frame(h->mjpeg); aether_mp4_player_release_frame(h->mp4);
}
esp_err_t aether_video_player_get_status(aether_video_player_handle_t h, aether_video_status_t *out)
{
    if (!h || !out) return ESP_ERR_INVALID_ARG;
    if (mode(h)) return aether_mp4_player_get_status(h->mp4, out);
    aether_mjpeg_status_t status;
    const esp_err_t error = aether_mjpeg_player_get_status(h->mjpeg, &status);
    if (error != ESP_OK) return error;
    memset(out, 0, sizeof(*out));
    out->state = status.state; out->last_error = status.last_error; out->frame_number = status.frame_number;
    out->width = status.width; out->height = status.height; out->fps = status.fps;
    out->actual_fps_x100 = status.actual_fps_x100; out->source_fps_x100 = status.source_fps_x100;
    out->read_us = status.read_us; out->decode_us = status.decode_us; out->callback_us = status.callback_us;
    out->late_frames = status.late_frames; out->elapsed_ms = status.elapsed_ms;
    memcpy(out->path, status.path, sizeof(out->path)); return ESP_OK;
}
esp_err_t aether_video_player_del(aether_video_player_handle_t h)
{
    if (!h) return ESP_OK;
    esp_err_t error = aether_video_player_stop_wait(h, 2000);
    if (error != ESP_OK) return error;
    if (h->mjpeg) { error = aether_mjpeg_player_del(h->mjpeg); if (error != ESP_OK) return error; h->mjpeg = NULL; }
    if (h->mp4) { error = aether_mp4_player_del(h->mp4); if (error != ESP_OK) return error; h->mp4 = NULL; }
    vSemaphoreDelete(h->commands); free(h); return ESP_OK;
}
