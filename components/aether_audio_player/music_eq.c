#include "music_eq.h"

#include <math.h>
#include <string.h>

enum { FADE_FRAMES = 384 }; /* 8ms at the player's 48 kHz output rate. */

static const float center_hz[AETHER_AUDIO_EQ_BANDS] = {100.0f, 300.0f, 1000.0f, 3000.0f, 10000.0f};

static aether_audio_eq_t normalize(const aether_audio_eq_t *config)
{
    aether_audio_eq_t normalized = {0};
    if (!config->enabled) return normalized;
    normalized.gain_db = config->gain_db > AETHER_AUDIO_EQ_MAX_GAIN_DB ? AETHER_AUDIO_EQ_MAX_GAIN_DB : config->gain_db;
    normalized.enabled = normalized.gain_db != 0;
    for (size_t i = 0; i < AETHER_AUDIO_EQ_BANDS; ++i) {
        int band = config->bands_db[i];
        if (band < -AETHER_AUDIO_EQ_MAX_DB) band = -AETHER_AUDIO_EQ_MAX_DB;
        if (band > AETHER_AUDIO_EQ_MAX_DB) band = AETHER_AUDIO_EQ_MAX_DB;
        normalized.bands_db[i] = (int8_t)band;
        normalized.enabled |= band != 0;
    }
    return normalized;
}

static bool same_config(const aether_audio_eq_t *a, const aether_audio_eq_t *b)
{
    return a->enabled == b->enabled && a->gain_db == b->gain_db &&
        memcmp(a->bands_db, b->bands_db, sizeof(a->bands_db)) == 0;
}

static void configure_chain(music_eq_chain_t *chain, const aether_audio_eq_t *config)
{
    memset(chain, 0, sizeof(*chain));
    chain->config = *config;
    if (!config->enabled) return;
    chain->gain = powf(10.0f, config->gain_db / 20.0f);
    for (size_t i = 0; i < AETHER_AUDIO_EQ_BANDS; ++i) {
        if (!config->bands_db[i]) continue;
        const float angle = (6.28318530718f / 48000.0f) * center_hz[i];
        const float amplitude = powf(10.0f, config->bands_db[i] / 40.0f);
        const float alpha = sinf(angle) / (2.0f * 1.1f);
        const float inv_a0 = 1.0f / (1.0f + alpha / amplitude);
        music_eq_section_t *section = &chain->sections[chain->section_count++];
        /* Normalized RBJ peaking biquad at the fixed post-resampler rate. */
        section->b0 = (1.0f + alpha * amplitude) * inv_a0;
        section->b1 = -2.0f * cosf(angle) * inv_a0;
        section->b2 = (1.0f - alpha * amplitude) * inv_a0;
        section->a1 = section->b1;
        section->a2 = (1.0f - alpha / amplitude) * inv_a0;
    }
}

static void process_chain(music_eq_chain_t *chain, float samples[2])
{
    if (!chain->config.enabled) return;
    samples[0] *= chain->gain;
    samples[1] *= chain->gain;
    for (size_t i = 0; i < chain->section_count; ++i) {
        music_eq_section_t *section = &chain->sections[i];
        for (size_t channel = 0; channel < 2; ++channel) {
            const float in = samples[channel];
            const float out = section->b0 * in + section->z1[channel];
            section->z1[channel] = section->b1 * in - section->a1 * out + section->z2[channel];
            section->z2[channel] = section->b2 * in - section->a2 * out;
            samples[channel] = out;
        }
    }

    /* Compress only peaks above the knee. One gain per stereo frame
     * preserves balance when either channel crosses the threshold. */
    const float knee = 0.88f * INT16_MAX;
    const float headroom = INT16_MAX - knee;
    const float peak = fmaxf(fabsf(samples[0]), fabsf(samples[1]));
    if (peak > knee) {
        const float excess = peak - knee;
        const float limited_peak = knee + excess / (1.0f + excess / headroom);
        const float gain = limited_peak / peak;
        samples[0] *= gain;
        samples[1] *= gain;
    }
}

static int16_t pcm_sample(float sample)
{
    if (sample >= INT16_MAX) return INT16_MAX;
    if (sample <= INT16_MIN) return INT16_MIN;
    return (int16_t)(sample + (sample < 0.0f ? -0.5f : 0.5f));
}

void music_eq_reset(music_eq_t *state)
{
    memset(state, 0, sizeof(*state));
}

void music_eq_process(music_eq_t *state, const aether_audio_eq_t *config, int16_t *pcm, size_t frames)
{
    const aether_audio_eq_t requested = normalize(config);
    if (!state->initialized) {
        configure_chain(&state->current, &requested);
        state->initialized = true;
    } else if (!state->fade_remaining && !same_config(&state->current.config, &requested)) {
        configure_chain(&state->next, &requested);
        state->fade_remaining = FADE_FRAMES;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        if (!state->fade_remaining && !state->current.config.enabled) break;
        int16_t *input = &pcm[frame * 2];
        float current[2] = {input[0], input[1]};
        process_chain(&state->current, current);
        if (state->fade_remaining) {
            float next[2] = {input[0], input[1]};
            process_chain(&state->next, next);
            const float mix = (float)(FADE_FRAMES - --state->fade_remaining) / FADE_FRAMES;
            current[0] += (next[0] - current[0]) * mix;
            current[1] += (next[1] - current[1]) * mix;
            if (!state->fade_remaining) state->current = state->next;
        }
        input[0] = pcm_sample(current[0]);
        input[1] = pcm_sample(current[1]);
    }
}
