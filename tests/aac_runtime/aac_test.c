#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp4_aac.h"
#include "esp_aac_dec.h"

typedef struct { esp_aac_dec_cfg_t config; uint64_t index; } fake_decoder_t;
typedef enum { NORMAL, NO_PROGRESS, OVER_CONSUMED, WRONG_FORMAT, SHORT_PCM,
               LARGE_PCM, BUFFER_SHORT, CODEC_ERROR, CANCEL_DURING, PARTIAL_HEADER } fault_t;
static unsigned opened, closed, decodes, resets, allocations, callbacks, maximum_block;
static bool fail_alloc, fail_open, partial_open, fail_reset, cancel_now, cancel_on_open;
static unsigned fail_callback_at, cancel_callback_at;
static fault_t fault;
static uint32_t rate;
static unsigned channels;
static uint64_t expected_total, received;
static bool verify_pcm;

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{ assert(caps == 3); ++allocations; return fail_alloc ? NULL : calloc(count, size); }
static int16_t source_sample(uint64_t index, unsigned channel)
{ return (int16_t)((int64_t)((index * (channel ? 607U : 293U) + channel * 901U) % 50001U) - 25000); }
static bool cancel(void *context) { assert(context == &cancel_now); return cancel_now; }

esp_audio_err_t esp_aac_dec_open(void *configuration, uint32_t size, void **out)
{
    assert(size == sizeof(esp_aac_dec_cfg_t));
    const esp_aac_dec_cfg_t *cfg = configuration;
    assert(cfg->bits_per_sample == 16 && cfg->no_adts_header && !cfg->aac_plus_enable);
    ++opened;
    *out = NULL;
    if (fail_open && !partial_open) return ESP_AUDIO_ERR_MEM_LACK;
    fake_decoder_t *decoder = calloc(1, sizeof(*decoder));
    assert(decoder); decoder->config = *cfg; *out = decoder;
    if (cancel_on_open) cancel_now = true;
    return fail_open ? ESP_AUDIO_ERR_MEM_LACK : ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_aac_dec_close(void *handle)
{ assert(handle); ++closed; free(handle); return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_aac_dec_reset(void *handle)
{ ++resets; if (fail_reset) return ESP_AUDIO_ERR_FAIL; ((fake_decoder_t *)handle)->index = 0; return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_aac_dec_decode(void *handle, esp_audio_dec_in_raw_t *raw,
                                   esp_audio_dec_out_frame_t *output, esp_audio_dec_info_t *info)
{
    fake_decoder_t *decoder = handle;
    assert(raw->len && output->len >= 4096);
    ++decodes;
    raw->consumed = raw->len;
    output->decoded_size = 0;
    if (fault == NO_PROGRESS) { raw->consumed = 0; return ESP_AUDIO_ERR_OK; }
    if (fault == OVER_CONSUMED) { ++raw->consumed; return ESP_AUDIO_ERR_OK; }
    if (fault == BUFFER_SHORT) { output->needed_size = 8192; return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH; }
    if (fault == CODEC_ERROR) return ESP_AUDIO_ERR_HEADER_PARSE;
    if (fault == CANCEL_DURING) cancel_now = true;
    if (fault == PARTIAL_HEADER && decodes == 1) { raw->consumed /= 2; return ESP_AUDIO_ERR_OK; }
    *info = (esp_audio_dec_info_t){.sample_rate = (uint32_t)decoder->config.sample_rate,
        .channel = decoder->config.channel, .bits_per_sample = 16};
    output->decoded_size = 1024U * info->channel * 2U;
    if (fault == WRONG_FORMAT) info->channel = 0;
    if (fault == SHORT_PCM) output->decoded_size /= 2U;
    if (fault == LARGE_PCM) { output->decoded_size = 4098; return ESP_AUDIO_ERR_OK; }
    int16_t *pcm = (int16_t *)output->buffer;
    for (unsigned i = 0; i < 1024; ++i) for (unsigned ch = 0; ch < decoder->config.channel; ++ch)
        *pcm++ = source_sample(decoder->index + i, ch);
    decoder->index += 1024;
    return ESP_AUDIO_ERR_OK;
}

static esp_err_t accept_pcm(const int16_t *pcm, size_t frames, void *context)
{
    assert(context == &received && frames && frames <= 1024);
    ++callbacks;
    if (frames > maximum_block) maximum_block = (unsigned)frames;
    if (fail_callback_at == callbacks) return ESP_FAIL;
    if (verify_pcm) for (size_t frame = 0; frame < frames; ++frame) {
        uint64_t numerator = (received + frame) * rate;
        uint64_t index = numerator / 48000U;
        int64_t fraction = (int64_t)(numerator % 48000U);
        assert(index < expected_total);
        uint64_t next = index + 1U < expected_total ? index + 1U : index;
        for (unsigned ch = 0; ch < 2; ++ch) {
            unsigned source_channel = channels == 1 ? 0 : ch;
            int16_t a = source_sample(index, source_channel), b = source_sample(next, source_channel);
            int16_t expected = (int16_t)(a + ((int64_t)b - a) * fraction / 48000);
            assert(pcm[frame * 2U + ch] == expected);
        }
    }
    received += frames;
    if (cancel_callback_at == callbacks) cancel_now = true;
    return ESP_OK;
}

static void fresh(void)
{
    opened = closed = decodes = resets = allocations = callbacks = maximum_block = 0;
    fail_alloc = fail_open = partial_open = fail_reset = cancel_now = cancel_on_open = false;
    fail_callback_at = cancel_callback_at = 0;
    fault = NORMAL; received = 0; verify_pcm = false;
}
static size_t asc(uint8_t output[64], unsigned object, unsigned frequency, unsigned channel,
                    unsigned flags, unsigned extension)
{
    memset(output, 0, 64);
    uint32_t values[] = {object, frequency, channel, flags};
    unsigned widths[] = {5, 4, 4, 3};
    size_t bits = 0;
    for (unsigned i = 0; i < 4; ++i) for (unsigned b = widths[i]; b; --b) {
        output[bits / 8U] |= ((values[i] >> (b - 1U)) & 1U) << (7U - bits % 8U); ++bits;
    }
    if (extension) {
        uint32_t extra = (0x2B7U << 6) | (5U << 1) | (extension == 2U);
        for (unsigned b = 17; b; --b) {
            output[bits / 8U] |= ((extra >> (b - 1U)) & 1U) << (7U - bits % 8U); ++bits;
        }
    }
    return (bits + 7U) / 8U;
}
static mp4_aac_handle_t create(unsigned frequency, unsigned channel)
{
    uint8_t config[64]; mp4_aac_handle_t handle;
    size_t length = asc(config, 2, frequency, channel, 0, 1);
    assert(mp4_aac_new(config, length, cancel, &cancel_now, &handle) == ESP_OK);
    return handle;
}
static void check_asc(void)
{
    uint8_t config[64]; mp4_aac_config_t info;
    for (unsigned frequency = 0; frequency < 16; ++frequency) for (unsigned channel = 0; channel < 8; ++channel) {
        size_t length = asc(config, 2, frequency, channel, 0, 0);
        esp_err_t result = mp4_aac_parse_asc(config, length, &info);
        assert((result == ESP_OK) == (frequency >= 3 && frequency <= 11 && (channel == 1 || channel == 2)));
    }
    for (unsigned flag = 1; flag < 8; ++flag) {
        size_t length = asc(config, 2, 4, 2, flag, 0);
        assert(mp4_aac_parse_asc(config, length, &info) == ESP_ERR_NOT_SUPPORTED);
    }
    size_t length = asc(config, 5, 4, 2, 0, 0);
    assert(mp4_aac_parse_asc(config, length, &info) == ESP_ERR_NOT_SUPPORTED);
    length = asc(config, 2, 4, 2, 0, 2);
    assert(mp4_aac_parse_asc(config, length, &info) == ESP_ERR_NOT_SUPPORTED);
    length = asc(config, 2, 4, 2, 0, 1);
    assert(mp4_aac_parse_asc(config, length, &info) == ESP_OK && info.sample_rate == 44100 && info.channels == 2);
    const uint8_t ffmpeg_lc[] = {0x12, 0x10, 0x56, 0xE5, 0};
    assert(mp4_aac_parse_asc(ffmpeg_lc, sizeof(ffmpeg_lc), &info) == ESP_OK);
    const uint8_t explicit_48k[] = {0x17, 0x80, 0x5D, 0xC0, 0x10};
    assert(mp4_aac_parse_asc(explicit_48k, sizeof(explicit_48k), &info) == ESP_OK && info.sample_rate == 48000 && info.channels == 2);
    for (size_t n = 0; n < 2; ++n) assert(mp4_aac_parse_asc(config, n, &info) == ESP_ERR_INVALID_SIZE);
    assert(mp4_aac_parse_asc(NULL, 2, &info) == ESP_ERR_INVALID_ARG);
    assert(mp4_aac_parse_asc(config, 65, &info) == ESP_ERR_INVALID_SIZE);
    uint32_t random = 1;
    for (unsigned i = 0; i < 20000; ++i) {
        for (unsigned j = 0; j < sizeof(config); ++j) { random = random * 1664525U + 1013904223U; config[j] = (uint8_t)(random >> 24); }
        (void)mp4_aac_parse_asc(config, i % 65U, &info);
    }
}
static void check_resampling(unsigned frequency, unsigned channel, uint32_t hz)
{
    fresh(); rate = hz; channels = channel; expected_total = 73U * 1024U; verify_pcm = true;
    mp4_aac_handle_t handle = create(frequency, channel);
    const uint8_t au[8] = {0};
    for (unsigned i = 0; i < 73; ++i) assert(mp4_aac_decode(handle, au, sizeof(au), accept_pcm, &received) == ESP_OK);
    assert(mp4_aac_finish(handle, accept_pcm, &received) == ESP_OK);
    uint64_t expected = (expected_total * 48000U + rate - 1U) / rate;
    assert(received == expected && maximum_block <= 1024);
    mp4_aac_status_t status;
    assert(mp4_aac_get_status(handle, &status) == ESP_OK);
    assert(status.input_frames == expected_total && status.output_frames == expected && status.access_units == 73 && status.finished);
    assert(mp4_aac_finish(handle, accept_pcm, &received) == ESP_OK && received == expected);
    assert(mp4_aac_decode(handle, au, sizeof(au), accept_pcm, &received) == ESP_ERR_INVALID_STATE);
    assert(mp4_aac_reset(handle) == ESP_OK);
    received = 0; expected_total = 1024;
    assert(mp4_aac_decode(handle, au, sizeof(au), accept_pcm, &received) == ESP_OK);
    assert(mp4_aac_finish(handle, accept_pcm, &received) == ESP_OK);
    assert(received == (1024U * 48000U + rate - 1U) / rate);
    mp4_aac_del(handle); assert(opened == 1 && closed == 1 && resets == 1);
}
static void check_faults(void)
{
    uint8_t config[64]; size_t length = asc(config, 2, 4, 2, 0, 0);
    mp4_aac_handle_t handle = (void *)1; const uint8_t au[8] = {0};
    fresh(); fail_alloc = true;
    assert(mp4_aac_new(config, length, cancel, &cancel_now, &handle) == ESP_ERR_NO_MEM && !handle && !opened);
    fresh(); fail_open = true;
    assert(mp4_aac_new(config, length, cancel, &cancel_now, &handle) == ESP_ERR_NO_MEM && !handle && !closed);
    fresh(); fail_open = partial_open = true;
    assert(mp4_aac_new(config, length, cancel, &cancel_now, &handle) == ESP_ERR_NO_MEM && !handle && closed == 1);
    fresh(); cancel_now = true;
    assert(mp4_aac_new(config, length, cancel, &cancel_now, &handle) == ESP_ERR_INVALID_STATE && !handle && !allocations);
    fresh(); cancel_on_open = true;
    assert(mp4_aac_new(config, length, cancel, &cancel_now, &handle) == ESP_ERR_INVALID_STATE && !handle && closed == 1);
    for (fault_t f = NO_PROGRESS; f <= CANCEL_DURING; f = (fault_t)(f + 1)) {
        fresh(); handle = create(4, 2); fault = f;
        assert(mp4_aac_decode(handle, au, sizeof(au), accept_pcm, &received) != ESP_OK && !callbacks);
        assert(mp4_aac_decode(handle, au, sizeof(au), accept_pcm, &received) == ESP_ERR_INVALID_STATE && decodes == 1);
        mp4_aac_del(handle); assert(closed == 1);
    }
    fresh(); handle = create(11, 2); cancel_callback_at = 1;
    assert(mp4_aac_decode(handle, au, sizeof(au), accept_pcm, &received) == ESP_ERR_INVALID_STATE && callbacks == 1 && received == 1024);
    mp4_aac_del(handle);
    fresh(); handle = create(11, 2); fail_callback_at = 2;
    assert(mp4_aac_decode(handle, au, sizeof(au), accept_pcm, &received) == ESP_FAIL && received == 1024);
    mp4_aac_status_t status; assert(mp4_aac_get_status(handle, &status) == ESP_OK && status.last_error == ESP_FAIL && status.output_frames == 1024);
    fail_reset = true; assert(mp4_aac_reset(handle) != ESP_OK); mp4_aac_del(handle);
    fresh(); handle = create(4, 1); fault = PARTIAL_HEADER;
    assert(mp4_aac_decode(handle, au, sizeof(au), accept_pcm, &received) == ESP_OK && decodes == 2);
    mp4_aac_del(handle);
    fresh(); handle = create(4, 2);
    assert(mp4_aac_decode(handle, au, 0, accept_pcm, &received) == ESP_ERR_INVALID_SIZE && !decodes);
    assert(mp4_aac_reset(handle) == ESP_OK);
    assert(mp4_aac_decode(handle, au, MP4_AAC_MAX_ACCESS_UNIT + 1U, accept_pcm, &received) == ESP_ERR_INVALID_SIZE && !decodes);
    mp4_aac_del(handle); mp4_aac_del(NULL);
}
int main(void)
{
    check_asc();
    const uint32_t rates[] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
        for (unsigned channel = 1; channel <= 2; ++channel) check_resampling(i + 3U, channel, rates[i]);
    check_faults();
    puts("AAC adapter: ASC/20k malformed configs, exact cross-AU resampling, <=1024-frame blocks, EOF/reset/cancel/OOM/codec/callback faults passed");
    return 0;
}
