/* Compile the production app_frames.c unchanged. Stubs model allocation,
 * mutex contention and LVGL's retained source pointer; no firmware SDK or
 * host libc is required. A nonzero return is the failing source line. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "app_frames.h"
#include "freertos/semphr.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

struct test_mutex { bool locked; };
typedef struct { void *pointer; bool live; } allocation_t;
static _Alignas(16) uint8_t arena[2U * 1024U * 1024U];
static allocation_t allocations[32];
static size_t arena_used;
static unsigned allocation_calls;
static unsigned fail_allocation;
static unsigned live_allocations;
static unsigned allocation_errors;
static unsigned mutex_errors;
static unsigned cache_drops;
static struct test_mutex *last_mutex;
static lv_display_t display;
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }

void *memcpy(void *destination, const void *source, size_t size)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    uint8_t *out = destination;
    for (size_t i = 0; i < size; ++i) out[i] = (uint8_t)value;
    return destination;
}

void *malloc(size_t size)
{
    ++allocation_calls;
    if (allocation_calls == fail_allocation) return NULL;
    const size_t aligned_size = (size + 15U) & ~(size_t)15U;
    if (allocation_calls >= 32 || aligned_size > sizeof(arena) - arena_used) {
        ++allocation_errors;
        return NULL;
    }
    void *pointer = arena + arena_used;
    arena_used += aligned_size;
    allocations[allocation_calls].pointer = pointer;
    allocations[allocation_calls].live = true;
    ++live_allocations;
    return pointer;
}

void *calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    void *pointer = malloc(count * size);
    if (pointer) memset(pointer, 0, count * size);
    return pointer;
}

void free(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 1; i < 32; ++i) {
        if (allocations[i].pointer == pointer) {
            if (!allocations[i].live) {
                ++allocation_errors;
                return;
            }
            allocations[i].live = false;
            --live_allocations;
            return;
        }
    }
    ++allocation_errors;
}

void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    (void)capabilities;
    return malloc(size);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    last_mutex = malloc(sizeof(*last_mutex));
    if (last_mutex) last_mutex->locked = false;
    return last_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
    if (!mutex) { ++mutex_errors; return pdFALSE; }
    if (mutex->locked) {
        /* A bounded producer/consumer must not try a blocking take. */
        if (timeout) ++mutex_errors;
        return pdFALSE;
    }
    mutex->locked = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    if (!mutex || !mutex->locked) { ++mutex_errors; return pdFALSE; }
    mutex->locked = false;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    if (!mutex || mutex->locked) ++mutex_errors;
    free(mutex);
}

void lv_image_set_src(lv_obj_t *image, const void *source)
{
    image->source = source;
    ++image->updates;
}
const void *lv_image_get_src(const lv_obj_t *image) { return image->source; }
void lv_obj_center(lv_obj_t *object) { ++object->centers; }
int32_t lv_obj_get_width(const lv_obj_t *object) { return object->width; }
int32_t lv_obj_get_height(const lv_obj_t *object) { return object->height; }
void lv_obj_set_size(lv_obj_t *object, int32_t w, int32_t h) { object->width=w;object->height=h;++object->resizes; }
void lv_obj_invalidate(lv_obj_t *object) { ++object->invalidations; }
lv_display_t *lv_obj_get_display(const lv_obj_t *object) { (void)object; return &display; }
void lv_display_trigger_activity(lv_display_t *target) { ++target->activity; }
void lv_image_cache_drop(const void *source) { if (source) ++cache_drops; }
void lv_display_add_event_cb(lv_display_t *d,lv_event_cb_t cb,lv_event_code_t code,void *ctx)
{ if (code==LV_EVENT_REFR_START) { d->refresh=cb;d->refresh_context=ctx; } }
uint32_t lv_display_remove_event_cb_with_user_data(lv_display_t *d,lv_event_cb_t cb,void *ctx)
{ if (d->refresh!=cb||d->refresh_context!=ctx)return 0;d->refresh=0;d->refresh_context=0;return 1; }
void lv_obj_add_event_cb(lv_obj_t *o,lv_event_cb_t cb,lv_event_code_t code,void *ctx)
{ if (code==LV_EVENT_DELETE) { o->on_delete=cb;o->delete_context=ctx; } }
void lv_obj_remove_event_cb_with_user_data(lv_obj_t *o,lv_event_cb_t cb,void *ctx)
{ if(o->on_delete==cb&&o->delete_context==ctx){o->on_delete=0;o->delete_context=0;} }
void *lv_event_get_user_data(lv_event_t *e){return e->user_data;}
lv_obj_t *lv_event_get_target_obj(lv_event_t *e){return e->target;}
lv_timer_t *lv_display_get_refr_timer(lv_display_t *d){return &d->timer;}
void lv_timer_resume(lv_timer_t *t){++t->resume;}
void lv_timer_ready(lv_timer_t *t){++t->ready;}

static void reset(unsigned fail_at)
{
    arena_used = 0;
    allocation_calls = 0;
    fail_allocation = fail_at;
    live_allocations = 0;
    allocation_errors = 0;
    mutex_errors = 0;
    cache_drops = 0;
    now_us = 0;
    last_mutex = NULL;
    memset(&display,0,sizeof(display));
    memset(allocations, 0, sizeof(allocations));
}

static void fill(uint16_t *pixels, size_t count, uint16_t value)
{
    for (size_t i = 0; i < count; ++i) pixels[i] = value;
}

static bool uniform(const uint16_t *pixels, size_t count, uint16_t value)
{
    for (size_t i = 0; i < count; ++i) if (pixels[i] != value) return false;
    return true;
}

static app_frames_handle_t direct_frames;
static unsigned direct_calls;
static bool direct_stable, direct_enabled;
static bool test_direct_present(lv_obj_t *image, const uint16_t *pixels,
    uint16_t width, uint16_t height)
{
    if (++direct_calls == 1) {
        direct_stable = width == 4 && height == 4 && !last_mutex->locked &&
            (const uint8_t *)pixels == image->source->data && uniform(pixels,16,13);
        for (uint16_t value = 20; value <= 30; ++value) {
            uint16_t next[16];
            fill(next,16,value);
            app_frames_push(direct_frames,next,4,4);
            direct_stable = direct_stable && uniform(pixels,16,13);
        }
    }
    return direct_enabled;
}

int test_startup_rollback(void)
{
    reset(0);
    CHECK(app_frames_new(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(!allocation_calls);
    /* Device, mutex and each of three PSRAM allocations must roll back. */
    for (unsigned fail = 1; fail <= 5; ++fail) {
        reset(fail);
        app_frames_handle_t frames = NULL;
        CHECK(app_frames_new(&frames) == ESP_ERR_NO_MEM);
        CHECK(!frames);
        CHECK(!live_allocations && !allocation_errors && !mutex_errors);
    }
    reset(0);
    app_frames_handle_t frames = NULL;
    CHECK(app_frames_new(&frames) == ESP_OK && frames);
    CHECK(live_allocations == 5);
    app_frames_delete(frames);
    CHECK(!live_allocations && !allocation_errors && !mutex_errors);
    app_frames_delete(NULL);
    return 0;
}

int test_copy_latest_front(void)
{
    reset(0);
    app_frames_handle_t frames = NULL;
    CHECK(app_frames_new(&frames) == ESP_OK);
    lv_obj_t image = {0};
    uint16_t source[16];
    app_frames_set_active(frames, true);
    fill(source, 16, 0x1234);
    app_frames_push(frames, source, 4, 4);
    fill(source, 16, 0xFFFF); /* Producer reuses its own buffer immediately. */
    app_frames_present(frames, &image);
    CHECK(image.updates == 1 && image.source);
    CHECK(image.source->data != (const uint8_t *)source);
    CHECK(uniform((const uint16_t *)image.source->data, 16, 0x1234));
    CHECK(image.source->header.w == 4 && image.source->header.h == 4);
    CHECK(image.source->header.stride == 8 && image.source->data_size == 32);
    CHECK(image.source->header.magic == LV_IMAGE_HEADER_MAGIC);
    CHECK(image.source->header.cf == LV_COLOR_FORMAT_RGB565);

    const uint16_t *front = (const uint16_t *)image.source->data;
    for (uint16_t value = 2; value <= 20; ++value) {
        fill(source, 16, value);
        app_frames_push(frames, source, 4, 4);
        CHECK(uniform(front, 16, 0x1234)); /* Current display source cannot be overwritten. */
    }
    app_frames_present(frames, &image);
    CHECK(image.updates == 2);
    CHECK(uniform((const uint16_t *)image.source->data, 16, 20));
    CHECK((const uint16_t *)image.source->data != front);
    app_frames_present(frames, &image); /* No redundant update without a new frame. */
    CHECK(image.updates == 2 && cache_drops == 2 && !display.activity);
    CHECK(image.invalidations == 2 && image.centers == 2);
    app_frames_delete(frames);
    CHECK(!live_allocations && !allocation_errors && !mutex_errors);

    reset(0);
    image=(lv_obj_t){0};
    CHECK(app_frames_new(&frames) == ESP_OK);
    app_frames_set_active(frames,true);
    app_frames_set_refresh_image(frames,&image);
    CHECK(display.refresh && image.on_delete);
    fill(source,16,10);
    app_frames_push(frames,source,4,4);
    app_frames_present(frames,&image);
    CHECK(!image.source&&!image.updates&&display.timer.resume==1&&display.timer.ready==1);
    fill(source,16,11);
    app_frames_push(frames,source,4,4);
    lv_event_t refresh={.user_data=display.refresh_context,.target=&display};
    display.refresh(&refresh);
    CHECK(image.updates==1 && uniform((const uint16_t *)image.source->data,16,11));
    fill(source,16,12);
    app_frames_push(frames,source,4,4);
    CHECK(uniform((const uint16_t *)image.source->data,16,11));
    display.refresh(&refresh);
    CHECK(image.updates==1&&image.invalidations==2&&!display.activity&&cache_drops==2);
    CHECK(uniform((const uint16_t *)image.source->data,16,12));
    app_frames_set_direct_presenter(frames,test_direct_present);
    direct_frames=frames;direct_enabled=true;direct_calls=0;
    fill(source,16,13);
    app_frames_push(frames,source,4,4);
    display.refresh(&refresh);
    CHECK(direct_stable&&direct_calls==1&&image.invalidations==2);
    CHECK(uniform((const uint16_t *)image.source->data,16,13));
    direct_enabled=false;
    display.refresh(&refresh);
    CHECK(direct_calls==2&&image.invalidations==3);
    CHECK(uniform((const uint16_t *)image.source->data,16,30)); /* Latest frame still composes. */
    lv_event_t deleted={.user_data=image.delete_context,.target=&image};
    image.on_delete(&deleted);
    CHECK(!display.refresh&&!image.on_delete);
    app_frames_set_active(frames,false);
    app_frames_delete(frames);
    CHECK(!live_allocations&&!allocation_errors&&!mutex_errors);
    return 0;
}

int test_disable_validation_contention(void)
{
    reset(0);
    app_frames_handle_t frames = NULL;
    CHECK(app_frames_new(&frames) == ESP_OK);
    lv_obj_t image = {0};
    uint16_t source[16];
    fill(source, 16, 7);
    app_frames_push(frames, source, 4, 4); /* Initially inactive. */
    app_frames_present(frames, &image);
    CHECK(!image.updates);
    app_frames_set_active(frames, true);
    app_frames_push(frames, source, 4, 4);
    app_frames_set_active(frames, false); /* A pending frame is cancelled. */
    app_frames_present(frames, &image);
    CHECK(!image.updates);
    app_frames_push(frames, source, 4, 4);
    app_frames_set_active(frames, true);
    app_frames_present(frames, &image);
    CHECK(!image.updates); /* Re-enable cannot resurrect an old pending frame. */

    app_frames_push(frames, source, 0, 4);
    app_frames_push(frames, source, 4, 0);
    app_frames_push(frames, source, 481, 4);
    app_frames_push(frames, source, 4, 481);
    app_frames_push(frames, NULL, 4, 4);
    app_frames_present(frames, &image);
    CHECK(!image.updates);
    last_mutex->locked = true;
    app_frames_push(frames, source, 4, 4);
    app_frames_present(frames, &image);
    last_mutex->locked = false;
    app_frames_present(frames, &image);
    CHECK(!image.updates && !mutex_errors); /* Contention drops rather than blocks. */
    app_frames_push(frames, source, 4, 4);
    app_frames_present(frames, &image);
    CHECK(image.updates == 1);
    CHECK(uniform((const uint16_t *)image.source->data, 16, 7));
    app_frames_delete(frames);
    CHECK(!live_allocations && !allocation_errors && !mutex_errors);
    return 0;
}

int test_max_frame(void)
{
    static uint16_t source[480U * 480U];
    reset(0);
    app_frames_handle_t frames = NULL;
    CHECK(app_frames_new(&frames) == ESP_OK);
    lv_obj_t image = {0};
    app_frames_set_active(frames, true);
    fill(source, 480U * 480U, 0xBEEF);
    app_frames_push(frames, source, 480, 480);
    app_frames_present(frames, &image);
    CHECK(image.source && image.source->data_size == 460800);
    CHECK(image.source->header.stride == 960);
    CHECK(uniform((const uint16_t *)image.source->data, 480U * 480U, 0xBEEF));
    app_frames_delete(frames);
    CHECK(!live_allocations && !allocation_errors && !mutex_errors);
    return 0;
}

int test_metrics_and_repeated_handover(void)
{
    reset(0);
    app_frames_handle_t frames = NULL;
    app_frames_metrics_t metrics;
    CHECK(app_frames_new(&frames) == ESP_OK);
    CHECK(app_frames_get_metrics(NULL, &metrics) == ESP_ERR_INVALID_ARG);
    CHECK(app_frames_get_metrics(frames, NULL) == ESP_ERR_INVALID_ARG);
    app_frames_set_active(frames, true);
    uint16_t source[16];
    fill(source, 16, 3);
    now_us = 100; app_frames_push(frames, source, 4, 4);
    now_us = 200; app_frames_push(frames, source, 4, 4);
    CHECK(app_frames_get_metrics(frames, &metrics) == ESP_OK);
    CHECK(metrics.received_frames == 2 && metrics.replaced_frames == 1 && !metrics.presented_frames);
    lv_obj_t image = {0};
    now_us = 250; app_frames_present(frames, &image);
    CHECK(app_frames_get_metrics(frames, &metrics) == ESP_OK);
    CHECK(metrics.presented_frames == 1 && metrics.queue_delay_us == 50 && metrics.max_queue_delay_us == 50);
    last_mutex->locked = true;
    CHECK(app_frames_get_metrics(frames, &metrics) == ESP_ERR_TIMEOUT);
    last_mutex->locked = false;
    for (unsigned i = 0; i < 10000; ++i) {
        now_us += 16667; app_frames_push(frames, source, 4, 4);
        now_us += 500; app_frames_present(frames, &image);
    }
    CHECK(app_frames_get_metrics(frames, &metrics) == ESP_OK);
    CHECK(metrics.received_frames == 10002 && metrics.presented_frames == 10001 && metrics.replaced_frames == 1);
    CHECK(metrics.queue_delay_us == 500 && metrics.max_queue_delay_us == 500);
    CHECK(live_allocations == 5);
    app_frames_set_active(frames, false);
    app_frames_set_active(frames, true);
    CHECK(app_frames_get_metrics(frames, &metrics) == ESP_OK && !metrics.received_frames && !metrics.presented_frames);
    app_frames_delete(frames);
    CHECK(!live_allocations && !allocation_errors && !mutex_errors);
    return 0;
}

int test_fitted_viewport(void)
{
    reset(0);
    app_frames_handle_t frames = NULL;
    CHECK(app_frames_new(&frames) == ESP_OK);
    lv_obj_t image = {.width=480,.height=480};
    static uint16_t source[480U*480U];
    app_frames_set_active(frames,true);
    app_frames_set_viewport(frames,480,480);
    app_frames_push(frames,source,320,180);app_frames_present(frames,&image);
    CHECK(image.width==480&&image.height==270&&image.resizes==1&&image.centers==1);
    app_frames_push(frames,source,320,180);app_frames_present(frames,&image);
    CHECK(image.resizes==1&&image.centers==1); /* No per-frame layout churn. */
    app_frames_push(frames,source,256,224);app_frames_present(frames,&image);
    CHECK(image.width==480&&image.height==420&&image.resizes==2&&image.centers==2);
    app_frames_push(frames,source,180,320);app_frames_present(frames,&image);
    CHECK(image.width==270&&image.height==480);
    app_frames_push(frames,source,480,1);app_frames_present(frames,&image);
    CHECK(image.width==480&&image.height==1);
    app_frames_set_viewport(frames,0,0);
    app_frames_push(frames,source,320,180);app_frames_present(frames,&image);
    CHECK(image.width==480&&image.height==1); /* Existing object geometry retained. */
    app_frames_delete(frames);
    CHECK(!live_allocations&&!allocation_errors&&!mutex_errors);
    return 0;
}
