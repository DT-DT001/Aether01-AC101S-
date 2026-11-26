#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    AETHER_DENOISE_RATE = 16000,
    AETHER_DENOISE_SAMPLES = 256,
    AETHER_DENOISE_RATIO = 3,
    AETHER_DENOISE_CAPTURE_FRAMES = AETHER_DENOISE_SAMPLES * AETHER_DENOISE_RATIO,
};

typedef struct aether_recorder_denoise aether_recorder_denoise_t;

bool aether_recorder_denoise_supported(void);
aether_recorder_denoise_t *aether_recorder_denoise_open(void);
void aether_recorder_denoise_close(aether_recorder_denoise_t *denoise);

/* The AC101S transport supplies packed 48 kHz stereo. The returned 16 kHz
 * mono block is owned by denoise and remains valid until the next call. */
const int16_t *aether_recorder_denoise_process(aether_recorder_denoise_t *denoise,
                                              const int16_t *stereo);
