#include "aether_recorder_denoise.h"

#include <stddef.h>

#ifdef AETHER_RECORDER_HAS_NSNET3
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_nsn_iface.h"
#include "esp_partition.h"
#include "model_path.h"

enum {
    FIR_HISTORY = 30,
    MODEL_NAME_BYTES = 32,
    MODEL_RECORD_BYTES = MODEL_NAME_BYTES + 4,
    FILE_RECORD_BYTES = MODEL_NAME_BYTES + 8,
    MAX_BUNDLE_MODELS = 8,
    MAX_FILES_PER_MODEL = 16,
    MAX_MODEL_INFO_BYTES = 4096,
    MAX_BUNDLE_BYTES = 512 * 1024,
};

static const char *TAG = "recorder_denoise";

/* ESP-SR 2.6.2 exports this NSNet3 operation table. Linking that exact
 * backend keeps the registry dispatcher and unused NSNet variants out. */
extern const esp_nsn_iface_t esp_nsnet3;

struct aether_recorder_denoise {
    esp_nsn_data_t *instance;
    srmodel_list_t *models;
    uint8_t *bundle;
    int16_t pcm[FIR_HISTORY + AETHER_DENOISE_CAPTURE_FRAMES];
    int16_t input[AETHER_DENOISE_SAMPLES];
    int16_t output[AETHER_DENOISE_SAMPLES];
};

/* Unity gain in Q15, with a 6 kHz low-pass. Same taps as the AC101S
 * intercom so frequencies above the new Nyquist limit do not fold back. */
static const int16_t fir_taps[FIR_HISTORY + 1] = {
    -39, -67, -68, 0, 156, 324, 327, 0, -621, -1189, -1139, 0, 2249, 5022, 7322,
    8214, 7322, 5022, 2249, 0, -1139, -1189, -621, 0, 327, 324, 156, 0, -68, -67, -39,
};

static uint32_t read_le32(const uint8_t *data)
{
    return data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
           (uint32_t)data[3] << 24;
}

/* ESP-SR 2.6.2's srmodel_load trusts counts and offsets. Validate its
 * pack_model.py format before handing it the flash mapping. An erased or
 * partially flashed model partition must leave the original recorder usable. */
static bool model_bundle_valid(const uint8_t *data, size_t size)
{
    if (size < 4) return false;
    const uint32_t model_count = read_le32(data);
    if (!model_count || model_count > MAX_BUNDLE_MODELS) return false;

    size_t header_end = 4;
    size_t first_payload = size;
    bool nsnet3_found = false;
    for (uint32_t i = 0; i < model_count; ++i) {
        if (size - header_end < MODEL_RECORD_BYTES) return false;
        const uint8_t *model = data + header_end;
        if (!memchr(model, 0, MODEL_NAME_BYTES)) return false;
        const bool nsnet3 = strcmp((const char *)model, "nsnet3") == 0;
        if (nsnet3 && nsnet3_found) return false;
        const uint32_t file_count = read_le32(model + MODEL_NAME_BYTES);
        header_end += MODEL_RECORD_BYTES;
        if (!file_count || file_count > MAX_FILES_PER_MODEL ||
            file_count > (size - header_end) / FILE_RECORD_BYTES) return false;

        bool info = false, index = false, weights = false;
        for (uint32_t j = 0; j < file_count; ++j) {
            const uint8_t *file = data + header_end;
            if (!memchr(file, 0, MODEL_NAME_BYTES)) return false;
            const uint32_t offset = read_le32(file + MODEL_NAME_BYTES);
            const uint32_t bytes = read_le32(file + MODEL_NAME_BYTES + 4);
            if (offset > size || !bytes || bytes > size - offset || bytes > INT_MAX) return false;
            if (offset < first_payload) first_payload = offset;
            const char *name = (const char *)file;
            if (!strcmp(name, "_MODEL_INFO_")) {
                if (info || bytes > MAX_MODEL_INFO_BYTES) return false;
                info = true;
            } else if (!strcmp(name, "nsnet3_index")) {
                index = true;
            } else if (!strcmp(name, "nsnet3_data")) {
                weights = true;
            }
            header_end += FILE_RECORD_BYTES;
        }
        if (!info || (nsnet3 && (!index || !weights))) return false;
        nsnet3_found |= nsnet3;
    }
    return nsnet3_found && first_payload >= header_end;
}

static const esp_partition_t *model_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
}

bool aether_recorder_denoise_supported(void)
{
    return model_partition() != NULL;
}

void aether_recorder_denoise_close(aether_recorder_denoise_t *denoise)
{
    if (!denoise) return;
    if (denoise->instance) esp_nsnet3.destroy(denoise->instance);
    if (denoise->models) esp_srmodel_deinit(denoise->models);
    free(denoise->bundle);
    free(denoise);
}

aether_recorder_denoise_t *aether_recorder_denoise_open(void)
{
    const esp_partition_t *partition = model_partition();
    if (!partition || partition->size > MAX_BUNDLE_BYTES || get_static_srmodels()) {
        ESP_LOGW(TAG, "NSNet3: model partition unavailable or registry busy");
        return NULL;
    }
    aether_recorder_denoise_t *denoise = heap_caps_calloc(1, sizeof(*denoise),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!denoise) {
        ESP_LOGW(TAG, "NSNet3: no PSRAM for processing buffers");
        return NULL;
    }

    /* Keep the model's data alive for ESP-SR and avoid relying on a large
     * contiguous flash mapping for the high-offset partition on S31. */
    const char *reason = "no PSRAM for model data";
    denoise->bundle = heap_caps_malloc(partition->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!denoise->bundle) goto unavailable;
    const esp_err_t read_error = esp_partition_read(partition, 0, denoise->bundle, partition->size);
    reason = "invalid model bundle";
    if (read_error != ESP_OK) {
        ESP_LOGW(TAG, "NSNet3: model partition read failed (%d)", (int)read_error);
        aether_recorder_denoise_close(denoise);
        return NULL;
    }
    if (!model_bundle_valid(denoise->bundle, partition->size)) goto unavailable;
    denoise->models = srmodel_load(denoise->bundle);
    char *name = esp_srmodel_filter(denoise->models, "nsnet3", NULL);
    reason = "model name missing from ESP-SR registry";
    if (!name) goto unavailable;
    denoise->instance = esp_nsnet3.create(name);
    reason = "ESP-SR model creation failed";
    if (!denoise->instance) goto unavailable;
    reason = "unsupported NSNet3 frame format";
    if (esp_nsnet3.get_samp_rate(denoise->instance) != AETHER_DENOISE_RATE ||
        esp_nsnet3.get_samp_chunksize(denoise->instance) != AETHER_DENOISE_SAMPLES)
        goto unavailable;
    ESP_LOGI(TAG, "NSNet3 ready: 16000 Hz, 256 samples");
    return denoise;

unavailable:
    ESP_LOGW(TAG, "NSNet3 unavailable: %s", reason);
    aether_recorder_denoise_close(denoise);
    return NULL;
}

const int16_t *aether_recorder_denoise_process(aether_recorder_denoise_t *denoise,
                                              const int16_t *stereo)
{
    for (size_t i = 0; i < AETHER_DENOISE_CAPTURE_FRAMES; ++i)
        denoise->pcm[FIR_HISTORY + i] = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2);

    for (size_t i = 0; i < AETHER_DENOISE_SAMPLES; ++i) {
        const int16_t *window = denoise->pcm + AETHER_DENOISE_RATIO * i;
        int32_t sum = 0;
        for (size_t tap = 0; tap <= FIR_HISTORY; ++tap)
            sum += (int32_t)window[tap] * fir_taps[tap];
        int32_t sample = sum / 32768;
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        denoise->input[i] = (int16_t)sample;
    }
    memmove(denoise->pcm, denoise->pcm + AETHER_DENOISE_CAPTURE_FRAMES,
            FIR_HISTORY * sizeof(denoise->pcm[0]));
    return esp_nsnet3.process(denoise->instance, denoise->input, denoise->output) < 0 ?
           NULL : denoise->output;
}
#else
bool aether_recorder_denoise_supported(void)
{
    return false;
}

aether_recorder_denoise_t *aether_recorder_denoise_open(void)
{
    return NULL;
}

void aether_recorder_denoise_close(aether_recorder_denoise_t *denoise)
{
    (void)denoise;
}

const int16_t *aether_recorder_denoise_process(aether_recorder_denoise_t *denoise,
                                              const int16_t *stereo)
{
    (void)denoise;
    (void)stereo;
    return NULL;
}
#endif
