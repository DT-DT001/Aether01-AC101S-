#include "mp4_demux.h"
#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define INDEX_BUDGET (8U*1024U*1024U)
#define INDEX_ALLOCATION_MAX (2U*1024U*1024U)
typedef union { size_t bytes; max_align_t alignment; } allocation_t;
static atomic_size_t index_bytes;
static bool reserve(size_t bytes)
{
    size_t used=atomic_load(&index_bytes);
    do { if(bytes>INDEX_ALLOCATION_MAX || used>INDEX_BUDGET-bytes) return false; }
    while(!atomic_compare_exchange_weak(&index_bytes,&used,used+bytes));
    return true;
}
static void *index_malloc(size_t bytes)
{
    if(!bytes) bytes=1;
    if(!reserve(bytes)) return NULL;
    allocation_t *p=heap_caps_malloc(sizeof(*p)+bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!p) { atomic_fetch_sub(&index_bytes,bytes); return NULL; }
    p->bytes=bytes; return p+1;
}
static void index_free(void *ptr)
{ if(ptr) { allocation_t *p=(allocation_t *)ptr-1; atomic_fetch_sub(&index_bytes,p->bytes); free(p); } }
static void *index_realloc(void *ptr,size_t bytes)
{
    if(!ptr) return index_malloc(bytes);
    if(!bytes) { index_free(ptr); return NULL; }
    allocation_t *old=(allocation_t *)ptr-1;
    const size_t old_size=old->bytes;
    if(bytes>INDEX_ALLOCATION_MAX || (bytes>old_size && !reserve(bytes-old_size))) return NULL;
    allocation_t *next=heap_caps_realloc(old,sizeof(*old)+bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!next) { if(bytes>old_size) atomic_fetch_sub(&index_bytes,bytes-old_size); return NULL; }
    if(bytes<old_size) atomic_fetch_sub(&index_bytes,old_size-bytes);
    next->bytes=bytes; return next+1;
}
#define MINIMP4_IMPLEMENTATION
#define MINIMP4_MAX_ALLOC_BYTES INDEX_ALLOCATION_MAX
#define malloc index_malloc
#define realloc index_realloc
#define free index_free
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "vendor/minimp4.h"
#pragma GCC diagnostic pop
#undef malloc
#undef realloc
#undef free

struct mp4_demux {
    MP4D_demux_t mp4;
    mp4_media_info_t info;
    int video_track,audio_track;
    FILE *file;
    int64_t file_size,cache_offset;
    size_t cache_size;
    uint8_t cache[4096];
    uint32_t reads;
    bool (*cancel)(void *);
    void *context;
    struct fragment_sample { uint32_t offset,size,stamp,duration; } *fragments;
    uint32_t fragment_count,fragment_capacity;
    struct mdat_range { uint32_t begin,end; } *mdats;
    uint32_t mdat_count,mdat_capacity;
    bool fragmented;
};
static int read_at(int64_t offset,void *buffer,size_t size,void *context)
{
    mp4_demux_t *d=context;
    if(offset<0 || offset>d->file_size || size>(uint64_t)(d->file_size-offset) ||
       (d->cancel && d->cancel(d->context))) return -1;
    if((++d->reads&4095U)==0) vTaskDelay(1);
    if(d->reads>16U*1024U*1024U) return -1; /* Bounded parse work on malformed metadata. */
    uint8_t *out=buffer;
    while(size) {
        if(offset<d->cache_offset || offset>=d->cache_offset+(int64_t)d->cache_size) {
            d->cache_offset=offset&~4095LL;
            if(d->cache_offset>LONG_MAX || fseek(d->file,(long)d->cache_offset,SEEK_SET)) return -1;
            d->cache_size=fread(d->cache,1,sizeof(d->cache),d->file);
            if(!d->cache_size) return -1;
        }
        size_t pos=(size_t)(offset-d->cache_offset),take=d->cache_size-pos;
        if(take>size) take=size;
        memcpy(out,d->cache+pos,take); out+=take; size-=take; offset+=(int64_t)take;
    }
    return 0;
}

/* Audio-only fMP4. Keep this index separate from minimp4's classic stbl
 * tables: a zero-sample initialization segment is valid, and its trex/tfhd/
 * trun defaults must not be confused with classic chunk offsets. */
#define TAG(a,b,c,e) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(e))
typedef struct { uint64_t begin,payload,end; uint32_t type; } atom_t;
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p)<<32|be32(p+4); }
static bool atom(mp4_demux_t *d,uint64_t begin,uint64_t end,atom_t *a)
{
    uint8_t h[16];
    if(begin>end || end-begin<8 || read_at((int64_t)begin,h,8,d)) return false;
    uint64_t size=be32(h); unsigned header=8;
    if(size==1) { if(end-begin<16 || read_at((int64_t)begin+8,h+8,8,d)) return false; size=be64(h+8); header=16; }
    else if(!size) size=end-begin;
    if(size<header || size>end-begin) return false;
    *a=(atom_t){.begin=begin,.payload=begin+header,.end=begin+size,.type=be32(h+4)}; return true;
}
static bool field(mp4_demux_t *d,const atom_t *a,uint64_t relative,void *p,size_t n)
{ return relative<=a->end-a->payload && n<=a->end-a->payload-relative && !read_at((int64_t)(a->payload+relative),p,n,d); }
typedef struct {
    uint32_t track_id,duration,size,description;
    uint64_t next_time,origin;
    bool timeline;
} fragment_defaults_t;
static esp_err_t fragment_init(mp4_demux_t *d,fragment_defaults_t *defaults)
{
    atom_t moov={0};
    for(uint64_t pos=0;pos<(uint64_t)d->file_size;) {
        atom_t a; if(!atom(d,pos,(uint64_t)d->file_size,&a)) return ESP_ERR_INVALID_RESPONSE;
        if(a.type==TAG('m','o','o','v')) moov=a;
        pos=a.end;
    }
    if(!moov.end) return ESP_ERR_INVALID_RESPONSE;
    unsigned track=0;
    for(uint64_t pos=moov.payload;pos<moov.end;) {
        atom_t a; if(!atom(d,pos,moov.end,&a)) return ESP_ERR_INVALID_RESPONSE;
        if(a.type==TAG('t','r','a','k')) {
            if(track++==(unsigned)d->audio_track) for(uint64_t child=a.payload;child<a.end;) {
                atom_t b; uint8_t bytes[24]; if(!atom(d,child,a.end,&b)) return ESP_ERR_INVALID_RESPONSE;
                if(b.type==TAG('t','k','h','d')) {
                    if(!field(d,&b,0,bytes,4) || bytes[0]>1) return ESP_ERR_INVALID_RESPONSE;
                    unsigned offset=bytes[0] ? 20 : 12;
                    if(!field(d,&b,offset,bytes,4)) return ESP_ERR_INVALID_RESPONSE;
                    defaults->track_id=be32(bytes);
                }
                child=b.end;
            }
        }
        pos=a.end;
    }
    if(!defaults->track_id) return ESP_ERR_INVALID_RESPONSE;
    bool found=false;
    for(uint64_t pos=moov.payload;pos<moov.end;) {
        atom_t a; if(!atom(d,pos,moov.end,&a)) return ESP_ERR_INVALID_RESPONSE;
        if(a.type==TAG('m','v','e','x')) for(uint64_t child=a.payload;child<a.end;) {
            atom_t b; uint8_t bytes[24]; if(!atom(d,child,a.end,&b)) return ESP_ERR_INVALID_RESPONSE;
            if(b.type==TAG('t','r','e','x')) {
                if(!field(d,&b,0,bytes,sizeof(bytes)) || be32(bytes)!=0) return ESP_ERR_INVALID_RESPONSE;
                if(be32(bytes+4)==defaults->track_id) {
                    defaults->description=be32(bytes+8); defaults->duration=be32(bytes+12); defaults->size=be32(bytes+16); found=true;
                }
            }
            child=b.end;
        }
        pos=a.end;
    }
    return found && defaults->description==1 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t append_fragment(mp4_demux_t *d,uint64_t offset,uint32_t size,uint64_t stamp,uint32_t duration)
{
    if((d->fragment_count&63U)==0 && d->cancel && d->cancel(d->context)) return ESP_ERR_INVALID_STATE;
    if((d->fragment_count&255U)==255) vTaskDelay(1);
    if(!size || size>8192 || !duration || offset>(uint64_t)d->file_size || size>(uint64_t)d->file_size-offset ||
       stamp>UINT32_MAX || duration>UINT32_MAX-stamp) return ESP_ERR_INVALID_RESPONSE;
    /* Offsets within the file are insufficient: data must belong to one
     * complete mdat payload, never a moov/moof header or another box. */
    uint32_t low=0,high=d->mdat_count;
    while(low<high) { uint32_t mid=low+(high-low)/2; if(d->mdats[mid].begin<=offset) low=mid+1; else high=mid; }
    if(!low || offset>d->mdats[low-1].end || size>d->mdats[low-1].end-offset) return ESP_ERR_INVALID_RESPONSE;
    /* 100k AAC-LC units is over 35 minutes at 48kHz. Longer files fail
     * explicitly instead of exhausting PSRAM during metadata indexing. */
    if(d->fragment_count>=100000) return ESP_ERR_NOT_SUPPORTED;
    if(d->fragment_count==d->fragment_capacity) {
        uint32_t capacity=d->fragment_capacity ? d->fragment_capacity*2 : 256;
        if(capacity>100000) capacity=100000;
        void *next=index_realloc(d->fragments,(size_t)capacity*sizeof(*d->fragments));
        if(!next) return ESP_ERR_NO_MEM;
        d->fragments=next; d->fragment_capacity=capacity;
    }
    d->fragments[d->fragment_count++]=(struct fragment_sample){(uint32_t)offset,size,(uint32_t)stamp,duration};
    return ESP_OK;
}
static esp_err_t fragment_traf(mp4_demux_t *d,const atom_t *moof,const atom_t *traf,unsigned traf_index,fragment_defaults_t *defaults)
{
    uint32_t track_id=0,duration=defaults->duration,size=defaults->size,description=defaults->description;
    uint32_t tfhd_flags=0; uint64_t base=moof->begin,decode_time=defaults->next_time;
    bool tfhd=false,tfdt=false;
    for(uint64_t pos=traf->payload;pos<traf->end;) {
        atom_t a; uint8_t bytes[32]; if(!atom(d,pos,traf->end,&a)) return ESP_ERR_INVALID_RESPONSE;
        if(a.type==TAG('t','f','h','d')) {
            if(tfhd || !field(d,&a,0,bytes,8) || bytes[0]) return ESP_ERR_INVALID_RESPONSE;
            tfhd=true; tfhd_flags=be32(bytes)&0xffffff; track_id=be32(bytes+4);
            if(track_id!=defaults->track_id) return ESP_OK;
            if(tfhd_flags&~0x03003bU) return ESP_ERR_NOT_SUPPORTED;
            uint64_t at=8;
            if(tfhd_flags&1) { if(!field(d,&a,at,bytes,8)) return ESP_ERR_INVALID_RESPONSE; base=be64(bytes); at+=8; }
            else if(!(tfhd_flags&0x020000) && traf_index) return ESP_ERR_NOT_SUPPORTED;
            if(tfhd_flags&2) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; description=be32(bytes); at+=4; }
            if(tfhd_flags&8) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; duration=be32(bytes); at+=4; }
            if(tfhd_flags&16) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; size=be32(bytes); at+=4; }
            if(tfhd_flags&32) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; }
        } else if(a.type==TAG('t','f','d','t')) {
            if(tfdt || !field(d,&a,0,bytes,8) || bytes[0]>1) return ESP_ERR_INVALID_RESPONSE;
            tfdt=true;
            if(bytes[0]) { if(!field(d,&a,4,bytes,8)) return ESP_ERR_INVALID_RESPONSE; decode_time=be64(bytes); }
            else decode_time=be32(bytes+4);
        } else if(a.type==TAG('s','e','n','c')) return ESP_ERR_NOT_SUPPORTED;
        pos=a.end;
    }
    if(!tfhd || track_id!=defaults->track_id) return ESP_ERR_INVALID_RESPONSE;
    if(description!=1 || (tfhd_flags&0x010000)) return ESP_ERR_NOT_SUPPORTED;
    /* This also makes adding signed trun.data_offset to base safe: the file
     * itself is bounded to INT32_MAX, so the signed sum fits int64_t. */
    if(base>(uint64_t)d->file_size) return ESP_ERR_INVALID_RESPONSE;
    if(!defaults->timeline) { defaults->origin=decode_time; defaults->timeline=true; }
    else if(decode_time!=defaults->next_time) return ESP_ERR_NOT_SUPPORTED;
    uint64_t offset=base; bool run_seen=false;
    for(uint64_t pos=traf->payload;pos<traf->end;) {
        atom_t a; uint8_t bytes[16]; if(!atom(d,pos,traf->end,&a)) return ESP_ERR_INVALID_RESPONSE;
        if(a.type==TAG('t','r','u','n')) {
            if(!field(d,&a,0,bytes,8) || bytes[0]>1) return ESP_ERR_INVALID_RESPONSE;
            uint32_t flags=be32(bytes)&0xffffff,count=be32(bytes+4); uint64_t at=8;
            if(flags&~0x000f05U || !count || count>100000-d->fragment_count) return ESP_ERR_NOT_SUPPORTED;
            if(flags&1) {
                if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE;
                int64_t located=(int64_t)base+(int32_t)be32(bytes); if(located<0) return ESP_ERR_INVALID_RESPONSE;
                offset=(uint64_t)located; at+=4;
            } else if(!run_seen && !(tfhd_flags&1)) return ESP_ERR_NOT_SUPPORTED;
            if(flags&4) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; at+=4; }
            for(uint32_t i=0;i<count;++i) {
                uint32_t sample_duration=duration,sample_size=size;
                if(flags&0x100) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; sample_duration=be32(bytes); at+=4; }
                if(flags&0x200) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; sample_size=be32(bytes); at+=4; }
                if(flags&0x400) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; at+=4; }
                if(flags&0x800) { if(!field(d,&a,at,bytes,4)) return ESP_ERR_INVALID_RESPONSE; if(be32(bytes)) return ESP_ERR_NOT_SUPPORTED; at+=4; }
                if(decode_time>UINT64_MAX-sample_duration || decode_time<defaults->origin) return ESP_ERR_INVALID_RESPONSE;
                esp_err_t error=append_fragment(d,offset,sample_size,decode_time-defaults->origin,sample_duration); if(error!=ESP_OK) return error;
                offset+=sample_size; decode_time+=sample_duration;
            }
            run_seen=true;
        }
        pos=a.end;
    }
    defaults->next_time=decode_time;
    return run_seen ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
static esp_err_t index_fragments(mp4_demux_t *d)
{
    fragment_defaults_t defaults={0}; esp_err_t error=fragment_init(d,&defaults); if(error!=ESP_OK) return error;
    for(uint64_t pos=0;pos<(uint64_t)d->file_size;) {
        atom_t a; if(!atom(d,pos,(uint64_t)d->file_size,&a)) return ESP_ERR_INVALID_RESPONSE;
        if(a.type==TAG('m','d','a','t')) {
            if(d->mdat_count>=100000) return ESP_ERR_NOT_SUPPORTED;
            if(d->mdat_count==d->mdat_capacity) {
                uint32_t capacity=d->mdat_capacity ? d->mdat_capacity*2 : 256; if(capacity>100000) capacity=100000;
                void *next=index_realloc(d->mdats,(size_t)capacity*sizeof(*d->mdats)); if(!next) return ESP_ERR_NO_MEM;
                d->mdats=next; d->mdat_capacity=capacity;
            }
            d->mdats[d->mdat_count++]=(struct mdat_range){(uint32_t)a.payload,(uint32_t)a.end};
        }
        pos=a.end;
    }
    if(!d->mdat_count) return ESP_ERR_INVALID_RESPONSE;
    for(uint64_t pos=0;pos<(uint64_t)d->file_size;) {
        atom_t a; if(!atom(d,pos,(uint64_t)d->file_size,&a)) return ESP_ERR_INVALID_RESPONSE;
        if(a.type==TAG('m','o','o','f')) {
            unsigned traf_index=0;
            for(uint64_t child=a.payload;child<a.end;) {
                atom_t b; if(!atom(d,child,a.end,&b)) return ESP_ERR_INVALID_RESPONSE;
                if(b.type==TAG('t','r','a','f')) { error=fragment_traf(d,&a,&b,traf_index++,&defaults); if(error!=ESP_OK) return error; }
                child=b.end;
            }
        }
        pos=a.end;
    }
    return d->fragment_count ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
typedef struct { uint8_t data[2048]; size_t size,bit; bool failed; } bits_t;
static uint32_t bits(bits_t *b,unsigned count)
{
    uint32_t value=0;
    if(count>32 || b->bit+count>b->size*8) { b->failed=true; return 0; }
    while(count--) { value=(value<<1)|((b->data[b->bit/8]>>(7-b->bit%8))&1U); ++b->bit; }
    return value;
}
static uint32_t ue(bits_t *b)
{
    unsigned zero=0;
    while(!b->failed && !bits(b,1)) if(++zero>30) { b->failed=true; return 0; }
    return zero ? ((1U<<zero)-1U)+bits(b,zero) : 0;
}
static void se(bits_t *b) { (void)ue(b); }
static bool rbsp(bits_t *b,const uint8_t *nal,size_t bytes)
{
    memset(b,0,sizeof(*b));
    if(bytes<2 || bytes>sizeof(b->data)) return false;
    unsigned zero=0;
    for(size_t i=1;i<bytes;++i) {
        if(zero>=2 && nal[i]==3) { zero=0; continue; }
        b->data[b->size++]=nal[i]; zero=nal[i]==0 ? zero+1 : 0;
    }
    return true;
}
static esp_err_t format_error(char *detail,size_t capacity,const char *message,esp_err_t code)
{ if(detail && capacity) snprintf(detail,capacity,"%s",message); return code; }
static esp_err_t sps_info(const uint8_t *nal,size_t bytes,mp4_media_info_t *info,char *detail,size_t capacity)
{
    bits_t b;
    if(!nal || !bytes || (nal[0]&31)!=7 || !rbsp(&b,nal,bytes))
        return format_error(detail,capacity,"H264 SPS missing, truncated or excessive",ESP_ERR_INVALID_RESPONSE);
    uint32_t profile=bits(&b,8),constraints=bits(&b,8),level=bits(&b,8);
    if(b.failed) return format_error(detail,capacity,"H264 SPS header is truncated",ESP_ERR_INVALID_RESPONSE);
    info->profile=(uint8_t)profile; info->constraints=(uint8_t)constraints; info->level=(uint8_t)level;
    if(profile!=66 || !(constraints&0x40)) {
        if(detail && capacity) snprintf(detail,capacity,
            "H264 SPS profile=%u constraints=0x%02x level=%u; constrained baseline required",
            (unsigned)profile,(unsigned)constraints,(unsigned)level);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if(ue(&b)>31 || ue(&b)>12)
        return format_error(detail,capacity,"Invalid H264 SPS id or frame-number width",ESP_ERR_INVALID_RESPONSE);
    uint32_t order=ue(&b);
    if(order==0) { if(ue(&b)>12) return format_error(detail,capacity,"Invalid H264 SPS picture-order width",ESP_ERR_INVALID_RESPONSE); }
    else if(order==1) {
        bits(&b,1); se(&b); se(&b); unsigned count=ue(&b);
        if(count>256) return format_error(detail,capacity,"Excessive H264 SPS reference offsets",ESP_ERR_INVALID_RESPONSE);
        for(unsigned i=0;i<count;++i) se(&b);
    } else if(order!=2) return format_error(detail,capacity,"Invalid H264 SPS picture-order type",ESP_ERR_INVALID_RESPONSE);
    if(ue(&b)>16) return format_error(detail,capacity,"H264 SPS exceeds 16 reference frames",ESP_ERR_NOT_SUPPORTED);
    bits(&b,1);
    uint32_t wm=ue(&b),hm=ue(&b),progressive=bits(&b,1);
    if(b.failed) return format_error(detail,capacity,"H264 SPS geometry is truncated",ESP_ERR_INVALID_RESPONSE);
    if(!progressive) return format_error(detail,capacity,"Interlaced H264 is unsupported",ESP_ERR_NOT_SUPPORTED);
    if(wm>=30 || hm>=30) {
        if(detail && capacity) snprintf(detail,capacity,"H264 coded frame %llux%llu exceeds 480x480",
            (unsigned long long)((uint64_t)wm+1)*16,(unsigned long long)((uint64_t)hm+1)*16);
        return ESP_ERR_NOT_SUPPORTED;
    }
    bits(&b,1); /* direct_8x8_inference */
    uint32_t left=0,right=0,top=0,bottom=0;
    if(bits(&b,1)) { left=ue(&b); right=ue(&b); top=ue(&b); bottom=ue(&b); }
    if(b.failed || left>240 || right>240 || top>240 || bottom>240)
        return format_error(detail,capacity,"Invalid/truncated H264 SPS crop offsets",ESP_ERR_INVALID_RESPONSE);
    const uint32_t cw=(wm+1)*16,ch=(hm+1)*16;
    if(2*(left+right)>=cw || 2*(top+bottom)>=ch)
        return format_error(detail,capacity,"H264 SPS crop removes the complete frame",ESP_ERR_INVALID_RESPONSE);
    info->coded_width=(uint16_t)cw; info->coded_height=(uint16_t)ch;
    info->width=(uint16_t)(cw-2*(left+right)); info->height=(uint16_t)(ch-2*(top+bottom));
    info->crop_left=(uint16_t)(left*2); info->crop_top=(uint16_t)(top*2);
    return ESP_OK;
}
static esp_err_t parameter(mp4_demux_t *d,bool pps,unsigned index,const uint8_t **nal,size_t *bytes)
{
    MP4D_track_t *track=&d->mp4.track[d->video_track];
    const uint8_t *p=track->dsi; size_t remain=track->dsi_bytes;
    for(unsigned group=0;group<2;++group) {
        if(!remain) return ESP_ERR_INVALID_RESPONSE;
        unsigned count=*p++; --remain;
        for(unsigned i=0;i<count;++i) {
            if(remain<2) return ESP_ERR_INVALID_RESPONSE;
            size_t n=(size_t)p[0]*256+p[1]; p+=2; remain-=2;
            if(!n || n>remain) return ESP_ERR_INVALID_RESPONSE;
            if(group==(unsigned)pps && i==index) { *nal=p; *bytes=n; return ESP_OK; }
            p+=n; remain-=n;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
static esp_err_t open_media(FILE *file,bool (*cancel)(void *),void *context,mp4_demux_t **out,char *detail,size_t capacity,bool audio_only)
{
    if(!file || !out) return ESP_ERR_INVALID_ARG;
    *out=NULL;
    mp4_demux_t *d=heap_caps_calloc(1,sizeof(*d),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); if(!d) return ESP_ERR_NO_MEM;
    d->file=file; d->cancel=cancel; d->context=context; d->video_track=d->audio_track=-1;
    esp_err_t error=ESP_ERR_INVALID_RESPONSE;
    if(fseek(file,0,SEEK_END)) goto fail;
    d->file_size=ftell(file);
    if(d->file_size<16 || d->file_size>INT32_MAX) goto fail;
    for(int64_t pos=0;pos<d->file_size;) {
        uint8_t box[16];
        if(read_at(pos,box,8,d)) goto fail;
        uint64_t size=(uint64_t)box[0]<<24|(uint32_t)box[1]<<16|(uint32_t)box[2]<<8|box[3];
        if(!memcmp(box+4,"moof",4)) {
            if(!audio_only) { error=format_error(detail,capacity,"Fragmented MP4 video is unsupported",ESP_ERR_NOT_SUPPORTED); goto fail; }
            d->fragmented=true;
        }
        if(size==1) {
            if(read_at(pos+8,box+8,8,d)) goto fail;
            size=0; for(unsigned i=8;i<16;++i) size=(size<<8)|box[i];
            if(size<16) goto fail;
        } else if(!size) size=(uint64_t)(d->file_size-pos);
        if(size<8 || size>(uint64_t)(d->file_size-pos)) goto fail;
        pos+=(int64_t)size;
    }
    if(!MP4D_open(&d->mp4,read_at,d,d->file_size)) goto fail;
    if(d->mp4.track_count>8) { error=ESP_ERR_NOT_SUPPORTED; goto fail; }
    for(unsigned i=0;i<d->mp4.track_count;++i) {
        MP4D_track_t *t=&d->mp4.track[i];
        if(t->handler_type==MP4D_HANDLER_TYPE_VIDE && d->video_track<0) d->video_track=(int)i;
        if(t->handler_type==MP4D_HANDLER_TYPE_SOUN && d->audio_track<0) d->audio_track=(int)i;
    }
    if(audio_only) goto select_audio;
    if(d->video_track<0) { error=format_error(detail,capacity,"MP4 has no video track",ESP_ERR_NOT_SUPPORTED); goto fail; }
    MP4D_track_t *v=&d->mp4.track[d->video_track];
    if(v->object_type_indication!=MP4_OBJECT_TYPE_AVC) {
        if(detail && capacity) snprintf(detail,capacity,
            "Video object_type=0x%02x profile=%u constraints=0x%02x level=%u; H264 AVC required",
            v->object_type_indication,v->avc_profile,v->avc_constraints,v->avc_level);
        error=ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    if(v->avc_profile!=66) {
        const char *name=v->avc_profile==77 ? "Main" : v->avc_profile==100 ? "High" : "unsupported";
        if(detail && capacity) snprintf(detail,capacity,
            "H264 %s object=0x%02x profile=%u constraints=0x%02x level=%u; constrained baseline required",
            name,v->object_type_indication,v->avc_profile,v->avc_constraints,v->avc_level);
        error=ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    if(!(v->avc_constraints&0x40)) {
        if(detail && capacity) snprintf(detail,capacity,
            "H264 Baseline object=0x%02x profile=66 constraints=0x%02x level=%u lacks constraint_set1; not enabled",
            v->object_type_indication,v->avc_constraints,v->avc_level);
        error=ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    if(v->has_composition_offsets || v->unsupported_edit) {
        error=format_error(detail,capacity,"B-frame/reordered or complex edit timing unsupported",ESP_ERR_NOT_SUPPORTED); goto fail;
    }
    if(!v->timescale || !v->sample_count || v->sample_count>300000 || v->timestamp_count<v->sample_count) goto fail;
    const uint8_t *nal=NULL; size_t bytes=0;
    error=parameter(d,false,0,&nal,&bytes);
    if(error!=ESP_OK) goto fail;
    error=sps_info(nal,bytes,&d->info,detail,capacity);
    if(error!=ESP_OK) goto fail;
    if(v->SampleDescription.video.width!=d->info.width || v->SampleDescription.video.height!=d->info.height) {
        if(detail && capacity) snprintf(detail,capacity,"MP4 frame %ux%u differs from H264 SPS %ux%u",
            v->SampleDescription.video.width,v->SampleDescription.video.height,d->info.width,d->info.height);
        error=ESP_ERR_INVALID_RESPONSE; goto fail;
    }
    /* All alternate parameter sets must stay within the same bounded format. */
    for(unsigned i=0;parameter(d,false,i,&nal,&bytes)==ESP_OK;++i) {
        mp4_media_info_t next={0}; error=sps_info(nal,bytes,&next,detail,capacity);
        if(error!=ESP_OK) goto fail;
        if(next.width!=d->info.width || next.height!=d->info.height ||
           next.coded_width!=d->info.coded_width || next.coded_height!=d->info.coded_height ||
           next.crop_left!=d->info.crop_left || next.crop_top!=d->info.crop_top) {
            error=format_error(detail,capacity,"Alternate H264 SPS changes resolution/cropping",ESP_ERR_NOT_SUPPORTED); goto fail;
        }
    }
    error=parameter(d,true,0,&nal,&bytes); if(error!=ESP_OK) goto fail;
    for(unsigned i=0;parameter(d,true,i,&nal,&bytes)==ESP_OK;++i) {
        bits_t p; if((nal[0]&31)!=8 || !rbsp(&p,nal,bytes)) { error=ESP_ERR_INVALID_RESPONSE; goto fail; }
        if(ue(&p)>255 || ue(&p)>31) { error=ESP_ERR_INVALID_RESPONSE; goto fail; }
        if(bits(&p,1)) { error=format_error(detail,capacity,"H264 CABAC unsupported",ESP_ERR_NOT_SUPPORTED); goto fail; }
        bits(&p,1);
        if(ue(&p) || p.failed) { error=format_error(detail,capacity,"H264 slice groups unsupported",ESP_ERR_NOT_SUPPORTED); goto fail; }
    }
    d->info.nal_length_size=(uint8_t)v->avc_length_size;
    d->info.video_samples=v->sample_count; d->info.video_timescale=v->timescale; d->info.video_edit=v->edit_media_time;
    uint64_t duration=(uint64_t)v->timestamp[v->sample_count-1]+v->duration[v->sample_count-1];
    if(duration<=(uint64_t)v->edit_media_time) { error=ESP_ERR_INVALID_RESPONSE; goto fail; }
    duration-=(uint64_t)v->edit_media_time;
    d->info.duration_ms=(uint32_t)(duration*1000/v->timescale);
    if(!d->info.duration_ms || d->info.duration_ms>3U*3600U*1000U) { error=ESP_ERR_NOT_SUPPORTED; goto fail; }
    d->info.fps_x100=(uint32_t)((uint64_t)v->sample_count*100*v->timescale/duration);
select_audio:
    if(d->audio_track>=0) {
        MP4D_track_t *a=&d->mp4.track[d->audio_track];
        if(a->object_type_indication!=MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3 || a->unsupported_edit ||
           !a->timescale || (!d->fragmented && (!a->sample_count || a->sample_count>600000 || a->timestamp_count<a->sample_count)) ||
           !a->dsi || a->dsi_bytes>64) {
            if(detail && capacity) snprintf(detail,capacity,"Unsupported/corrupt MP4 audio (object_type=0x%02x, ASC bytes=%u)",a->object_type_indication,a->dsi_bytes);
            error=ESP_ERR_NOT_SUPPORTED; goto fail;
        }
        if(d->fragmented) {
            error=index_fragments(d);
            if(error!=ESP_OK) { format_error(detail,capacity,"Unsupported/corrupt fMP4 AAC fragments, offsets or discontinuous timeline",error); goto fail; }
        }
        uint64_t audio_end=d->fragmented ? (uint64_t)d->fragments[d->fragment_count-1].stamp+d->fragments[d->fragment_count-1].duration :
            (uint64_t)a->timestamp[a->sample_count-1]+a->duration[a->sample_count-1];
        if(audio_end<=(uint64_t)a->edit_media_time || (audio_end-(uint64_t)a->edit_media_time)*1000/a->timescale>3U*3600U*1000U) {
            error=format_error(detail,capacity,"Invalid/excessive MP4 audio duration",ESP_ERR_NOT_SUPPORTED); goto fail;
        }
        d->info.has_audio=true; d->info.audio_samples=d->fragmented ? d->fragment_count : a->sample_count; d->info.audio_timescale=a->timescale;
        d->info.audio_edit=a->edit_media_time; d->info.asc=a->dsi; d->info.asc_size=a->dsi_bytes;
        d->info.audio_duration_units=audio_end-(uint64_t)a->edit_media_time;
        d->info.audio_object_type=(uint8_t)a->object_type_indication;
        if(audio_only) d->info.duration_ms=(uint32_t)(d->info.audio_duration_units*1000/a->timescale);
    } else if(audio_only) {
        error=format_error(detail,capacity,"MP4/M4A has no audio track",ESP_ERR_NOT_SUPPORTED); goto fail;
    }
    *out=d; return ESP_OK;
fail:
    if(cancel && cancel(context)) error=ESP_ERR_INVALID_STATE;
    if(detail && capacity && !detail[0]) format_error(detail,capacity,"Invalid, truncated or excessive MP4 metadata",error);
    mp4_demux_close(d); return error;
}
esp_err_t mp4_demux_open(FILE *file,bool (*cancel)(void *),void *context,mp4_demux_t **out,char *detail,size_t capacity)
{ return open_media(file,cancel,context,out,detail,capacity,false); }
esp_err_t mp4_demux_open_audio(FILE *file,bool (*cancel)(void *),void *context,mp4_demux_t **out,char *detail,size_t capacity)
{ return open_media(file,cancel,context,out,detail,capacity,true); }
const mp4_media_info_t *mp4_demux_info(mp4_demux_t *d) { return d ? &d->info : NULL; }
esp_err_t mp4_demux_sample(mp4_demux_t *d,bool audio,uint32_t index,mp4_sample_t *sample)
{
    if(!d || !sample) return ESP_ERR_INVALID_ARG;
    if(audio && d->fragmented) {
        if(index>=d->fragment_count) return ESP_ERR_NOT_FOUND;
        struct fragment_sample *f=&d->fragments[index];
        *sample=(mp4_sample_t){.offset=f->offset,.size=f->size,.duration=f->duration,
            .pts_us=((int64_t)f->stamp-d->info.audio_edit)*1000000/d->info.audio_timescale};
        return ESP_OK;
    }
    int track=audio ? d->audio_track : d->video_track;
    if(track<0) return ESP_ERR_NOT_FOUND;
    MP4D_track_t *t=&d->mp4.track[track];
    if(index>=t->sample_count) return ESP_ERR_NOT_FOUND;
    unsigned size=0,stamp=0,duration=0;
    uint64_t offset=MP4D_frame_offset(&d->mp4,(unsigned)track,index,&size,&stamp,&duration);
    if(!size || size>512U*1024U || !duration || offset>(uint64_t)d->file_size || size>(uint64_t)d->file_size-offset)
        return ESP_ERR_INVALID_RESPONSE;
    if(index && stamp<t->timestamp[index-1]) return ESP_ERR_NOT_SUPPORTED;
    *sample=(mp4_sample_t){.offset=offset,.size=size,.duration=duration,
        .pts_us=((int64_t)stamp-t->edit_media_time)*1000000/t->timescale};
    return ESP_OK;
}
esp_err_t mp4_demux_read(FILE *file,const mp4_sample_t *sample,uint8_t *buffer,size_t capacity)
{
    if(!file || !sample || !buffer) return ESP_ERR_INVALID_ARG;
    if(sample->size>capacity || sample->offset>LONG_MAX) return ESP_ERR_INVALID_SIZE;
    if(fseek(file,(long)sample->offset,SEEK_SET) || fread(buffer,1,sample->size,file)!=sample->size) return ESP_FAIL;
    return ESP_OK;
}
esp_err_t mp4_demux_headers(mp4_demux_t *d,uint8_t *out,size_t capacity,size_t *used)
{
    if(!d || !out || !used) return ESP_ERR_INVALID_ARG;
    *used=0;
    for(unsigned group=0;group<2;++group) for(unsigned i=0;;++i) {
        const uint8_t *nal; size_t bytes;
        esp_err_t e=parameter(d,group!=0,i,&nal,&bytes);
        if(e==ESP_ERR_NOT_FOUND) break;
        if(e!=ESP_OK) return e;
        if(capacity-*used<4 || bytes>capacity-*used-4) return ESP_ERR_INVALID_SIZE;
        memcpy(out+*used,"\0\0\0\1",4); *used+=4; memcpy(out+*used,nal,bytes); *used+=bytes;
    }
    return ESP_OK;
}
esp_err_t mp4_annexb(const uint8_t *in,size_t bytes,uint8_t prefix,unsigned max_nals,uint8_t *out,size_t capacity,size_t *used)
{
    if(!in || !out || !used || prefix<1 || prefix>4 || prefix==3) return ESP_ERR_INVALID_ARG;
    *used=0; unsigned nals=0;
    while(bytes) {
        if(bytes<prefix || ++nals>max_nals) return ESP_ERR_INVALID_RESPONSE;
        uint32_t size=0; for(unsigned i=0;i<prefix;++i) size=(size<<8)|*in++;
        bytes-=prefix;
        if(!size || size>bytes || capacity-*used<4 || size>capacity-*used-4) return ESP_ERR_INVALID_SIZE;
        unsigned type=in[0]&31;
        if(type==1 || type==5) {
            bits_t b; if(!rbsp(&b,in,size>sizeof(b.data) ? sizeof(b.data) : size)) return ESP_ERR_INVALID_RESPONSE;
            ue(&b); uint32_t slice=ue(&b);
            if(b.failed || slice>9 || (slice%5!=0 && slice%5!=2)) return ESP_ERR_NOT_SUPPORTED;
        } else if(type==7) {
            mp4_media_info_t format={0}; esp_err_t e=sps_info(in,size,&format,NULL,0); if(e!=ESP_OK) return e;
        } else if(type==8) {
            bits_t p; if(!rbsp(&p,in,size)) return ESP_ERR_INVALID_RESPONSE;
            if(ue(&p)>255 || ue(&p)>31 || bits(&p,1)) return ESP_ERR_NOT_SUPPORTED;
            bits(&p,1); if(ue(&p) || p.failed) return ESP_ERR_NOT_SUPPORTED;
        } else if(type!=6 && type!=9 && type!=12) return ESP_ERR_NOT_SUPPORTED;
        memcpy(out+*used,"\0\0\0\1",4); *used+=4;
        memcpy(out+*used,in,size); *used+=size; in+=size; bytes-=size;
    }
    return *used ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
void mp4_demux_close(mp4_demux_t *d) { if(d) { MP4D_close(&d->mp4); index_free(d->fragments); index_free(d->mdats); free(d); } }
