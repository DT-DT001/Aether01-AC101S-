#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef struct mp4_demux mp4_demux_t;
typedef struct {
    uint16_t width,height,coded_width,coded_height,crop_left,crop_top;
    uint8_t profile,constraints,level,nal_length_size;
    uint32_t duration_ms,video_samples,audio_samples,video_timescale,audio_timescale;
    uint32_t fps_x100;
    bool has_audio;
    int64_t video_edit,audio_edit;
    uint64_t audio_duration_units;
    uint8_t audio_object_type;
    const uint8_t *asc;
    size_t asc_size;
} mp4_media_info_t;
typedef struct {
    uint64_t offset;
    uint32_t size,duration;
    int64_t pts_us;
} mp4_sample_t;

esp_err_t mp4_demux_open(FILE *,bool (*cancel)(void *),void *context,mp4_demux_t **,
                         char *detail,size_t detail_size);
/** Select audio only; unrelated video codecs are not parsed or restricted. */
esp_err_t mp4_demux_open_audio(FILE *,bool (*cancel)(void *),void *context,mp4_demux_t **,
                              char *detail,size_t detail_size);
const mp4_media_info_t *mp4_demux_info(mp4_demux_t *);
esp_err_t mp4_demux_sample(mp4_demux_t *,bool audio,uint32_t index,mp4_sample_t *);
/** Read from a task-owned FILE. The demux metadata may be shared by one audio
 * and one video reader, but never share an individual FILE between tasks. */
esp_err_t mp4_demux_read(FILE *,const mp4_sample_t *,uint8_t *,size_t);
/** Build SPS/PPS Annex-B prefix. */
esp_err_t mp4_demux_headers(mp4_demux_t *,uint8_t *,size_t,size_t *);
/** Convert one avc1 access unit in a separate output buffer; validate VCL type. */
esp_err_t mp4_annexb(const uint8_t *,size_t,uint8_t,unsigned,uint8_t *,size_t,size_t *);
void mp4_demux_close(mp4_demux_t *);
