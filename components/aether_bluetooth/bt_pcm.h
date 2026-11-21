#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Application-task converter only. One owner; no work/allocation in A2DP pull. */
typedef int (*bt_pcm_output_t)(const int16_t *stereo, size_t frames, void *context);
typedef struct {
    uint32_t rate;
    uint64_t input_frames, phase;
    int16_t previous[2];
    bool have_previous;
} bt_pcm_t;
bool bt_pcm_reset(bt_pcm_t *state, uint32_t peer_rate);
int bt_pcm_write(bt_pcm_t *state, const void *pcm, size_t frames,
                  bt_pcm_output_t output, void *context);
