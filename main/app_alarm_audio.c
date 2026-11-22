#include "app_alarm_audio.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

enum { SAMPLE_RATE = 48000, ATTACK_FRAMES = 240, RELEASE_FRAMES = 720, MAX_NOTE_MS = 120 };

typedef struct {
    uint16_t frequency;
    uint8_t duration_ms;
} note_t;

typedef struct {
    const char *name;
    note_t notes[4];
    uint8_t count;
    uint8_t gap_ms;
    bool bell;
} melody_t;

static const melody_t melodies[APP_ALARM_TONE_COUNT] = {
    {"清晨", {{660, 95}, {830, 95}, {1047, 120}}, 3, 25, false},
    {"双响", {{880, 120}, {880, 120}}, 2, 70, false},
    {"风铃", {{784, 105}, {1175, 90}, {988, 120}}, 3, 40, true},
    {"急促", {{1000, 75}, {700, 75}, {1000, 75}, {700, 90}}, 4, 12, false},
};

const char *app_alarm_tone_name(unsigned tone)
{
    return tone < APP_ALARM_TONE_COUNT ? melodies[tone].name : "";
}

static int triangle(unsigned phase)
{
    const unsigned height = phase < SAMPLE_RATE / 2 ? phase : SAMPLE_RATE - phase;
    return (int)height - SAMPLE_RATE / 4;
}

static void render_note(int16_t *pcm, unsigned frames, unsigned frequency, bool bell)
{
    unsigned phase = 0;
    unsigned overtone = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        int level = 10000;
        int wave = triangle(phase);
        phase = (phase + frequency) % SAMPLE_RATE;
        if (bell) {
            wave = (3 * wave + triangle(overtone)) / 4;
            overtone = (overtone + frequency * 5 / 2) % SAMPLE_RATE;
            level -= (int)(4300U * frame / frames);
        }
        if (frame < ATTACK_FRAMES) level = level * (int)frame / ATTACK_FRAMES;
        const unsigned remaining = frames - frame - 1;
        if (remaining < RELEASE_FRAMES) level = level * (int)remaining / RELEASE_FRAMES;
        const int16_t sample = wave * level / (SAMPLE_RATE / 4);
        pcm[2 * frame] = sample;
        pcm[2 * frame + 1] = sample;
    }
}

esp_err_t app_alarm_audio_play(ac101s_audio_handle_t audio, unsigned tone,
                               bool (*continue_playing)(void *), void *context)
{
    if (!audio || tone >= APP_ALARM_TONE_COUNT) return ESP_ERR_INVALID_ARG;
    int16_t *pcm = heap_caps_malloc(SAMPLE_RATE / 1000 * MAX_NOTE_MS * 2 * sizeof(*pcm),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) return ESP_ERR_NO_MEM;

    const melody_t *melody = &melodies[tone];
    esp_err_t error = ESP_OK;
    for (unsigned i = 0; i < melody->count; ++i) {
        const note_t note = melody->notes[i];
        const unsigned frames = SAMPLE_RATE / 1000 * note.duration_ms;
        render_note(pcm, frames, note.frequency, melody->bell);
        if (continue_playing && !continue_playing(context)) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        error = ac101s_audio_play_alert(audio, pcm, frames);
        if (error != ESP_OK) break;
        if (i + 1 < melody->count) vTaskDelay(pdMS_TO_TICKS(melody->gap_ms));
    }
    free(pcm);
    return error;
}
