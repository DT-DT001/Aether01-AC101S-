#include "aether_mp4_audio.h"
#include "mp4_demux.h"
#include "mp4_aac.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

/* 8kHz LC produces at most 6144 48kHz frames per 1024-frame AU. Leave room
 * for the previous interpolation endpoint and the final resampler sample. */
#define QUEUE_FRAMES 8192U
struct aether_mp4_audio {
    FILE *file;
    bool (*cancel)(void *);
    void *context;
    mp4_demux_t *demux;
    mp4_aac_handle_t decoder;
    mp4_media_info_t media;
    aether_mp4_audio_info_t info;
    uint8_t *packet;
    int16_t *queue;
    size_t queued,queue_offset;
    uint32_t index;
    uint64_t trim,cursor,total_frames;
    bool drained;
};
static bool cancelled(aether_mp4_audio_handle_t h)
{ return h->cancel && h->cancel(h->context); }
static esp_err_t collect(const int16_t *pcm,size_t frames,void *context)
{
    aether_mp4_audio_handle_t h=context;
    if(cancelled(h)) return ESP_ERR_INVALID_STATE;
    size_t trim=h->trim<frames ? (size_t)h->trim : frames;
    h->trim-=trim; pcm+=trim*2; frames-=trim;
    uint64_t remaining=h->total_frames-h->cursor-h->queued;
    if(frames>remaining) frames=(size_t)remaining;
    if(frames>QUEUE_FRAMES-h->queued) return ESP_ERR_INVALID_SIZE;
    memcpy(h->queue+h->queued*2,pcm,frames*4); h->queued+=frames;
    return ESP_OK;
}
esp_err_t aether_mp4_audio_open(FILE *file,bool (*cancel)(void *),void *context,
                               aether_mp4_audio_handle_t *out,char *detail,size_t detail_size)
{
    if(!file || !out) return ESP_ERR_INVALID_ARG;
    *out=NULL;
    aether_mp4_audio_handle_t h=calloc(1,sizeof(*h)); if(!h) return ESP_ERR_NO_MEM;
    h->file=file; h->cancel=cancel; h->context=context;
    esp_err_t error=mp4_demux_open_audio(file,cancel,context,&h->demux,detail,detail_size);
    if(error!=ESP_OK) goto fail;
    h->media=*mp4_demux_info(h->demux);
    error=mp4_aac_new(h->media.asc,h->media.asc_size,cancel,context,&h->decoder);
    if(error!=ESP_OK) {
        if(detail && detail_size) snprintf(detail,detail_size,"Unsupported/corrupt AAC ASC (object_type=0x%02x, ASC bytes=%u); need LC mono/stereo 8..48kHz",h->media.audio_object_type,(unsigned)h->media.asc_size);
        goto fail;
    }
    mp4_aac_status_t source;
    error=mp4_aac_get_status(h->decoder,&source); if(error!=ESP_OK) goto fail;
    h->info=(aether_mp4_audio_info_t){.sample_rate=source.sample_rate,.channels=source.channels,
        .object_type=h->media.audio_object_type,.duration_ms=h->media.duration_ms,.asc_size=h->media.asc_size};
    memcpy(h->info.asc,h->media.asc,h->media.asc_size);
    h->total_frames=h->media.audio_duration_units*48000/h->media.audio_timescale;
    h->trim=(uint64_t)h->media.audio_edit*48000/h->media.audio_timescale;
    h->packet=heap_caps_malloc(8192,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    h->queue=heap_caps_malloc(QUEUE_FRAMES*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!h->packet || !h->queue) { error=ESP_ERR_NO_MEM; goto fail; }
    *out=h; return ESP_OK;
fail:
    if(detail && detail_size && !detail[0]) snprintf(detail,detail_size,"MP4 audio open failed (0x%x)",(unsigned)error);
    aether_mp4_audio_close(h); return error;
}
esp_err_t aether_mp4_audio_get_info(aether_mp4_audio_handle_t h,aether_mp4_audio_info_t *info)
{ if(!h || !info) return ESP_ERR_INVALID_ARG; *info=h->info; return ESP_OK; }
size_t aether_mp4_audio_read(aether_mp4_audio_handle_t h,int16_t *stereo,size_t max_frames,esp_err_t *error)
{
    if(error) *error=ESP_OK;
    if(!h || !stereo || !max_frames) { if(error) *error=ESP_ERR_INVALID_ARG; return 0; }
    esp_err_t failure=h->info.last_error;
    if(failure!=ESP_OK) goto fail;
    if(cancelled(h)) { failure=ESP_ERR_INVALID_STATE; goto fail; }
    while(!h->queued && !h->drained) {
        if(cancelled(h)) { failure=ESP_ERR_INVALID_STATE; goto fail; }
        h->queue_offset=0;
        if(h->index<h->media.audio_samples) {
            mp4_sample_t sample;
            failure=mp4_demux_sample(h->demux,true,h->index,&sample); if(failure!=ESP_OK) goto fail;
            failure=mp4_demux_read(h->file,&sample,h->packet,8192); if(failure!=ESP_OK) goto fail;
            failure=mp4_aac_decode(h->decoder,h->packet,sample.size,collect,h); if(failure!=ESP_OK) goto fail;
            ++h->index;
        } else {
            failure=mp4_aac_finish(h->decoder,collect,h); if(failure!=ESP_OK) goto fail;
            h->drained=true;
        }
    }
    size_t count=h->queued<max_frames ? h->queued : max_frames;
    memcpy(stereo,h->queue+h->queue_offset*2,count*4);
    h->queue_offset+=count; h->queued-=count; h->cursor+=count;
    h->info.position_ms=(uint32_t)(h->cursor*1000/48000);
    h->info.finished=h->drained && !h->queued;
    if(h->info.finished) h->info.position_ms=h->info.duration_ms;
    return count;
fail:
    h->info.last_error=failure; if(error) *error=failure; return 0;
}
esp_err_t aether_mp4_audio_seek(aether_mp4_audio_handle_t h,uint32_t position_ms)
{
    if(!h || position_ms>h->info.duration_ms) return ESP_ERR_INVALID_ARG;
    if(cancelled(h)) return ESP_ERR_INVALID_STATE;
    esp_err_t error=mp4_aac_reset(h->decoder); if(error!=ESP_OK) { h->info.last_error=error; return error; }
    const int64_t target=(int64_t)position_ms*1000;
    uint32_t index=0;
    for(uint32_t low=0,high=h->media.audio_samples;low<high;) {
        uint32_t mid=low+(high-low)/2; mp4_sample_t sample;
        error=mp4_demux_sample(h->demux,true,mid,&sample); if(error!=ESP_OK) { h->info.last_error=error; return error; }
        if(sample.pts_us<=target) { index=mid; low=mid+1; } else high=mid;
    }
    if(index) --index;
    mp4_sample_t sample; error=mp4_demux_sample(h->demux,true,index,&sample);
    if(error!=ESP_OK) { h->info.last_error=error; return error; }
    h->index=index; h->queued=h->queue_offset=0; h->drained=false;
    h->trim=target>sample.pts_us ? (uint64_t)(target-sample.pts_us)*48000/1000000 : 0;
    h->cursor=(uint64_t)position_ms*48; if(h->cursor>h->total_frames) h->cursor=h->total_frames;
    h->info.position_ms=position_ms; h->info.finished=false; h->info.last_error=ESP_OK;
    return ESP_OK;
}
void aether_mp4_audio_close(aether_mp4_audio_handle_t h)
{ if(h) { mp4_aac_del(h->decoder); mp4_demux_close(h->demux); free(h->packet); free(h->queue); free(h); } }
