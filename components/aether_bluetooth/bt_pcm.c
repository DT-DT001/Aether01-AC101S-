#include "bt_pcm.h"
#include <limits.h>
#include <string.h>

bool bt_pcm_reset(bt_pcm_t *state, uint32_t rate)
{
    if (!state || (rate != 44100 && rate != 48000)) return false;
    memset(state, 0, sizeof(*state));
    state->rate = rate;
    return true;
}

int bt_pcm_write(bt_pcm_t *state, const void *data, size_t frames,
                  bt_pcm_output_t output, void *context)
{
    if (!state || !data || !output || (state->rate != 44100 && state->rate != 48000)) return -1;
    const uint8_t *bytes = data;
    if (state->rate == 48000) {
        /* The driver may supply an unaligned buffer: copy bounded chunks. */
        int16_t block[128 * 2];
        while (frames) {
            size_t count = frames > 128 ? 128 : frames;
            memcpy(block, bytes, count * 4);
            int error = output(block, count, context);
            if (error) return error;
            bytes += count * 4; frames -= count;
            state->input_frames += count;
        }
        return 0;
    }
    int16_t block[128 * 2];
    size_t pending = 0;
    for (size_t i = 0; i < frames; ++i) {
        int16_t current[2];
        memcpy(current, bytes + i * 4, sizeof(current));
        if (state->input_frames >= UINT64_MAX / state->rate) return -1;
        const uint64_t end = state->input_frames * state->rate;
        if (state->have_previous) {
            const uint64_t start = end - state->rate;
            while (state->phase < end) {
                const int64_t fraction = (int64_t)(state->phase - start);
                for (unsigned ch = 0; ch < 2; ++ch)
                    block[pending * 2 + ch] = (int16_t)(state->previous[ch] +
                        ((int64_t)current[ch] - state->previous[ch]) * fraction / state->rate);
                ++pending;
                state->phase += 48000;
                if (pending == 128) {
                    int error = output(block, pending, context);
                    if (error) return error;
                    pending = 0;
                }
            }
        }
        memcpy(state->previous, current, sizeof(current));
        state->have_previous = true;
        ++state->input_frames;
    }
    return pending ? output(block, pending, context) : 0;
}
