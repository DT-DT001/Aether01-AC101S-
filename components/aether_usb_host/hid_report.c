#include "hid_report.h"
#include "aether_usb_host.h"
#include <limits.h>
#include <string.h>
typedef struct { uint32_t page,size,count,id; int32_t min,max; } globals_t;
typedef struct { uint32_t usages[32],lo,hi; unsigned count; bool range; } locals_t;
static uint32_t value(const uint8_t *p,unsigned n) { uint32_t v=0;for(unsigned i=0;i<n;++i)v|=(uint32_t)p[i]<<(i*8);return v; }
static int32_t signed_value(uint32_t v,unsigned n) { if(n&&n<4&&(v&(1U<<(n*8-1))))v|=UINT32_MAX<<(n*8);return (int32_t)v; }
static uint32_t usage(const locals_t *l,unsigned i) {
    if(i<l->count)return l->usages[i];
    if(l->range&&l->lo<=l->hi&&i<=l->hi-l->lo)return l->lo+i;
    return l->count?l->usages[l->count-1]:0;
}
static bool add(aether_hid_report_t *r,const globals_t *g,uint32_t offset,
                uint16_t use,uint16_t count,hid_field_kind_t kind) {
    if(r->count==AETHER_HID_FIELDS||offset>8192||g->size==0||g->size>32)return false;
    r->fields[r->count++]=(aether_hid_field_t){.offset=(uint16_t)offset,.count=count,.usage=use,
        .size=(uint8_t)g->size,.id=(uint8_t)g->id,.kind=(uint8_t)kind,.min=g->min,.max=g->max};
    unsigned j=0;for(;j<r->reports;++j)if(r->report_ids[j]==g->id)break;
    if(j==r->reports){if(j==AETHER_HID_REPORTS)return false;r->report_ids[r->reports++]=(uint8_t)g->id;}
    if(kind==HID_BUTTON&&use>r->button_max[j])r->button_max[j]=use;
    if(kind==HID_KEY_ARRAY||kind==HID_KEY_BITMAP||kind==HID_KEY)r->has_keyboard=true;
    if(kind==HID_MOUSE_X||kind==HID_MOUSE_Y||kind==HID_MOUSE_WHEEL||kind==HID_MOUSE_BUTTON)r->has_mouse=true;
    return true;
}
aether_hid_parse_result_t aether_hid_parse_ex(aether_hid_report_t *r,const uint8_t *d,size_t n) {
    if(!r||!d||!n)return AETHER_HID_PARSE_MALFORMED;
    if(n>4096)return AETHER_HID_PARSE_LIMIT;
    memset(r,0,sizeof(*r));globals_t g={0},stack[8];unsigned depth=0,collections=0;uint8_t applications[16]={0},allowed=0;locals_t l={0};uint32_t offsets[256]={0};
    for(size_t at=0;at<n;){
        const uint8_t p=d[at++];if(p==0xfe){if(n-at<2)return AETHER_HID_PARSE_MALFORMED;unsigned z=d[at];at+=2;if(z>n-at)return AETHER_HID_PARSE_MALFORMED;at+=z;continue;}
        unsigned z=p&3U;if(z==3)z=4;if(z>n-at)return AETHER_HID_PARSE_MALFORMED;
        uint32_t v=value(d+at,z);int32_t sv=signed_value(v,z);at+=z;
        const unsigned type=(p>>2)&3U,tag=p>>4;
        if(type==1){switch(tag){case 0:g.page=v;break;case 1:g.min=sv;break;
            case 2:g.max=g.min<0?sv:(int32_t)v;break;case 7:g.size=v;break;
            case 8:if(!v||v>255)return AETHER_HID_PARSE_MALFORMED;g.id=v;r->ids=true;break;
            case 9:g.count=v;break;case 10:if(depth==8)return AETHER_HID_PARSE_LIMIT;stack[depth++]=g;break;
            case 11:if(!depth)return AETHER_HID_PARSE_MALFORMED;g=stack[--depth];break;default:break;}continue;}
        if(type==2){if(tag==0){if(l.count==32)return AETHER_HID_PARSE_LIMIT;l.usages[l.count++]=v;}
            else if(tag==1){l.lo=v;l.range=true;}else if(tag==2){l.hi=v;l.range=true;}
            continue;}
        if(type!=0)continue;
        if(tag==10){
            if(collections==16)return AETHER_HID_PARSE_LIMIT;
            applications[collections++]=allowed;
            if(v==1){uint32_t u=usage(&l,0),page=u>65535?u>>16:g.page;u&=65535;
                allowed=page==1&&(u==1||u==2)?2:
                    ((page==1&&(u==4||u==5||u==6||u==0x80))||(page==0x0c&&u==1));}
        } else if(tag==12){if(!collections)return AETHER_HID_PARSE_MALFORMED;allowed=applications[--collections];}
        if(tag==8){
            if(!g.size||!g.count)return AETHER_HID_PARSE_MALFORMED;
            if(g.count>256||g.size>8192||g.count>(8192-offsets[g.id])/g.size)
                return AETHER_HID_PARSE_LIMIT;
            const uint32_t base=offsets[g.id];offsets[g.id]+=g.size*g.count;
            if(allowed&&!(v&1U)){
                if(g.page==7&&!(v&2U)){if(!add(r,&g,base,0,(uint16_t)g.count,HID_KEY_ARRAY))return AETHER_HID_PARSE_LIMIT;}
                else if(g.page==7&&l.range&&l.count==0){if(!add(r,&g,base,(uint16_t)l.lo,(uint16_t)g.count,HID_KEY_BITMAP))return AETHER_HID_PARSE_LIMIT;}
                else if(v&2U)for(unsigned i=0;i<g.count;++i){
                    uint32_t u=usage(&l,i);uint32_t page=u>65535?u>>16:g.page;u&=65535;
                    hid_field_kind_t kind;
                    if(page==7)kind=HID_KEY;else if(page==9)kind=allowed==2?HID_MOUSE_BUTTON:HID_BUTTON;
                    else if(page==1&&u==0x30){if(allowed==2){if(!(v&4U))continue;kind=HID_MOUSE_X;}else kind=HID_X;}
                    else if(page==1&&u==0x31){if(allowed==2){if(!(v&4U))continue;kind=HID_MOUSE_Y;}else kind=HID_Y;}
                    else if(page==1&&u==0x38&&allowed==2&&(v&4U))kind=HID_MOUSE_WHEEL;
                    else if(page==1&&u==0x39)kind=HID_HAT;
                    else if(page==1&&u>=0x90&&u<=0x93)kind=HID_DPAD;
                    else if((page==1&&(u==0x85||u==0x86))||(page==0x0c&&u==0x223))kind=HID_EXIT;else continue;
                    if(!add(r,&g,base+i*g.size,(uint16_t)u,1,kind))return AETHER_HID_PARSE_LIMIT;
                }
            }
        }
        memset(&l,0,sizeof l); /* Locals reset after every Main item. */
    }
    if(depth||collections)return AETHER_HID_PARSE_MALFORMED;
    return r->count ? AETHER_HID_PARSE_MAPPED : AETHER_HID_PARSE_UNMAPPED;
}
bool aether_hid_parse(aether_hid_report_t *r,const uint8_t *d,size_t n)
{
    return aether_hid_parse_ex(r,d,n)==AETHER_HID_PARSE_MAPPED;
}
static uint16_t key(uint32_t k) { switch(k){
    case 0x52:case 0x1a:return AETHER_HID_UP;case 0x51:case 0x16:return AETHER_HID_DOWN;
    case 0x50:case 0x04:return AETHER_HID_LEFT;case 0x4f:case 0x07:return AETHER_HID_RIGHT;
    case 0x1d:return AETHER_HID_A;case 0x1b:return AETHER_HID_B;case 0xe5:case 0xe1:return AETHER_HID_SELECT;
    case 0x28:return AETHER_HID_START;case 0x29:return AETHER_HID_EXIT;
    case 0x13:return AETHER_HID_PAUSE;case 0x15:return AETHER_HID_RESET;default:return 0;} }
static uint16_t button(uint32_t b,uint16_t maximum) {
    if(b==1)return AETHER_HID_A;
    if(b==2)return AETHER_HID_B;
    if(maximum<=4){if(b==3)return AETHER_HID_SELECT;if(b==4)return AETHER_HID_START;}
    else {if(b==9)return AETHER_HID_SELECT;if(b==10)return AETHER_HID_START;}
    /* Button numbers do not identify L3/R3/Home. Never guess destructive actions. */
    return 0;
}
static bool bits(const uint8_t *p,size_t n,uint32_t at,unsigned count,uint32_t *v) {
    if(count>32||at>n*8||count>n*8-at)return false;
    *v=0;
    for(unsigned i=0;i<count;++i)*v|=(uint32_t)((p[(at+i)/8]>>((at+i)%8))&1U)<<i;
    return true;
}
void aether_hid_release(aether_hid_report_t *r)
{
    if(!r)return;
    memset(r->states,0,sizeof r->states);memset(r->non_keyboard_states,0,sizeof r->non_keyboard_states);memset(r->keyboard_states,0,sizeof r->keyboard_states);
    memset(r->mouse_states,0,sizeof r->mouse_states);
}
static void key_bit(uint8_t *keys,uint32_t use) { if(use&&use<256)keys[use/8]|=(uint8_t)(1U<<(use%8)); }
static int16_t clamp_delta(int64_t value) { return (int16_t)(value<INT16_MIN?INT16_MIN:value>INT16_MAX?INT16_MAX:value); }
static void input_snapshot(const aether_hid_report_t *r,aether_hid_input_t *input)
{
    memset(input,0,sizeof(*input));input->keyboard=r->has_keyboard;input->mouse=r->has_mouse;
    for(unsigned n=0;n<r->reports;++n){
        for(unsigned b=0;b<32;++b)input->keys[b]|=r->keyboard_states[n][b];
        input->buttons|=r->mouse_states[n];
        input->non_keyboard_buttons|=r->non_keyboard_states[n];
    }
}
bool aether_hid_decode_input(aether_hid_report_t *r,const uint8_t *p,size_t n,uint16_t *out,aether_hid_input_t *input) {
    if(!r||!p||!out||!input)return false;
    if(!n){aether_hid_release(r);*out=0;input_snapshot(r,input);return true;}
    uint8_t id=0;
    if(r->ids){id=*p++;--n;}
    unsigned report=0;for(;report<r->reports;++report)if(r->report_ids[report]==id)break;
    if(report==r->reports)return false;
    uint16_t state=0,keyboard_buttons=0;uint8_t keys[32]={0},mouse_buttons=0;int64_t dx=0,dy=0,wheel=0;
    for(unsigned j=0;j<r->count;++j){const aether_hid_field_t *f=&r->fields[j];if(f->id!=id)continue;
        for(unsigned i=0;i<f->count;++i){uint32_t v;
            if(!bits(p,n,f->offset+i*f->size,f->size,&v))goto invalid;
            int32_t s=(int32_t)v;if(f->min<0&&f->size<32&&(v&(1U<<(f->size-1))))s=(int32_t)(v|(UINT32_MAX<<f->size));
            switch(f->kind){
            case HID_KEY_ARRAY:if(v>=1&&v<=3)goto invalid;keyboard_buttons|=key(v);key_bit(keys,v);break;
            case HID_KEY_BITMAP:if(v){keyboard_buttons|=key(f->usage+i);key_bit(keys,f->usage+i);}break;
            case HID_KEY:if(v){keyboard_buttons|=key(f->usage);key_bit(keys,f->usage);}break;
            case HID_BUTTON:if(v)state|=button(f->usage,r->button_max[report]);break;
            case HID_EXIT:if(v)state|=AETHER_HID_EXIT;break;
            case HID_X:case HID_Y:{int64_t span=(int64_t)f->max-f->min;if(span<=0)break;
                /* Nullable axes can encode an out-of-range value (for example
                 * -128 with logical range -127..127). It is not full travel. */
                if(s<f->min||s>f->max)break;
                if(s<f->min+span/4)state|=f->kind==HID_X?AETHER_HID_LEFT:AETHER_HID_UP;
                if(s>f->max-span/4)state|=f->kind==HID_X?AETHER_HID_RIGHT:AETHER_HID_DOWN;
                break;}
            case HID_HAT:{int64_t h=(int64_t)s-f->min;if(s<f->min||s>f->max||h<0||h>7)break;
                /* Four-position hats use quarter turns; eight-position hats
                 * include diagonals. Both zero- and one-based ranges work. */
                if((int64_t)f->max-f->min==3)h*=2;
                static const uint16_t hats[8]={AETHER_HID_UP,AETHER_HID_UP|AETHER_HID_RIGHT,AETHER_HID_RIGHT,
                    AETHER_HID_DOWN|AETHER_HID_RIGHT,AETHER_HID_DOWN,AETHER_HID_DOWN|AETHER_HID_LEFT,
                    AETHER_HID_LEFT,AETHER_HID_UP|AETHER_HID_LEFT};state|=hats[h];break;}
            case HID_DPAD:if(v){static const uint16_t dp[4]={AETHER_HID_UP,AETHER_HID_DOWN,AETHER_HID_RIGHT,AETHER_HID_LEFT};state|=dp[f->usage-0x90];}break;
            case HID_MOUSE_X:dx+=s;break;
            case HID_MOUSE_Y:dy+=s;break;
            case HID_MOUSE_WHEEL:wheel+=s;break;
            case HID_MOUSE_BUTTON:if(v&&f->usage>=1&&f->usage<=8)mouse_buttons|=(uint8_t)(1U<<(f->usage-1));break;
            }
        }
    }
    r->non_keyboard_states[report]=state;
    r->states[report]=state|keyboard_buttons;memcpy(r->keyboard_states[report],keys,sizeof keys);r->mouse_states[report]=mouse_buttons;
    *out=0;for(unsigned i=0;i<r->reports;++i)*out|=r->states[i];
    input_snapshot(r,input);input->dx=clamp_delta(dx);input->dy=clamp_delta(dy);input->wheel=clamp_delta(wheel);return true;
invalid:
    aether_hid_release(r);*out=0;input_snapshot(r,input);return true;
}
bool aether_hid_decode(aether_hid_report_t *r,const uint8_t *p,size_t n,uint16_t *out)
{
    aether_hid_input_t input;return aether_hid_decode_input(r,p,n,out,&input);
}
