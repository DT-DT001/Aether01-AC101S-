#include "mp3_stream.h"
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Keep upstream files unmodified; decoder, read-ahead and seek index live in
 * PSRAM. A corrupt file cannot request an unbounded single allocation. */
static void *mp3_alloc(size_t bytes)
{
    return bytes <= 4U * 1024U * 1024U ?
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
}
static void *mp3_realloc(void *ptr, size_t bytes)
{
    return bytes <= 4U * 1024U * 1024U ?
        heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
}

#define MINIMP3_NO_STDIO
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_SCAN_LIMIT (256 * 1024)
/* Load declarations first, then override only the implementation's IO size.
 * Upstream remains byte-for-byte unchanged. A read callback must still satisfy
 * the full request: returning 16KiB early would be interpreted as EOF.
 * 32KiB exceeds upstream's mandatory 16KiB/10-frame search window while
 * avoiding the old synchronous 112--128KiB playback refill burst. */
#include "vendor/minimp3/minimp3_ex.h"
#ifndef AETHER_MP3_IO_BYTES
#define AETHER_MP3_IO_BYTES (32U*1024U)
#endif
#undef MINIMP3_IO_SIZE
#define MINIMP3_IO_SIZE AETHER_MP3_IO_BYTES
_Static_assert(MINIMP3_IO_SIZE>MINIMP3_BUF_SIZE,"MP3 IO buffer must exceed frame search window");
#define MINIMP3_IMPLEMENTATION
#define malloc mp3_alloc
#define realloc mp3_realloc
#include "vendor/minimp3/minimp3_ex.h"
#undef malloc
#undef realloc

struct aether_mp3_stream {
    mp3dec_ex_t decoder;
    mp3dec_io_t io;
    FILE *file;
    aether_mp3_cancel_cb_t cancel;
    void *context;
    bool io_error, canceled, ready;
    aether_mp3_metrics_t metrics;
    uint64_t source_base;
    uint64_t phase, current_frame, total_frames;
    int16_t before[2], after[2];
    int16_t pcm[2048];
    size_t buffered, consumed;
};

static bool canceled(aether_mp3_stream_t *s)
{
    if (s->cancel && s->cancel(s->context)) s->canceled = true;
    return s->canceled;
}

static size_t file_read(void *buffer, size_t bytes, void *context)
{
    aether_mp3_stream_t *s = context;
    const int64_t started=esp_timer_get_time();
    size_t done = 0;
    while (done < bytes && !canceled(s)) {
        size_t amount = bytes - done;
        if (amount > 16384) amount = 16384;
        const size_t got = fread((uint8_t *)buffer + done, 1, amount, s->file);
        done += got;
        if (got != amount) { if (ferror(s->file)) s->io_error = true; break; }
    }
    const uint32_t elapsed=(uint32_t)(esp_timer_get_time()-started);
    s->metrics.read_total_us+=elapsed; ++s->metrics.read_calls;
    if(done>s->metrics.refill_max_bytes) s->metrics.refill_max_bytes=(uint32_t)done;
    if(elapsed>s->metrics.refill_max_us) s->metrics.refill_max_us=elapsed;
    return done;
}

void aether_mp3_get_metrics(const aether_mp3_stream_t *s,aether_mp3_metrics_t *metrics)
{
    if(!metrics) return;
    if(!s) { memset(metrics,0,sizeof(*metrics)); return; }
    *metrics=s->metrics; metrics->io_buffer_bytes=(uint32_t)s->decoder.file.size;
}
void aether_mp3_clear_metrics(aether_mp3_stream_t *s)
{ if(s) memset(&s->metrics,0,sizeof(s->metrics)); }

static int file_seek(uint64_t position, void *context)
{
    aether_mp3_stream_t *s = context;
    if (canceled(s) || s->source_base > LONG_MAX ||
        position > (uint64_t)LONG_MAX - s->source_base ||
        fseek(s->file, (long)(s->source_base + position), SEEK_SET)) {
        s->io_error = true;
        return -1;
    }
    return 0;
}

static esp_err_t decoder_error(aether_mp3_stream_t *s, int code)
{
    if (s->canceled) return ESP_ERR_INVALID_STATE;
    if (s->io_error || code == MP3D_E_IOERROR) return ESP_FAIL;
    if (code == MP3D_E_MEMORY) return ESP_ERR_NO_MEM;
    return code ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}

esp_err_t aether_mp3_open(FILE *file, aether_mp3_cancel_cb_t cancel, void *context,
                          aether_mp3_stream_t **out)
{
    if (!file || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    aether_mp3_stream_t *s = mp3_alloc(sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    memset(s, 0, sizeof(*s));
    s->file = file; s->cancel = cancel; s->context = context;
    s->io = (mp3dec_io_t){.read = file_read, .read_data = s, .seek = file_seek, .seek_data = s};
    /* Xing/LAME duration is used when present. Otherwise the worker scans the
     * stream; reads/seeks remain cancelable on stop or card removal. A first
     * nonzero seek may build the sample index, also on this worker. */
    int rc = mp3dec_ex_open_cb(&s->decoder, &s->io, MP3D_SEEK_TO_SAMPLE);
    esp_err_t err = decoder_error(s, rc);
    if (err == ESP_OK && s->decoder.vbr_tag_found && !s->decoder.samples &&
        s->decoder.start_offset && s->decoder.start_offset <= LONG_MAX) {
        /* An unfinalized Xing/Info tag can advertise zero audio frames even
         * though valid audio follows. Upstream trusts this advisory count and
         * stops its scan. Skip that tag frame and scan the real stream once;
         * all decoder offsets (including later seeks) remain relative to this
         * base, while the FILE callbacks translate to original source bytes.
         * Do not retry read errors, cancellation or arbitrary invalid files. */
        s->source_base = s->decoder.start_offset;
        ESP_LOGW("mp3_stream", "zero-count VBR tag; scanning audio after byte %" PRIu64,
                 s->source_base);
        mp3dec_ex_close(&s->decoder);
        rc = mp3dec_ex_open_cb(&s->decoder, &s->io, MP3D_SEEK_TO_SAMPLE);
        err = decoder_error(s, rc);
    }
    if (err == ESP_OK && (s->decoder.info.hz < 8000 || s->decoder.info.hz > 48000 ||
        (s->decoder.info.channels != 1 && s->decoder.info.channels != 2) ||
        !s->decoder.samples)) err = ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        ESP_LOGW("mp3_stream", "open rejected: err=0x%x rc=%d hz=%d ch=%d layer=%d "
                 "samples=%" PRIu64 " detected=%" PRIu64 " vbr=%d index=%zu start=%" PRIu64
                 " end=%" PRIu64 " base=%" PRIu64,
                 (unsigned)err, rc, s->decoder.info.hz, s->decoder.info.channels,
                 s->decoder.info.layer, s->decoder.samples, s->decoder.detected_samples,
                 s->decoder.vbr_tag_found,
                 s->decoder.index.num_frames, s->decoder.start_offset,
                 s->decoder.end_offset, s->source_base);
        aether_mp3_close(s);
        return err;
    }
    s->total_frames = s->decoder.samples / s->decoder.info.channels;
    *out = s;
    return ESP_OK;
}

void aether_mp3_close(aether_mp3_stream_t *s)
{
    if (s) { mp3dec_ex_close(&s->decoder); free(s); }
}
uint32_t aether_mp3_rate(const aether_mp3_stream_t *s) { return (uint32_t)s->decoder.info.hz; }
uint16_t aether_mp3_channels(const aether_mp3_stream_t *s) { return (uint16_t)s->decoder.info.channels; }
uint32_t aether_mp3_duration_ms(const aether_mp3_stream_t *s)
{ return (uint32_t)(s->total_frames * 1000 / (unsigned)s->decoder.info.hz); }
uint32_t aether_mp3_position_ms(const aether_mp3_stream_t *s)
{ return (uint32_t)(s->phase / 48 / (unsigned)s->decoder.info.hz); }

esp_err_t aether_mp3_seek(aether_mp3_stream_t *s, uint32_t position_ms)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    uint64_t frame = (uint64_t)position_ms * s->decoder.info.hz / 1000;
    if (frame > s->total_frames) frame = s->total_frames;
    const esp_err_t err = decoder_error(s, mp3dec_ex_seek(&s->decoder, frame * s->decoder.info.channels));
    if (err != ESP_OK) return err;
    s->phase = frame * 48000;
    s->current_frame = frame;
    s->ready = false;
    s->buffered = s->consumed = 0;
    return ESP_OK;
}

static bool next_sample(aether_mp3_stream_t *s, int16_t sample[2])
{
    if (s->consumed == s->buffered) {
        s->buffered = mp3dec_ex_read(&s->decoder, s->pcm, sizeof(s->pcm) / sizeof(s->pcm[0]));
        s->consumed = 0;
    }
    if (s->buffered - s->consumed < (size_t)s->decoder.info.channels) return false;
    sample[0] = s->pcm[s->consumed++];
    sample[1] = s->decoder.info.channels == 1 ? sample[0] : s->pcm[s->consumed++];
    return true;
}

size_t aether_mp3_read(aether_mp3_stream_t *s, int16_t *output, size_t frames, esp_err_t *err)
{
    *err = ESP_OK;
    size_t done = 0;
    while (done < frames && s->phase / 48000 < s->total_frames) {
        if (canceled(s)) { *err = ESP_ERR_INVALID_STATE; break; }
        if (!s->ready) {
            if (!next_sample(s, s->before)) goto truncated;
            if (s->current_frame + 1 < s->total_frames) {
                if (!next_sample(s, s->after)) goto truncated;
            } else memcpy(s->after, s->before, sizeof(s->after));
            s->ready = true;
        }
        const uint64_t target = s->phase / 48000;
        while (s->current_frame < target) {
            memcpy(s->before, s->after, sizeof(s->before));
            ++s->current_frame;
            if (s->current_frame + 1 < s->total_frames) {
                if (!next_sample(s, s->after)) goto truncated;
            } else memcpy(s->after, s->before, sizeof(s->after));
        }
        const int32_t fraction = (int32_t)(s->phase % 48000);
        for (unsigned ch = 0; ch < 2; ++ch) output[2 * done + ch] = (int16_t)(s->before[ch] +
            ((int64_t)s->after[ch] - s->before[ch]) * fraction / 48000);
        ++done;
        s->phase += s->decoder.info.hz;
    }
    return done;
truncated:
    *err = decoder_error(s, s->decoder.last_error);
    if (*err == ESP_OK) *err = ESP_ERR_INVALID_SIZE;
    return done;
}
