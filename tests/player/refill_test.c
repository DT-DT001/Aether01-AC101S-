/* Real minimp3 and an original standards-valid long silent stream.
 * A deterministic fread model costs 0.5us/byte (2MB/s), never short-reads a
 * request prematurely. This is a latency model, not a hardware benchmark. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "mp3_stream.h"
static uint64_t modeled_us;
static size_t max_fread;
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __real___fread_chk(void *,size_t,size_t,size_t,FILE *);
static size_t account_read(size_t got,size_t size)
{
    const size_t bytes=got*size;
    modeled_us+=bytes/2;
    if(bytes>max_fread) max_fread=bytes;
    return got;
}
size_t __wrap_fread(void *p,size_t size,size_t n,FILE *f)
{ return account_read(__real_fread(p,size,n,f),size); }
/* Ubuntu's fortified stdio may rewrite fread to this entry point. Keep the
 * real bounds check and ASan active; intercept both possible call symbols. */
size_t __wrap___fread_chk(void *p,size_t bounds,size_t size,size_t n,FILE *f)
{ return account_read(__real___fread_chk(p,bounds,size,n,f),size); }
int64_t esp_timer_get_time(void) { return (int64_t)modeled_us; }
int main(int argc,char **argv)
{
    assert(argc==2);const unsigned expected=(unsigned)strtoul(argv[1],NULL,10);
    FILE *f=tmpfile();assert(f);
    uint8_t frame[417]={0xff,0xfb,0x90,0};
    for(unsigned i=0;i<512;++i) assert(fwrite(frame,1,sizeof frame,f)==sizeof frame);
    rewind(f);
    aether_mp3_stream_t *s=NULL;assert(aether_mp3_open(f,NULL,NULL,&s)==ESP_OK);
    aether_mp3_clear_metrics(s);max_fread=0;
    aether_mp3_metrics_t m;aether_mp3_get_metrics(s,&m);assert(m.io_buffer_bytes==expected);
    const uint64_t started=modeled_us;
    size_t total=0,n;esp_err_t error;
    do {
        int16_t pcm[2048];n=aether_mp3_read(s,pcm,1024,&error);assert(error==ESP_OK);
        for(size_t i=0;i<n*2;++i) assert(!pcm[i]);
        total+=n;
    } while(n);
    const uint64_t source=512U*1152U;
    assert(total==(source*48000+44100-1)/44100);
    aether_mp3_get_metrics(s,&m);
    assert(m.read_total_us==modeled_us-started && m.read_calls>1);
    assert(m.refill_max_bytes==expected && m.refill_max_us==expected/2);
    assert(max_fread==16384); /* Splitting calls did not shorten the whole burst. */
    if(expected==131072) assert(m.refill_max_us>30000);
    else assert(expected==32768 && m.refill_max_us<30000);
    printf("PASS real minimp3 IO=%u full_frames=%zu max_sync_refill=%u bytes / %u modeled_us, fread_chunk<=%zu\n",
        expected,total,m.refill_max_bytes,m.refill_max_us,max_fread);
    assert(aether_mp3_seek(s,1000)==ESP_OK);
    assert(aether_mp3_position_ms(s)==1000);
    size_t seek_frames=0;
    do {
        int16_t pcm[2048]; n=aether_mp3_read(s,pcm,1024,&error); assert(error==ESP_OK);
        for(size_t i=0;i<n*2;++i) assert(!pcm[i]);
        seek_frames+=n;
    } while(n);
    assert(seek_frames==total-48000);
    printf("PASS IO=%u indexed seek=1000ms remaining_frames=%zu same silent PCM through EOF\n",expected,seek_frames);
    aether_mp3_close(s);fclose(f);return 0;
}
