#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static size_t strlcpy(char *out, const char *in, size_t size)
{
    size_t n = strlen(in);
    if (size) { size_t copy = n < size - 1 ? n : size - 1; memcpy(out, in, copy); out[copy] = 0; }
    return n;
}
#include "../../components/aether_mjpeg_player/aether_mjpeg_player.c"

struct test_task { pthread_t thread; void (*run)(void *); void *argument; };
struct test_decoder { unsigned marker; };
static struct test_task *tasks[64];
static unsigned task_count;
static atomic_bool inserted = true, block_decode, decoding;
static atomic_uint leases, engines, callbacks, decode_calls, alloc_calls;
static atomic_int fail_alloc, fail_engine, fail_decode;
static pthread_mutex_t allocation_lock = PTHREAD_MUTEX_INITIALIZER;
static void *allocations[128];
static unsigned live_jpeg;
static uint8_t *fixture;
static size_t fixture_size;
static int64_t callback_times[64];

void __real_free(void *);
void __wrap_free(void *pointer)
{
    pthread_mutex_lock(&allocation_lock);
    for (unsigned i = 0; i < 128; ++i) if (pointer && allocations[i] == pointer) {
        allocations[i] = NULL; assert(live_jpeg); --live_jpeg; break;
    }
    pthread_mutex_unlock(&allocation_lock);
    __real_free(pointer);
}
int64_t esp_timer_get_time(void)
{
    struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
TickType_t xTaskGetTickCount(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
void vTaskDelay(TickType_t ms)
{
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
    nanosleep(&delay, NULL);
}
static void *task_run(void *argument)
{
    struct test_task *task = argument; task->run(task->argument); return NULL;
}
BaseType_t xTaskCreate(void (*run)(void *), const char *name, uint32_t stack,
                       void *argument, int priority, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)priority;
    assert(task_count < 64);
    struct test_task *task = calloc(1, sizeof(*task)); assert(task);
    task->run = run; task->argument = argument; tasks[task_count++] = task;
    if (out) *out = task;
    assert(!pthread_create(&task->thread, NULL, task_run, task)); return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; pthread_exit(NULL); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test error"; }
esp_err_t aether_storage_acquire(aether_storage_handle_t storage, uint32_t wait)
{ (void)storage; (void)wait; if (!inserted) return ESP_ERR_INVALID_STATE; ++leases; return ESP_OK; }
void aether_storage_release(aether_storage_handle_t storage)
{ (void)storage; assert(leases > 0); --leases; }
bool aether_storage_card_inserted(aether_storage_handle_t storage) { (void)storage; return inserted; }

void *jpeg_alloc_decoder_mem(size_t bytes, const jpeg_decode_memory_alloc_cfg_t *cfg, size_t *allocated)
{
    (void)cfg; unsigned call = ++alloc_calls;
    if ((int)call == fail_alloc) { *allocated = 0; return NULL; }
    void *memory = malloc(bytes); assert(memory); *allocated = bytes;
    pthread_mutex_lock(&allocation_lock);
    unsigned i; for (i = 0; i < 128 && allocations[i]; ++i) {}
    assert(i < 128); allocations[i] = memory; ++live_jpeg;
    pthread_mutex_unlock(&allocation_lock); return memory;
}
esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *cfg, jpeg_decoder_handle_t *out)
{
    assert(cfg->timeout_ms == 80);
    if (fail_engine) return ESP_FAIL;
    *out = calloc(1, sizeof(**out)); assert(*out); ++engines; return ESP_OK;
}
esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t engine) { free(engine); assert(engines); --engines; return ESP_OK; }
esp_err_t jpeg_decoder_get_info(const uint8_t *data, uint32_t length, jpeg_decode_picture_info_t *info)
{
    for (uint32_t i = 0; i + 8 < length; ++i) if (data[i] == 0xFF && data[i+1] == 0xC0) {
        info->height = ((uint32_t)data[i+5] << 8) | data[i+6];
        info->width = ((uint32_t)data[i+7] << 8) | data[i+8]; return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t engine, const jpeg_decode_cfg_t *cfg,
                               const uint8_t *data, uint32_t length, uint8_t *out,
                               uint32_t capacity, uint32_t *decoded)
{
    assert(engine && cfg->output_format == JPEG_DECODE_OUT_FORMAT_RGB565);
    ++decode_calls; decoding = true;
    while (block_decode) vTaskDelay(1);
    if (fail_decode) { decoding = false; return ESP_FAIL; }
    jpeg_decode_picture_info_t info;
    assert(jpeg_decoder_get_info(data, length, &info) == ESP_OK);
    *decoded = info.width * info.height * 2;
    assert(*decoded <= capacity);
    for (uint32_t i = 0; i < *decoded / 2; ++i) ((uint16_t *)out)[i] = 0x1234;
    vTaskDelay(2); decoding = false; return ESP_OK;
}
static void frame(const uint16_t *pixels, uint16_t width, uint16_t height,
                   uint32_t number, void *context)
{
    (void)context; assert(width == 16 && height == 16 && number && pixels[255] == 0x1234);
    unsigned i = atomic_load(&callbacks); if (i < 64) callback_times[i] = esp_timer_get_time();
    atomic_fetch_add(&callbacks, 1);
}
static void join_tasks(void)
{
    for (unsigned i = 0; i < task_count; ++i) { assert(!pthread_join(tasks[i]->thread, NULL)); free(tasks[i]); }
    task_count = 0;
}
static void reset(void)
{
    assert(!task_count && !leases && !engines && !live_jpeg);
    inserted = true; block_decode = decoding = false; callbacks = decode_calls = alloc_calls = 0;
    fail_alloc = fail_engine = fail_decode = 0;
}
static void raw_file(const char *path, unsigned frames)
{
    FILE *file = fopen(path, "wb"); assert(file);
    for (unsigned i = 0; i < frames; ++i) assert(fwrite(fixture, 1, fixture_size, file) == fixture_size);
    assert(!fclose(file));
}
static void u32(FILE *file, uint32_t value)
{
    uint8_t bytes[] = {value, value >> 8, value >> 16, value >> 24};
    assert(fwrite(bytes, 1, 4, file) == 4);
}
static long chunk_begin(FILE *file, const char *id)
{
    assert(fwrite(id, 1, 4, file) == 4); long size = ftell(file); assert(size >= 0);
    u32(file, 0); return size;
}
static void chunk_end(FILE *file, long size_offset)
{
    long end = ftell(file); assert(end >= size_offset + 4);
    uint32_t size = (uint32_t)(end - size_offset - 4);
    assert(!fseek(file, size_offset, SEEK_SET)); u32(file, size);
    assert(!fseek(file, end, SEEK_SET)); if (size & 1) assert(fputc(0, file) == 0);
}
/* A real classic MJPEG AVI: AVIMAINHEADER, AVI stream header/format, and
 * padded 00dc chunks. The image bytes are the same baseline-JPEG fixture. */
static void avi_file(const char *path, unsigned frames, uint32_t period)
{
    FILE *file = fopen(path, "wb"); assert(file);
    long riff = chunk_begin(file, "RIFF"); assert(fwrite("AVI ", 1, 4, file) == 4);
    long hdrl = chunk_begin(file, "LIST"); assert(fwrite("hdrl", 1, 4, file) == 4);
    long avih = chunk_begin(file, "avih");
    uint32_t header[] = {period, 0, 0, 0, frames, 0, 1, (uint32_t)fixture_size, 16, 16, 0, 0, 0, 0};
    for (unsigned i = 0; i < sizeof(header)/sizeof(header[0]); ++i) u32(file, header[i]);
    chunk_end(file, avih);
    long strl = chunk_begin(file, "LIST"); assert(fwrite("strl", 1, 4, file) == 4);
    long strh = chunk_begin(file, "strh"); assert(fwrite("vidsMJPG", 1, 8, file) == 8);
    uint32_t stream[] = {0, 0, 0, period, 1000000, 0, frames, (uint32_t)fixture_size, UINT32_MAX, 0, 0, 0x00100010};
    for (unsigned i = 0; i < sizeof(stream)/sizeof(stream[0]); ++i) u32(file, stream[i]);
    chunk_end(file, strh);
    long strf = chunk_begin(file, "strf");
    u32(file, 40); u32(file, 16); u32(file, 16); u32(file, 0x00180001);
    assert(fwrite("MJPG", 1, 4, file) == 4);
    u32(file, 16 * 16 * 3); for (unsigned i = 0; i < 4; ++i) u32(file, 0);
    chunk_end(file, strf); chunk_end(file, strl); chunk_end(file, hdrl);
    long junk = chunk_begin(file, "JUNK");
    assert(fwrite(fixture, 1, fixture_size, file) == fixture_size); chunk_end(file, junk);
    long movi = chunk_begin(file, "LIST"); assert(fwrite("movi", 1, 4, file) == 4);
    for (unsigned i = 0; i < frames; ++i) {
        long video = chunk_begin(file, "00dc");
        assert(fwrite(fixture, 1, fixture_size, file) == fixture_size); chunk_end(file, video);
    }
    chunk_end(file, movi); chunk_end(file, riff); assert(!fclose(file));
}
static aether_mjpeg_player_handle_t start(const char *path, uint8_t fps)
{
    aether_mjpeg_player_handle_t player = NULL;
    assert(aether_mjpeg_player_new((void *)1, &player) == ESP_OK);
    assert(aether_mjpeg_player_play(player, path, fps, frame, NULL) == ESP_OK);
    return player;
}
static aether_mjpeg_status_t wait_state(aether_mjpeg_player_handle_t player, aether_mjpeg_state_t state)
{
    aether_mjpeg_status_t status;
    int64_t end = esp_timer_get_time() + 3000000;
    do {
        assert(aether_mjpeg_player_get_status(player, &status) == ESP_OK);
        if (status.state == state) return status;
        vTaskDelay(2);
    } while (esp_timer_get_time() < end);
    fprintf(stderr, "expected state %d, got %d error %x callbacks %u\n", state, status.state, status.last_error, callbacks);
    abort();
}
static void wait_callbacks(unsigned count)
{
    int64_t end = esp_timer_get_time() + 1000000;
    while (callbacks < count && esp_timer_get_time() < end) vTaskDelay(1);
    assert(callbacks >= count);
}
static void destroy(aether_mjpeg_player_handle_t player)
{
    assert(aether_mjpeg_player_del(player) == ESP_OK); join_tasks();
    assert(!leases && !engines && !live_jpeg);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *input = fopen(argv[1], "rb"); assert(input && !fseek(input, 0, SEEK_END));
    fixture_size = (size_t)ftell(input); rewind(input); fixture = malloc(fixture_size); assert(fixture);
    assert(fread(fixture, 1, fixture_size, input) == fixture_size); fclose(input);
    char path[] = "/tmp/aether-video-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);

    reset(); raw_file(path, 8);
    aether_mjpeg_player_handle_t player = start(path, 30);
    aether_mjpeg_status_t status = wait_state(player, AETHER_MJPEG_FINISHED);
    assert(callbacks == 8 && status.frame_number == 8 && status.width == 16 && status.height == 16);
    assert(status.actual_fps_x100 > 2300 && status.actual_fps_x100 < 3600 && status.decode_us >= 1000);
    assert(callback_times[7] - callback_times[0] > 180000);
    destroy(player); puts("PASS production worker: real baseline JPEG stream, frame cadence, metrics, EOF and cleanup");

    const uint32_t native_periods[] = {40000, 33367, 33333};
    for (unsigned rate = 0; rate < 3; ++rate) {
        reset(); avi_file(path, 8, native_periods[rate]); player = start(path, 0);
        status = wait_state(player, AETHER_MJPEG_FINISHED);
        assert(callbacks == 8 && decode_calls == 8 && status.frame_number == 8);
        assert(status.source_fps_x100 == 100000000U / native_periods[rate]);
        assert(status.fps == (rate == 0 ? 25 : 30));
        int64_t interval = callback_times[7] - callback_times[0];
        assert(interval > 7LL * native_periods[rate] * 9 / 10);
        assert(interval < 7LL * native_periods[rate] * 5 / 4);
        destroy(player);
    }
    puts("PASS production AVI worker: native 25/29.97/30 cadence, padded frames and header-thumbnail exclusion");

    reset(); raw_file(path, 8); player = start(path, 0);
    status = wait_state(player, AETHER_MJPEG_FINISHED);
    assert(status.fps == 24 && !status.source_fps_x100 && callbacks == 8);
    assert(callback_times[7] - callback_times[0] > 260000); destroy(player);
    puts("PASS automatic raw stream defaults to 24 fps without invented source timing");

    for (unsigned bad = 0; bad < 3; ++bad) {
        reset();
        if (bad == 0) raw_file(path, 0);
        if (bad == 1) avi_file(path, 1, 20000); /* Automatic 50 fps is unsupported. */
        if (bad == 2) {
            size_t marker = 0;
            for (size_t i = 0; i + 8 < fixture_size; ++i)
                if (fixture[i] == 0xFF && fixture[i+1] == 0xC0) { marker = i + 8; break; }
            assert(marker && fixture[marker] == 16); fixture[marker] = 17;
            raw_file(path, 1); fixture[marker] = 16;
        }
        player = start(path, 0); status = wait_state(player, AETHER_MJPEG_ERROR);
        assert(status.last_error == (bad == 0 ? ESP_ERR_INVALID_SIZE : ESP_ERR_NOT_SUPPORTED));
        assert(!callbacks && !decode_calls); destroy(player);
    }
    puts("PASS empty input, unsupported automatic cadence and unaligned dimensions fail explicitly before decode");

    reset(); avi_file(path, 50, 40000); player = start(path, 0); wait_callbacks(2);
    assert(aether_mjpeg_player_play(player, path, 0, frame, NULL) == ESP_ERR_INVALID_STATE);
    assert(aether_mjpeg_player_set_fps(player, 31) == ESP_ERR_INVALID_ARG);
    assert(aether_mjpeg_player_set_fps(player, 255) == ESP_ERR_INVALID_ARG);
    assert(aether_mjpeg_player_set_paused(player, true) == ESP_OK);
    wait_state(player, AETHER_MJPEG_PAUSED); vTaskDelay(180);
    assert(aether_mjpeg_player_set_fps(player, 30) == ESP_OK);
    assert(aether_mjpeg_player_set_paused(player, false) == ESP_OK);
    wait_callbacks(6);
    assert(aether_mjpeg_player_set_fps(player, 0) == ESP_OK);
    wait_callbacks(8);
    assert(aether_mjpeg_player_stop_wait(player, 1000) == ESP_OK);
    assert(aether_mjpeg_player_get_status(player, &status) == ESP_OK);
    assert(status.fps == 25 && status.actual_fps_x100 > 1800 && status.actual_fps_x100 < 4000);
    assert(aether_mjpeg_player_play(player, path, 31, frame, NULL) == ESP_ERR_INVALID_ARG);
    destroy(player);
    puts("PASS pause/resume and fixed/native rate changes retain cadence metrics; invalid arguments and double play rejected");

    reset(); raw_file(path, 8); player = start(path, 1); wait_callbacks(1);
    assert(aether_mjpeg_player_set_paused(player, true) == ESP_OK);
    wait_state(player, AETHER_MJPEG_PAUSED);
    unsigned before = callbacks; vTaskDelay(40); assert(callbacks == before);
    inserted = false;
    status = wait_state(player, AETHER_MJPEG_ERROR);
    assert(status.last_error == ESP_ERR_INVALID_STATE && !leases);
    destroy(player); puts("PASS pause interrupts long frame wait; removal while paused reports an error and drains the lease");

    reset(); raw_file(path, 8); block_decode = true; player = start(path, 30);
    while (!decoding) vTaskDelay(1);
    aether_mjpeg_player_release_frame(player);
    assert(aether_mjpeg_player_stop_wait(player, 0) == ESP_ERR_TIMEOUT);
    block_decode = false; wait_state(player, AETHER_MJPEG_STOPPED);
    assert(!callbacks); destroy(player);
    puts("PASS stop/release during decode suppresses late callbacks and releases retained output once");

    for (int failure = 1; failure <= 4; ++failure) {
        reset(); raw_file(path, 1);
        if (failure <= 2) fail_alloc = failure;
        if (failure == 3) fail_engine = 1;
        if (failure == 4) fail_decode = 1;
        player = start(path, 30); status = wait_state(player, AETHER_MJPEG_ERROR);
        assert(!callbacks && !leases && status.last_error != ESP_OK); destroy(player);
    }
    puts("PASS input/output allocation, engine and decode failures release engine/file/mount ownership");

    reset(); raw_file(path, 1); player = start(path, 30); wait_state(player, AETHER_MJPEG_FINISHED);
    for (unsigned run = 0; run < 30; ++run) {
        aether_mjpeg_player_release_frame(player);
        assert(aether_mjpeg_player_play(player, path, 30, frame, NULL) == ESP_OK);
        wait_state(player, AETHER_MJPEG_FINISHED);
    }
    assert(callbacks == 31); destroy(player);
    puts("PASS 30 retained-buffer release/restart cycles without JPEG buffer or lease leaks");
    unlink(path); free(fixture); return 0;
}
