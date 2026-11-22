#include "ac101s_audio.h"
#include "ac101s_codec.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ac101s_audio";

enum {
    AUDIO_DMA_FRAMES = 240,
    PCM16_STEREO_BYTES = 4,
    ALERT_MAX_FRAMES = 5760,
};

struct ac101s_audio_dev_t {
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    i2c_master_dev_handle_t codec;
    gpio_num_t pa_enable;
    uint32_t sample_rate_hz;
    bool started;
    bool local_ready;
    bool alert_cleanup_pending;
    bool external_started;
    bool external_ready;
    bool capture_active;
    bool capture_ready;
    bool capture_external;
    const void *board_owner;
    bool board_ready;
    uint8_t master_volume;
    int requested_volume;
    float mic_gain_db;
    ac101s_audio_output_t output;
    ac101s_audio_sink_t sink;
    ac101s_audio_sink_t usb_sink;
    ac101s_audio_source_t usb_source;
    ac101s_audio_input_t input;
    SemaphoreHandle_t mutex;
    atomic_uint route_epoch, stop_serial, stop_completed;
    portMUX_TYPE route_lock;
    ac101s_audio_write_metrics_t write_metrics;
    esp_err_t (*route_cancel)(void *);
    void *route_cancel_context;
};

/* Normal I/O keeps its 1s bound. Only contention with a route handover gets
 * enough time for the transport's bounded (3s) media-start handshake. */
static bool take_io_mutex(ac101s_audio_handle_t handle)
{
    const unsigned epoch = atomic_load(&handle->route_epoch);
    const TickType_t began = xTaskGetTickCount();
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) return true;
    const unsigned after = atomic_load(&handle->route_epoch);
    if (!(after & 1U) && after == epoch) return false;
    const TickType_t elapsed = xTaskGetTickCount() - began;
    const TickType_t budget = pdMS_TO_TICKS(4500);
    return elapsed < budget && xSemaphoreTake(handle->mutex, budget - elapsed) == pdTRUE;
}

esp_err_t ac101s_audio_new(const ac101s_audio_config_t *config,
                           ac101s_audio_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(config && config->i2c_bus && out_handle &&
                        GPIO_IS_VALID_OUTPUT_GPIO(config->pa_enable), ESP_ERR_INVALID_ARG,
                        TAG, "invalid argument");

    *out_handle = NULL;
    if (config->sample_rate_hz && config->sample_rate_hz != 48000) return ESP_ERR_NOT_SUPPORTED;
    ac101s_audio_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");
    handle->pa_enable = config->pa_enable;
    handle->sample_rate_hz = 48000;
    handle->master_volume = 100;
    handle->requested_volume = 100;
    atomic_init(&handle->route_epoch, 0);
    atomic_init(&handle->stop_serial, 0);
    atomic_init(&handle->stop_completed, 0);
    handle->route_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    handle->mutex = xSemaphoreCreateMutex();
    if (!handle->mutex) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << config->pa_enable,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&pa_cfg);
    if (ret != ESP_OK) {
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ret;
    }
    ESP_GOTO_ON_ERROR(gpio_set_level(config->pa_enable, 0), fail, TAG, "disable PA");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = AUDIO_DMA_FRAMES;
    chan_cfg.auto_clear = true;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&chan_cfg, &handle->tx, &handle->rx), fail,
                      TAG, "create I2S channels");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(handle->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = config->mclk,
            .bclk = config->bclk,
            .ws = config->ws,
            .dout = config->dout,
            .din = config->din,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(handle->tx, &std_cfg), fail,
                      TAG, "initialize I2S TX");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(handle->rx, &std_cfg), fail,
                      TAG, "initialize I2S RX");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(handle->tx), fail, TAG, "enable I2S TX");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(handle->rx), fail, TAG, "enable I2S RX");

    ESP_GOTO_ON_ERROR(ac101s_codec_new(config->i2c_bus, &handle->codec), fail, TAG,
                      "init AC101S at I2C 0x33");
    ESP_LOGI(TAG, "AC101S path prepared at %lu Hz; PA remains disabled",
             (unsigned long)handle->sample_rate_hz);
    *out_handle = handle;
    return ESP_OK;

fail:
    ac101s_audio_del(handle);
    return ret;
}

static int speaker_volume(int requested, uint8_t master)
{
    const int logical = requested * master / 100;
    /* Round upward so a previously nonzero logical level remains audible.
     * True zero still mutes the DAC and keeps the amplifier disabled. */
    return (logical * AC101S_AUDIO_SPEAKER_VOLUME_MAX + 99) / 100;
}

static ac101s_audio_sink_t *output_sink(ac101s_audio_handle_t handle, ac101s_audio_output_t output)
{
    return output == AC101S_OUTPUT_EXTERNAL ? &handle->sink :
           output == AC101S_OUTPUT_USB ? &handle->usb_sink : NULL;
}

static esp_err_t start_locked(ac101s_audio_handle_t handle, int volume, float mic_gain_db)
{
    if (handle->started || handle->capture_active) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(gpio_set_level(handle->pa_enable, 0), TAG, "disable PA");
    ac101s_audio_sink_t *sink = handle->board_owner ? NULL : output_sink(handle, handle->output);
    if (sink) {
        if (handle->output == AC101S_OUTPUT_USB && handle->sample_rate_hz != 48000) return ESP_ERR_NOT_SUPPORTED;
        if (!sink->start || !sink->connected || !sink->connected(sink->context))
            return ESP_ERR_INVALID_STATE;
        handle->requested_volume = volume;
        handle->mic_gain_db = mic_gain_db;
        const esp_err_t error = sink->start(sink->context);
        if (error != ESP_OK) {
            /* A failed/timeout start may still own a pending USB operation.
             * A failed cleanup keeps that route owned until stop retries. */
            if (sink->stop(sink->context) != ESP_OK)
                handle->external_started = handle->started = true;
            handle->external_ready = false;
            return error;
        }
        handle->external_ready = true;
        handle->external_started = handle->started = true;
        ESP_LOGI(TAG, "audio start output=external stream_volume=%d master=%u", volume, handle->master_volume);
        return ESP_OK;
    }
    const int effective = speaker_volume(volume, handle->master_volume);
    esp_err_t err = ac101s_codec_start(handle->codec, false, mic_gain_db);
    if (err == ESP_OK) err = ac101s_codec_set_speaker(handle->codec, effective);
    if (err == ESP_OK && effective) err = gpio_set_level(handle->pa_enable, 1);
    if (err != ESP_OK) {
        gpio_set_level(handle->pa_enable, 0);
        handle->started = ac101s_codec_stop(handle->codec) != ESP_OK;
        return err;
    }
    handle->requested_volume = volume;
    handle->mic_gain_db = mic_gain_db;
    handle->external_started = false;
    handle->local_ready = handle->started = true;
    ESP_LOGI(TAG, "audio start output=speaker stream_volume=%d master=%u codec_volume=%d mute=%u PA_command=%u",
        volume, handle->master_volume, effective, effective == 0, effective > 0);
    return ESP_OK;
}

static esp_err_t stop_locked(ac101s_audio_handle_t handle);

static esp_err_t recover_alert_locked(ac101s_audio_handle_t handle)
{
    return handle->alert_cleanup_pending ? stop_locked(handle) : ESP_OK;
}

esp_err_t ac101s_audio_start(ac101s_audio_handle_t handle, int volume, float mic_gain_db)
{
    ESP_RETURN_ON_FALSE(handle && handle->codec, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    ESP_RETURN_ON_FALSE(volume >= 0 && volume <= 100 && isfinite(mic_gain_db) &&
                        mic_gain_db >= 0.0f && mic_gain_db <= 42.0f,
                        ESP_ERR_INVALID_ARG, TAG, "invalid volume or microphone gain");
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = recover_alert_locked(handle);
    if (error == ESP_OK) {
        /* A new session may acknowledge an earlier STOP only once the old
         * session is already inactive under the codec mutex. */
        if (!handle->started) atomic_store(&handle->stop_completed, atomic_load(&handle->stop_serial));
        error = handle->board_owner ? ESP_ERR_INVALID_STATE : start_locked(handle, volume, mic_gain_db);
    }
    xSemaphoreGive(handle->mutex);
    return error;
}

static esp_err_t stop_locked(ac101s_audio_handle_t handle)
{
    const esp_err_t pa_err = gpio_set_level(handle->pa_enable, 0);
    handle->local_ready = false;
    if (!handle->started) {
        if (pa_err == ESP_OK) handle->alert_cleanup_pending = false;
        return pa_err;
    }
    if (handle->external_started) {
        ac101s_audio_sink_t *sink = output_sink(handle, handle->output);
        handle->external_ready = false;
        const esp_err_t error = sink && sink->stop ? sink->stop(sink->context) : ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) handle->external_started = handle->started = false;
        return pa_err != ESP_OK ? pa_err : error;
    }
    const esp_err_t err = ac101s_codec_stop(handle->codec);
    if (err == ESP_OK) handle->started = false;
    if (err == ESP_OK && pa_err == ESP_OK) handle->alert_cleanup_pending = false;
    return pa_err != ESP_OK ? pa_err : err;
}

static esp_err_t capture_stop_locked(ac101s_audio_handle_t handle)
{
    /* An old recorder cleanup must not mute/stop a newer playback session. */
    if (!handle->capture_active) return ESP_OK;
    handle->capture_ready = false;
    if (handle->capture_external) {
        const esp_err_t err = handle->usb_source.stop(handle->usb_source.context);
        if (err == ESP_OK) handle->capture_active = handle->capture_external = false;
        return err;
    }
    const esp_err_t pa_err = gpio_set_level(handle->pa_enable, 0);
    const esp_err_t err = ac101s_codec_stop(handle->codec);
    if (err == ESP_OK) handle->capture_active = false;
    return pa_err != ESP_OK ? pa_err : err;
}

static esp_err_t capture_start_locked(ac101s_audio_handle_t handle, float gain_db)
{
    if (handle->started || handle->capture_active) return ESP_ERR_INVALID_STATE;
    esp_err_t err = gpio_set_level(handle->pa_enable, 0);
    if (err != ESP_OK) return err;
    handle->capture_active = true; /* Also owns partial-open rollback. */
    if (!handle->board_owner && handle->input == AC101S_INPUT_USB) {
        if (!handle->usb_source.start || !handle->usb_source.connected ||
            !handle->usb_source.connected(handle->usb_source.context)) {
            handle->capture_active = false;
            return ESP_ERR_INVALID_STATE;
        }
        handle->capture_external = true;
        err = handle->usb_source.start(handle->usb_source.context);
        if (err != ESP_OK) capture_stop_locked(handle);
        else handle->capture_ready = true;
        return err;
    }
    err = ac101s_codec_start(handle->codec, true, gain_db);
    if (err != ESP_OK) {
        capture_stop_locked(handle);
    } else {
        handle->capture_ready = true;
        ESP_LOGI(TAG, "capture start local ADC 48000Hz PCM16 stereo, gain=%.0fdB, DAC muted, PA off", gain_db);
    }
    return err;
}

esp_err_t ac101s_audio_capture_start(ac101s_audio_handle_t handle, float gain_db)
{
    if (!handle || !handle->codec || !handle->rx || !isfinite(gain_db) || gain_db < 0 || gain_db > 42)
        return ESP_ERR_INVALID_ARG;
    if (handle->sample_rate_hz != 48000) return ESP_ERR_NOT_SUPPORTED;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = recover_alert_locked(handle);
    if (err == ESP_OK)
        err = handle->board_owner ? ESP_ERR_INVALID_STATE : capture_start_locked(handle, gain_db);
    xSemaphoreGive(handle->mutex);
    return err;
}

static esp_err_t board_pcm_locked(ac101s_audio_handle_t handle, bool capture, void *data,
                                  size_t bytes, TickType_t began, TickType_t budget)
{
    size_t offset = 0;
    while (offset < bytes) {
        const TickType_t elapsed = xTaskGetTickCount() - began;
        if (elapsed >= budget) return ESP_ERR_TIMEOUT;
        TickType_t wait = budget - elapsed;
        size_t chunk = bytes - offset;
        if (handle->board_owner) {
            /* IDF applies its timeout per DMA queue wait. One buffer's worth
             * of PCM may straddle two descriptors, so reserve half the
             * remaining time per wait. The codec mutex serializes all I/O. */
            const size_t dma_bytes = AUDIO_DMA_FRAMES * PCM16_STEREO_BYTES;
            if (chunk > dma_bytes) chunk = dma_bytes;
            wait /= 2;
        }
        const uint32_t timeout_ms = wait * portTICK_PERIOD_MS;
        size_t count = 0;
        /* Preserve the actual byte count. A partial DMA read must never
         * publish unread memory as a valid capture frame. */
        const esp_err_t err = capture
            ? i2s_channel_read(handle->rx, (uint8_t *)data + offset, chunk, &count, timeout_ms)
            : i2s_channel_write(handle->tx, (uint8_t *)data + offset, chunk, &count, timeout_ms);
        if (err != ESP_OK) return err;
        if (!count || count > chunk || count % PCM16_STEREO_BYTES) return ESP_FAIL;
        if (capture) {
            /* Use the mono ADC from the first slot in the app's stereo PCM contract. */
            uint8_t *frame = (uint8_t *)data + offset;
            for (size_t i = 0; i < count; i += PCM16_STEREO_BYTES) {
                memcpy(frame + i + sizeof(int16_t), frame + i, sizeof(int16_t));
            }
        }
        offset += count;
    }
    return ESP_OK;
}

esp_err_t ac101s_audio_play_alert(ac101s_audio_handle_t handle, const int16_t *pcm, size_t frames)
{
    static const char alert_owner;
    if (!handle || !handle->codec || !handle->tx || !pcm || !frames || frames > ALERT_MAX_FRAMES)
        return ESP_ERR_INVALID_ARG;
    if (handle->sample_rate_hz != 48000) return ESP_ERR_NOT_SUPPORTED;
    if (xSemaphoreTake(handle->mutex, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;

    esp_err_t error = recover_alert_locked(handle);
    if (error == ESP_OK && (handle->started || handle->capture_active || handle->board_owner ||
        atomic_load(&handle->stop_serial) != atomic_load(&handle->stop_completed)))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && handle->master_volume) {
        const int previous_volume = handle->requested_volume;
        const float previous_gain = handle->mic_gain_db;
        /* The local-only owner token keeps a configured USB/BT sink untouched.
         * Holding the mutex over the burst makes ordinary starts serialize
         * behind it instead of mistaking it for an active media session. */
        handle->board_owner = &alert_owner;
        error = start_locked(handle, 85, 24.0f);
        if (error == ESP_OK) {
            const size_t bytes = frames * PCM16_STEREO_BYTES;
            error = board_pcm_locked(handle, false, (void *)pcm, bytes,
                                     xTaskGetTickCount(), pdMS_TO_TICKS(200));
            /* An accepted write still leaves up to one I2S DMA ring queued. */
            if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(40));
        }
        const esp_err_t stopped = stop_locked(handle);
        handle->alert_cleanup_pending = stopped != ESP_OK;
        if (error == ESP_OK) error = stopped;
        handle->board_owner = NULL;
        handle->requested_volume = previous_volume;
        handle->mic_gain_db = previous_gain;
    }
    xSemaphoreGive(handle->mutex);
    return error;
}

esp_err_t ac101s_audio_capture_read(ac101s_audio_handle_t handle, void *data, size_t bytes)
{
    if (!handle || !data || !bytes || bytes > INT_MAX || bytes % 4U) return ESP_ERR_INVALID_ARG;
    const TickType_t began = xTaskGetTickCount(), budget = pdMS_TO_TICKS(1000);
    if (xSemaphoreTake(handle->mutex, budget) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (handle->board_owner || !handle->capture_active || !handle->capture_ready) goto done;
    if (handle->capture_external) {
        const TickType_t elapsed = xTaskGetTickCount() - began;
        if (elapsed >= budget) err = ESP_ERR_TIMEOUT;
        else if (!handle->usb_source.connected(handle->usb_source.context)) err = ESP_ERR_INVALID_STATE;
        else err = handle->usb_source.read(handle->usb_source.context, data, bytes,
                                           (budget - elapsed) * portTICK_PERIOD_MS);
        goto done;
    }
    err = board_pcm_locked(handle, true, data, bytes, began, budget);
done:
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_capture_stop(ac101s_audio_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t err = handle->board_owner ? ESP_OK : capture_stop_locked(handle);
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_board_start(ac101s_audio_handle_t handle, const void *owner,
                                  bool capture, int volume, float gain_db)
{
    if (!handle || !owner || !handle->codec || !handle->rx || !handle->tx ||
        volume < 0 || volume > 100 || !isfinite(gain_db) || gain_db < 0 || gain_db > 42)
        return ESP_ERR_INVALID_ARG;
    if (handle->sample_rate_hz != 48000) return ESP_ERR_NOT_SUPPORTED;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(60)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = recover_alert_locked(handle);
    if (err == ESP_OK && !handle->board_owner && !handle->capture_active && !handle->started) {
        handle->board_owner = owner;
        err = capture ? capture_start_locked(handle, gain_db) : start_locked(handle, volume, gain_db);
        handle->board_ready = err == ESP_OK;
        if (!handle->capture_active && !handle->started) handle->board_owner = NULL;
    } else if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_board_pcm(ac101s_audio_handle_t handle, const void *owner,
                                void *data, size_t bytes, uint32_t timeout_ms)
{
    if (!handle || !owner || !data || !bytes || bytes > INT_MAX || bytes % 4U ||
        !timeout_ms || timeout_ms > 1000) return ESP_ERR_INVALID_ARG;
    const TickType_t began = xTaskGetTickCount();
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t budget = ticks ? ticks : 1;
    if (xSemaphoreTake(handle->mutex, budget) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (handle->board_owner == owner && handle->board_ready)
        err = board_pcm_locked(handle, handle->capture_active, data, bytes, began, budget);
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_board_stop(ac101s_audio_handle_t handle, const void *owner)
{
    if (!handle || !owner) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(60)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (handle->board_owner == owner) {
        handle->board_ready = false;
        err = handle->capture_active ? capture_stop_locked(handle) : stop_locked(handle);
        if (!handle->capture_active && !handle->started) handle->board_owner = NULL;
    }
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_stop(ac101s_audio_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    atomic_fetch_add(&handle->stop_serial, 1);
    /* A transport start may hold mutex while awaiting an ACK. Its stop is a
     * cancellation signal and must not wait behind that very handshake. */
    portENTER_CRITICAL(&handle->route_lock);
    esp_err_t (*cancel)(void *) = handle->route_cancel;
    void *context = handle->route_cancel_context;
    portEXIT_CRITICAL(&handle->route_lock);
    if (cancel) cancel(context);
    const TickType_t began = xTaskGetTickCount(), budget = pdMS_TO_TICKS(1000);
    for (;;) {
        if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(10)) == pdTRUE) break;
        if ((TickType_t)(xTaskGetTickCount() - began) >= budget) return ESP_ERR_TIMEOUT;
        portENTER_CRITICAL(&handle->route_lock);
        cancel = handle->route_cancel; context = handle->route_cancel_context;
        portEXIT_CRITICAL(&handle->route_lock);
        /* Recheck even if the first snapshot was NULL: a competing route may
         * have acquired mutex and published its handshake after that point. */
        if (cancel) cancel(context);
    }
    const unsigned requested_stop = atomic_load(&handle->stop_serial);
    const esp_err_t err = handle->board_owner ? ESP_OK : stop_locked(handle);
    if (err == ESP_OK) atomic_store(&handle->stop_completed, requested_stop);
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_del(ac101s_audio_handle_t handle)
{
    if (!handle) {
        return ESP_OK;
    }
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t capture_error = capture_stop_locked(handle);
    if (capture_error != ESP_OK) {
        xSemaphoreGive(handle->mutex);
        return capture_error;
    }
    const esp_err_t playback_error = stop_locked(handle);
    if (playback_error != ESP_OK) {
        xSemaphoreGive(handle->mutex);
        return playback_error;
    }
    if (handle->codec) {
        const esp_err_t err = i2c_master_bus_rm_device(handle->codec);
        if (err != ESP_OK) {
            xSemaphoreGive(handle->mutex);
            return err;
        }
    }
    if (handle->tx) {
        i2s_channel_disable(handle->tx);
        i2s_del_channel(handle->tx);
    }
    if (handle->rx) {
        i2s_channel_disable(handle->rx);
        i2s_del_channel(handle->rx);
    }
    gpio_set_level(handle->pa_enable, 0);
    gpio_reset_pin(handle->pa_enable);
    xSemaphoreGive(handle->mutex);
    vSemaphoreDelete(handle->mutex);
    free(handle);
    return ESP_OK;
}

static uint32_t write_elapsed_us(int64_t began)
{
    const int64_t elapsed = esp_timer_get_time() - began;
    return elapsed <= 0 ? 0 : elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void record_write(ac101s_audio_handle_t handle, uint32_t mutex_us,
                         uint32_t output_us, bool acquired, esp_err_t error)
{
    portENTER_CRITICAL(&handle->route_lock);
    ac101s_audio_write_metrics_t *m = &handle->write_metrics;
    ++m->write_calls;
    m->mutex_wait_us = mutex_us;
    if (mutex_us > m->mutex_wait_max_us) m->mutex_wait_max_us = mutex_us;
    m->output_write_us = output_us;
    if (output_us > m->output_write_max_us) m->output_write_max_us = output_us;
    if (!acquired) ++m->mutex_timeouts;
    else if (error != ESP_OK) ++m->output_errors;
    m->last_error = error;
    portEXIT_CRITICAL(&handle->route_lock);
}

esp_err_t ac101s_audio_get_write_metrics(ac101s_audio_handle_t handle,
                                        ac101s_audio_write_metrics_t *out)
{
    if (!handle || !out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&handle->route_lock);
    *out = handle->write_metrics;
    portEXIT_CRITICAL(&handle->route_lock);
    return ESP_OK;
}

esp_err_t ac101s_audio_write(ac101s_audio_handle_t handle, void *data, size_t bytes)
{
    ESP_RETURN_ON_FALSE(handle && data && bytes && bytes <= INT_MAX && !(bytes % 4U),
                        ESP_ERR_INVALID_ARG, TAG, "invalid stereo PCM buffer");
    const int64_t mutex_begin = esp_timer_get_time();
    const bool acquired = take_io_mutex(handle);
    const uint32_t mutex_us = write_elapsed_us(mutex_begin);
    if (!acquired) {
        record_write(handle, mutex_us, 0, false, ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    const int64_t output_begin = esp_timer_get_time();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!handle->board_owner && handle->started && handle->external_started && handle->external_ready) {
        ac101s_audio_sink_t *sink = output_sink(handle, handle->output);
        int16_t block[256 * 2];
        const int effective = handle->requested_volume * handle->master_volume / 100;
        const int32_t gain = effective * effective;
        const TickType_t started = xTaskGetTickCount();
        err = ESP_OK;
        for (size_t offset = 0; offset < bytes;) {
            if (!sink->connected(sink->context)) err = ESP_ERR_INVALID_STATE;
            else err = ESP_OK;
            const uint64_t elapsed = (uint64_t)(TickType_t)(xTaskGetTickCount() - started) * portTICK_PERIOD_MS;
            if (elapsed >= 1000) { err = ESP_ERR_TIMEOUT; break; }
            size_t amount = bytes - offset;
            if (amount > sizeof(block)) amount = sizeof(block);
            memcpy(block, (const uint8_t *)data + offset, amount);
            for (size_t i = 0; i < amount / sizeof(*block); ++i)
                block[i] = (int16_t)((int32_t)block[i] * gain / 10000);
            if (err == ESP_OK) err = sink->write(sink->context, block, amount, 1000 - (uint32_t)elapsed);
            if (err != ESP_OK && handle->output == AC101S_OUTPUT_USB && !sink->connected(sink->context)) {
                /* USB removal is a route change, not a decoder failure. Keep
                 * the current media session and feed its remaining original
                 * PCM through the capped local speaker path immediately. */
                err = stop_locked(handle);
                if (err == ESP_OK) {
                    handle->output = AC101S_OUTPUT_SPEAKER;
                    /* A waiting media STOP can publish while USB cleanup is
                     * pending under this mutex. Removal must not resurrect
                     * that stopped stream on the speaker. */
                    if (atomic_load(&handle->stop_serial) != atomic_load(&handle->stop_completed))
                        err = ESP_ERR_INVALID_STATE;
                    else err = start_locked(handle, handle->requested_volume, handle->mic_gain_db);
                    if (err == ESP_OK && atomic_load(&handle->stop_serial) != atomic_load(&handle->stop_completed)) {
                        const esp_err_t stopped = stop_locked(handle);
                        err = stopped == ESP_OK ? ESP_ERR_INVALID_STATE : stopped;
                    }
                }
                if (err == ESP_OK)
                    err = board_pcm_locked(handle, false, (uint8_t *)data + offset, bytes - offset,
                                           xTaskGetTickCount(), pdMS_TO_TICKS(1000));
                break;
            }
            if (err != ESP_OK) break;
            offset += amount;
        }
    } else if (!handle->board_owner && handle->local_ready) {
        err = board_pcm_locked(handle, false, data, bytes, xTaskGetTickCount(), pdMS_TO_TICKS(1000));
    }
    const uint32_t output_us = write_elapsed_us(output_begin);
    record_write(handle, mutex_us, output_us, true, err);
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_read(ac101s_audio_handle_t handle, void *data, size_t bytes)
{
    ESP_RETURN_ON_FALSE(handle && data && bytes && bytes <= INT_MAX && !(bytes % 4U),
                        ESP_ERR_INVALID_ARG, TAG, "invalid stereo PCM buffer");
    if (!take_io_mutex(handle)) return ESP_ERR_TIMEOUT;
    const esp_err_t err = handle->board_owner ? ESP_ERR_INVALID_STATE :
        handle->external_started ? ESP_ERR_NOT_SUPPORTED : handle->local_ready
        ? board_pcm_locked(handle, true, data, bytes, xTaskGetTickCount(), pdMS_TO_TICKS(1000))
        : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(handle->mutex);
    return err;
}

static esp_err_t local_volume_locked(ac101s_audio_handle_t handle, uint8_t master)
{
    const int effective = speaker_volume(handle->requested_volume, master);
    esp_err_t error = gpio_set_level(handle->pa_enable, 0);
    if (error == ESP_OK) error = ac101s_codec_set_speaker(handle->codec, effective);
    if (error == ESP_OK && effective) error = gpio_set_level(handle->pa_enable, 1);
    return error;
}

esp_err_t ac101s_audio_set_master_volume(ac101s_audio_handle_t handle, uint8_t volume)
{
    if (!handle || volume > 100) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = ESP_OK;
    if (handle->local_ready && (!handle->board_owner || handle->board_ready)) {
        error = local_volume_locked(handle, volume);
        if (error != ESP_OK) {
            /* Preserve the old requested value; only re-enable the PA if
             * restoring its previous volume and mute state also succeeds. */
            if (local_volume_locked(handle, handle->master_volume) != ESP_OK)
                gpio_set_level(handle->pa_enable, 0);
        }
    }
    if (error == ESP_OK) handle->master_volume = volume;
    ESP_LOGI(TAG, "master volume requested=%u applied=%u output=%s active=%u result=%d", volume,
        handle->master_volume, handle->output == AC101S_OUTPUT_EXTERNAL ? "external" : "speaker", handle->started, error);
    xSemaphoreGive(handle->mutex);
    return error;
}

esp_err_t ac101s_audio_get_master_volume(ac101s_audio_handle_t handle, uint8_t *volume)
{
    if (!handle || !volume) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *volume = handle->master_volume;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t ac101s_audio_set_external_sink(ac101s_audio_handle_t handle, const ac101s_audio_sink_t *sink)
{
    if (!handle || (sink && (!sink->start || !sink->stop || !sink->write || !sink->connected)))
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t error = handle->started || handle->capture_active ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (error == ESP_OK) {
        handle->sink = sink ? *sink : (ac101s_audio_sink_t){0};
        if (!sink && handle->output == AC101S_OUTPUT_EXTERNAL) handle->output = AC101S_OUTPUT_SPEAKER;
    }
    xSemaphoreGive(handle->mutex);
    return error;
}

esp_err_t ac101s_audio_set_usb_sink(ac101s_audio_handle_t handle, const ac101s_audio_sink_t *sink)
{
    if (!handle || (sink && (!sink->start || !sink->stop || !sink->write || !sink->connected))) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t err = handle->started || handle->capture_active ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) {
        handle->usb_sink = sink ? *sink : (ac101s_audio_sink_t){0};
        if (!sink && handle->output == AC101S_OUTPUT_USB) handle->output = AC101S_OUTPUT_SPEAKER;
    }
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_set_usb_source(ac101s_audio_handle_t handle, const ac101s_audio_source_t *source)
{
    if (!handle || (source && (!source->start || !source->stop || !source->read || !source->connected))) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t err = handle->started || handle->capture_active ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) {
        handle->usb_source = source ? *source : (ac101s_audio_source_t){0};
        if (!source && handle->input == AC101S_INPUT_USB) handle->input = AC101S_INPUT_BOARD;
    }
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_set_input(ac101s_audio_handle_t handle, ac101s_audio_input_t input)
{
    if (!handle || (input != AC101S_INPUT_BOARD && input != AC101S_INPUT_USB)) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (handle->capture_active && input != handle->input) err = ESP_ERR_INVALID_STATE;
    else if (input == AC101S_INPUT_USB && (!handle->usb_source.connected || !handle->usb_source.connected(handle->usb_source.context)))
        err = ESP_ERR_INVALID_STATE;
    else handle->input = input;
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t ac101s_audio_get_input(ac101s_audio_handle_t handle, ac101s_audio_input_t *input)
{
    if (!handle || !input) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *input = handle->input;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t ac101s_audio_set_output(ac101s_audio_handle_t handle, ac101s_audio_output_t output)
{
    if (!handle || (output != AC101S_OUTPUT_SPEAKER && output != AC101S_OUTPUT_EXTERNAL && output != AC101S_OUTPUT_USB))
        return ESP_ERR_INVALID_ARG;
    const unsigned stop_serial = atomic_load(&handle->stop_serial);
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t alert_recovery = recover_alert_locked(handle);
    if (alert_recovery != ESP_OK) {
        xSemaphoreGive(handle->mutex);
        return alert_recovery;
    }
    if (handle->board_owner || handle->capture_active) {
        const esp_err_t result = output == handle->output ? ESP_OK : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(handle->mutex);
        return result;
    }
    const unsigned requested_stop = atomic_load(&handle->stop_serial);
    if (requested_stop != atomic_load(&handle->stop_completed)) {
        /* STOP may have published its serial before this route acquired the
         * mutex, even if this function captured the same new serial. Complete
         * that stop instead of starting/rolling back either output. */
        const esp_err_t stopped = stop_locked(handle);
        if (stopped == ESP_OK) atomic_store(&handle->stop_completed, requested_stop);
        xSemaphoreGive(handle->mutex);
        return stopped == ESP_OK ? ESP_ERR_INVALID_STATE : stopped;
    }
    if (output == handle->output) {
        const esp_err_t err = handle->external_started && !handle->external_ready ? ESP_ERR_INVALID_STATE : ESP_OK;
        xSemaphoreGive(handle->mutex); return err;
    }
    ac101s_audio_sink_t *sink = output_sink(handle, output);
    if (sink && (!sink->connected || !sink->connected(sink->context))) {
        xSemaphoreGive(handle->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const bool was_started = handle->local_ready || handle->external_ready;
    const ac101s_audio_output_t previous = handle->output;
    atomic_fetch_add(&handle->route_epoch, 1);
    portENTER_CRITICAL(&handle->route_lock);
    ac101s_audio_sink_t *cancel = sink ? sink : output_sink(handle, previous);
    handle->route_cancel = cancel ? cancel->stop : NULL;
    handle->route_cancel_context = cancel ? cancel->context : NULL;
    portEXIT_CRITICAL(&handle->route_lock);
    esp_err_t error = stop_locked(handle);
    if (error == ESP_OK) {
        handle->output = output;
        if (was_started && stop_serial == atomic_load(&handle->stop_serial))
            error = start_locked(handle, handle->requested_volume, handle->mic_gain_db);
        if (error != ESP_OK) {
            const esp_err_t cleanup = stop_locked(handle);
            if (cleanup == ESP_OK) handle->output = previous;
            /* A failed route change must not silently publish the new route. */
            if (cleanup == ESP_OK && stop_serial == atomic_load(&handle->stop_serial) &&
                start_locked(handle, handle->requested_volume, handle->mic_gain_db) != ESP_OK)
                gpio_set_level(handle->pa_enable, 0);
            if (cleanup != ESP_OK) error = cleanup;
        }
    }
    if (stop_serial != atomic_load(&handle->stop_serial)) {
        const esp_err_t stopped = stop_locked(handle);
        error = stopped == ESP_OK ? ESP_ERR_INVALID_STATE : stopped;
    }
    portENTER_CRITICAL(&handle->route_lock);
    handle->route_cancel = NULL; handle->route_cancel_context = NULL;
    portEXIT_CRITICAL(&handle->route_lock);
    atomic_fetch_add(&handle->route_epoch, 1);
    xSemaphoreGive(handle->mutex);
    return error;
}

esp_err_t ac101s_audio_get_output(ac101s_audio_handle_t handle, ac101s_audio_output_t *output)
{
    if (!handle || !output) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *output = handle->output;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}
