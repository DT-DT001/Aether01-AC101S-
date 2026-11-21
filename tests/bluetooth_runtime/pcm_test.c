#include "bt_pcm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t frames; uint32_t rate; bool fail; } output_t;
static int16_t source(uint64_t frame, unsigned channel)
{ return (int16_t)((int64_t)((frame * (channel ? 617U : 251U) + channel * 300U) % 60001U) - 30000); }
static int check(const int16_t *pcm, size_t frames, void *context)
{
    output_t *output = context;
    if (output->fail) return 123;
    assert(frames && frames <= 128);
    for (size_t i = 0; i < frames; ++i) {
        const uint64_t numerator = (output->frames + i) * 48000U;
        const uint64_t index = numerator / output->rate;
        const int64_t fraction = (int64_t)(numerator % output->rate);
        for (unsigned ch = 0; ch < 2; ++ch) {
            int16_t a = source(index,ch), b = source(index+1,ch);
            int16_t expected = (int16_t)(a + ((int64_t)b-a)*fraction/output->rate);
            assert(pcm[i*2+ch] == expected);
        }
    }
    output->frames += frames;
    return 0;
}
int main(void)
{
    for (uint32_t rate = 44100; rate <= 48000; rate += 3900) {
        bt_pcm_t state; assert(bt_pcm_reset(&state,rate));
        output_t output = {.rate = rate};
        uint64_t source_frames = 0;
        /* Deliberately unaligned, irregular caller chunks across 100k frames. */
        uint8_t bytes[4*1031+1];
        while (source_frames < 100000) {
            size_t count = (source_frames * 71U + 1U) % 1031U + 1U;
            if (count > 100000-source_frames) count = 100000-source_frames;
            for (size_t i=0;i<count;++i) for(unsigned ch=0;ch<2;++ch) {
                int16_t value=source(source_frames+i,ch); memcpy(bytes+1+i*4+ch*2,&value,2);
            }
            assert(bt_pcm_write(&state,bytes+1,count,check,&output)==0);
            source_frames += count;
            const uint64_t expected = rate == 48000 ? source_frames : ((source_frames-1U)*rate+47999U)/48000U;
            assert(output.frames == expected);
        }
        assert(state.input_frames == 100000);
        assert(bt_pcm_reset(&state,rate)); output.frames = 0;
        int16_t pair[4] = {source(0,0),source(0,1),source(1,0),source(1,1)};
        output.fail = true; assert(bt_pcm_write(&state,pair,2,check,&output)==123);
    }
    bt_pcm_t state;
    assert(!bt_pcm_reset(&state,32000)); assert(!bt_pcm_reset(NULL,44100));
    puts("Bluetooth PCM: unaligned 48k/44.1k cross-chunk exact phase, 200k frames, callback faults passed");
}
