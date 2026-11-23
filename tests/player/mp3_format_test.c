#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mp3_stream.h"

typedef struct {
    int16_t *pcm;
    size_t frames;
    uint32_t rate, duration;
    uint16_t channels;
} decoded_t;

int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }

static unsigned cancel_calls, cancel_at;
static bool cancel(void *context)
{
    (void)context;
    return cancel_at && ++cancel_calls >= cancel_at;
}

static void copy_fixture(FILE *file)
{
    FILE *source = fopen("tests/player/fixtures/sine-stereo.mp3", "rb");
    assert(source);
    uint8_t buffer[1024];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), source)))
        assert(fwrite(buffer, 1, count, file) == count);
    assert(!ferror(source));
    fclose(source);
}

/* Original encoded audio remains unchanged. These wrappers are generated at
 * test time, so no third-party recording or new encoder is needed. */
static FILE *wrapped(unsigned id3_size, int xing_frames, bool audio)
{
    FILE *file = tmpfile();
    assert(file);
    if (id3_size) {
        const uint8_t header[] = {'I', 'D', '3', 4, 0, 0,
            (id3_size >> 21) & 127, (id3_size >> 14) & 127,
            (id3_size >> 7) & 127, id3_size & 127};
        assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
        for (unsigned i = 0; i < id3_size; ++i) assert(fputc(0, file) == 0);
    }
    if (xing_frames >= 0) {
        uint8_t frame[417] = {0xff, 0xfb, 0x90, 0};
        memcpy(frame + 36, "Xing", 4);
        frame[43] = 1; /* FRAMES flag */
        frame[44] = (unsigned)xing_frames >> 24;
        frame[45] = (unsigned)xing_frames >> 16;
        frame[46] = (unsigned)xing_frames >> 8;
        frame[47] = (unsigned)xing_frames;
        assert(fwrite(frame, 1, sizeof(frame), file) == sizeof(frame));
    }
    if (audio) copy_fixture(file);
    rewind(file);
    return file;
}

static decoded_t decode(FILE *file)
{
    aether_mp3_stream_t *stream = NULL;
    assert(aether_mp3_open(file, NULL, NULL, &stream) == ESP_OK);
    decoded_t output = {.rate = aether_mp3_rate(stream),
        .channels = aether_mp3_channels(stream), .duration = aether_mp3_duration_ms(stream)};
    const size_t capacity = (size_t)output.duration * 48 + 4800;
    output.pcm = calloc(capacity * 2, sizeof(*output.pcm));
    assert(output.pcm);
    for (;;) {
        int16_t buffer[2 * 1024];
        esp_err_t error;
        const size_t frames = aether_mp3_read(stream, buffer, 1024, &error);
        assert(error == ESP_OK);
        assert(output.frames + frames <= capacity);
        memcpy(output.pcm + 2 * output.frames, buffer, frames * 2 * sizeof(*buffer));
        output.frames += frames;
        if (!frames) break;
    }
    assert(output.frames);
    assert(aether_mp3_seek(stream, 500) == ESP_OK);
    assert(aether_mp3_position_ms(stream) == 500);
    int16_t seeked[2 * 512];
    esp_err_t error;
    const size_t frames = aether_mp3_read(stream, seeked, 512, &error);
    assert(error == ESP_OK && frames == 512);
    for (size_t i = 0; i < frames * 2; ++i)
        assert(abs((int)seeked[i] - output.pcm[2 * 24000 + i]) <= 2);
    aether_mp3_close(stream);
    return output;
}

static void match(FILE *file, const decoded_t *reference)
{
    decoded_t actual = decode(file);
    assert(actual.rate == reference->rate && actual.channels == reference->channels);
    assert(actual.duration == reference->duration && actual.frames == reference->frames);
    assert(!memcmp(actual.pcm, reference->pcm, actual.frames * 2 * sizeof(*actual.pcm)));
    free(actual.pcm);
    fclose(file);
}

static void test_wrappers(void)
{
    FILE *file = wrapped(0, -1, true);
    decoded_t reference = decode(file);
    fclose(file);
    assert(reference.rate == 44100 && reference.channels == 2 && reference.frames == 47648);
    match(wrapped(1, -1, true), &reference);
    match(wrapped(10000, -1, true), &reference);
    match(wrapped(250000, -1, true), &reference);
    match(wrapped(0, 38, true), &reference);
    puts("PASS original encoded MP3, ID3v2.4 small/large tags and valid Xing duration/seek");
    match(wrapped(0, 0, true), &reference);
    match(wrapped(250000, 0, true), &reference);
    puts("PASS zero-count Xing recovery preserves full PCM, duration, source offsets and seek");
    free(reference.pcm);
}

static void test_seek_index_prefix(void)
{
    const unsigned copies = 96;
    FILE *tagged = wrapped(250000, 38 * copies, false);
    FILE *reference = wrapped(0, -1, false);
    /* Omit the fixture's truncated 39th frame before concatenating it. */
    uint8_t complete_frames[15846];
    FILE *fixture = fopen("tests/player/fixtures/sine-stereo.mp3", "rb");
    assert(fixture && fread(complete_frames, sizeof(complete_frames), 1, fixture) == 1);
    fclose(fixture);
    assert(fseek(tagged, 0, SEEK_END) == 0);
    for (unsigned i = 0; i < copies; ++i) {
        assert(fwrite(complete_frames, sizeof(complete_frames), 1, tagged) == 1);
        assert(fwrite(complete_frames, sizeof(complete_frames), 1, reference) == 1);
    }
    rewind(tagged);
    rewind(reference);

    aether_mp3_stream_t *stream = NULL, *baseline = NULL;
    assert(aether_mp3_open(tagged, NULL, NULL, &stream) == ESP_OK);
    assert(aether_mp3_open(reference, NULL, NULL, &baseline) == ESP_OK);
    assert(aether_mp3_duration_ms(stream) == aether_mp3_duration_ms(baseline));

    const unsigned targets[] = {2000, 1200, 3200, 73121, 71160, 82052, 0, 3100};
    unsigned reads[sizeof(targets) / sizeof(targets[0])];
    for (size_t index = 0; index < sizeof(targets) / sizeof(targets[0]); ++index) {
        aether_mp3_clear_metrics(stream);
        assert(aether_mp3_seek(stream, targets[index]) == ESP_OK);
        aether_mp3_metrics_t metrics;
        aether_mp3_get_metrics(stream, &metrics);
        reads[index] = metrics.read_calls;
        assert(aether_mp3_seek(baseline, targets[index]) == ESP_OK);
        assert(aether_mp3_position_ms(stream) == aether_mp3_position_ms(baseline));
        int16_t actual[1024], expected[1024];
        esp_err_t error;
        assert(aether_mp3_read(stream, actual, 512, &error) == 512 && error == ESP_OK);
        assert(aether_mp3_read(baseline, expected, 512, &error) == 512 && error == ESP_OK);
        for (size_t sample = 0; sample < 1024; ++sample)
            assert(abs((int)actual[sample] - expected[sample]) <= 2);
    }
    assert(reads[0] <= 12 && reads[1] <= 4 && reads[2] <= 9);
    assert(reads[3] > 60 && reads[4] <= 4 && reads[5] <= 24);
    printf("PASS 95s Xing MP3 exact forward/backwards seek; first early seek %u reads, cached seek %u reads, prefix extension %u reads\n",
           reads[0], reads[1], reads[2]);
    aether_mp3_close(stream);
    aether_mp3_close(baseline);
    fclose(tagged);
    fclose(reference);
}

static void test_rejection_and_cancel(void)
{
    aether_mp3_stream_t *stream = (void *)1;
    FILE *file = wrapped(0, 0, false);
    assert(aether_mp3_open(file, NULL, NULL, &stream) != ESP_OK && !stream);
    fclose(file);
    file = tmpfile(); assert(file);
    assert(fwrite("fLaCnot-an-mp3", 1, 14, file) == 14); rewind(file);
    assert(aether_mp3_open(file, NULL, NULL, &stream) != ESP_OK && !stream);
    fclose(file);
    cancel_at = UINT32_MAX;
    cancel_calls = 0;
    file = wrapped(250000, 0, true);
    assert(aether_mp3_open(file, cancel, NULL, &stream) == ESP_OK);
    const unsigned open_callbacks = cancel_calls;
    assert(open_callbacks > 4);
    aether_mp3_close(stream);
    fclose(file);
    for (cancel_at = 1; cancel_at <= open_callbacks; ++cancel_at) {
        cancel_calls = 0;
        file = wrapped(250000, 0, true);
        esp_err_t error = aether_mp3_open(file, cancel, NULL, &stream);
        assert(error == ESP_ERR_INVALID_STATE && !stream);
        fclose(file);
    }
    cancel_at = 0;
    puts("PASS empty Xing/non-MP3 rejection and cancelable initial/fallback scans");
}

static void test_mpeg_rates(void)
{
    const struct { uint8_t header[4]; unsigned rate, bytes, samples; } formats[] = {
        {{0xff, 0xfb, 0x90, 0}, 44100, 417, 1152},
        {{0xff, 0xfb, 0x94, 0}, 48000, 384, 1152},
        {{0xff, 0xfb, 0x98, 0}, 32000, 576, 1152},
        {{0xff, 0xf3, 0x90, 0}, 22050, 261, 576},
        {{0xff, 0xf3, 0x98, 0}, 16000, 360, 576},
        {{0xff, 0xe3, 0x90, 0}, 11025, 522, 576},
        {{0xff, 0xe3, 0x98, 0}, 8000, 720, 576},
    };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        for (unsigned mono = 0; mono < 2; ++mono) {
            FILE *file = tmpfile(); assert(file);
            uint8_t frame[720] = {0};
            memcpy(frame, formats[i].header, 4);
            if (mono) frame[3] = 0xc0;
            for (unsigned j = 0; j < 40; ++j)
                assert(fwrite(frame, 1, formats[i].bytes, file) == formats[i].bytes);
            rewind(file);
            aether_mp3_stream_t *stream = NULL;
            assert(aether_mp3_open(file, NULL, NULL, &stream) == ESP_OK);
            assert(aether_mp3_rate(stream) == formats[i].rate);
            assert(aether_mp3_channels(stream) == (mono ? 1 : 2));
            const uint64_t source_frames = 40U * formats[i].samples;
            assert(aether_mp3_duration_ms(stream) == source_frames * 1000 / formats[i].rate);
            size_t total = 0, count;
            do {
                int16_t pcm[2 * 1024];
                esp_err_t error;
                count = aether_mp3_read(stream, pcm, 1024, &error);
                assert(error == ESP_OK);
                for (size_t j = 0; j < 2 * count; ++j) assert(pcm[j] == 0);
                total += count;
            } while (count);
            assert(total == (source_frames * 48000 + formats[i].rate - 1) / formats[i].rate);
            aether_mp3_close(stream);
            fclose(file);
        }
    }
    puts("PASS MPEG-1/2/2.5 8-48kHz mono/stereo full decode and resampling boundaries");
}

int main(void)
{
    setbuf(stdout, NULL);
    test_wrappers();
    test_seek_index_prefix();
    test_rejection_and_cancel();
    test_mpeg_rates();
    return 0;
}
