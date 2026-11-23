#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/aether_video_player/aether_video_player.c"

struct aether_mjpeg_player_t { atomic_int state; uint8_t fps; bool screen_paused; };
struct aether_mp4_player { atomic_int state; bool screen_paused; };
static atomic_llong clock_us;
static unsigned lock_cost, wait_cost[2], wait_budget[2], deletes[2], stops[2];
static bool fail_mp4_new, fail_mp4_del;
static uint32_t last_seek; static unsigned seek_calls;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t signal_gate = PTHREAD_COND_INITIALIZER;
static int block_at;
static bool entered, resumed;
int64_t esp_timer_get_time(void) { return clock_us; }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{pthread_mutex_t *m=malloc(sizeof(*m));assert(m&&!pthread_mutex_init(m,NULL));return m;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t ms)
{
    if(ms&&lock_cost){unsigned cost=lock_cost;lock_cost=0;clock_us+=(int64_t)(cost<ms?cost:ms)*1000;if(cost>ms)return pdFALSE;}
    return (ms?pthread_mutex_lock(m):pthread_mutex_trylock(m))==0;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {return !pthread_mutex_unlock(m);}
void vSemaphoreDelete(SemaphoreHandle_t m) {assert(!pthread_mutex_destroy(m));free(m);}
static void block_here(int point)
{
    pthread_mutex_lock(&gate);
    if(block_at==point){entered=true;pthread_cond_broadcast(&signal_gate);while(!resumed)pthread_cond_wait(&signal_gate,&gate);}
    pthread_mutex_unlock(&gate);
}
esp_err_t aether_mjpeg_player_new(aether_storage_handle_t s,aether_mjpeg_player_handle_t *h)
{assert(s);*h=calloc(1,sizeof(**h));return *h?ESP_OK:ESP_ERR_NO_MEM;}
esp_err_t aether_mp4_player_new(aether_storage_handle_t s,ac101s_audio_handle_t a,aether_mp4_player_handle_t *h)
{(void)a;assert(s);if(fail_mp4_new)return ESP_ERR_NO_MEM;*h=calloc(1,sizeof(**h));return *h?ESP_OK:ESP_ERR_NO_MEM;}
esp_err_t aether_mjpeg_player_play(aether_mjpeg_player_handle_t h,const char *p,uint8_t fps,aether_mjpeg_frame_cb_t cb,void *c)
{assert(h&&p&&cb);(void)c;block_here(2);h->fps=fps;h->state=AETHER_MJPEG_STARTING;return ESP_OK;}
esp_err_t aether_mp4_player_play(aether_mp4_player_handle_t h,const char *p,aether_mp4_frame_cb_t cb,void *c)
{assert(h&&p&&cb);(void)c;block_here(2);h->state=AETHER_MJPEG_STARTING;return ESP_OK;}
esp_err_t aether_mjpeg_player_stop(aether_mjpeg_player_handle_t h)
{assert(h);++stops[0];h->state=AETHER_MJPEG_STOPPED;return ESP_OK;}
esp_err_t aether_mp4_player_stop(aether_mp4_player_handle_t h)
{assert(h);++stops[1];h->state=AETHER_MJPEG_STOPPED;return ESP_OK;}
static esp_err_t wait_for(unsigned n,uint32_t ms)
{wait_budget[n]=ms;clock_us+=(int64_t)(wait_cost[n]<ms?wait_cost[n]:ms)*1000;return wait_cost[n]<=ms?ESP_OK:ESP_ERR_TIMEOUT;}
esp_err_t aether_mjpeg_player_stop_wait(aether_mjpeg_player_handle_t h,uint32_t ms)
{assert(h);return wait_for(0,ms);}
esp_err_t aether_mp4_player_stop_wait(aether_mp4_player_handle_t h,uint32_t ms)
{assert(h);return wait_for(1,ms);}
void aether_mjpeg_player_release_frame(aether_mjpeg_player_handle_t h) {(void)h;}
void aether_mp4_player_release_frame(aether_mp4_player_handle_t h) {(void)h;}
esp_err_t aether_mjpeg_player_get_status(aether_mjpeg_player_handle_t h,aether_mjpeg_status_t *s)
{if(!h)return ESP_ERR_INVALID_ARG;block_here(1);memset(s,0,sizeof(*s));s->state=h->state;s->fps=h->fps;s->source_fps_x100=2398;return ESP_OK;}
esp_err_t aether_mp4_player_get_status(aether_mp4_player_handle_t h,aether_mp4_status_t *s)
{if(!h)return ESP_ERR_INVALID_ARG;memset(s,0,sizeof(*s));s->state=h->state;s->fps=24;s->source_fps_x100=2398;s->has_audio=true;return ESP_OK;}
esp_err_t aether_mjpeg_player_set_paused(aether_mjpeg_player_handle_t h,bool p) {h->state=p?AETHER_MJPEG_PAUSED:AETHER_MJPEG_PLAYING;return ESP_OK;}
esp_err_t aether_mp4_player_set_paused(aether_mp4_player_handle_t h,bool p) {h->state=p?AETHER_MJPEG_PAUSED:AETHER_MJPEG_PLAYING;return ESP_OK;}
void aether_mjpeg_player_set_screen_paused(aether_mjpeg_player_handle_t h,bool p) {h->screen_paused=p;}
void aether_mp4_player_set_screen_paused(aether_mp4_player_handle_t h,bool p) {h->screen_paused=p;}
esp_err_t aether_mjpeg_player_set_fps(aether_mjpeg_player_handle_t h,uint8_t fps) {h->fps=fps;return ESP_OK;}
esp_err_t aether_mp4_player_seek_with_reset(aether_mp4_player_handle_t h,uint32_t ms,void (*reset)(void *),void *context)
{assert(h);if(reset)reset(context);last_seek=ms;++seek_calls;return ESP_OK;}
esp_err_t aether_mp4_player_seek(aether_mp4_player_handle_t h,uint32_t ms) {assert(h);last_seek=ms;++seek_calls;return ESP_OK;}
esp_err_t aether_mp4_player_set_volume(aether_mp4_player_handle_t h,uint8_t v) {assert(h&&v<=100);return ESP_OK;}
esp_err_t aether_mjpeg_player_del(aether_mjpeg_player_handle_t h) {assert(h);++deletes[0];free(h);return ESP_OK;}
esp_err_t aether_mp4_player_del(aether_mp4_player_handle_t h) {assert(h);++deletes[1];if(fail_mp4_del){fail_mp4_del=false;return ESP_FAIL;}free(h);return ESP_OK;}
static void frame(const uint16_t *p,uint16_t w,uint16_t h,uint32_t n,void *c) {(void)p;(void)w;(void)h;(void)n;(void)c;}
static aether_video_player_handle_t create(void)
{aether_video_player_handle_t h=NULL;assert(aether_video_player_new((void*)1,(void*)2,&h)==ESP_OK);return h;}
typedef struct {aether_video_player_handle_t h;const char *path;esp_err_t result;} play_thread_t;
static void *play_thread(void *arg)
{play_thread_t *p=arg;p->result=aether_video_player_play(p->h,p->path,7,frame,NULL);return NULL;}
static void test_start_stop_race(void)
{
    const char *paths[]={"/sdcard/movie.avi","/sdcard/movie.MP4"};
    for(unsigned k=0;k<2;++k)for(int point=1;point<=2;++point){
        aether_video_player_handle_t h=create();
        pthread_mutex_lock(&gate);block_at=point;entered=resumed=false;pthread_mutex_unlock(&gate);
        play_thread_t p={.h=h,.path=paths[k]};pthread_t thread;assert(!pthread_create(&thread,NULL,play_thread,&p));
        pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&signal_gate,&gate);pthread_mutex_unlock(&gate);
        assert(aether_video_player_stop(h)==ESP_OK);
        pthread_mutex_lock(&gate);resumed=true;pthread_cond_broadcast(&signal_gate);pthread_mutex_unlock(&gate);
        assert(!pthread_join(thread,NULL));assert(p.result==ESP_OK);
        assert(h->mjpeg->state==AETHER_MJPEG_STOPPED&&h->mp4->state==AETHER_MJPEG_STOPPED);
        block_at=0;assert(aether_video_player_del(h)==ESP_OK);
    }
    puts("PASS asynchronous stop during status-check and backend acceptance cancels both MJPEG/MP4 starts");
}
static void test_budget_and_delete(void)
{
    aether_video_player_handle_t h=create();clock_us=0;lock_cost=20;wait_cost[0]=20;wait_cost[1]=20;
    assert(aether_video_player_stop_wait(h,50)==ESP_ERR_TIMEOUT);
    assert(wait_budget[0]==30&&wait_budget[1]==10&&clock_us==50000);
    clock_us=0;lock_cost=20;wait_budget[0]=wait_budget[1]=999;
    assert(aether_video_player_stop_wait(h,10)==ESP_ERR_TIMEOUT&&clock_us==10000);
    assert(wait_budget[0]==999&&wait_budget[1]==999);
    wait_cost[0]=wait_cost[1]=0;clock_us=0;
    assert(aether_video_player_stop_wait(h,0)==ESP_OK&&wait_budget[0]==0&&wait_budget[1]==0);
    const unsigned before=deletes[0];fail_mp4_del=true;
    assert(aether_video_player_del(h)==ESP_FAIL&&h->mjpeg==NULL&&h->mp4);
    assert(deletes[0]==before+1);
    assert(aether_video_player_del(h)==ESP_OK&&deletes[0]==before+1);
    fail_mp4_new=true;h=(void*)1;
    assert(aether_video_player_new((void*)1,(void*)2,&h)==ESP_ERR_NO_MEM&&!h);fail_mp4_new=false;
    puts("PASS shared lock/backend timeout budget, zero polls, partial delete retry and constructor cleanup");
}
static void reset_consumer(void *context) { ++*(unsigned *)context; }
static void test_routing(void)
{
    aether_video_player_handle_t h=create();aether_video_status_t status;
    assert(aether_video_player_play(h,"/sdcard/a.MOV",3,frame,NULL)==ESP_OK);
    assert(aether_video_player_get_status(h,&status)==ESP_OK&&status.source_fps_x100==2398&&status.fps==24&&status.has_audio);
    assert(aether_video_player_set_fps(h,30)==ESP_ERR_NOT_SUPPORTED);
    unsigned resets=0;
    assert(aether_video_player_seek_with_reset(h,731,reset_consumer,&resets)==ESP_OK && seek_calls==1 && last_seek==731 && resets==1);
    assert(aether_video_player_set_paused(h,true)==ESP_OK&&h->mp4->state==AETHER_MJPEG_PAUSED);
    aether_video_player_set_screen_paused(h,true);
    assert(h->mjpeg->screen_paused && h->mp4->screen_paused);
    aether_video_player_set_screen_paused(h,false);
    assert(!h->mjpeg->screen_paused && !h->mp4->screen_paused && h->mp4->state==AETHER_MJPEG_PAUSED);
    assert(aether_video_player_play(h,"/sdcard/a.avi",0,frame,NULL)==ESP_ERR_INVALID_STATE);
    assert(aether_video_player_stop_wait(h,100)==ESP_OK);
    assert(aether_video_player_play(h,"/sdcard/a.avi",0,frame,NULL)==ESP_OK&&h->mjpeg->fps==0);
    assert(aether_video_player_set_fps(h,20)==ESP_OK&&h->mjpeg->fps==20);
    assert(aether_video_player_seek_with_reset(h,400,reset_consumer,&resets)==ESP_ERR_NOT_SUPPORTED && seek_calls==1 && resets==1);
    assert(aether_video_player_get_status(h,&status)==ESP_OK&&status.source_fps_x100==2398&&!status.has_audio);
    assert(aether_video_player_del(h)==ESP_OK);
    puts("PASS case-insensitive backend routing, native MP4 cadence, active exclusion and MJPEG override");
}
int main(void)
{setbuf(stdout,NULL);test_routing();test_budget_and_delete();test_start_stop_race();return 0;}
