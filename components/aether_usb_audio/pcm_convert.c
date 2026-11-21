#include "pcm_convert.h"
#include <string.h>
bool aether_pcm_converter_init(aether_pcm_converter_t *s, uint32_t ir,
    uint8_t ic, uint32_t orate, uint8_t oc)
{
    if (!s || !ir || !orate || (ic != 1 && ic != 2) || (oc != 1 && oc != 2)) return false;
    memset(s,0,sizeof(*s)); s->input_rate=ir; s->output_rate=orate;
    s->input_channels=ic; s->output_channels=oc; return true;
}
size_t aether_pcm_convert(aether_pcm_converter_t *s, const int16_t *input,
    size_t frames, int16_t *output, size_t capacity, size_t *consumed)
{
    if (consumed) *consumed=0;
    if (!s || !input || !output || !consumed || !s->input_rate || !s->output_rate) return 0;
    size_t n=0, used=0;
    for (;used<frames;++used) {
        const int16_t left=input[used*s->input_channels];
        const int16_t right=s->input_channels==2 ? input[used*2+1] : left;
        if (!s->have_previous) {
            s->previous[0]=left; s->previous[1]=right; s->have_previous=true;
            continue;
        }
        const uint32_t needed=s->phase<s->output_rate ?
            (s->output_rate-s->phase+s->input_rate-1)/s->input_rate : 0;
        if (needed>capacity-n) break; /* Never consume a half-converted frame. */
        while (s->phase<s->output_rate) {
            const int32_t l=s->previous[0]+(int32_t)(((int64_t)left-s->previous[0])*s->phase/s->output_rate);
            const int32_t rr=s->previous[1]+(int32_t)(((int64_t)right-s->previous[1])*s->phase/s->output_rate);
            if (s->output_channels==2) { output[n*2]=(int16_t)l; output[n*2+1]=(int16_t)rr; }
            else output[n]=(int16_t)((l+rr)/2);
            ++n; s->phase+=s->input_rate;
        }
        s->phase-=s->output_rate;
        s->previous[0]=left; s->previous[1]=right;
    }
    *consumed=used; return n;
}
