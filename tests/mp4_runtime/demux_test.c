#include "mp4_demux.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *heap_caps_malloc(size_t size,unsigned caps) { (void)caps; return malloc(size); }
void *heap_caps_realloc(void *ptr,size_t size,unsigned caps) { (void)caps; return realloc(ptr,size); }
void *heap_caps_calloc(size_t count,size_t size,unsigned caps) { (void)caps; return calloc(count,size); }
static bool stop_after_yield,was_yielded;
void vTaskDelay(unsigned ticks) { (void)ticks; if(stop_after_yield) was_yielded=true; }
static bool cancel(void *context) { return context!=NULL; }
static bool cancel_yield(void *context) { (void)context; return was_yielded; }
static void reason(const unsigned char *bytes,size_t length,esp_err_t expected,const char *first,const char *second)
{
    FILE *file=tmpfile(); assert(file); assert(fwrite(bytes,1,length,file)==length);
    mp4_demux_t *demux=NULL; char detail[112]={0};
    assert(mp4_demux_open(file,NULL,NULL,&demux,detail,sizeof detail)==expected && !demux);
    assert(strstr(detail,first) && (!second || strstr(detail,second)));
    fclose(file);
}
static void probe(const unsigned char *bytes,size_t length,bool audio_only)
{
    FILE *file=tmpfile(); assert(file);
    assert(fwrite(bytes,1,length,file)==length);
    mp4_demux_t *demux=NULL; char detail[112]={0};
    esp_err_t result=audio_only ? mp4_demux_open_audio(file,NULL,NULL,&demux,detail,sizeof(detail)) : mp4_demux_open(file,NULL,NULL,&demux,detail,sizeof(detail));
    if(result==ESP_OK) {
        const mp4_media_info_t *info=mp4_demux_info(demux);
        for(unsigned audio=0;audio<2;++audio) {
            unsigned count=audio ? info->audio_samples : info->video_samples;
            for(unsigned i=0;i<count && i<1000;++i) { mp4_sample_t s; if(mp4_demux_sample(demux,audio!=0,i,&s)!=ESP_OK) break; }
        }
    }
    mp4_demux_close(demux); fclose(file);
}
int main(int argc,char **argv)
{
    assert(argc==2); char path[512],detail[112];
    for(unsigned audio=0;audio<2;++audio) {
        snprintf(path,sizeof(path),"%s/%s",argv[1],audio ? "baseline-aac.mp4" : "baseline.mp4");
        FILE *file=fopen(path,"rb"); assert(file); mp4_demux_t *demux=NULL; detail[0]=0;
        esp_err_t error=mp4_demux_open(file,NULL,NULL,&demux,detail,sizeof(detail));
        if(error) fprintf(stderr,"open %s failed %x: %s\n",path,error,detail);
        assert(error==ESP_OK); const mp4_media_info_t *info=mp4_demux_info(demux);
        assert(info->width==32 && info->height==32 && info->video_samples==12 && info->fps_x100==1200);
        assert(info->duration_ms==1000 && info->has_audio==(bool)audio);
        uint8_t headers[4096],packet[8192],annex[8192]; size_t bytes=0;
        assert(mp4_demux_headers(demux,headers,sizeof(headers),&bytes)==ESP_OK && bytes>8);
        for(unsigned i=0;i<12;++i) {
            mp4_sample_t sample; assert(mp4_demux_sample(demux,false,i,&sample)==ESP_OK);
            assert(sample.pts_us==(int64_t)i*1000000/12);
            assert(mp4_demux_read(file,&sample,packet,sizeof(packet))==ESP_OK);
            assert(mp4_annexb(packet,sample.size,info->nal_length_size,256,annex,sizeof(annex),&bytes)==ESP_OK);
            assert(annex[3]==1 && (annex[4]&31)==5);
        }
        mp4_sample_t sample; assert(mp4_demux_sample(demux,false,12,&sample)==ESP_ERR_NOT_FOUND);
        if(audio) {
            assert(info->audio_samples==48 && info->asc_size==2 && info->asc[0]==0x11);
            assert(mp4_demux_sample(demux,true,30,&sample)==ESP_OK && sample.pts_us==640000);
            assert(mp4_demux_sample(demux,true,3,&sample)==ESP_OK && sample.pts_us==64000);
        }
        mp4_demux_close(demux); fclose(file);
    }
    puts("PASS real MP4 tracks/avcC/sample offsets/native timestamps/AAC ASC and Annex-B access units");
    const char *bad[]={"high-profile.mp4","fragmented.mp4"};
    for(unsigned i=0;i<2;++i) {
        snprintf(path,sizeof(path),"%s/%s",argv[1],bad[i]); FILE *file=fopen(path,"rb"); assert(file);
        mp4_demux_t *demux=NULL; detail[0]=0;
        assert(mp4_demux_open(file,NULL,NULL,&demux,detail,sizeof(detail))==ESP_ERR_NOT_SUPPORTED && !demux && detail[0]); fclose(file);
    }
    snprintf(path,sizeof(path),"%s/baseline.mp4",argv[1]); FILE *file=fopen(path,"rb"); assert(file);
    mp4_demux_t *demux=NULL; detail[0]=0;
    assert(mp4_demux_open(file,cancel,(void *)1,&demux,detail,sizeof(detail))==ESP_ERR_INVALID_STATE); fclose(file);
    puts("PASS unsupported High/fMP4 and cancellation fail explicitly without an output handle");
    file=fopen(path,"rb"); assert(file); assert(!fseek(file,0,SEEK_END));
    size_t length=(size_t)ftell(file); rewind(file);
    unsigned char *original=malloc(length),*mutated=malloc(length); assert(original && mutated);
    assert(fread(original,1,length,file)==length); fclose(file);
    for(size_t cut=0;cut<length;cut+=113) probe(original,cut,false);
    size_t avcc=0;
    for(size_t i=0;i+4<length;++i) if(!memcmp(original+i,"avcC",4)) { avcc=i; break; }
    assert(avcc && avcc+12<length);
    memcpy(mutated,original,length); mutated[avcc+5]=77;
    reason(mutated,length,ESP_ERR_NOT_SUPPORTED,"Main object=0x21 profile=77","constraints=0xc0 level=12");
    memcpy(mutated,original,length); mutated[avcc+5]=100;
    reason(mutated,length,ESP_ERR_NOT_SUPPORTED,"High object=0x21 profile=100","constraints=0xc0 level=12");
    memcpy(mutated,original,length); mutated[avcc+6]=0;
    reason(mutated,length,ESP_ERR_NOT_SUPPORTED,"profile=66 constraints=0x00","lacks constraint_set1");
    memcpy(mutated,original,length); memcpy(mutated+avcc,"zzzz",4);
    reason(mutated,length,ESP_ERR_NOT_SUPPORTED,"object_type=0x00","H264 AVC required");
    memcpy(mutated,original,length); mutated[avcc+12]=0x68;
    reason(mutated,length,ESP_ERR_INVALID_RESPONSE,"SPS missing, truncated or excessive",NULL);
    puts("PASS Main/High, missing baseline constraint, non-AVC and corrupt SPS expose distinct actual parameters without changing supported formats");
    memcpy(mutated,original,length); mutated[avcc+10]=mutated[avcc+11]=255;
    probe(mutated,length,false); /* A declared SPS longer than the avcC box. */
    unsigned seed=0x734210a1;
    for(unsigned test=0;test<256;++test) {
        memcpy(mutated,original,length);
        for(unsigned mutation=0;mutation<4;++mutation) {
            seed=seed*1664525U+1013904223U;
            size_t offset=length-((seed>>8)%((length<2500) ? length : 2500))-1;
            mutated[offset]^=(unsigned char)(seed|1U);
        }
        probe(mutated,length,false);
    }
    free(mutated); free(original);
    puts("PASS truncated containers, oversized avcC parameter length and deterministic metadata mutations remain bounded and memory-safe");
    snprintf(path,sizeof(path),"%s/iso5-long.m4a",argv[1]); file=fopen(path,"rb"); assert(file);
    stop_after_yield=true; was_yielded=false; detail[0]=0; demux=NULL;
    assert(mp4_demux_open_audio(file,cancel_yield,NULL,&demux,detail,sizeof(detail))==ESP_ERR_INVALID_STATE && !demux && was_yielded);
    stop_after_yield=false; fclose(file);
    snprintf(path,sizeof(path),"%s/iso5-fragmented.mp3",argv[1]); file=fopen(path,"rb"); assert(file); assert(!fseek(file,0,SEEK_END));
    length=(size_t)ftell(file); rewind(file); original=malloc(length); mutated=malloc(length); assert(original && mutated);
    assert(fread(original,1,length,file)==length); fclose(file);
    for(size_t cut=0;cut<length;cut+=17) probe(original,cut,true);
    for(unsigned test=0;test<256;++test) {
        memcpy(mutated,original,length);
        for(unsigned mutation=0;mutation<4;++mutation) { seed=seed*1664525U+1013904223U; mutated[(seed>>8)%length]^=(unsigned char)(seed|1U); }
        probe(mutated,length,true);
    }
    free(mutated); free(original);
    puts("PASS default-valued fMP4 sample indexing yields/cancels; fMP4 truncation and metadata mutations are memory-safe");
    return 0;
}
