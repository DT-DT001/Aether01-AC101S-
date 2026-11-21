#include "mp4_aac.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_aac_dec.h"
#include "esp_heap_caps.h"

/* The official esp_audio_codec 2.6.2 decoder is a separately licensed
 * dependency. Use its ESP32-S31 portable-C variant (ASM disabled), never an
 * ESP32-S3 archive. See the component's Espressif Modified MIT LICENSE. */
#define AAC_SOURCE_FRAMES 1024U
#define AAC_PCM_BYTES (AAC_SOURCE_FRAMES * 2U * sizeof(int16_t))
#define AAC_ASC_MAX_BYTES 64U

typedef struct { const uint8_t *data; size_t bits, position; } bit_reader_t;

struct mp4_aac {
    void *decoder;
    mp4_aac_cancel_cb_t cancel;
    void *cancel_context;
    mp4_aac_status_t status;
    uint64_t phase;
    int16_t previous[2];
    bool have_previous;
    size_t pending;
    int16_t pcm[AAC_SOURCE_FRAMES * 2U];
    int16_t output[MP4_AAC_OUTPUT_BLOCK_FRAMES * 2U];
};

static bool get_bits(bit_reader_t *reader, unsigned count, uint32_t *value)
{
    if (count > 24U || reader->position > reader->bits || count > reader->bits - reader->position) return false;
    uint32_t result = 0;
    while (count--) {
        const size_t bit = reader->position++;
        result = (result << 1) | ((reader->data[bit / 8U] >> (7U - bit % 8U)) & 1U);
    }
    *value = result;
    return true;
}

static bool zero_tail(bit_reader_t reader)
{
    uint32_t value;
    while (reader.position < reader.bits) {
        if (!get_bits(&reader, 1, &value) || value) return false;
    }
    return true;
}

esp_err_t mp4_aac_parse_asc(const uint8_t *asc, size_t length, mp4_aac_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    if (!asc) return ESP_ERR_INVALID_ARG;
    if (length < 2U || length > AAC_ASC_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    static const uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                     24000, 22050, 16000, 12000, 11025, 8000, 7350};
    bit_reader_t reader = {.data = asc, .bits = length * 8U};
    uint32_t object, frequency, rate, channels, flag;
    if (!get_bits(&reader, 5, &object) || !get_bits(&reader, 4, &frequency)) return ESP_ERR_INVALID_SIZE;
    if (object != 2U) return ESP_ERR_NOT_SUPPORTED;
    if (frequency == 15U) {
        if (!get_bits(&reader, 24, &rate)) return ESP_ERR_INVALID_SIZE;
        bool standard = false;
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) if (rates[i] == rate) standard = true;
        if (!standard) return ESP_ERR_NOT_SUPPORTED;
    } else {
        if (frequency >= sizeof(rates) / sizeof(rates[0])) return ESP_ERR_NOT_SUPPORTED;
        rate = rates[frequency];
    }
    if (rate < 8000U || rate > 48000U) return ESP_ERR_NOT_SUPPORTED;
    if (!get_bits(&reader, 4, &channels)) return ESP_ERR_INVALID_SIZE;
    if (channels != 1U && channels != 2U) return ESP_ERR_NOT_SUPPORTED;
    /* frameLengthFlag, dependsOnCoreCoder, extensionFlag. */
    for (unsigned i = 0; i < 3; ++i) {
        if (!get_bits(&reader, 1, &flag)) return ESP_ERR_INVALID_SIZE;
        if (flag) return ESP_ERR_NOT_SUPPORTED;
    }
    if (!zero_tail(reader)) {
        /* FFmpeg writes this optional sync extension even for plain LC.
         * Accept explicit SBR absence, and reject actual SBR/PS extensions. */
        uint32_t sync, extension;
        if (!get_bits(&reader, 11, &sync) || !get_bits(&reader, 5, &extension) ||
            !get_bits(&reader, 1, &flag)) return ESP_ERR_INVALID_SIZE;
        if (sync != 0x2B7U || extension != 5U || flag || !zero_tail(reader)) return ESP_ERR_NOT_SUPPORTED;
    }
    config->sample_rate = rate;
    config->channels = (uint8_t)channels;
    return ESP_OK;
}

static esp_err_t decoder_result(esp_audio_err_t result)
{
    switch (result) {
    case ESP_AUDIO_ERR_OK: return ESP_OK;
    case ESP_AUDIO_ERR_MEM_LACK: return ESP_ERR_NO_MEM;
    case ESP_AUDIO_ERR_INVALID_PARAMETER: return ESP_ERR_INVALID_ARG;
    case ESP_AUDIO_ERR_NOT_SUPPORT: return ESP_ERR_NOT_SUPPORTED;
    case ESP_AUDIO_ERR_DATA_LACK:
    case ESP_AUDIO_ERR_BUFF_NOT_ENOUGH: return ESP_ERR_INVALID_SIZE;
    default: return ESP_ERR_INVALID_RESPONSE;
    }
}

static esp_err_t fail(mp4_aac_handle_t handle, esp_err_t error)
{
    handle->status.last_error = error;
    return error;
}

static bool canceled(mp4_aac_handle_t handle)
{
    return handle->cancel && handle->cancel(handle->cancel_context);
}

esp_err_t mp4_aac_new(const uint8_t *asc, size_t length, mp4_aac_cancel_cb_t cancel,
                      void *cancel_context, mp4_aac_handle_t *out_handle)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;
    *out_handle = NULL;
    mp4_aac_config_t info;
    esp_err_t error = mp4_aac_parse_asc(asc, length, &info);
    if (error != ESP_OK) return error;
    if (cancel && cancel(cancel_context)) return ESP_ERR_INVALID_STATE;
    mp4_aac_handle_t handle = heap_caps_calloc(1, sizeof(*handle), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!handle) return ESP_ERR_NO_MEM;
    handle->cancel = cancel;
    handle->cancel_context = cancel_context;
    handle->status.sample_rate = info.sample_rate;
    handle->status.channels = info.channels;
    esp_aac_dec_cfg_t config = {
        .sample_rate = (int32_t)info.sample_rate,
        .channel = info.channels,
        .bits_per_sample = 16,
        .no_adts_header = true,
        .aac_plus_enable = false,
    };
    error = decoder_result(esp_aac_dec_open(&config, sizeof(config), &handle->decoder));
    if (error == ESP_OK && !handle->decoder) error = ESP_ERR_INVALID_RESPONSE;
    if (error == ESP_OK && canceled(handle)) error = ESP_ERR_INVALID_STATE;
    if (error != ESP_OK) { mp4_aac_del(handle); return error; }
    *out_handle = handle;
    return ESP_OK;
}

static esp_err_t flush_output(mp4_aac_handle_t handle, mp4_aac_pcm_cb_t callback, void *context)
{
    if (!handle->pending) return ESP_OK;
    if (canceled(handle)) return fail(handle, ESP_ERR_INVALID_STATE);
    esp_err_t error = callback(handle->output, handle->pending, context);
    if (error != ESP_OK) return fail(handle, error);
    handle->status.output_frames += handle->pending;
    handle->pending = 0;
    return ESP_OK;
}

static esp_err_t emit(mp4_aac_handle_t handle, int16_t left, int16_t right,
                        mp4_aac_pcm_cb_t callback, void *context)
{
    handle->output[handle->pending * 2U] = left;
    handle->output[handle->pending * 2U + 1U] = right;
    if (++handle->pending == MP4_AAC_OUTPUT_BLOCK_FRAMES) return flush_output(handle, callback, context);
    return ESP_OK;
}

static esp_err_t resample(mp4_aac_handle_t handle, size_t frames,
                            mp4_aac_pcm_cb_t callback, void *context)
{
    const unsigned channels = handle->status.channels;
    for (size_t frame = 0; frame < frames; ++frame) {
        if (canceled(handle)) return fail(handle, ESP_ERR_INVALID_STATE);
        const int16_t current[2] = {handle->pcm[frame * channels],
            handle->pcm[frame * channels + (channels == 1U ? 0U : 1U)]};
        const uint64_t index = handle->status.input_frames;
        if (index >= UINT64_MAX / MP4_AAC_OUTPUT_RATE) return fail(handle, ESP_ERR_INVALID_SIZE);
        if (handle->have_previous) {
            const uint64_t end = index * MP4_AAC_OUTPUT_RATE;
            const uint64_t start = end - MP4_AAC_OUTPUT_RATE;
            while (handle->phase < end) {
                const int64_t fraction = (int64_t)(handle->phase - start);
                int16_t pair[2];
                for (unsigned channel = 0; channel < 2; ++channel) {
                    /* At the native 48k output rate every position is exact:
                     * avoid a 64-bit interpolate/divide on the RV32 target. */
                    pair[channel] = !fraction ? handle->previous[channel] :
                        (int16_t)(handle->previous[channel] +
                        ((int64_t)current[channel] - handle->previous[channel]) * fraction / MP4_AAC_OUTPUT_RATE);
                }
                esp_err_t error = emit(handle, pair[0], pair[1], callback, context);
                if (error != ESP_OK) return error;
                handle->phase += handle->status.sample_rate;
            }
        }
        memcpy(handle->previous, current, sizeof(current));
        handle->have_previous = true;
        ++handle->status.input_frames;
    }
    return ESP_OK;
}

esp_err_t mp4_aac_decode(mp4_aac_handle_t handle, const uint8_t *access_unit, size_t length,
                         mp4_aac_pcm_cb_t callback, void *context)
{
    if (!handle || !access_unit || !callback) return ESP_ERR_INVALID_ARG;
    if (handle->status.last_error != ESP_OK || handle->status.finished) return ESP_ERR_INVALID_STATE;
    if (!length || length > MP4_AAC_MAX_ACCESS_UNIT) return fail(handle, ESP_ERR_INVALID_SIZE);
    size_t consumed = 0, decoded_frames = 0;
    unsigned calls = 0;
    while (consumed < length) {
        if (canceled(handle)) return fail(handle, ESP_ERR_INVALID_STATE);
        /* A complete MP4 AU cannot require unbounded zero-progress retries. */
        if (++calls > 8U) return fail(handle, ESP_ERR_INVALID_RESPONSE);
        esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)access_unit + consumed, .len = (uint32_t)(length - consumed)};
        esp_audio_dec_out_frame_t output = {.buffer = (uint8_t *)handle->pcm, .len = AAC_PCM_BYTES};
        esp_audio_dec_info_t info = {0};
        esp_err_t error = decoder_result(esp_aac_dec_decode(handle->decoder, &raw, &output, &info));
        if (canceled(handle)) return fail(handle, ESP_ERR_INVALID_STATE);
        if (error != ESP_OK) return fail(handle, error);
        if (!raw.consumed || raw.consumed > length - consumed || output.decoded_size > AAC_PCM_BYTES)
            return fail(handle, ESP_ERR_INVALID_RESPONSE);
        consumed += raw.consumed;
        if (output.decoded_size) {
            if (info.sample_rate != handle->status.sample_rate || info.channel != handle->status.channels ||
                info.bits_per_sample != 16U || output.decoded_size % (info.channel * sizeof(int16_t)))
                return fail(handle, ESP_ERR_NOT_SUPPORTED);
            const size_t frames = output.decoded_size / (info.channel * sizeof(int16_t));
            if (frames != AAC_SOURCE_FRAMES || decoded_frames) return fail(handle, ESP_ERR_NOT_SUPPORTED);
            decoded_frames += frames;
            error = resample(handle, frames, callback, context);
            if (error != ESP_OK) return error;
        }
    }
    ++handle->status.access_units;
    return flush_output(handle, callback, context);
}

esp_err_t mp4_aac_finish(mp4_aac_handle_t handle, mp4_aac_pcm_cb_t callback, void *context)
{
    if (!handle || !callback) return ESP_ERR_INVALID_ARG;
    if (handle->status.last_error != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (handle->status.finished) return ESP_OK;
    if (canceled(handle)) return fail(handle, ESP_ERR_INVALID_STATE);
    const uint64_t end = handle->status.input_frames * MP4_AAC_OUTPUT_RATE;
    while (handle->have_previous && handle->phase < end) {
        esp_err_t error = emit(handle, handle->previous[0], handle->previous[1], callback, context);
        if (error != ESP_OK) return error;
        handle->phase += handle->status.sample_rate;
    }
    esp_err_t error = flush_output(handle, callback, context);
    if (error == ESP_OK) handle->status.finished = true;
    return error;
}

esp_err_t mp4_aac_reset(mp4_aac_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    esp_err_t error = decoder_result(esp_aac_dec_reset(handle->decoder));
    if (error != ESP_OK) return fail(handle, error);
    handle->status.access_units = handle->status.input_frames = handle->status.output_frames = 0;
    handle->status.last_error = ESP_OK;
    handle->status.finished = false;
    handle->phase = 0;
    handle->pending = 0;
    handle->have_previous = false;
    memset(handle->previous, 0, sizeof(handle->previous));
    return ESP_OK;
}

esp_err_t mp4_aac_get_status(mp4_aac_handle_t handle, mp4_aac_status_t *status)
{
    if (!handle || !status) return ESP_ERR_INVALID_ARG;
    *status = handle->status;
    return ESP_OK;
}

void mp4_aac_del(mp4_aac_handle_t handle)
{
    if (!handle) return;
    if (handle->decoder) esp_aac_dec_close(handle->decoder);
    free(handle);
}
