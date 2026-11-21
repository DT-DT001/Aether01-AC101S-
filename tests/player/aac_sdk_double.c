/* Host-only AAC primitive double. The container parser, AAC adapter, pull
 * stream, music worker, timing/leases/controls are production code. Actual
 * Espressif AAC machine code is verified by the target fixture separately. */
#include "esp_aac_dec.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef struct { esp_aac_dec_cfg_t config; } decoder_t;
esp_audio_err_t esp_aac_dec_open(void *configuration, uint32_t bytes, void **out)
{
    assert(bytes == sizeof(esp_aac_dec_cfg_t));
    decoder_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_AUDIO_ERR_MEM_LACK;
    d->config = *(esp_aac_dec_cfg_t *)configuration; *out = d;
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_aac_dec_decode(void *decoder, esp_audio_dec_in_raw_t *in,
                                   esp_audio_dec_out_frame_t *out, esp_audio_dec_info_t *info)
{
    decoder_t *d = decoder;
    assert(d && in->len == 6 && !memcmp(in->buffer, "\x21\0\0\0\0\x1c", 6));
    assert(out->len >= 1024U * d->config.channel * 2);
    memset(out->buffer, 0, 1024U * d->config.channel * 2);
    out->decoded_size = 1024U * d->config.channel * 2;
    in->consumed = in->len;
    *info = (esp_audio_dec_info_t){.sample_rate = (uint32_t)d->config.sample_rate,
        .channel = d->config.channel, .bits_per_sample = 16, .frame_size = 1024};
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_aac_dec_reset(void *decoder) { assert(decoder); return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_aac_dec_close(void *decoder) { free(decoder); return ESP_AUDIO_ERR_OK; }
