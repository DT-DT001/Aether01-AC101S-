#pragma once

#include "aether_audio_player.h"

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1[2], z2[2];
} music_eq_section_t;

typedef struct {
    aether_audio_eq_t config;
    music_eq_section_t sections[AETHER_AUDIO_EQ_BANDS];
    size_t section_count;
    float gain;
} music_eq_chain_t;

/* Owned by the player worker. Initialize to zero or reset after a seek. */
typedef struct {
    music_eq_chain_t current;
    music_eq_chain_t next;
    unsigned fade_remaining;
    bool initialized;
} music_eq_t;

void music_eq_reset(music_eq_t *state);
void music_eq_process(music_eq_t *state, const aether_audio_eq_t *config, int16_t *pcm, size_t frames);
