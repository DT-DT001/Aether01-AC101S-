#include "wav_stream.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t aether_wav_parse(FILE *file, aether_wav_info_t *info)
{
    if (!file || !info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    if (fseek(file, 0, SEEK_END)) return ESP_FAIL;
    const long length = ftell(file);
    if (length < 12 || fseek(file, 0, SEEK_SET)) return ESP_ERR_INVALID_SIZE;
    uint8_t head[12];
    if (fread(head, 1, 12, file) != 12 || memcmp(head, "RIFF", 4) ||
        memcmp(head + 8, "WAVE", 4)) return ESP_ERR_INVALID_RESPONSE;
    const uint64_t end = (uint64_t)le32(head + 4) + 8;
    if (end < 12 || end > (uint64_t)length || end > LONG_MAX) return ESP_ERR_INVALID_SIZE;
    bool format = false, data = false;
    for (uint64_t pos = 12; pos + 8 <= end;) {
        uint8_t chunk[8];
        if (fseek(file, (long)pos, SEEK_SET) || fread(chunk, 1, 8, file) != 8) return ESP_FAIL;
        const uint32_t bytes = le32(chunk + 4);
        const uint64_t next = pos + 8 + bytes + (bytes & 1U);
        if (next > end) return ESP_ERR_INVALID_SIZE;
        if (!memcmp(chunk, "fmt ", 4)) {
            uint8_t fmt[16];
            if (bytes < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
                return ESP_ERR_INVALID_SIZE;
            }
            info->channels = le16(fmt + 2);
            info->sample_rate = le32(fmt + 4);
            if (le16(fmt) != 1 || le16(fmt + 14) != 16 ||
                (info->channels != 1 && info->channels != 2) ||
                info->sample_rate < 8000 || info->sample_rate > 48000) return ESP_ERR_NOT_SUPPORTED;
            if (le16(fmt + 12) != info->channels * 2U ||
                le32(fmt + 8) != info->sample_rate * info->channels * 2U) return ESP_ERR_INVALID_RESPONSE;
            format = true;
        } else if (!memcmp(chunk, "data", 4) && !data) {
            info->data_offset = (uint32_t)(pos + 8);
            info->data_bytes = bytes;
            data = true;
        }
        if (format && data) break;
        pos = next;
    }
    if (!format || !data) return ESP_ERR_NOT_FOUND;
    if (info->data_bytes % (info->channels * 2U)) return ESP_ERR_INVALID_SIZE;
    info->frames = info->data_bytes / (info->channels * 2U);
    return fseek(file, info->data_offset, SEEK_SET) ? ESP_FAIL : ESP_OK;
}

esp_err_t aether_wav_seek(aether_wav_stream_t *s, uint32_t position_ms)
{
    if (!s || !s->file || !s->info.sample_rate) return ESP_ERR_INVALID_ARG;
    uint64_t frame = (uint64_t)position_ms * s->info.sample_rate / 1000;
    if (frame > s->info.frames) frame = s->info.frames;
    const uint64_t position = s->info.data_offset + frame * s->info.channels * 2;
    if (position > LONG_MAX || fseek(s->file, (long)position, SEEK_SET)) return ESP_FAIL;
    s->phase = frame * 48000;
    s->buffered = 0;
    s->first_frame = (uint32_t)frame;
    clearerr(s->file);
    return ESP_OK;
}

static bool sample_at(aether_wav_stream_t *s, uint32_t frame, int16_t pair[2])
{
    if (frame >= s->info.frames) frame = s->info.frames - 1;
    if (!s->buffered || frame < s->first_frame || frame >= s->first_frame + s->buffered) {
        const uint64_t position = s->info.data_offset + (uint64_t)frame * s->info.channels * 2;
        if (position > LONG_MAX || fseek(s->file, (long)position, SEEK_SET)) return false;
        size_t wanted = s->info.frames - frame;
        if (wanted > 1024) wanted = 1024;
        const size_t samples = fread(s->input, sizeof(int16_t) * s->info.channels, wanted, s->file);
        if (samples != wanted) return false;
        s->first_frame = frame;
        s->buffered = samples;
    }
    const int16_t *p = s->input + (frame - s->first_frame) * s->info.channels;
    pair[0] = p[0];
    pair[1] = s->info.channels == 1 ? p[0] : p[1];
    return true;
}

size_t aether_wav_read(aether_wav_stream_t *s, int16_t *out, size_t frames, esp_err_t *error)
{
    *error = ESP_OK;
    size_t done = 0;
    while (done < frames && s->phase / 48000 < s->info.frames) {
        const uint32_t frame = (uint32_t)(s->phase / 48000);
        const int32_t fraction = (int32_t)(s->phase % 48000);
        int16_t a[2], b[2];
        if (!sample_at(s, frame, a) || !sample_at(s, frame + 1, b)) {
            *error = ESP_ERR_INVALID_SIZE;
            break;
        }
        for (unsigned ch = 0; ch < 2; ++ch) {
            out[done * 2 + ch] = (int16_t)((int32_t)a[ch] +
                ((int64_t)b[ch] - a[ch]) * fraction / 48000);
        }
        ++done;
        s->phase += s->info.sample_rate;
    }
    return done;
}

uint32_t aether_wav_position_ms(const aether_wav_stream_t *s)
{
    if (!s->info.sample_rate) return 0;
    return (uint32_t)((s->phase / 48) / s->info.sample_rate);
}

void aether_pcm_gain(int16_t *samples, size_t count, int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    /* An amplitude curve gives useful control at the quiet end. */
    const int32_t gain = volume * volume;
    for (size_t i = 0; i < count; ++i) samples[i] = (int16_t)((int32_t)samples[i] * gain / 10000);
}
