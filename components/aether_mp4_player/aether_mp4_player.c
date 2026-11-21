#include "aether_media_fs.h"
#include "aether_mp4_player.h"
#include "mp4_demux.h"
#include "mp4_aac.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_dec_param.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#endif

#define FRAME_BYTES (480U*480U*2U)
#define PACKET_BYTES (512U*1024U)
#define HEADER_BYTES (16U*1024U)
#define AUDIO_TASK_PRIORITY 7
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
#define VIDEO_TASK_CORE 0
#else
#define VIDEO_TASK_CORE 1
#endif
#define AUDIO_TASK_CORE 0
#define TASK_STACK_CAPS (MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT)
static const char *TAG="mp4_player";
struct aether_mp4_player {
    aether_storage_handle_t storage;
    aether_media_fs_lease_t media_lease;
    ac101s_audio_handle_t audio;
    portMUX_TYPE lock;
    atomic_bool stop,pause;
    atomic_uint seek_revision;
    SemaphoreHandle_t publish,lifecycle;
    TaskHandle_t video_worker; /* External lifecycle owner alone creates/reaps. */
    bool running,release_frame,clock_waiting_audio;
    uint8_t volume;
    int64_t started_us,paused_us,pause_start_us,audio_clock_position_us,audio_clock_wait_started_us;
    aether_mp4_status_t status;
    aether_mp4_frame_cb_t callback;
    void *context;
    uint16_t *pixels;
};
typedef struct {
    aether_mp4_player_handle_t h;
    mp4_demux_t *demux;
    mp4_media_info_t info;
    atomic_bool cancel_audio,audio_done,audio_pause_interrupted,audio_was_cancelled;
    atomic_bool audio_worker_exit,audio_worker_exited;
    atomic_uint audio_requested_epoch,audio_completed_epoch;
    atomic_int audio_result;
    TaskHandle_t audio_worker;
    uint64_t trim_frames;
    uint32_t revision;
    int64_t audio_start_us;
    int16_t *audio_output;
    uint64_t audio_write_total_us;
    int64_t audio_previous_write_end_us;
    mp4_aac_handle_t audio_decoder; /* Session-owned; one audio child at a time. */
    uint8_t *idr_cache; /* Two bits/sample: unknown, non-IDR, validated IDR. Video owner only. */
} session_t;

#ifdef ESP_PLATFORM
typedef struct { int32_t affinity; uint32_t core_mask; } placement_snapshot_t;
static placement_snapshot_t placement_snapshot(void)
{
    const int32_t core=(int32_t)xPortGetCoreID();
    return (placement_snapshot_t){.affinity=(int32_t)xTaskGetCoreID(NULL),
        .core_mask=core>=0 && core<32 ? UINT32_C(1)<<(unsigned)core : 0};
}
static void cache_stack(aether_mp4_player_handle_t h,bool video)
{
    unsigned marker=0;
    const uint32_t address=(uint32_t)(uintptr_t)&marker;
    const bool hp=esp_ptr_internal(&marker) && esp_ptr_dma_capable(&marker);
    portENTER_CRITICAL(&h->lock);
    if(video) { h->status.video_stack_addr=address; h->status.video_stack_hp=hp; }
    else { h->status.audio_stack_addr=address; h->status.audio_stack_hp=hp; }
    portEXIT_CRITICAL(&h->lock);
}
static void cache_stack_watermark(aether_mp4_player_handle_t h,bool video)
{
    /* One scan at final exit, never in the steady-state media path. IDF reports bytes. */
    const uint32_t free_min=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&h->lock);
    if(video) h->status.video_stack_free_min_bytes=free_min;
    else h->status.audio_stack_free_min_bytes=free_min;
    portEXIT_CRITICAL(&h->lock);
}
#endif

static void copy_text(char *out,const char *in,size_t capacity)
{ if(capacity) { size_t n=strlen(in); if(n>=capacity) n=capacity-1; memcpy(out,in,n); out[n]=0; } }
static void failure_detail(aether_mp4_player_handle_t h,const char *stage,esp_err_t error)
{
    portENTER_CRITICAL(&h->lock);
    if(!h->status.detail[0]) snprintf(h->status.detail,sizeof(h->status.detail),
        "%s failed: %s (0x%x)",stage,esp_err_to_name(error),(unsigned)error);
    portEXIT_CRITICAL(&h->lock);
}
static bool cancelled(void *context)
{
    session_t *s=context;
    return atomic_load(&s->h->stop) || atomic_load(&s->cancel_audio) ||
           atomic_load(&s->h->seek_revision)!=s->revision ||
           !aether_media_fs_available(&s->h->media_lease);
}
static int64_t position_us(aether_mp4_player_handle_t h)
{
    const int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&h->lock);
    const int64_t end=h->pause_start_us ? h->pause_start_us : now;
    const int64_t position=h->status.seeking ? (int64_t)h->status.seek_requested_ms*1000 :
        h->clock_waiting_audio ? h->audio_clock_position_us :
        h->started_us ? end-h->started_us-h->paused_us : 0;
    portEXIT_CRITICAL(&h->lock);
    return position>0 ? position : 0;
}
/* Codec open/AAC preroll can take longer than a video frame. Do not count that
 * startup time as presentation lateness after a seek: it otherwise leaves the
 * audio behind the wall clock and may immediately enter repeated IDR recovery.
 * A paused preview is complete even though this private audio gate remains. */
static void commit_audio_clock(session_t *s)
{
    aether_mp4_player_handle_t h=s->h;
    const int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&h->lock);
    if(h->clock_waiting_audio && !atomic_load(&h->pause) && !atomic_load(&h->stop) &&
       atomic_load(&h->seek_revision)==s->revision) {
        h->started_us=now-h->audio_clock_position_us;
        const uint32_t waited=(uint32_t)(now-h->audio_clock_wait_started_us);
        h->status.audio_clock_wait_us=waited;
        if(waited>h->status.audio_clock_wait_max_us) h->status.audio_clock_wait_max_us=waited;
        h->paused_us=0; h->pause_start_us=0; h->clock_waiting_audio=false;
    }
    portEXIT_CRITICAL(&h->lock);
}
static bool wait_until(session_t *s,int64_t target_us)
{
    while(!cancelled(s)) {
        if(atomic_load(&s->h->pause)) return false;
        if(atomic_load(&s->audio_done)) {
            if(atomic_load(&s->audio_result)!=ESP_OK) return false;
            /* A shorter audio track can end without a non-trimmed seek sample. */
            commit_audio_clock(s);
        }
        int64_t left=target_us-position_us(s->h);
        if(left<=0) return true;
        uint32_t ms=(uint32_t)((left+999)/1000); if(ms>5) ms=5;
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    }
    return false;
}
static esp_err_t pcm_output(const int16_t *pcm,size_t frames,void *context)
{
    session_t *s=context;
    if(cancelled(s)) return ESP_ERR_INVALID_STATE;
    if(atomic_load(&s->h->pause)) { atomic_store(&s->audio_pause_interrupted,true); return ESP_ERR_INVALID_STATE; }
    if(s->trim_frames) {
        const size_t trim=s->trim_frames<frames ? (size_t)s->trim_frames : frames;
        pcm+=trim*2; frames-=trim; s->trim_frames-=trim;
    }
    if(!frames) return ESP_OK;
    if(frames>1024) return ESP_ERR_INVALID_SIZE;
    commit_audio_clock(s);
    portENTER_CRITICAL(&s->h->lock);
    const uint8_t volume=s->h->volume;
    portEXIT_CRITICAL(&s->h->lock);
    for(size_t i=0;i<frames*2;++i) s->audio_output[i]=(int16_t)((int32_t)pcm[i]*volume/100);
    const int64_t begin=esp_timer_get_time();
    const uint32_t gap=s->audio_previous_write_end_us ? (uint32_t)(begin-s->audio_previous_write_end_us) : 0;
    esp_err_t error=ac101s_audio_write(s->h->audio,s->audio_output,frames*4);
    const int64_t end=esp_timer_get_time();
    const uint32_t write=(uint32_t)(end-begin);
    s->audio_write_total_us+=write; s->audio_previous_write_end_us=end;
#ifdef ESP_PLATFORM
    const placement_snapshot_t placement=placement_snapshot();
#endif
    portENTER_CRITICAL(&s->h->lock);
#ifdef ESP_PLATFORM
    s->h->status.audio_affinity=placement.affinity; s->h->status.audio_core_mask|=placement.core_mask;
#endif
    s->h->status.audio_write_us=write; s->h->status.audio_supply_gap_us=gap;
    if(write>s->h->status.audio_write_max_us) s->h->status.audio_write_max_us=write;
    if(gap>s->h->status.audio_supply_gap_max_us) s->h->status.audio_supply_gap_max_us=gap;
    portEXIT_CRITICAL(&s->h->lock);
    return error;
}
/* Sample timestamps are monotonic for the supported no-B-picture streams. */
static esp_err_t sample_at_or_before(session_t *s,bool audio,int64_t target,uint32_t *index)
{
    uint32_t found=0;
    for(uint32_t low=0,high=audio ? s->info.audio_samples : s->info.video_samples;low<high;) {
        if(cancelled(s)) return ESP_ERR_INVALID_STATE;
        uint32_t middle=low+(high-low)/2; mp4_sample_t sample;
        esp_err_t error=mp4_demux_sample(s->demux,audio,middle,&sample); if(error!=ESP_OK) return error;
        if(sample.pts_us<=target) { found=middle; low=middle+1; } else high=middle;
    }
    *index=found; return ESP_OK;
}
static esp_err_t audio_reposition(session_t *s,mp4_aac_handle_t decoder,int64_t target,uint32_t *index)
{
    if(target==0) {
        /* Reusing a decoder at the beginning still requires the SDK reset.
         * Compute encoder priming from exact edit units, not rounded PTS us. */
        *index=0;
        s->trim_frames=s->info.audio_edit>0 ? (uint64_t)s->info.audio_edit*48000/s->info.audio_timescale : 0;
        return mp4_aac_reset(decoder);
    }
    esp_err_t error=sample_at_or_before(s,true,target,index); if(error!=ESP_OK) return error;
    if(*index) --*index; /* One complete AAC-LC overlap/preroll AU before the target AU. */
    mp4_sample_t sample; error=mp4_demux_sample(s->demux,true,*index,&sample); if(error!=ESP_OK) return error;
    s->trim_frames=target>sample.pts_us ? (uint64_t)(target-sample.pts_us)*48000/1000000 : 0;
    return mp4_aac_reset(decoder);
}
static void audio_segment(session_t *s)
{
    aether_mp4_player_handle_t h=s->h;
    FILE *file=NULL; uint8_t *packet=NULL; mp4_aac_handle_t decoder=NULL;
    bool codec_started=false;
    esp_err_t error=ESP_OK;
    const char *stage="AAC session startup";
    uint32_t index=0;
    if(cancelled(s)) { error=ESP_ERR_INVALID_STATE; goto done; }
    stage="AAC file open";
    file=fopen(h->status.path,"rb");
    if(!file) { error=ESP_ERR_NOT_FOUND; goto done; }
    stage="AAC buffer allocation";
    packet=heap_caps_malloc(8192,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s->audio_output=heap_caps_malloc(1024*2*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!packet || !s->audio_output) { error=ESP_ERR_NO_MEM; goto done; }
    stage="AAC-LC decoder/ASC setup";
    decoder=s->audio_decoder;
    const bool reused_decoder=decoder!=NULL;
    if(!decoder) {
        error=mp4_aac_new(s->info.asc,s->info.asc_size,cancelled,s,&decoder);
        if(error!=ESP_OK) goto done;
        s->audio_decoder=decoder;
    }
    mp4_aac_status_t audio_info;
    error=mp4_aac_get_status(decoder,&audio_info); if(error!=ESP_OK) goto done;
    ESP_LOGI(TAG,"audio format path=%s AAC-LC rate=%lu channels=%u ASC=%u bytes output=48000Hz/stereo",
        h->status.path,(unsigned long)audio_info.sample_rate,audio_info.channels,(unsigned)s->info.asc_size);
    portENTER_CRITICAL(&h->lock);
    h->status.audio_rate=audio_info.sample_rate; h->status.audio_channels=audio_info.channels;
    portEXIT_CRITICAL(&h->lock);
    /* Keep AAC's private hot allocations in their original placement. Seek
     * resets this same instance after the previous segment published audio_done,
     * rather than reopening it after H264 has allocated its new reference DPB. */
    s->trim_frames=s->info.audio_edit>0 ? (uint64_t)s->info.audio_edit*48000/s->info.audio_timescale : 0;
    if(reused_decoder || s->audio_start_us>0) {
        stage="AAC seek preroll/trim";
        error=audio_reposition(s,decoder,s->audio_start_us,&index); if(error!=ESP_OK) goto done;
    }
    while(index<s->info.audio_samples && !cancelled(s)) {
        const bool interrupted=atomic_exchange(&s->audio_pause_interrupted,false);
        if(atomic_load(&h->pause) || interrupted) {
            const bool rebuild=codec_started || interrupted;
            const int64_t held_position=position_us(h),held_at=esp_timer_get_time();
            portENTER_CRITICAL(&h->lock);
            if(!h->clock_waiting_audio && !atomic_load(&h->stop) && atomic_load(&h->seek_revision)==s->revision) {
                h->clock_waiting_audio=true; h->audio_clock_position_us=held_position;
                h->audio_clock_wait_started_us=held_at;
            }
            portEXIT_CRITICAL(&h->lock);
            s->audio_previous_write_end_us=0; /* Intentional pause is not starvation. */
            if(codec_started) {
                stage="audio pause/codec stop";
                error=ac101s_audio_stop(h->audio);
                if(error!=ESP_OK) goto done;
                codec_started=false;
            }
            while(atomic_load(&h->pause) && !cancelled(s)) vTaskDelay(1);
            if(cancelled(s)) break;
            atomic_store(&s->audio_pause_interrupted,false);
            /* Closing I2S discards queued audio. Resume at the common media
             * clock, rather than continuing from the ahead-of-DMA cursor. */
            if(rebuild) {
                const int64_t resume=position_us(h);
                stage="audio resume sample lookup";
                error=audio_reposition(s,decoder,resume,&index); if(error!=ESP_OK) goto done;
            }
            /* A fresh seek task paused before its first output has no queued
             * PCM to discard. Keep its exact target/preroll rather than trimming
             * away the scheduler latency after the user resumes. */
        }
        if(!codec_started) {
            stage="audio output start";
            error=ac101s_audio_start(h->audio,100,0);
            if(error!=ESP_OK) goto done;
            codec_started=true;
        }
        stage="audio sample lookup";
        const int64_t lookup_begin=esp_timer_get_time();
        mp4_sample_t sample; error=mp4_demux_sample(s->demux,true,index,&sample);
        const uint32_t lookup_us=(uint32_t)(esp_timer_get_time()-lookup_begin);
        portENTER_CRITICAL(&h->lock);
        h->status.audio_lookup_us=lookup_us;
        if(lookup_us>h->status.audio_lookup_max_us) h->status.audio_lookup_max_us=lookup_us;
        portEXIT_CRITICAL(&h->lock);
        if(error!=ESP_OK) goto done;
        if(!wait_until(s,sample.pts_us-50000)) continue;
        stage="audio sample read";
        const int64_t read_begin=esp_timer_get_time();
        error=mp4_demux_read(file,&sample,packet,8192); if(error!=ESP_OK) goto done;
        const int64_t read_end=esp_timer_get_time();
        const uint64_t write_before=s->audio_write_total_us;
        const bool preroll=s->trim_frames!=0;
        stage="AAC decode/audio write";
        error=mp4_aac_decode(decoder,packet,sample.size,pcm_output,s);
        const int64_t decoded=esp_timer_get_time();
        const uint64_t waited=s->audio_write_total_us-write_before;
        const uint32_t read_us=(uint32_t)(read_end-read_begin);
        const uint64_t total=(uint64_t)(decoded-read_end);
        const uint32_t decode_us=(uint32_t)(total>waited ? total-waited : 0);
#ifdef ESP_PLATFORM
        const placement_snapshot_t placement=placement_snapshot();
#endif
        portENTER_CRITICAL(&h->lock);
#ifdef ESP_PLATFORM
        h->status.audio_affinity=placement.affinity; h->status.audio_core_mask|=placement.core_mask;
#endif
        h->status.audio_read_us=read_us; h->status.audio_decode_us=decode_us;
        if(read_us>h->status.audio_read_max_us) h->status.audio_read_max_us=read_us;
        if(decode_us>h->status.audio_decode_max_us) h->status.audio_decode_max_us=decode_us;
        portEXIT_CRITICAL(&h->lock);
        if(!preroll && position_us(h)>sample.pts_us+50000) {
            portENTER_CRITICAL(&h->lock); ++h->status.audio_late_units; portEXIT_CRITICAL(&h->lock);
        }
        /* Timings remain cached in status. Never print periodic diagnostics
         * on this real-time supply path: a slow/disconnected console can hold
         * the audio worker long enough to starve its DMA queue. */
        if(error!=ESP_OK) {
            if(atomic_load(&s->audio_pause_interrupted) && !cancelled(s)) continue;
            goto done;
        }
        ++index;
    }
    if(cancelled(s)) error=ESP_ERR_INVALID_STATE;
    else {
        stage="AAC final PCM/audio write";
        error=mp4_aac_finish(decoder,pcm_output,s);
        if(error==ESP_ERR_INVALID_STATE && atomic_load(&s->audio_pause_interrupted)) error=ESP_OK;
    }
done:
    if(codec_started) {
        /* Keep audio_done false until DMA/transport ownership is really gone.
         * Parent stop_wait can time out, but cannot start another decoder over
         * a retained failed close or free this audio worker's session early. */
        for(;;) {
            const esp_err_t stopped=ac101s_audio_stop(h->audio);
            if(stopped==ESP_OK) break;
            if(error==ESP_OK) { stage="audio output stop"; error=stopped; }
            portENTER_CRITICAL(&h->lock);h->status.audio_error=stopped;portEXIT_CRITICAL(&h->lock);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    const bool was_cancelled=cancelled(s);
    if(error!=ESP_OK && !was_cancelled) {
        failure_detail(h,stage,error);
        ESP_LOGE(TAG,"audio failed path=%s stage=%s sample=%lu/%lu error=%s (0x%x) ASC=%u bytes",
            h->status.path,stage,(unsigned long)index,(unsigned long)s->info.audio_samples,
            esp_err_to_name(error),(unsigned)error,(unsigned)s->info.asc_size);
    }
    /* Parent owns decoder lifetime. Only the task-private IO/PCM buffers leave
     * here; audio_done is the barrier before another segment may reset/reuse it. */
    free(packet); free(s->audio_output); s->audio_output=NULL;
    if(file) fclose(file);
    atomic_store(&s->audio_result,error);
    atomic_store(&s->audio_was_cancelled,was_cancelled);
}
static void audio_task(void *argument)
{
    session_t *s=argument;
#ifdef ESP_PLATFORM
    cache_stack(s->h,false);
    unsigned stack_probe=0;
    ESP_LOGD(TAG,"AAC task placement core=%d stack=%p internal=%u dma=%u",
        xPortGetCoreID(),(void *)&stack_probe,(unsigned)esp_ptr_internal(&stack_probe),
        (unsigned)esp_ptr_dma_capable(&stack_probe));
#endif
    for(;;) {
        if(atomic_load(&s->audio_worker_exit)) break;
        const unsigned requested=atomic_load(&s->audio_requested_epoch);
        if(requested==atomic_load(&s->audio_completed_epoch)) {
            ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
            continue;
        }
        audio_segment(s);
        /* A segment ACK permits reset/reposition, not freeing this worker's
         * session or stack. Notifications may arrive before we park; the epoch
         * comparison prevents stale wakeups from replaying an old segment. */
        atomic_store(&s->audio_completed_epoch,requested);
        atomic_store(&s->audio_done,true);
    }
#ifdef ESP_PLATFORM
    cache_stack_watermark(s->h,false);
#endif
    /* ACK is not kernel suspension. Parent waits for both before external
     * WithCaps deletion; no session access or temporary helper after ACK. */
    atomic_store(&s->audio_worker_exited,true);
    for(;;) vTaskSuspend(NULL);
}
static esp_err_t h264_error(esp_h264_err_t error)
{
    if(error==ESP_H264_ERR_OK) return ESP_OK;
    if(error==ESP_H264_ERR_MEM) return ESP_ERR_NO_MEM;
    if(error==ESP_H264_ERR_UNSUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    return ESP_ERR_INVALID_RESPONSE;
}
static uint8_t channel(int value) { return value<0 ? 0 : value>255 ? 255 : (uint8_t)value; }
static esp_err_t convert_frame_scalar(session_t *s,esp_h264_dec_handle_t decoder,const esp_h264_dec_out_frame_t *picture)
{
    esp_h264_dec_param_sw_handle_t parameters;
    esp_h264_resolution_t resolution;
    esp_err_t error=h264_error(esp_h264_dec_sw_get_param_hd(decoder,&parameters));
    if(error!=ESP_OK) return error;
    error=h264_error(esp_h264_dec_get_resolution(parameters,&resolution));
    if(error!=ESP_OK) return error;
    unsigned width=resolution.width,height=resolution.height,x=s->info.crop_left,y=s->info.crop_top;
    if(width==s->info.width && height==s->info.height) x=y=0;
    else if(width!=s->info.coded_width || height!=s->info.coded_height) return ESP_ERR_INVALID_RESPONSE;
    if(!width || !height || (width&1) || (height&1) || width>480 || height>480 ||
       picture->out_size<(size_t)width*height*3/2 || !picture->outbuf) return ESP_ERR_INVALID_SIZE;
    const uint8_t *yy=picture->outbuf,*u=yy+(size_t)width*height,*v=u+(size_t)width*height/4;
    uint16_t *out=s->h->pixels;
    for(unsigned row=0;row<s->info.height;++row) {
        if((row&15U)==0 && cancelled(s)) return ESP_ERR_INVALID_STATE;
        for(unsigned col=0;col<s->info.width;++col) {
            const unsigned sx=x+col,sy=y+row;
            const int a=(int)yy[sy*width+sx]-16,b=(int)u[(sy/2)*(width/2)+sx/2]-128,c=(int)v[(sy/2)*(width/2)+sx/2]-128;
            uint8_t r=channel((298*a+409*c+128)>>8),g=channel((298*a-100*b-208*c+128)>>8),bl=channel((298*a+516*b+128)>>8);
            *out++=(uint16_t)((r&248U)<<8|(g&252U)<<3|bl>>3);
        }
    }
    return ESP_OK;
}
/* I420 shares each U/V sample across a2x2 luma block. Reuse chroma
 * terms with exact scalar rounding; odd geometry retains the scalar path. */
static inline __attribute__((always_inline)) uint16_t rgb565_luma(uint8_t y,int32_t red,int32_t green,int32_t blue)
{
    const int32_t luma=298*((int32_t)y-16);
    const uint8_t r=channel((luma+red)>>8),g=channel((luma+green)>>8),b=channel((luma+blue)>>8);
    return (uint16_t)((r&248U)<<8|(g&252U)<<3|b>>3);
}
static esp_err_t convert_frame(session_t *s,esp_h264_dec_handle_t decoder,const esp_h264_dec_out_frame_t *picture)
{
    if((s->info.width|s->info.height|s->info.crop_left|s->info.crop_top)&1U)
        return convert_frame_scalar(s,decoder,picture);
    esp_h264_dec_param_sw_handle_t parameters;
    esp_h264_resolution_t resolution;
    esp_err_t error=h264_error(esp_h264_dec_sw_get_param_hd(decoder,&parameters));
    if(error!=ESP_OK) return error;
    error=h264_error(esp_h264_dec_get_resolution(parameters,&resolution));
    if(error!=ESP_OK) return error;
    unsigned width=resolution.width,height=resolution.height,x=s->info.crop_left,y=s->info.crop_top;
    if(width==s->info.width && height==s->info.height) x=y=0;
    else if(width!=s->info.coded_width || height!=s->info.coded_height) return ESP_ERR_INVALID_RESPONSE;
    if(!width || !height || (width&1) || (height&1) || width>480 || height>480 ||
       picture->out_size<(size_t)width*height*3/2 || !picture->outbuf) return ESP_ERR_INVALID_SIZE;
    const uint8_t *yy=picture->outbuf,*u=yy+(size_t)width*height,*v=u+(size_t)width*height/4;
    for(unsigned row=0;row<s->info.height;row+=2) {
        if((row&15U)==0 && cancelled(s)) return ESP_ERR_INVALID_STATE;
        const uint8_t *y0=yy+(size_t)(y+row)*width+x,*y1=y0+width;
        const uint8_t *ur=u+(size_t)((y+row)/2)*(width/2)+x/2;
        const uint8_t *vr=v+(size_t)((y+row)/2)*(width/2)+x/2;
        uint16_t *out0=s->h->pixels+(size_t)row*s->info.width,*out1=out0+s->info.width;
        for(unsigned col=0;col<s->info.width;col+=2) {
            const int32_t cb=(int32_t)*ur++-128,cr=(int32_t)*vr++-128;
            const int32_t red=409*cr+128,green=-100*cb-208*cr+128,blue=516*cb+128;
            out0[0]=rgb565_luma(y0[0],red,green,blue);
            out0[1]=rgb565_luma(y0[1],red,green,blue);
            out1[0]=rgb565_luma(y1[0],red,green,blue);
            out1[1]=rgb565_luma(y1[1],red,green,blue);
            y0+=2; y1+=2; out0+=2; out1+=2;
        }
    }
    return ESP_OK;
}

static esp_err_t decoder_new(esp_h264_dec_handle_t *decoder)
{
    const esp_h264_dec_cfg_sw_t config={.pic_type=ESP_H264_RAW_FMT_I420};
    esp_err_t error=h264_error(esp_h264_dec_sw_new(&config,decoder));
    if(error==ESP_OK) error=h264_error(esp_h264_dec_open(*decoder));
    return error;
}
static esp_err_t feed(session_t *s,esp_h264_dec_handle_t decoder,uint8_t *bytes,size_t size,
                       int64_t pts,bool draw,int64_t presentation_end_us,bool *produced)
{
    *produced=false;
    unsigned loops=0;
    bool handled=false;
    uint32_t h264_us=0,convert_us=0,output_frames=0,converted_frames=0,presentation_drops=0;
    esp_err_t error=ESP_OK;
    while(size && !cancelled(s)) {
        if(++loops>512) { error=ESP_ERR_INVALID_RESPONSE; break; }
        esp_h264_dec_in_frame_t input={.raw_data={.buffer=bytes,.len=(uint32_t)size},.pts=(uint32_t)(pts/1000),.dts=(uint32_t)(pts/1000)};
        esp_h264_dec_out_frame_t output={0};
        const int64_t h264_begin=esp_timer_get_time();
        const esp_h264_err_t result=esp_h264_dec_process(decoder,&input,&output);
        h264_us+=(uint32_t)(esp_timer_get_time()-h264_begin);
        error=h264_error(result);
        if(error!=ESP_OK) {
            ESP_LOGE(TAG,"H264 process result=%d bytes=%lu consumed=%lu output=%lu pts_ms=%lu",
                result,(unsigned long)input.raw_data.len,(unsigned long)input.consume,
                (unsigned long)output.out_size,(unsigned long)input.pts);
            break;
        }
        if(output.out_size) ++output_frames;
        if(input.consume>size || (!input.consume && !output.out_size)) { error=ESP_ERR_INVALID_RESPONSE; break; }
        if(output.out_size && draw && !handled) {
            handled=true;
            /* Decode every reference packet, but do not spend RGB/display time
             * on an expired normal-playback picture. First pictures, seek
             * previews and decoder priming disable this presentation deadline. */
            if(presentation_end_us>=0 && position_us(s->h)>=presentation_end_us) ++presentation_drops;
            else {
                const int64_t convert_begin=esp_timer_get_time();
                error=convert_frame(s,decoder,&output);
                convert_us+=(uint32_t)(esp_timer_get_time()-convert_begin);
                if(error!=ESP_OK) break;
                *produced=true; ++converted_frames;
            }
        }
        bytes+=input.consume; size-=input.consume;
    }
#ifdef ESP_PLATFORM
    const placement_snapshot_t placement=placement_snapshot();
#endif
    portENTER_CRITICAL(&s->h->lock);
#ifdef ESP_PLATFORM
    s->h->status.video_affinity=placement.affinity; s->h->status.video_core_mask|=placement.core_mask;
#endif
    s->h->status.h264_us=h264_us; s->h->status.convert_us=convert_us;
    s->h->status.h264_total_us+=h264_us; s->h->status.convert_total_us+=convert_us;
    if(h264_us>s->h->status.h264_max_us) s->h->status.h264_max_us=h264_us;
    if(convert_us>s->h->status.convert_max_us) s->h->status.convert_max_us=convert_us;
    s->h->status.h264_output_frames+=output_frames; s->h->status.converted_frames+=converted_frames;
    s->h->status.presentation_drops+=presentation_drops;
    portEXIT_CRITICAL(&s->h->lock);
    return error!=ESP_OK ? error : cancelled(s) ? ESP_ERR_INVALID_STATE : ESP_OK;
}
static esp_err_t prime_decoder(session_t *s,esp_h264_dec_handle_t decoder,
    const uint8_t *headers,size_t bytes,uint8_t *scratch,int64_t pts)
{
    /* tinyh264's h264bsdExtractNalUnit removes emulation-prevention bytes IN
     * PLACE. Never decode our retained avcC-derived SPS/PPS cache directly:
     * its original length/escaped bytes must survive seek and lag resets.
     * packet scratch is used here so an already-converted IDR in annex stays
     * intact during lag recovery. */
    if(bytes>PACKET_BYTES) return ESP_ERR_INVALID_SIZE;
    memcpy(scratch,headers,bytes);
    bool produced;
    return feed(s,decoder,scratch,bytes,pts,false,-1,&produced);
}
static bool has_idr(const uint8_t *bytes,size_t size)
{
    for(size_t i=0;i+4<size;++i) if(!bytes[i] && !bytes[i+1] && !bytes[i+2] && bytes[i+3]==1 && (bytes[i+4]&31)==5) return true;
    return false;
}
static unsigned cached_idr(session_t *s,uint32_t index)
{ return s->idr_cache ? (s->idr_cache[index/4]>>(index%4*2))&3U : 0; }
static void cache_idr(session_t *s,uint32_t index,bool idr)
{
    if(s->idr_cache) {
        const unsigned shift=index%4*2;
        s->idr_cache[index/4]=(uint8_t)((s->idr_cache[index/4]&~(3U<<shift))|((idr ? 2U : 1U)<<shift));
    }
}
static esp_err_t probe_idr(session_t *s,FILE *file,const mp4_sample_t *sample,bool *idr)
{
    /* Length prefixes and NAL headers determine IDR presence. Seeking need not
     * read/copy every compressed picture twice. The selected IDR and all GOP
     * dependencies still pass full Annex-B validation before decoder input. */
    uint64_t offset=sample->offset;
    size_t left=sample->size;
    unsigned nals=0;
    *idr=false;
    while(left) {
        if(cancelled(s)) return ESP_ERR_INVALID_STATE;
        const unsigned prefix=s->info.nal_length_size;
        if(left<=prefix || ++nals>256 || offset>LONG_MAX) return ESP_ERR_INVALID_RESPONSE;
        uint8_t header[5];
        if(fseek(file,(long)offset,SEEK_SET) || fread(header,1,prefix+1,file)!=prefix+1) return ESP_FAIL;
        uint32_t bytes=0;
        for(unsigned i=0;i<prefix;++i) bytes=bytes<<8|header[i];
        if(!bytes || bytes>left-prefix) return ESP_ERR_INVALID_SIZE;
        if((header[prefix]&31U)==5) { *idr=true; return ESP_OK; }
        offset+=prefix+bytes; left-=prefix+bytes;
        if((nals&31U)==0) vTaskDelay(1);
    }
    return ESP_OK;
}
static esp_err_t preceding_idr(session_t *s,FILE *file,uint32_t target,uint32_t *index)
{
    /* Inspect the actual VCL NAL, not an untrusted stss sync-sample flag. */
    unsigned scanned=0;
    int64_t yielded=esp_timer_get_time();
    for(uint32_t i=target+1;i>0;) {
        --i; if(cancelled(s)) return ESP_ERR_INVALID_STATE;
        unsigned known=cached_idr(s,i);
        if(!known) {
            mp4_sample_t sample; esp_err_t error=mp4_demux_sample(s->demux,false,i,&sample); if(error!=ESP_OK) return error;
            bool idr=false; error=probe_idr(s,file,&sample,&idr); if(error!=ESP_OK) return error;
            cache_idr(s,i,idr); known=idr ? 2U : 1U;
        }
        if(known==2U) { *index=i; return ESP_OK; }
        const int64_t now=esp_timer_get_time();
        if((++scanned&31U)==0 || now-yielded>=4000) { vTaskDelay(1); yielded=esp_timer_get_time(); }
    }
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t reserve_audio(session_t *s)
{
    /* Reserve HP DRAM before H264 opens/allocates its DPB. Epoch zero parks
     * immediately and never opens the decoder, file or I2S hardware. The task
     * may run before this creation call publishes s->audio_worker. */
    s->audio_worker=NULL;
    if(xTaskCreatePinnedToCoreWithCaps(audio_task,"mp4_aac",24576,s,AUDIO_TASK_PRIORITY,
        &s->audio_worker,AUDIO_TASK_CORE,TASK_STACK_CAPS)==pdPASS) return ESP_OK;
    atomic_store(&s->audio_result,ESP_ERR_NO_MEM);
    atomic_store(&s->audio_worker_exited,true);
    return ESP_ERR_NO_MEM;
}
static esp_err_t start_audio(session_t *s)
{
    if(!s->audio_worker || !atomic_load(&s->audio_done) || atomic_load(&s->audio_worker_exit) ||
       atomic_load(&s->audio_requested_epoch)!=atomic_load(&s->audio_completed_epoch)) return ESP_ERR_INVALID_STATE;
    atomic_store(&s->audio_done,false); atomic_store(&s->audio_result,ESP_OK);
    atomic_store(&s->audio_pause_interrupted,false); atomic_store(&s->audio_was_cancelled,false);
    s->audio_write_total_us=0; s->audio_previous_write_end_us=0;
    atomic_fetch_add(&s->audio_requested_epoch,1);
    xTaskNotifyGive(s->audio_worker);
    return ESP_OK;
}
static void player_task(void *argument)
{
    aether_mp4_player_handle_t h=argument;
#ifdef ESP_PLATFORM
    cache_stack(h,true);
    unsigned stack_probe=0;
    ESP_LOGD(TAG,"H264 task placement core=%d stack=%p internal=%u dma=%u",
        xPortGetCoreID(),(void *)&stack_probe,(unsigned)esp_ptr_internal(&stack_probe),
        (unsigned)esp_ptr_dma_capable(&stack_probe));
#endif
    session_t *s=calloc(1,sizeof(*s)); FILE *file=NULL;
    esp_h264_dec_handle_t decoder=NULL; uint8_t *packet=NULL,*annex=NULL,*headers=NULL;
    bool leased=false,audio_created=false,finished=false,seek_ready=false,seek_preview=false;
    bool await_idr=false; int64_t first_produced=-1,seek_target_us=0,seek_decode_started=0;
    uint32_t seek_target_index=0,produced_since_seek=0,lag_skip_burst=0;
    size_t header_bytes=0;
    esp_err_t error=s ? ESP_OK : ESP_ERR_NO_MEM;
    const char *stage="MP4 session allocation";
    uint32_t index=0;
    if(!s) goto done;
    s->h=h; s->revision=atomic_load(&h->seek_revision); atomic_init(&s->cancel_audio,false); atomic_init(&s->audio_done,true);
    atomic_init(&s->audio_worker_exit,false); atomic_init(&s->audio_worker_exited,false);
    atomic_init(&s->audio_requested_epoch,0); atomic_init(&s->audio_completed_epoch,0);
    atomic_init(&s->audio_pause_interrupted,false); atomic_init(&s->audio_result,ESP_OK);
    atomic_init(&s->audio_was_cancelled,false);
    stage="TF lease acquire";
    error=aether_media_fs_acquire(h->storage,h->status.path,&h->media_lease); if(error!=ESP_OK) goto done; leased=true;
    if(cancelled(s)) { error=ESP_ERR_INVALID_STATE; goto done; }
    stage="video file open";
    file=fopen(h->status.path,"rb"); if(!file) { error=ESP_ERR_NOT_FOUND; goto done; }
    char detail[112]={0};
    stage="MP4 demux/format validation";
    error=mp4_demux_open(file,cancelled,s,&s->demux,detail,sizeof(detail));
    if(error!=ESP_OK) {
        portENTER_CRITICAL(&h->lock); copy_text(h->status.detail,detail,sizeof(h->status.detail)); portEXIT_CRITICAL(&h->lock); goto done;
    }
    s->info=*mp4_demux_info(s->demux);
    ESP_LOGI(TAG,"format path=%s H264 video_object=0x21 profile=%u constraints=0x%02x level=%u display=%ux%u coded=%ux%u crop=%u,%u fps=%lu.%02lu samples=%lu timescale=%lu duration_ms=%lu audio=%u audio_object=0x%02x ASC=%u",
        h->status.path,s->info.profile,s->info.constraints,s->info.level,s->info.width,s->info.height,
        s->info.coded_width,s->info.coded_height,s->info.crop_left,s->info.crop_top,
        (unsigned long)(s->info.fps_x100/100),(unsigned long)(s->info.fps_x100%100),
        (unsigned long)s->info.video_samples,(unsigned long)s->info.video_timescale,
        (unsigned long)s->info.duration_ms,s->info.has_audio,s->info.audio_object_type,(unsigned)s->info.asc_size);
    portENTER_CRITICAL(&h->lock);
    h->status.width=s->info.width; h->status.height=s->info.height; h->status.duration_ms=s->info.duration_ms;
    h->status.h264_profile=s->info.profile; h->status.h264_level=s->info.level;
    h->status.source_fps_x100=s->info.fps_x100; h->status.fps=(uint8_t)((s->info.fps_x100+50)/100);
    h->status.has_audio=s->info.has_audio;
    portEXIT_CRITICAL(&h->lock);
    if(s->info.has_audio && !h->audio) { error=ESP_ERR_NOT_SUPPORTED; copy_text(detail,"MP4 audio present but codec hardware unavailable",sizeof(detail)); goto fail_detail; }
    if(s->info.fps_x100>3000) { error=ESP_ERR_NOT_SUPPORTED; copy_text(detail,"MP4 source exceeds supported 30 fps",sizeof(detail)); goto fail_detail; }
    if(s->info.has_audio) {
        stage="AAC HP stack reservation (24KiB)";
        error=reserve_audio(s); if(error!=ESP_OK) goto done;
    }
    stage="video PSRAM buffer allocation";
    packet=heap_caps_malloc(PACKET_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    annex=heap_caps_malloc(PACKET_BYTES+HEADER_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    headers=heap_caps_malloc(HEADER_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&h->lock); uint16_t *pixels=h->pixels; portEXIT_CRITICAL(&h->lock);
    if(!pixels) {
        pixels=heap_caps_malloc(FRAME_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        portENTER_CRITICAL(&h->lock); h->pixels=pixels; portEXIT_CRITICAL(&h->lock);
    }
    if(!packet || !annex || !headers || !pixels) { error=ESP_ERR_NO_MEM; goto done; }
    /* Optional bounded PSRAM hint: failure degrades seek IO, never playback.
     * The demux limits video_samples to300000, so this uses at most75000B. */
    s->idr_cache=heap_caps_calloc((s->info.video_samples+3U)/4U,1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    stage="H264 SPS/PPS header extraction";
    error=mp4_demux_headers(s->demux,headers,HEADER_BYTES,&header_bytes); if(error!=ESP_OK) goto done;
    stage="H264 decoder creation/open";
    error=decoder_new(&decoder); if(error!=ESP_OK) goto done;
    stage="H264 SPS/PPS decoder input";
    error=prime_decoder(s,decoder,headers,header_bytes,packet,0); if(error!=ESP_OK) goto done;
    seek_ready=true;
    const int64_t started=esp_timer_get_time();
    portENTER_CRITICAL(&h->lock);
    h->started_us=started; h->paused_us=0; h->status.seek_supported=true;
    h->clock_waiting_audio=s->info.has_audio; h->audio_clock_position_us=0; h->audio_clock_wait_started_us=started;
    h->pause_start_us=atomic_load(&h->pause) ? started : 0;
    portEXIT_CRITICAL(&h->lock);
    if(s->info.has_audio) {
        stage="AAC task allocation (24KiB stack)";
        error=start_audio(s); if(error!=ESP_OK) goto done;
        audio_created=true;
    }
decode_video:
    for(;index<s->info.video_samples && !cancelled(s);) {
        stage="audio worker completion";
        if(audio_created && atomic_load(&s->audio_done) && atomic_load(&s->audio_result)!=ESP_OK) { error=atomic_load(&s->audio_result); goto done; }
        const bool paused=atomic_load(&h->pause);
        portENTER_CRITICAL(&h->lock); h->status.state=paused ? AETHER_MJPEG_PAUSED : AETHER_MJPEG_PLAYING; portEXIT_CRITICAL(&h->lock);
        if(paused && !seek_preview) { vTaskDelay(1); continue; }
        stage="video sample lookup";
        const int64_t lookup_begin=esp_timer_get_time();
        mp4_sample_t sample; error=mp4_demux_sample(s->demux,false,index,&sample);
        const uint32_t lookup_us=(uint32_t)(esp_timer_get_time()-lookup_begin);
        portENTER_CRITICAL(&h->lock);
        h->status.video_lookup_us=lookup_us;
        if(lookup_us>h->status.video_lookup_max_us) h->status.video_lookup_max_us=lookup_us;
        portEXIT_CRITICAL(&h->lock);
        if(error!=ESP_OK) goto done;
        const int64_t read_start=esp_timer_get_time();
        stage="video sample read";
        error=mp4_demux_read(file,&sample,packet,PACKET_BYTES); if(error!=ESP_OK) goto done;
        stage="H264 access-unit/Annex-B validation";
        size_t bytes=0; error=mp4_annexb(packet,sample.size,s->info.nal_length_size,256,annex,PACKET_BYTES+HEADER_BYTES,&bytes); if(error!=ESP_OK) goto done;
        cache_idr(s,index,has_idr(annex,bytes));
        if(!seek_preview && position_us(h)-sample.pts_us>500000) await_idr=true;
        if(await_idr && !has_idr(annex,bytes)) {
            ++lag_skip_burst;
            const uint32_t read_us=(uint32_t)(esp_timer_get_time()-read_start);
            portENTER_CRITICAL(&h->lock);
            ++h->status.dropped_frames;
            if(lag_skip_burst>h->status.lag_skip_burst_max) h->status.lag_skip_burst_max=lag_skip_burst;
            if(read_us>h->status.read_max_us) h->status.read_max_us=read_us;
            portEXIT_CRITICAL(&h->lock);
            ++index; vTaskDelay(1); continue;
        }
        if(await_idr) {
            stage="H264 lag recovery/decoder reset";
            portENTER_CRITICAL(&h->lock); ++h->status.lag_rebuilds; portEXIT_CRITICAL(&h->lock);
            esp_h264_dec_close(decoder); esp_h264_dec_del(decoder); decoder=NULL;
            error=decoder_new(&decoder); if(error!=ESP_OK) goto done;
            error=prime_decoder(s,decoder,headers,header_bytes,packet,sample.pts_us); if(error!=ESP_OK) goto done;
            await_idr=false; lag_skip_burst=0;
        }
        const int64_t read_done=esp_timer_get_time();
        const int64_t presentation_end_us=sample.pts_us+(int64_t)sample.duration*1000000/s->info.video_timescale;
        const bool force_picture=seek_preview || !produced_since_seek;
        bool produced=false;
        stage="H264 sample decode/I420 conversion";
        error=feed(s,decoder,annex,bytes,sample.pts_us,seek_preview ? index>=seek_target_index : sample.pts_us>=0,
            force_picture ? -1 : presentation_end_us,&produced);
        const int64_t decoded=esp_timer_get_time();
        portENTER_CRITICAL(&h->lock);
        if((uint32_t)(read_done-read_start)>h->status.read_max_us) h->status.read_max_us=(uint32_t)(read_done-read_start);
        if((uint32_t)(decoded-read_done)>h->status.decode_max_us) h->status.decode_max_us=(uint32_t)(decoded-read_done);
        portEXIT_CRITICAL(&h->lock);
        if(error!=ESP_OK) goto done;
        if(cancelled(s)) break;
        if(produced) {
            /* This iteration owns one prepared RGB image. Pausing or waiting
             * for its PTS must retain it here, never re-feed the same P packet
             * or overwrite it by decoding the next picture. */
            const bool preview=seek_preview;
            for(;;) {
                if(cancelled(s)) { error=ESP_ERR_INVALID_STATE; goto done; }
                if(audio_created && atomic_load(&s->audio_done) && atomic_load(&s->audio_result)!=ESP_OK) {
                    error=atomic_load(&s->audio_result); goto done;
                }
                if(!preview) {
                    const bool holding=atomic_load(&h->pause);
                    portENTER_CRITICAL(&h->lock);
                    h->status.state=holding ? AETHER_MJPEG_PAUSED : AETHER_MJPEG_PLAYING;
                    portEXIT_CRITICAL(&h->lock);
                    if(holding) { vTaskDelay(1); continue; }
                    if(!wait_until(s,sample.pts_us)) continue;
                }
                if(xSemaphoreTake(h->publish,pdMS_TO_TICKS(10))!=pdTRUE) continue;
                if(cancelled(s)) { xSemaphoreGive(h->publish); error=ESP_ERR_INVALID_STATE; goto done; }
                /* A pause can arrive during the gate wait. Leave command
                 * acceptance available while holding this same prepared RGB. */
                if(!preview && atomic_load(&h->pause)) { xSemaphoreGive(h->publish); continue; }
                break;
            }
            /* RGB work is already paid for: this is our newest displayable
             * picture. Publish it even if conversion/gate wait crossed its
             * end, rather than holding the older image while decoding again. */
            if(preview) {
                /* Preroll must not advance the presentation clock. Start audio
                 * and video together only after the requested image exists. */
                const int64_t now=esp_timer_get_time();
                portENTER_CRITICAL(&h->lock);
                h->started_us=now-seek_target_us; h->paused_us=0;
                h->pause_start_us=atomic_load(&h->pause) ? now : 0;
                h->clock_waiting_audio=s->info.has_audio; h->audio_clock_position_us=seek_target_us;
                h->audio_clock_wait_started_us=now;
                h->status.seek_preroll_us=(uint32_t)(now-seek_decode_started);
                h->status.seeking=false; h->status.position_ms=(uint32_t)(seek_target_us/1000);
                portEXIT_CRITICAL(&h->lock);
                seek_preview=false;
                if(s->info.has_audio) {
                    s->audio_start_us=seek_target_us;
                    stage="AAC seek task allocation (24KiB stack)";
                    error=start_audio(s);
                    if(error!=ESP_OK) { xSemaphoreGive(h->publish); goto done; }
                    audio_created=true;
                }
            }
            portENTER_CRITICAL(&h->lock); const uint32_t number=++h->status.frame_number; portEXIT_CRITICAL(&h->lock);
            ++produced_since_seek;
            const int64_t callback_begin=esp_timer_get_time();
            h->callback(h->pixels,s->info.width,s->info.height,number,h->context);
            xSemaphoreGive(h->publish);
            const int64_t active=position_us(h),completed=esp_timer_get_time();
            if(first_produced<0) first_produced=active;
            portENTER_CRITICAL(&h->lock);
            h->status.position_ms=preview ? (uint32_t)(seek_target_us/1000) : sample.pts_us>0 ? (uint32_t)(sample.pts_us/1000) : 0;
            h->status.elapsed_ms=(uint32_t)(active/1000); h->status.read_us=(uint32_t)(read_done-read_start);
            h->status.decode_us=(uint32_t)(decoded-read_done); h->status.callback_us=(uint32_t)(completed-callback_begin);
            if(h->status.callback_us>h->status.callback_max_us) h->status.callback_max_us=h->status.callback_us;
            if(active>sample.pts_us+(int64_t)sample.duration*1000000/s->info.video_timescale) ++h->status.late_frames;
            if(produced_since_seek>1 && active>first_produced) h->status.actual_fps_x100=(uint32_t)((uint64_t)(produced_since_seek-1)*100000000/(uint64_t)(active-first_produced));
            portEXIT_CRITICAL(&h->lock);
        }
        ++index; vTaskDelay(1);
    }
    if(cancelled(s)) { error=ESP_ERR_INVALID_STATE; goto done; }
    if(seek_preview) { error=ESP_ERR_INVALID_RESPONSE; copy_text(detail,"Seek produced no target picture",sizeof(detail)); goto fail_detail; }
    if(!h->status.frame_number) { error=ESP_ERR_INVALID_RESPONSE; copy_text(detail,"H264 stream produced no displayable picture",sizeof(detail)); goto fail_detail; }
    /* The last picture's PTS is its start, not the end of the clip. Retain it
     * for its declared duration, including pause/resume at the final frame. */
    stage="final video duration wait";
    while(position_us(h)<(int64_t)s->info.duration_ms*1000 && !cancelled(s)) {
        if(audio_created && atomic_load(&s->audio_done)) {
            if(atomic_load(&s->audio_result)!=ESP_OK) { error=atomic_load(&s->audio_result); goto done; }
            commit_audio_clock(s);
        }
        const bool paused=atomic_load(&h->pause);
        portENTER_CRITICAL(&h->lock); h->status.state=paused ? AETHER_MJPEG_PAUSED : AETHER_MJPEG_PLAYING; portEXIT_CRITICAL(&h->lock);
        vTaskDelay(1);
    }
    stage="audio completion wait";
    if(audio_created) while(!atomic_load(&s->audio_done) && !cancelled(s)) vTaskDelay(1);
    if(audio_created && atomic_load(&s->audio_done)) error=atomic_load(&s->audio_result);
    if(cancelled(s)) error=ESP_ERR_INVALID_STATE;
    finished=error==ESP_OK;
    goto done;
fail_detail:
    portENTER_CRITICAL(&h->lock); copy_text(h->status.detail,detail,sizeof(h->status.detail)); portEXIT_CRITICAL(&h->lock);
done:
    if(seek_ready && s && !atomic_load(&h->stop) && aether_media_fs_available(&h->media_lease) &&
       atomic_load(&h->seek_revision)!=s->revision) {
        /* The video owner alone replaces session resources. The AAC task owns
         * its separate FILE and decoder until its final completion publication. */
        atomic_store(&s->cancel_audio,true);
        if(audio_created) while(!atomic_load(&s->audio_done)) vTaskDelay(1);
        audio_created=false;
        if(atomic_load(&h->stop) || !aether_media_fs_available(&h->media_lease)) goto finish_cleanup;
        portENTER_CRITICAL(&h->lock);
        s->revision=atomic_load(&h->seek_revision);
        seek_target_us=(int64_t)h->status.seek_requested_ms*1000;
        h->status.actual_fps_x100=0; h->status.audio_error=ESP_OK;
        portEXIT_CRITICAL(&h->lock);
        atomic_store(&s->cancel_audio,false);
        error=ESP_OK; finished=false; seek_preview=true; await_idr=false;
        first_produced=-1; produced_since_seek=0; lag_skip_burst=0;
        stage="seek target sample lookup";
        const int64_t seek_scan_started=esp_timer_get_time();
        error=sample_at_or_before(s,false,seek_target_us,&seek_target_index); if(error!=ESP_OK) goto done;
        stage="seek preceding IDR scan";
        error=preceding_idr(s,file,seek_target_index,&index); if(error!=ESP_OK) goto done;
        portENTER_CRITICAL(&h->lock); h->status.seek_scan_us=(uint32_t)(esp_timer_get_time()-seek_scan_started); portEXIT_CRITICAL(&h->lock);
        seek_decode_started=esp_timer_get_time();
        stage="seek H264 decoder reset";
        if(decoder) { esp_h264_dec_close(decoder); esp_h264_dec_del(decoder); decoder=NULL; }
        error=decoder_new(&decoder); if(error!=ESP_OK) goto done;
        error=prime_decoder(s,decoder,headers,header_bytes,packet,seek_target_us); if(error!=ESP_OK) goto done;
        ESP_LOGI(TAG,"seek path=%s target_ms=%lu IDR_sample=%lu preview_sample=%lu paused=%u scan_us=%lu",
            h->status.path,(unsigned long)(seek_target_us/1000),(unsigned long)index,
            (unsigned long)seek_target_index,(unsigned)atomic_load(&h->pause),(unsigned long)h->status.seek_scan_us);
        goto decode_video;
    }
finish_cleanup:
    /* Close command acceptance before releasing decoder/FILE resources. A seek
     * accepted between the first done-check and this gate must still restart. */
    while(xSemaphoreTake(h->publish,pdMS_TO_TICKS(10))!=pdTRUE) vTaskDelay(1);
    if(seek_ready && s && !atomic_load(&h->stop) && aether_media_fs_available(&h->media_lease) &&
       atomic_load(&h->seek_revision)!=s->revision) {
        xSemaphoreGive(h->publish); goto done;
    }
    portENTER_CRITICAL(&h->lock); h->status.seek_supported=false; portEXIT_CRITICAL(&h->lock);
    xSemaphoreGive(h->publish);
    if(s) {
        atomic_store(&s->cancel_audio,true);
        if(audio_created) while(!atomic_load(&s->audio_done)) vTaskDelay(1);
        if(audio_created) {
            esp_err_t audio_error=atomic_load(&s->audio_result);
            portENTER_CRITICAL(&h->lock);
            h->status.audio_error=audio_error==ESP_ERR_INVALID_STATE && atomic_load(&s->audio_was_cancelled) ? ESP_OK : audio_error;
            if(audio_error!=ESP_OK && audio_error!=ESP_ERR_INVALID_STATE && !h->status.detail[0])
                copy_text(h->status.detail,"AAC-LC decode or audio hardware output failed",sizeof(h->status.detail));
            portEXIT_CRITICAL(&h->lock);
        }
    }
    if(s && s->audio_worker) {
        atomic_store(&s->audio_worker_exit,true);
        xTaskNotifyGive(s->audio_worker);
        while(!atomic_load(&s->audio_worker_exited)) vTaskDelay(1);
        while(eTaskGetState(s->audio_worker)!=eSuspended) vTaskDelay(1);
        vTaskDeleteWithCaps(s->audio_worker); s->audio_worker=NULL;
    }
    if(s) { mp4_aac_del(s->audio_decoder); s->audio_decoder=NULL; }
    if(decoder) { esp_h264_dec_close(decoder); esp_h264_dec_del(decoder); }
    free(packet); free(annex); free(headers);
    if(s) { mp4_demux_close(s->demux); free(s->idr_cache); }
    if(file) fclose(file);
    if(leased) aether_media_fs_release(&h->media_lease);
    free(s);
    const bool stopped=atomic_load(&h->stop);
    if(error!=ESP_OK && !stopped) failure_detail(h,stage,error);
    char path[192],reason[112]; uint32_t frames;
    portENTER_CRITICAL(&h->lock);
    h->status.last_error=stopped ? ESP_OK : error; h->status.seeking=false; h->clock_waiting_audio=false;
    const aether_mjpeg_state_t final_state=stopped ? AETHER_MJPEG_STOPPED : error!=ESP_OK ? AETHER_MJPEG_ERROR : finished ? AETHER_MJPEG_FINISHED : AETHER_MJPEG_STOPPED;
    if(finished) h->status.position_ms=h->status.duration_ms;
    copy_text(path,h->status.path,sizeof(path)); copy_text(reason,h->status.detail,sizeof(reason)); frames=h->status.frame_number;
    portEXIT_CRITICAL(&h->lock);
#ifdef ESP_PLATFORM
    cache_stack_watermark(h,true);
#endif
    if(error!=ESP_OK && !stopped) ESP_LOGE(TAG,"failed path=%s stage=%s sample=%lu error=%s (0x%x) frames=%lu detail=%s",
        path,stage,(unsigned long)index,esp_err_to_name(error),(unsigned)error,(unsigned long)frames,reason);
    else ESP_LOGI(TAG,"%s path=%s frames=%lu",stopped ? "stopped" : finished ? "finished" : "ended",path,(unsigned long)frames);
    /* release_frame can arrive while the detached buffer is being freed. Drain
     * those requests before the final ACK. Once ACK is visible the task must
     * never touch h, log, or free anything: only the external owner may reap. */
    for(;;) {
        uint16_t *release=NULL;
        portENTER_CRITICAL(&h->lock);
        if(h->release_frame && h->pixels) {
            release=h->pixels; h->pixels=NULL; h->release_frame=false;
        } else {
            h->release_frame=false; h->status.state=final_state; h->running=false;
        }
        portEXIT_CRITICAL(&h->lock);
        if(!release) break;
        free(release);
    }
    for(;;) vTaskSuspend(NULL);
}
/* Caller holds lifecycle, never publish. An ACK alone cannot authorize static
 * stack/TCB release: the kernel must have actually switched the worker out. */
static esp_err_t reap_video_locked(aether_mp4_player_handle_t h,int64_t deadline)
{
    const TaskHandle_t worker=h->video_worker;
    if(!worker) return ESP_OK;
    for(;;) {
        portENTER_CRITICAL(&h->lock); const bool running=h->running; portEXIT_CRITICAL(&h->lock);
        if(!running && eTaskGetState(worker)==eSuspended) {
            vTaskDeleteWithCaps(worker); h->video_worker=NULL;
            return ESP_OK;
        }
        if(esp_timer_get_time()>=deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
static TickType_t remaining_ticks(int64_t deadline)
{
    const int64_t left=deadline-esp_timer_get_time();
    if(left<=0) return 0;
    const TickType_t ticks=pdMS_TO_TICKS((uint32_t)((left+999)/1000));
    return ticks ? ticks : 1;
}
esp_err_t aether_mp4_player_new(aether_storage_handle_t storage,ac101s_audio_handle_t audio,aether_mp4_player_handle_t *out)
{
    if(!storage || !out) return ESP_ERR_INVALID_ARG;
    *out=calloc(1,sizeof(**out)); if(!*out) return ESP_ERR_NO_MEM;
    (*out)->storage=storage; (*out)->audio=audio; (*out)->volume=65;
    (*out)->lock=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    (*out)->publish=xSemaphoreCreateMutex();
    if(!(*out)->publish) { free(*out); *out=NULL; return ESP_ERR_NO_MEM; }
    (*out)->lifecycle=xSemaphoreCreateMutex();
    if(!(*out)->lifecycle) { vSemaphoreDelete((*out)->publish); free(*out); *out=NULL; return ESP_ERR_NO_MEM; }
    atomic_init(&(*out)->seek_revision,0);
    atomic_init(&(*out)->stop,false); atomic_init(&(*out)->pause,false); return ESP_OK;
}
esp_err_t aether_mp4_player_play(aether_mp4_player_handle_t h,const char *path,aether_mp4_frame_cb_t callback,void *context)
{
    if(!h || !path || !path[0] || !callback) return ESP_ERR_INVALID_ARG;
    if(strlen(path)>=sizeof(h->status.path)) return ESP_ERR_INVALID_SIZE;
    if(xSemaphoreTake(h->lifecycle,0)!=pdTRUE) return ESP_ERR_INVALID_STATE;
    if(reap_video_locked(h,esp_timer_get_time())!=ESP_OK) {
        xSemaphoreGive(h->lifecycle); return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&h->lock);
    if(h->running) { portEXIT_CRITICAL(&h->lock); xSemaphoreGive(h->lifecycle); return ESP_ERR_INVALID_STATE; }
    h->status=(aether_mp4_status_t){.state=AETHER_MJPEG_STARTING}; copy_text(h->status.path,path,sizeof(h->status.path));
    h->callback=callback; h->context=context; h->release_frame=false;
    h->started_us=h->paused_us=h->pause_start_us=0; h->clock_waiting_audio=false; h->audio_clock_position_us=0;
    atomic_store(&h->seek_revision,0); atomic_store(&h->stop,false); atomic_store(&h->pause,false); h->running=true;
    portEXIT_CRITICAL(&h->lock);
    ESP_LOGI(TAG,"start path=%s",path);
    TaskHandle_t created=NULL;
    if(xTaskCreatePinnedToCoreWithCaps(player_task,"mp4_video",24576,h,5,&created,VIDEO_TASK_CORE,TASK_STACK_CAPS)!=pdPASS) {
        portENTER_CRITICAL(&h->lock); h->running=false; h->status.state=AETHER_MJPEG_ERROR; h->status.last_error=ESP_ERR_NO_MEM;
        copy_text(h->status.detail,"MP4 video task allocation failed (24KiB stack)",sizeof(h->status.detail)); portEXIT_CRITICAL(&h->lock);
        ESP_LOGE(TAG,"failed path=%s stage=video_task_create error=ESP_ERR_NO_MEM stack=24576",path);
        xSemaphoreGive(h->lifecycle);
        return ESP_ERR_NO_MEM;
    }
    h->video_worker=created;
    xSemaphoreGive(h->lifecycle);
    return ESP_OK;
}
esp_err_t aether_mp4_player_set_paused(aether_mp4_player_handle_t h,bool paused)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    const int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&h->lock);
    if(!h->running) { portEXIT_CRITICAL(&h->lock); return ESP_ERR_INVALID_STATE; }
    if(paused!=atomic_load(&h->pause)) {
        if(h->started_us) {
            if(paused) h->pause_start_us=now;
            else { h->paused_us+=now-h->pause_start_us; h->pause_start_us=0; }
        }
        atomic_store(&h->pause,paused);
    }
    portEXIT_CRITICAL(&h->lock); return ESP_OK;
}
esp_err_t aether_mp4_player_seek(aether_mp4_player_handle_t h,uint32_t position_ms)
{ return aether_mp4_player_seek_with_reset(h,position_ms,NULL,NULL); }
esp_err_t aether_mp4_player_seek_with_reset(aether_mp4_player_handle_t h,uint32_t position_ms,
    void (*reset)(void *),void *context)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    if(xSemaphoreTake(h->publish,0)!=pdTRUE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->lock);
    esp_err_t error=ESP_OK;
    if(!h->running || atomic_load(&h->stop) || !h->status.seek_supported) error=ESP_ERR_INVALID_STATE;
    else if(position_ms>h->status.duration_ms) error=ESP_ERR_INVALID_ARG;
    portEXIT_CRITICAL(&h->lock);
    if(error==ESP_OK) {
        if(reset) reset(context);
        portENTER_CRITICAL(&h->lock);
        if(atomic_load(&h->stop)) error=ESP_ERR_INVALID_STATE;
        else {
            if(position_ms==h->status.duration_ms && position_ms) --position_ms;
            h->status.seek_requested_ms=position_ms; h->status.seeking=true;
            atomic_fetch_add(&h->seek_revision,1);
        }
        portEXIT_CRITICAL(&h->lock);
    }
    xSemaphoreGive(h->publish);
    return error;
}
esp_err_t aether_mp4_player_set_volume(aether_mp4_player_handle_t h,uint8_t volume)
{ if(!h || volume>100) return ESP_ERR_INVALID_ARG; portENTER_CRITICAL(&h->lock); h->volume=volume; portEXIT_CRITICAL(&h->lock); return ESP_OK; }
esp_err_t aether_mp4_player_stop(aether_mp4_player_handle_t h)
{ if(!h) return ESP_ERR_INVALID_ARG; atomic_store(&h->stop,true); return ESP_OK; }
esp_err_t aether_mp4_player_stop_wait(aether_mp4_player_handle_t h,uint32_t timeout)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    aether_mp4_player_stop(h); const int64_t deadline=esp_timer_get_time()+(int64_t)timeout*1000;
    if(xSemaphoreTake(h->lifecycle,remaining_ticks(deadline))!=pdTRUE) return ESP_ERR_TIMEOUT;
    aether_mp4_player_stop(h);
    const esp_err_t error=reap_video_locked(h,deadline);
    xSemaphoreGive(h->lifecycle); return error;
}
void aether_mp4_player_release_frame(aether_mp4_player_handle_t h)
{
    if(!h) return;
    uint16_t *release=NULL; portENTER_CRITICAL(&h->lock);
    if(h->running) h->release_frame=true;
    else { release=h->pixels; h->pixels=NULL; }
    portEXIT_CRITICAL(&h->lock); free(release);
}
esp_err_t aether_mp4_player_get_status(aether_mp4_player_handle_t h,aether_mp4_status_t *status)
{ if(!h || !status) return ESP_ERR_INVALID_ARG; portENTER_CRITICAL(&h->lock); *status=h->status; portEXIT_CRITICAL(&h->lock); return ESP_OK; }
esp_err_t aether_mp4_player_del(aether_mp4_player_handle_t h)
{
    if(!h) return ESP_ERR_INVALID_ARG;
    aether_mp4_player_stop(h); const int64_t deadline=esp_timer_get_time()+3000000;
    if(xSemaphoreTake(h->lifecycle,remaining_ticks(deadline))!=pdTRUE) return ESP_ERR_TIMEOUT;
    aether_mp4_player_stop(h);
    const esp_err_t error=reap_video_locked(h,deadline);
    if(error!=ESP_OK) { xSemaphoreGive(h->lifecycle); return error; }
    /* The caller has exclusive object-lifetime ownership: no concurrent or
     * queued API calls may overlap del. All worker ownership is now gone. */
    aether_mp4_player_release_frame(h);
    xSemaphoreGive(h->lifecycle); vSemaphoreDelete(h->lifecycle);
    vSemaphoreDelete(h->publish); free(h); return ESP_OK;
}
