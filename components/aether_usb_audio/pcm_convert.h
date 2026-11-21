#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct {
    uint32_t input_rate, output_rate, phase;
    uint8_t input_channels, output_channels;
    bool have_previous;
    int16_t previous[2];
} aether_pcm_converter_t;
bool aether_pcm_converter_init(aether_pcm_converter_t *s, uint32_t input_rate,
    uint8_t input_channels, uint32_t output_rate, uint8_t output_channels);
size_t aether_pcm_convert(aether_pcm_converter_t *s, const int16_t *input,
    size_t frames, int16_t *output, size_t output_capacity_frames, size_t *consumed);
