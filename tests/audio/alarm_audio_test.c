#include "app_alarm_audio.h"
#include "freertos/FreeRTOS.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned notes, gaps, first_frequency_crossings;
static unsigned error_note, cancel_after;
static ac101s_audio_handle_t test_audio;

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    ++gaps;
}

esp_err_t ac101s_audio_play_alert(ac101s_audio_handle_t audio, const int16_t *pcm, size_t frames)
{
    assert(audio == test_audio);
    assert(frames >= 3600 && frames <= 5760);
    assert(pcm[0] == 0 && pcm[2 * (frames - 1)] == 0);
    unsigned crossings = 0;
    int peak = 0;
    for (size_t frame = 0; frame < frames; ++frame) {
        const int sample = pcm[2 * frame];
        assert(sample == pcm[2 * frame + 1]);
        if (abs(sample) > peak) peak = abs(sample);
        if (frame > 0 && sample > 0 && pcm[2 * (frame - 1)] <= 0) ++crossings;
    }
    assert(peak >= 6000 && peak <= 10000);
    assert(crossings * 48000 / frames >= 600);
    if (++notes == 1) first_frequency_crossings = crossings * 48000 / frames;
    return notes == error_note ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static bool keep_playing(void *context)
{
    assert(context == &cancel_after);
    return !cancel_after || notes < cancel_after;
}

int main(void)
{
    const int codec_token = 1;
    test_audio = (ac101s_audio_handle_t)(const void *)&codec_token;
    assert(app_alarm_audio_play(NULL, 0, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(app_alarm_audio_play(test_audio, APP_ALARM_TONE_COUNT, NULL, NULL) == ESP_ERR_INVALID_ARG);
    const unsigned note_counts[] = {3, 2, 3, 4};
    unsigned first_frequencies[APP_ALARM_TONE_COUNT];
    for (unsigned tone = 0; tone < APP_ALARM_TONE_COUNT; ++tone) {
        assert(*app_alarm_tone_name(tone));
        for (unsigned prior = 0; prior < tone; ++prior) {
            assert(strcmp(app_alarm_tone_name(tone), app_alarm_tone_name(prior)));
        }
        notes = gaps = 0;
        assert(app_alarm_audio_play(test_audio, tone, keep_playing, &cancel_after) == ESP_OK);
        assert(notes == note_counts[tone] && gaps == notes - 1);
        first_frequencies[tone] = first_frequency_crossings;
        for (unsigned prior = 0; prior < tone; ++prior)
            assert(abs((int)first_frequencies[tone] - (int)first_frequencies[prior]) >= 40);
    }
    notes = gaps = 0;
    cancel_after = 1;
    assert(app_alarm_audio_play(test_audio, 0, keep_playing, &cancel_after) == ESP_ERR_INVALID_STATE);
    assert(notes == 1 && gaps == 1);
    notes = gaps = 0;
    error_note = 2;
    assert(app_alarm_audio_play(test_audio, 3, NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(notes == 2 && gaps == 1);
    puts("PASS four distinct bounded alarm melodies, audible PCM levels, note envelopes, cancellation and codec-busy exit");
    return 0;
}
