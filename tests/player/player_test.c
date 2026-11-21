#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define PLAYER_LOG_TEST
#include "../../components/aether_audio_player/aether_audio_player.c"

struct test_task { pthread_t thread; void (*run)(void *); void *argument; };
int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }

static aether_audio_player_handle_t profile_subject;
static unsigned profile_logs;
void test_player_log_info(const char *tag, const char *format, ...)
{
    assert(!strcmp(tag, "audio_player"));
    if (strncmp(format, "profile ", 8)) return;
    assert(profile_subject);
    /* A slow console must not own the state lock. Taking a fresh snapshot
     * would deadlock here if the helper printed from its critical section. */
    aether_audio_player_status_t snapshot;
    assert(aether_audio_player_get_status(profile_subject, &snapshot) == ESP_OK);
    assert(snapshot.state == AETHER_AUDIO_PAUSED || !snapshot.active);
    ++profile_logs;
}

static void test_profile_snapshot(void)
{
    struct aether_audio_player_t p = {.state_lock = portMUX_INITIALIZER_UNLOCKED};
    profile_subject = &p;
    p.status.pull_max_us = 1200;
    p.status.refill_max_bytes = 32768;
    p.status.refill_max_us = 1000;
    p.status.state = AETHER_AUDIO_PLAYING;
    p.status.active = true;
    aether_audio_player_log_profile(NULL);
    aether_audio_player_log_profile(&p);
    assert(profile_logs == 0);
    p.status.state = AETHER_AUDIO_PAUSED;
    const aether_audio_player_status_t before = p.status;
    aether_audio_player_log_profile(&p);
    assert(profile_logs == 1 && !memcmp(&p.status, &before, sizeof(before)));
    p.status.active = false;
    p.status.state = AETHER_AUDIO_STOPPED;
    aether_audio_player_log_profile(&p);
    assert(profile_logs == 2 && p.status.refill_max_us == 1000);
    clear_profile_locked(&p);
    aether_audio_player_log_profile(&p);
    assert(profile_logs == 2 && !p.status.pull_max_us);
    profile_subject = NULL;
    assert(pthread_mutex_destroy(&p.state_lock) == 0);
    puts("PASS cached profile ignores active supply, releases lock before logging, preserves pause/stop metrics");
}

static struct test_task *tasks[16];
static size_t task_count;
static atomic_bool inserted = true, codec_on;
static atomic_uint writes, starts, leases, nonsilent;
static atomic_uchar saved_volume = 65, saved_repeat;
static atomic_uint commits, saved_eq;
static atomic_bool have_saved_eq;
static atomic_bool fail_commit;
static atomic_uint failed_commits;
static atomic_bool hold_codec_write, codec_write_entered, fail_codec_stop;
static atomic_uint codec_stop_calls;

void vTaskDelay(TickType_t ms)
{
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}
TickType_t xTaskGetTickCount(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000ULL + t.tv_nsec / 1000000);
}
static void *task_run(void *argument)
{
    struct test_task *t = argument;
    t->run(t->argument);
    return NULL;
}
BaseType_t xTaskCreate(void (*run)(void *), const char *name, uint32_t stack,
                       void *argument, int priority, TaskHandle_t *out)
{
    assert(!strcmp(name,"music_player") && stack==24576 && priority==7);
    assert(task_count < 16);
    struct test_task *t = calloc(1, sizeof(*t));
    assert(t);
    t->run = run; t->argument = argument;
    tasks[task_count++] = t;
    *out = t;
    assert(pthread_create(&t->thread, NULL, task_run, t) == 0);
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ms)
{ (void)clear; vTaskDelay(ms > 3 ? 3 : ms); return 0; }
void vTaskDelete(TaskHandle_t t) { (void)t; pthread_exit(NULL); }
void test_yield(void) { sched_yield(); }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    assert(m); assert(pthread_mutex_init(m, NULL) == 0); return m;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t timeout)
{
    if (!timeout) return pthread_mutex_trylock(m) == 0;
    return pthread_mutex_lock(m) == 0;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0; }
void vSemaphoreDelete(SemaphoreHandle_t m) { assert(pthread_mutex_destroy(m) == 0); free(m); }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "error"; }
uint32_t esp_random(void) { return 77; }
esp_err_t aether_storage_acquire(aether_storage_handle_t s, uint32_t timeout)
{ (void)s; (void)timeout; if (!inserted) return ESP_ERR_INVALID_STATE; ++leases; return ESP_OK; }
void aether_storage_release(aether_storage_handle_t s) { (void)s; assert(leases > 0); --leases; }
bool aether_storage_card_inserted(aether_storage_handle_t s) { (void)s; return inserted; }
esp_err_t ac101s_audio_start(ac101s_audio_handle_t a, int volume, float gain)
{ (void)a; assert(volume == 100 && gain == 24); assert(!codec_on); codec_on = true; ++starts; return ESP_OK; }
esp_err_t ac101s_audio_stop(ac101s_audio_handle_t a) { (void)a;++codec_stop_calls;if(fail_codec_stop)return ESP_FAIL;codec_on=false;return ESP_OK; }
esp_err_t ac101s_audio_write(ac101s_audio_handle_t a, void *data, size_t bytes)
{
    (void)a; assert(codec_on && bytes && !(bytes % 4));
    int16_t *samples = data;
    bool any = false;
    for (size_t i = 0; i < bytes / 2; ++i) if (samples[i]) any = true;
    if (any) ++nonsilent;
    ++writes;
    if (hold_codec_write) {
        codec_write_entered = true;
        const TickType_t started = xTaskGetTickCount();
        while (hold_codec_write) {
            assert(xTaskGetTickCount() - started < 3000);
            vTaskDelay(1);
        }
    }
    vTaskDelay(2);
    return ESP_OK;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)mode; assert(!strcmp(name, "music")); *handle = 1; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v)
{ (void)h; *v = !strcmp(key, "volume") ? saved_volume : saved_repeat; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v)
{ (void)h; if (!strcmp(key, "volume")) saved_volume = v; else saved_repeat = v; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *value)
{
    assert(h == 1 && !strcmp(key, "eq_v1"));
    if (!have_saved_eq) return ESP_ERR_NVS_NOT_FOUND;
    *value = saved_eq;
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    assert(h == 1 && !strcmp(key, "eq_v1"));
    saved_eq = value;
    have_saved_eq = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{ (void)h; if (fail_commit) { ++failed_commits; return ESP_FAIL; } ++commits; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static void put16(FILE *f, uint16_t value)
{ fputc(value & 255, f); fputc(value >> 8, f); }
static void put32(FILE *f, uint32_t value)
{ put16(f, value & 65535); put16(f, value >> 16); }
static void wav(FILE *f, unsigned rate, unsigned channels, unsigned frames, bool odd)
{
    rewind(f);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * channels * 2 + (odd ? 10 : 0));
    fwrite("WAVE", 1, 4, f);
    if (odd) { fwrite("JUNK", 1, 4, f); put32(f, 1); put16(f, 0x5A); }
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, channels);
    put32(f, rate); put32(f, rate * channels * 2); put16(f, channels * 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * channels * 2);
    for (unsigned i = 0; i < frames; ++i) {
        put16(f, i % 2 ? 32767 : (uint16_t)-32768);
        if (channels == 2) put16(f, i % 2 ? (uint16_t)-32768 : 32767);
    }
    fflush(f); rewind(f);
}

static void test_wav(void)
{
    FILE *f = tmpfile(); assert(f);
    wav(f, 44100, 1, 44100, true);
    aether_wav_stream_t *s = calloc(1, sizeof(*s)); assert(s);
    s->file = f;
    assert(aether_wav_parse(f, &s->info) == ESP_OK);
    assert(s->info.sample_rate == 44100 && s->info.channels == 1 && s->info.frames == 44100);
    int16_t output[1024 * 2]; esp_err_t err;
    size_t total = 0, count;
    while ((count = aether_wav_read(s, output, 1024, &err)) != 0) {
        assert(err == ESP_OK);
        for (size_t i = 0; i < count; ++i) assert(output[2 * i] == output[2 * i + 1]);
        total += count;
    }
    assert(total == 48000 && aether_wav_position_ms(s) == 1000);
    assert(aether_wav_seek(s, 500) == ESP_OK);
    assert(aether_wav_position_ms(s) == 500);
    assert(aether_wav_read(s, output, 1, &err) == 1 && output[0] == -32768);
    aether_pcm_gain(output, 2, 0); assert(!output[0] && !output[1]);
    output[0] = -32768; output[1] = 32767;
    aether_pcm_gain(output, 2, 100); assert(output[0] == -32768 && output[1] == 32767);
    aether_pcm_gain(output, 2, 50); assert(output[0] == -8192 && output[1] == 8191);
    free(s); fclose(f);
    puts("PASS 44.1k mono interpolation/duration/seek/full-scale arithmetic/silence");

    f = tmpfile(); wav(f, 8000, 2, 1025, false);
    s = calloc(1, sizeof(*s)); s->file = f;
    assert(aether_wav_parse(f, &s->info) == ESP_OK);
    total = 0;
    while ((count = aether_wav_read(s, output, 1024, &err))) { assert(err == ESP_OK); total += count; }
    assert(total == 1025 * 6);
    assert(aether_wav_seek(s, 0) == ESP_OK);
    assert(ftruncate(fileno(f), 60) == 0);
    assert(aether_wav_read(s, output, 1024, &err) == 0 && err == ESP_ERR_INVALID_SIZE);
    free(s); fclose(f);
    puts("PASS 8k stereo buffering boundary and post-open truncated stream");

    f = tmpfile(); wav(f, 48000, 2, 16, false);
    aether_wav_info_t info;
    fseek(f, 4, SEEK_SET); put32(f, UINT32_MAX); fflush(f);
    assert(aether_wav_parse(f, &info) == ESP_ERR_INVALID_SIZE);
    wav(f, 48000, 2, 16, false);
    fseek(f, 20, SEEK_SET); put16(f, 3); fflush(f);
    assert(aether_wav_parse(f, &info) == ESP_ERR_NOT_SUPPORTED);
    wav(f, 48000, 2, 16, false);
    fseek(f, 40, SEEK_SET); put32(f, 63); fflush(f);
    assert(aether_wav_parse(f, &info) == ESP_ERR_INVALID_SIZE);
    fclose(f);
    puts("PASS RIFF overflow/unsupported encoding/unaligned PCM rejection");
}

static aether_audio_player_status_t wait_state(aether_audio_player_handle_t h,
                                               aether_audio_player_state_t state)
{
    aether_audio_player_status_t status = {0};
    for (unsigned i = 0; i < 1000; ++i) {
        aether_audio_player_get_status(h, &status);
        if (status.state == state) return status;
        vTaskDelay(1);
    }
    fprintf(stderr, "wanted state %d, got %d error %x\n", state, status.state, status.last_error);
    assert(0); return status;
}

static void test_playlist_refresh(void)
{
    struct aether_audio_player_t p = {.state_lock = portMUX_INITIALIZER_UNLOCKED};
    p.command_lock = xSemaphoreCreateMutex();
    const char *old[] = {"/sdcard/Music/one.wav", "/sdcard/Music/two.wav"};
    const char *reordered[] = {old[1], old[0]};
    assert(aether_audio_player_set_playlist(&p, old, 2) == ESP_OK);
    assert(xSemaphoreTake(p.command_lock, 0) == pdTRUE);
    assert(aether_audio_player_select(&p, 1) == ESP_ERR_INVALID_STATE);
    assert(aether_audio_player_next(&p, 1) == ESP_ERR_INVALID_STATE);
    assert(!p.status.active && !p.advance_requested && !p.autoplay);
    assert(xSemaphoreGive(p.command_lock) == pdTRUE);
    p.terminate = true;
    assert(aether_audio_player_next(&p, 1) == ESP_ERR_INVALID_STATE);
    p.terminate = false;
    p.status.active = true;
    strcpy(p.status.path, old[0]);
    select_locked(&p, 1);
    assert(aether_audio_player_set_playlist(&p, reordered, 2) == ESP_ERR_INVALID_STATE);
    assert(p.advance_requested && p.stop_requested && p.requested_track == 1);
    assert(!strcmp(p.playlist[1], old[1]));
    p.advance_requested = false; p.stop_requested = false;
    assert(aether_audio_player_set_playlist(&p, reordered, 2) == ESP_OK);
    assert(p.status.track_index == 1 && !strcmp(p.status.path, old[0]));
    assert(aether_audio_player_set_playlist(&p, NULL, 0) == ESP_OK);
    vSemaphoreDelete(p.command_lock);
    assert(pthread_mutex_destroy(&p.state_lock) == 0);
    puts("PASS playlist refresh preserves pending selection and remaps current song outside critical section");
}

static bool another_media_busy(void *context)
{
    return *(bool *)context;
}

static void test_persistence_guard(void)
{
    struct aether_audio_player_t p = {.state_lock = portMUX_INITIALIZER_UNLOCKED};
    p.preferences_dirty = true;
    p.status.volume = 33;
    p.status.repeat = AETHER_AUDIO_REPEAT_ONE;
    aether_audio_eq_t eq = {.bands_db = {-6, 3, 0, -1, 6}, .gain_db = 4, .enabled = true};
    assert(aether_audio_player_set_eq(&p, &eq) == ESP_OK);
    const uint32_t revision = p.preference_revision;
    assert(aether_audio_player_set_eq(&p, &eq) == ESP_OK && p.preference_revision == revision);
    eq.bands_db[0] = -7;
    assert(aether_audio_player_set_eq(&p, &eq) == ESP_ERR_INVALID_ARG);
    eq.bands_db[0] = -6;
    eq.gain_db = 7;
    assert(aether_audio_player_set_eq(&p, &eq) == ESP_ERR_INVALID_ARG);
    eq.gain_db = 4;
    assert(p.preference_revision == revision);
    p.preference_changed = xTaskGetTickCount() - 1000;
    p.status.active = true;
    p.status.state = AETHER_AUDIO_STARTING;
    const unsigned baseline = commits;
    save_preferences(&p, false);
    assert(commits == baseline && p.preferences_dirty);
    aether_audio_player_status_t pending_view;
    assert(aether_audio_player_get_status(&p, &pending_view) == ESP_OK && pending_view.preferences_pending);
    p.status.state = AETHER_AUDIO_PLAYING;
    save_preferences(&p, false);
    assert(commits == baseline && p.preferences_dirty);
    p.status.active = false;
    p.status.state = AETHER_AUDIO_STOPPED;
    bool busy = true;
    aether_audio_player_set_persistence_guard(&p, another_media_busy, &busy);
    save_preferences(&p, false);
    assert(commits == baseline && p.preferences_dirty);
    busy = false;
    save_preferences(&p, false);
    assert(commits == baseline + 1 && !p.preferences_dirty && p.status.settings_error == ESP_OK);
    assert(aether_audio_player_get_status(&p, &pending_view) == ESP_OK && !pending_view.preferences_pending);
    assert(saved_volume == 33 && saved_repeat == AETHER_AUDIO_REPEAT_ONE);
    assert(have_saved_eq && saved_eq == 0x14c5690U);
    p.status.eq = (aether_audio_eq_t){0};
    load_preferences(&p);
    for (unsigned i = 0; i < AETHER_AUDIO_EQ_BANDS; ++i) assert(p.status.eq.bands_db[i] == eq.bands_db[i]);
    assert(p.status.eq.enabled && p.status.eq.gain_db == eq.gain_db);
    saved_eq = 0xffffffffU;
    p.status.eq = (aether_audio_eq_t){0};
    load_preferences(&p);
    assert(!p.status.eq.enabled && pack_eq(&p.status.eq) == 0x66666U);
    have_saved_eq = false;
    puts("PASS EQ settings validated, saved and restored; corrupt settings use flat defaults");
    saved_volume = 65; saved_repeat = AETHER_AUDIO_REPEAT_LIST;
    assert(pthread_mutex_destroy(&p.state_lock) == 0);
    puts("PASS queued next track and other running media defer music preference Flash writes until idle");
}

struct stop_wait_job { aether_audio_player_handle_t handle; esp_err_t result; };
static void *run_stop_wait(void *argument)
{
    struct stop_wait_job *job = argument;
    job->result = aether_audio_player_stop_wait(job->handle, 2000);
    return NULL;
}

static void test_stop_barrier(aether_audio_player_handle_t h, const char *path)
{
    hold_codec_write = true;
    codec_write_entered = false;
    assert(aether_audio_player_play(h, path, 55) == ESP_OK);
    for (unsigned i = 0; i < 1000 && !codec_write_entered; ++i) vTaskDelay(1);
    assert(codec_write_entered);
    struct stop_wait_job job = {.handle = h, .result = ESP_FAIL};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_stop_wait, &job) == 0);
    bool stopping = false;
    for (unsigned i = 0; i < 1000; ++i) {
        portENTER_CRITICAL(&h->state_lock);
        stopping = h->stop_requested;
        portEXIT_CRITICAL(&h->state_lock);
        if (stopping) break;
        vTaskDelay(1);
    }
    assert(stopping);
    assert(aether_audio_player_select(h, 1) == ESP_ERR_INVALID_STATE);
    assert(aether_audio_player_next(h, 1) == ESP_ERR_INVALID_STATE);
    assert(aether_audio_player_next(h, -1) == ESP_ERR_INVALID_STATE);
    assert(aether_audio_player_play(h, path, 55) == ESP_ERR_INVALID_STATE);
    hold_codec_write = false;
    assert(pthread_join(thread, NULL) == 0 && job.result == ESP_OK);
    vTaskDelay(20);
    aether_audio_player_status_t status;
    assert(aether_audio_player_get_status(h, &status) == ESP_OK);
    assert(!status.active && status.state == AETHER_AUDIO_STOPPED);
    assert(!leases && !codec_on);
    puts("PASS stop barrier rejects concurrent selection/next/play while real worker drains codec write");
}

static void test_player(void)
{
    char first[] = "/tmp/aether_music_one_XXXXXX", second[] = "/tmp/aether_music_two_XXXXXX";
    int fd = mkstemp(first); assert(fd >= 0);
    FILE *f = fdopen(fd, "w+b"); wav(f, 44100, 1, 44100 * 30, false); fclose(f);
    fd = mkstemp(second); assert(fd >= 0);
    f = fdopen(fd, "w+b"); wav(f, 48000, 2, 96000, false); fclose(f);
    aether_audio_player_handle_t h;
    assert(aether_audio_player_new((void *)1, (void *)2, &h) == ESP_OK);
    const aether_audio_eq_t loud = {.bands_db = {6, 0, 2, -1, 0}, .gain_db = 6, .enabled = true};
    assert(aether_audio_player_set_eq(h, &loud) == ESP_OK);
    assert(aether_audio_player_set_volume(h, 0) == ESP_OK);
    assert(aether_audio_player_play(h, first, 0) == ESP_OK);
    wait_state(h, AETHER_AUDIO_PLAYING);
    for (unsigned i = 0; i < 100 && writes < 3; ++i) vTaskDelay(1);
    assert(writes >= 3 && nonsilent == 0 && leases == 1);
    assert(aether_audio_player_set_paused(h, true) == ESP_OK);
    aether_audio_player_status_t s = wait_state(h, AETHER_AUDIO_PAUSED);
    const uint32_t paused_position = s.position_ms;
    vTaskDelay(10);
    aether_audio_player_get_status(h, &s);
    assert(s.position_ms == paused_position && !codec_on);
    const unsigned previous_commits = commits;
    for (int volume = 1; volume <= 30; ++volume) {
        assert(aether_audio_player_set_volume(h, volume) == ESP_OK);
        vTaskDelay(2);
    }
    assert(commits == previous_commits); /* Slider traffic is not NVS traffic. */
    fail_commit = true;
    for (unsigned i = 0; i < 1500 && !failed_commits; ++i) vTaskDelay(1);
    aether_audio_player_get_status(h, &s);
    assert(failed_commits && s.settings_error == ESP_FAIL);
    fail_commit = false;
    for (unsigned i = 0; i < 1500; ++i) {
        aether_audio_player_get_status(h, &s);
        if (s.settings_error == ESP_OK) break;
        vTaskDelay(1);
    }
    assert(s.settings_error == ESP_OK && commits > previous_commits);
    assert(aether_audio_player_seek(h, 2000) == ESP_OK);
    for (unsigned i = 0; i < 100; ++i) {
        aether_audio_player_get_status(h, &s);
        if (s.position_ms == 2000) break;
        vTaskDelay(1);
    }
    assert(s.position_ms == 2000);
    assert(aether_audio_player_set_volume(h, 80) == ESP_OK);
    assert(aether_audio_player_set_paused(h, false) == ESP_OK);
    wait_state(h, AETHER_AUDIO_PLAYING);
    for (unsigned i = 0; i < 100 && !nonsilent; ++i) vTaskDelay(1);
    assert(nonsilent && starts >= 2);
    assert(aether_audio_player_stop_wait(h, 1000) == ESP_OK);
    assert(leases == 0 && !codec_on);
    puts("PASS worker silence/pause/seek/resume/codec-release and nonblocking controls");

    const char *paths[] = {first, second};
    assert(aether_audio_player_set_playlist(h, paths, 2) == ESP_OK);
    test_stop_barrier(h, first);
    assert(aether_audio_player_play(h, first, 55) == ESP_OK);
    wait_state(h, AETHER_AUDIO_PLAYING);
    assert(aether_audio_player_next(h, 1) == ESP_OK);
    bool found = false;
    for (unsigned i = 0; i < 500; ++i) {
        aether_audio_player_get_status(h, &s);
        if (!strcmp(s.path, second) && s.state == AETHER_AUDIO_PLAYING) { found = true; break; }
        vTaskDelay(1);
    }
    assert(found);
    assert(aether_audio_player_select(h, 0) == ESP_OK);
    assert(aether_audio_player_select(h, 1) == ESP_OK);
    /* Latest selection stays in the worker, with no foreground page timer. */
    found = false;
    for (unsigned i = 0; i < 500; ++i) {
        aether_audio_player_get_status(h, &s);
        if (!strcmp(s.path, second) && s.state == AETHER_AUDIO_PLAYING) { found = true; break; }
        vTaskDelay(1);
    }
    assert(found);
    assert(aether_audio_player_stop_wait(h, 1000) == ESP_OK);
    vTaskDelay(30);
    aether_audio_player_get_status(h, &s);
    assert(!s.active && s.state == AETHER_AUDIO_STOPPED);
    puts("PASS playlist next and explicit stop suppresses automatic replay");

    assert(aether_audio_player_play(h, first, 55) == ESP_OK);
    wait_state(h, AETHER_AUDIO_PLAYING);
    fail_codec_stop=true;
    const TickType_t release_began=xTaskGetTickCount();
    assert(aether_audio_player_release_session(h)==ESP_OK);
    assert(xTaskGetTickCount()-release_began<20);
    assert(aether_audio_player_stop_wait(h,40)==ESP_ERR_TIMEOUT);
    aether_audio_player_get_status(h,&s);
    assert(s.active&&s.closing&&codec_on&&leases==1);
    assert(aether_audio_player_play(h,first,55)==ESP_ERR_INVALID_STATE);
    assert(aether_audio_player_select(h,0)==ESP_ERR_INVALID_ARG);
    assert(aether_audio_player_next(h,1)==ESP_ERR_INVALID_STATE);
    assert(aether_audio_player_set_playlist(h,paths,2)==ESP_ERR_INVALID_STATE);
    const unsigned retained_starts=starts;
    vTaskDelay(110);assert(starts==retained_starts&&codec_on&&leases==1);
    fail_codec_stop=false;
    for(unsigned i=0;i<1500;++i){aether_audio_player_get_status(h,&s);if(!s.active&&!s.closing)break;vTaskDelay(1);}
    assert(!s.active&&!s.closing&&!codec_on&&!leases&&!s.track_count&&!s.path[0]);
    assert(codec_stop_calls>1&&starts==retained_starts);
    assert(aether_audio_player_set_playlist(h,paths,2)==ESP_OK);
    puts("PASS page-exit release is nonblocking; failed codec close retains active ownership/lease and rejects restarts until retry, then frees playlist/path and permits fresh entry");


    assert(aether_audio_player_set_repeat(h, AETHER_AUDIO_REPEAT_LIST) == ESP_OK);
    assert(aether_audio_player_play(h, first, 55) == ESP_OK);
    wait_state(h, AETHER_AUDIO_PLAYING);
    assert(aether_audio_player_seek(h, 30000) == ESP_OK);
    found = false;
    for (unsigned i = 0; i < 500; ++i) {
        aether_audio_player_get_status(h, &s);
        if (!strcmp(s.path, second) && s.state == AETHER_AUDIO_PLAYING) { found = true; break; }
        vTaskDelay(1);
    }
    assert(found);
    assert(aether_audio_player_set_paused(h, true) == ESP_OK);
    wait_state(h, AETHER_AUDIO_PAUSED);
    inserted = false;
    s = wait_state(h, AETHER_AUDIO_ERROR);
    assert(!s.active && s.last_error == ESP_ERR_INVALID_STATE && !leases && !codec_on);
    vTaskDelay(20); aether_audio_player_get_status(h, &s); assert(!s.active);
    inserted = true;
    puts("PASS natural EOF advances playlist; card removal while paused closes lease and stops list");

    assert(aether_audio_player_set_repeat(h, AETHER_AUDIO_SHUFFLE) == ESP_OK);
    assert(aether_audio_player_set_volume(h, 37) == ESP_OK);
    assert(aether_audio_player_del(h) == ESP_OK);
    assert(saved_volume == 37 && saved_repeat == AETHER_AUDIO_SHUFFLE && commits);
    assert(aether_audio_player_new((void *)1, (void *)2, &h) == ESP_OK);
    aether_audio_player_get_status(h, &s);
    assert(s.volume == 37 && s.repeat == AETHER_AUDIO_SHUFFLE);
    assert(aether_audio_player_play(h, "/tmp/no_such_aether_song", 37) == ESP_OK);
    s = wait_state(h, AETHER_AUDIO_ERROR);
    assert(s.last_error == ESP_ERR_NOT_FOUND && !leases);
    const unsigned before_mp3 = nonsilent;
    assert(aether_audio_player_play(h, "tests/player/fixtures/sine-stereo.mp3", 70) == ESP_OK);
    s = wait_state(h, AETHER_AUDIO_FINISHED);
    assert(s.mp3 && s.sample_rate == 44100 && s.channels == 2 && nonsilent > before_mp3);
    assert(!leases && !codec_on);
    const char *aac_paths[] = {"audio-only.m4a", "high-profile-aac.mp4", "iso5-fragmented.mp3"};
    for (unsigned i = 0; i < 3; ++i) {
        char path[256];
        snprintf(path, sizeof(path), "build/host_mp4_runtime/%s", aac_paths[i]);
        assert(aether_audio_player_play(h, path, 70) == ESP_OK);
        s = wait_state(h, AETHER_AUDIO_FINISHED);
        assert(s.aac && !s.mp3 && s.sample_rate == 48000 && s.channels == 2);
        assert(s.duration_ms == 1024 && s.position_ms == 1024 && !leases && !codec_on);
    }
    puts("PASS music detects actual MP4 container including iso5 fragmented .mp3; AAC pull EOF releases codec/card");
    assert(aether_audio_player_del(h) == ESP_OK);
    assert(!leases && !codec_on);
    unlink(first); unlink(second);
    for (size_t i = 0; i < task_count; ++i) {
        assert(pthread_join(tasks[i]->thread, NULL) == 0);
        free(tasks[i]);
    }
    puts("PASS NVS volume/mode restore, file errors and task teardown");
}

static bool cancel_mp3(void *context) { return *(bool *)context; }

static void test_mp3(void)
{
    const char *paths[] = {"tests/player/fixtures/silence-stereo.mp3", "tests/player/fixtures/silence-mono.mp3"};
    for (unsigned ch = 0; ch < 2; ++ch) {
        FILE *f = fopen(paths[ch], "rb"); assert(f);
        bool cancel = false;
        aether_mp3_stream_t *s = NULL;
        assert(aether_mp3_open(f, cancel_mp3, &cancel, &s) == ESP_OK);
        assert(aether_mp3_rate(s) == 44100 && aether_mp3_channels(s) == (ch ? 1 : 2));
        assert(aether_mp3_duration_ms(s) == 1044);
        int16_t pcm[2048]; esp_err_t err;
        size_t total = 0, frames;
        while ((frames = aether_mp3_read(s, pcm, 1024, &err))) {
            assert(err == ESP_OK);
            for (size_t i = 0; i < frames * 2; ++i) assert(pcm[i] == 0);
            total += frames;
        }
        assert(err == ESP_OK && total == 50156);
        assert(aether_mp3_seek(s, 500) == ESP_OK);
        assert(aether_mp3_position_ms(s) == 500);
        assert(aether_mp3_read(s, pcm, 1024, &err) == 1024 && err == ESP_OK);
        cancel = true;
        assert(aether_mp3_read(s, pcm, 1024, &err) == 0 && err == ESP_ERR_INVALID_STATE);
        aether_mp3_close(s); fclose(f);
    }
    FILE *f = tmpfile(); assert(f);
    fputs("invalid MP3 data", f); fflush(f); rewind(f);
    bool cancel = false;
    aether_mp3_stream_t *s = NULL;
    assert(aether_mp3_open(f, cancel_mp3, &cancel, &s) != ESP_OK && !s);
    fclose(f);
    f = fopen("tests/player/fixtures/sine-stereo.mp3", "rb"); assert(f);
    assert(aether_mp3_open(f, cancel_mp3, &cancel, &s) == ESP_OK);
    assert(aether_mp3_duration_ms(s) == 992); /* Frame count of the checked-in CBR fixture. */
    int16_t *signal = calloc(65536 * 2, sizeof(*signal)); assert(signal);
    size_t total = 0, got;
    esp_err_t err;
    while ((got = aether_mp3_read(s, signal + total * 2, 1024, &err))) {
        assert(err == ESP_OK);
        total += got; assert(total < 65536 - 1024);
    }
    assert(err == ESP_OK && total > 43200);
    unsigned crosses[2] = {0};
    uint64_t amplitude[2] = {0};
    for (size_t i = 5000; i < total; ++i) for (unsigned ch = 0; ch < 2; ++ch) {
        amplitude[ch] += (unsigned)abs(signal[i * 2 + ch]);
        if (signal[(i - 1) * 2 + ch] <= 0 && signal[i * 2 + ch] > 0) ++crosses[ch];
    }
    assert(amplitude[0] > 10000000 && amplitude[1] > 10000000 && crosses[0] > 300);
    assert(crosses[1] > crosses[0] * 18 / 10 && crosses[1] < crosses[0] * 22 / 10);
    assert(aether_mp3_seek(s, 500) == ESP_OK);
    int16_t seek_pcm[2048];
    assert(aether_mp3_read(s, seek_pcm, 1024, &err) == 1024 && err == ESP_OK);
    for (size_t i = 0; i < 2048; ++i) assert(abs(seek_pcm[i] - signal[24000 * 2 + i]) <= 2);
    free(signal); aether_mp3_close(s); fclose(f);
    puts("PASS real MPEG Layer III mono/stereo decode, 44.1k conversion, sample seek, cancellation and malformed input");
    puts("PASS original 440/880Hz MP3 has nonzero audio, distinct channels and sample-accurate indexed seek");
}

int main(void)
{
    test_profile_snapshot();
    test_wav();
    test_mp3();
    test_playlist_refresh();
    test_persistence_guard();
    test_player();
    puts("All player host regressions passed.");
    return 0;
}
