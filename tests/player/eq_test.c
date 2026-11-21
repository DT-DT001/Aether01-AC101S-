#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "music_eq.h"

static void test_exact_bypass(void)
{
    music_eq_t state = {0};
    int16_t pcm[] = {INT16_MIN, INT16_MAX, -32, 51, -8, 23, 0, -1};
    int16_t original[sizeof(pcm) / sizeof(*pcm)];
    memcpy(original, pcm, sizeof(pcm));
    const aether_audio_eq_t off = {.bands_db = {6, 5, 4, 3, 2}, .gain_db = 6};
    const aether_audio_eq_t flat = {.enabled = true};
    music_eq_process(&state, &off, pcm, 4);
    assert(memcmp(pcm, original, sizeof(pcm)) == 0);
    music_eq_process(&state, &flat, pcm, 4);
    assert(memcmp(pcm, original, sizeof(pcm)) == 0);
}

static float band_power_ratio(size_t band, int8_t gain_db, float tone_hz)
{
    music_eq_t state = {0};
    aether_audio_eq_t eq = {.enabled = true};
    eq.bands_db[band] = gain_db;
    const float phase_step = (6.28318530718f / 48000.0f) * tone_hz;
    float input_power = 0.0f, output_power = 0.0f;
    int16_t pcm[480 * 2];
    for (unsigned block = 0; block < 8; ++block) {
        float block_power = 0.0f;
        for (size_t i = 0; i < 480; ++i) {
            const float phase = (float)(block * 480 + i) * phase_step;
            pcm[i * 2] = (int16_t)(2000.0f * sinf(phase));
            pcm[i * 2 + 1] = 0;
            block_power += (float)pcm[i * 2] * pcm[i * 2];
        }
        music_eq_process(&state, &eq, pcm, 480);
        if (block < 4) continue;
        input_power += block_power;
        for (size_t i = 0; i < 480; ++i) {
            output_power += (float)pcm[i * 2] * pcm[i * 2];
            assert(pcm[i * 2 + 1] == 0);
        }
    }
    return output_power / input_power;
}

static void test_response(void)
{
    const float centers[] = {100.0f, 300.0f, 1000.0f, 3000.0f, 10000.0f};
    for (size_t band = 0; band < AETHER_AUDIO_EQ_BANDS; ++band) {
        const float boost = band_power_ratio(band, 6, centers[band]);
        assert(boost > 3.80f && boost < 4.20f);
    }
    const float cut = band_power_ratio(2, -6, 1000.0f);
    assert(cut > 0.23f && cut < 0.27f);
    const float off_band = band_power_ratio(0, 6, 10000.0f);
    assert(off_band > 0.98f && off_band < 1.02f);
}

static void test_gain_and_limiting(void)
{
    music_eq_t state = {0};
    const aether_audio_eq_t gain = {.enabled = true, .gain_db = 6};
    int16_t pcm[] = {1000, -1000, 32760, -16380, -32768, 16384, 0, 0};
    music_eq_process(&state, &gain, pcm, 4);
    assert(pcm[0] >= 1994 && pcm[0] <= 1996 && pcm[1] == -pcm[0]);
    assert(pcm[2] > 31000 && pcm[2] < INT16_MAX && pcm[3] < 0);
    assert(pcm[2] + 2 * pcm[3] >= -1 && pcm[2] + 2 * pcm[3] <= 1);
    assert(pcm[4] < -31000 && pcm[4] > INT16_MIN && pcm[5] > 0);
    assert(pcm[4] + 2 * pcm[5] >= -1 && pcm[4] + 2 * pcm[5] <= 1);
    assert(pcm[6] == 0 && pcm[7] == 0);
}

static void test_transition_and_reset(void)
{
    music_eq_t state = {0};
    const aether_audio_eq_t off = {0};
    const aether_audio_eq_t gain = {.enabled = true, .gain_db = 6};
    int16_t first[] = {1000, -1000};
    music_eq_process(&state, &off, first, 1);
    int16_t fade[480 * 2];
    for (size_t i = 0; i < 480; ++i) {
        fade[i * 2] = 1000;
        fade[i * 2 + 1] = -1000;
    }
    music_eq_process(&state, &gain, fade, 480);
    assert(fade[0] > 1000 && fade[0] < 1010 && fade[958] >= 1994);
    for (size_t i = 1; i < 480; ++i) {
        const int delta = fade[i * 2] - fade[(i - 1) * 2];
        assert(delta >= 0 && delta <= 4);
        assert(fade[i * 2] == -fade[i * 2 + 1]);
        fade[(i - 1) * 2] = 1000;
        fade[(i - 1) * 2 + 1] = -1000;
    }
    fade[958] = 1000;
    fade[959] = -1000;
    music_eq_process(&state, &off, fade, 480);
    assert(fade[0] > 1985 && fade[0] < 1996);
    assert(fade[958] == 1000 && fade[959] == -1000);

    const aether_audio_eq_t bass = {.enabled = true, .bands_db = {6}};
    music_eq_reset(&state);
    int16_t impulse[] = {20000, 0, 0, 0};
    music_eq_process(&state, &bass, impulse, 2);
    assert(impulse[2] != 0 && impulse[3] == 0);
    music_eq_reset(&state);
    int16_t silence[] = {0, 0, 0, 0};
    music_eq_process(&state, &bass, silence, 2);
    assert(silence[0] == 0 && silence[1] == 0 && silence[2] == 0 && silence[3] == 0);
}

int main(void)
{
    test_exact_bypass();
    test_response();
    test_gain_and_limiting();
    test_transition_and_reset();
    puts("PASS music EQ bypass, all five band responses, stereo isolation, linked limiter, live transition and reset");
    return 0;
}
