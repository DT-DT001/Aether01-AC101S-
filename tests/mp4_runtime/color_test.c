#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_INVALID_RESPONSE 3
typedef int esp_err_t;
typedef struct {unsigned width,height;} esp_h264_resolution_t;
typedef struct {esp_h264_resolution_t size;int error;} fake_decoder_t;
typedef fake_decoder_t *esp_h264_dec_handle_t;
typedef esp_h264_dec_handle_t esp_h264_dec_param_sw_handle_t;
typedef struct {const uint8_t*outbuf;size_t out_size;} esp_h264_dec_out_frame_t;
typedef struct {uint16_t*pixels;} player_t;
typedef struct {
    struct {unsigned width,height,coded_width,coded_height,crop_left,crop_top;} info;
    player_t*h;
    unsigned cancel_on,checks;
} session_t;
static esp_err_t h264_error(int error){return error;}
static int esp_h264_dec_sw_get_param_hd(esp_h264_dec_handle_t d,esp_h264_dec_param_sw_handle_t*p){*p=d;return d->error;}
static int esp_h264_dec_get_resolution(esp_h264_dec_param_sw_handle_t d,esp_h264_resolution_t*r){*r=d->size;return d->error;}
static bool cancelled(session_t*s){return ++s->checks==s->cancel_on;}
#include "production_color.inc"

static uint32_t seed=0x9e3779b9;
static unsigned tests;
static uint32_t random32(void){seed=seed*1664525U+1013904223U;return seed;}
static void compare(unsigned cw,unsigned ch,unsigned x,unsigned y,unsigned w,unsigned h,int uniform,unsigned cancel_on,bool display_resolution)
{
    enum { GUARD=64 };
    assert(cw && ch && !(cw&1) && !(ch&1) && x+w<=cw && y+h<=ch);
    const size_t bytes=(size_t)cw*ch*3/2,pixels=(size_t)w*h;
    uint8_t*input=malloc(bytes+GUARD*2),*before=malloc(bytes+GUARD*2);
    uint16_t*a=malloc((pixels+GUARD*2)*sizeof(*a)),*b=malloc((pixels+GUARD*2)*sizeof(*b));
    assert(input&&before&&a&&b);memset(input,0x6d,bytes+GUARD*2);
    uint8_t*data=input+GUARD;
    for(size_t i=0;i<bytes;++i)data[i]=(uint8_t)(random32()>>24);
    if(uniform>=0){
        memset(data,uniform&255,(size_t)cw*ch);
        memset(data+(size_t)cw*ch,(uniform>>8)&255,(size_t)cw*ch/4);
        memset(data+(size_t)cw*ch*5/4,(uniform>>16)&255,(size_t)cw*ch/4);
    }
    memcpy(before,input,bytes+GUARD*2);
    for(size_t i=0;i<pixels+GUARD*2;++i)a[i]=b[i]=0xa55a;
    player_t p0={a+GUARD},p1={b+GUARD};
    session_t s0={.info={w,h,cw,ch,x,y},.h=&p0,.cancel_on=cancel_on},s1=s0;s1.h=&p1;
    fake_decoder_t dec={{display_resolution?w:cw,display_resolution?h:ch},0};
    esp_h264_dec_out_frame_t pic={data,bytes};
    int e0=convert_frame_scalar(&s0,(esp_h264_dec_handle_t)&dec,&pic);
    int e1=convert_frame(&s1,(esp_h264_dec_handle_t)&dec,&pic);
    if(e0!=e1||memcmp(a,b,(pixels+GUARD*2)*sizeof(*a))||s0.checks!=s1.checks){
        fprintf(stderr,"mismatch coded%ux%u crop%u,%u display%ux%u cancel%u res%u e%d/%d checks%u/%u\n",cw,ch,x,y,w,h,cancel_on,display_resolution,e0,e1,s0.checks,s1.checks);abort();
    }
    assert(!memcmp(input,before,bytes+GUARD*2));
    for(unsigned i=0;i<GUARD;++i)assert(a[i]==0xa55a&&a[GUARD+pixels+i]==0xa55a&&b[i]==0xa55a&&b[GUARD+pixels+i]==0xa55a);
    if(cancel_on && cancel_on<=((h-1)/16+1)) {
        assert(e0==ESP_ERR_INVALID_STATE);
        const size_t completed=(size_t)(cancel_on-1)*16*w;
        for(size_t i=completed;i<pixels;++i)assert(a[GUARD+i]==0xa55a);
    }else assert(e0==ESP_OK);
    free(input);free(before);free(a);free(b);++tests;
}
int main(void)
{
    const unsigned ys[]={0,1,15,16,17,128,234,235,236,254,255};
    const unsigned cs[]={0,1,16,127,128,129,240,254,255};
    for(unsigned i=0;i<sizeof ys/sizeof ys[0];++i)
      for(unsigned j=0;j<sizeof cs/sizeof cs[0];++j)
        for(unsigned k=0;k<sizeof cs/sizeof cs[0];++k)
          compare(8,8,0,0,8,8,(int)(ys[i]|cs[j]<<8|cs[k]<<16),0,false);
    puts("PASS full-range luma/chroma extremes retain clipping, rounding and RGB565 packing exactly");
    for(unsigned i=0;i<2500;++i){
        const unsigned cw=2*(1+random32()%240),ch=2*(1+random32()%240);
        unsigned x=2*(random32()%(cw/2)),y=2*(random32()%(ch/2));
        unsigned w=2*(1+random32()%((cw-x)/2)),h=2*(1+random32()%((ch-y)/2));
        compare(cw,ch,x,y,w,h,-1,0,false);
    }
    puts("PASS2500 deterministic random even geometries/strides/crops and all480px source bounds match production scalar pixel-for-pixel");
    for(unsigned i=0;i<800;++i){
        unsigned x=random32()%31,y=random32()%31,w=1+random32()%(64-x),h=1+random32()%(64-y);
        compare(64,64,x,y,w,h,-1,0,false);
    }
    compare(480,480,0,0,480,480,-1,0,false);
    compare(480,480,4,2,320,240,-1,0,true);
    puts("PASS odd crop/dimensions use scalar fallback; full-size and decoder-already-cropped resolution conventions remain exact");
    for(unsigned stop=1;stop<=31;++stop){
        compare(480,480,0,0,480,480,-1,stop,false);
        compare(480,480,3,5,471,473,-1,stop,false);
    }
    printf("PASS%u total cases:16-row cancellation checks and partial output exact, input immutable, input/output prefix/suffix canaries intact; no performance claim\n",tests);
}
