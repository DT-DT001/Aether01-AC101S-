#include "vendor/minimp4.h"
#include <stdio.h>

static unsigned long long chunk_steps;
#include "chunk_cache_selected.inc"

int main(void)
{
    enum { CHUNKS=2488, SAMPLES=7773 };
    MP4D_demux_t mp4={0};
    MP4D_track_t track={0};
    MP4D_sample_to_chunk_t groups[CHUNKS];
    MP4D_file_offset_t chunks[CHUNKS],expected[SAMPLES];
    unsigned sizes[SAMPLES],times[SAMPLES],durations[SAMPLES];
    unsigned sample=0,group_count=0,previous=0;
    MP4D_file_offset_t offset=70000;
    for(unsigned chunk=0;chunk<CHUNKS;++chunk) {
        /* Original synthetic3/4-AU grouping matching ordinary15fps/48k MP4
         * interleaving geometry. No recording data or user bytes are copied. */
        unsigned count=chunk==0 ? 1 : chunk%8==1 ? 4 : 3;
        if(count!=previous) groups[group_count++]=(MP4D_sample_to_chunk_t){chunk+1,count};
        previous=count; chunks[chunk]=offset;
        for(unsigned i=0;i<count;++i) {
            assert(sample<SAMPLES);
            sizes[sample]=80+sample%400; times[sample]=sample*1024; durations[sample]=1024;
            expected[sample]=offset; offset+=sizes[sample++];
        }
        offset+=10000+chunk%129; /* Independent video packets between audio chunks. */
    }
    assert(sample==SAMPLES && group_count==623);
    track.sample_count=SAMPLES; track.chunk_count=CHUNKS; track.chunk_offset=chunks;
    track.sample_to_chunk_count=group_count; track.sample_to_chunk=groups;
    track.entry_size=sizes; track.timestamp=times; track.duration=durations; track.timestamp_count=SAMPLES;
    mp4.track=&track; mp4.track_count=1;
    unsigned bytes,stamp,duration;
    for(unsigned i=6000;i<7600;++i) {
        assert(MP4D_frame_offset(&mp4,0,i,&bytes,&stamp,&duration)==expected[i]);
        assert(bytes==sizes[i] && stamp==times[i] && duration==1024);
    }
    const unsigned long long late_steps=chunk_steps;
#if EXPECT_FAST
    assert(late_steps<5000);
#else
    assert(late_steps>1000000);
#endif
    unsigned seed=0x934bee31;
    for(unsigned i=0;i<2000;++i) {
        seed=seed*1664525U+1013904223U;
        unsigned index=seed%SAMPLES;
        assert(MP4D_frame_offset(&mp4,0,index,&bytes,&stamp,&duration)==expected[index]);
        assert(bytes==sizes[index] && stamp==times[index]);
    }
    for(unsigned i=SAMPLES-4;i<SAMPLES;++i)
        assert(MP4D_frame_offset(&mp4,0,i,&bytes,NULL,NULL)==expected[i]);
    for(unsigned i=SAMPLES-1;i>=SAMPLES-4;--i)
        assert(MP4D_frame_offset(&mp4,0,i,&bytes,NULL,NULL)==expected[i]);
    printf("PASS actual minimp4%s: late1600 AAC samples=%llu chunk visits;623 groups/2488 chunks/7773 samples, random/backward/final-chunk offsets exact\n",
           EXPECT_FAST ? " current-chunk cache" : " isolated old-cache control",late_steps);
    return 0;
}
