/* Optional independent decoder check, not linked into production firmware.
 * Obtain MiniAAC's 0BSD maac.h separately; pass its directory as -I. */
#define MAAC_IMPLEMENTATION
#include "maac.h"
#include "fixture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    for (unsigned channels = 1; channels <= 2; ++channels) {
        maac_raw *decoder = calloc(1, sizeof(*decoder));
        maac_channel *pcm = calloc(2, sizeof(*pcm));
        assert(decoder && pcm);
        maac_raw_init(decoder);
        for (unsigned ch = 0; ch < channels; ++ch) maac_channel_init(pcm + ch);
        const uint8_t *asc = channels == 1 ? AAC_FIXTURE_MONO_ASC : AAC_FIXTURE_STEREO_ASC;
        const uint8_t *au = channels == 1 ? AAC_FIXTURE_MONO_AU : AAC_FIXTURE_STEREO_AU;
        const size_t length = channels == 1 ? sizeof(AAC_FIXTURE_MONO_AU) : sizeof(AAC_FIXTURE_STEREO_AU);
        assert(maac_raw_config(decoder, asc, 2) == MAAC_OK);
        decoder->out_channels = pcm;
        decoder->num_out_channels = channels;
        for (unsigned frame = 0; frame < AAC_FIXTURE_AUS; ++frame) {
            maac_bitreader reader;
            maac_bitreader_init(&reader);
            reader.data = au;
            reader.len = length;
            assert(maac_raw_decode(decoder, &reader) == MAAC_OK);
            for (unsigned ch = 0; ch < channels; ++ch) {
                assert(pcm[ch].n_samples == 1024);
                for (unsigned i = 0; i < 1024; ++i) assert(pcm[ch].samples[i] == 0);
            }
        }
        free(pcm);
        free(decoder);
    }
    puts("Original mono/stereo AAC-LC fixture: real MiniAAC decoder, 48 AUs each, all 1024 samples zero");
    return 0;
}
