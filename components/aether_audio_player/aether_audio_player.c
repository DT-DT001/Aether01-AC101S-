#include "aether_media_fs.h"
#include "aether_audio_player.h"
#include "wav_stream.h"
#include "mp3_stream.h"
#include "music_eq.h"
#include "aether_mp4_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define MUSIC_TASK_PRIORITY 7
#define MUSIC_SAVE_DELAY_MS 800U

static const char *TAG = "audio_player";
static const char *const resume_key = "resume_v1";

typedef struct {
    uint32_t position_ms, duration_ms;
    char path[192];
} music_resume_t;

struct aether_audio_player_t {
    ac101s_audio_handle_t audio;
    aether_storage_handle_t storage;
    aether_media_fs_lease_t media_lease;
    SemaphoreHandle_t command_lock;
    TaskHandle_t task;
    portMUX_TYPE state_lock;
    aether_audio_player_status_t status;
    bool stop_requested;
    bool pause_requested;
    bool screen_suspended;
    bool standby_music; /* The current uninterrupted music session reached playback. */
    bool terminate;
    bool exited;
    uint32_t release_serial;
    bool preferences_dirty;
    uint32_t preference_revision;
    TickType_t preference_changed;
    music_resume_t resume;
    bool resume_dirty, resume_selected;
    TickType_t resume_changed;
    uint32_t seek_revision;
    uint32_t seek_ms;
    char (*playlist)[192];
    bool advance_requested;
    bool autoplay;
    uint16_t requested_track;
    bool (*persistence_busy)(void *);
    void *persistence_context;
};

static void clear_profile_locked(aether_audio_player_handle_t h)
{
    h->status.io_profile_valid=false;
    h->status.pull_us=h->status.pull_max_us=h->status.read_us=h->status.read_max_us=0;
    h->status.decode_no_io_us=h->status.decode_max_us=h->status.write_us=h->status.write_max_us=0;
    h->status.supply_gap_us=h->status.supply_gap_max_us=0;
    h->status.refill_max_bytes=h->status.refill_max_us=0;
}

typedef struct {
    bool stop, pause, terminate;
    int volume;
    aether_audio_eq_t eq;
    uint32_t seek_revision, seek_ms;
} controls_t;

static bool pause_locked(aether_audio_player_handle_t h)
{
    return h->pause_requested || (h->screen_suspended && !h->standby_music);
}

static controls_t controls(aether_audio_player_handle_t h)
{
    portENTER_CRITICAL(&h->state_lock);
    const controls_t c = {.stop = h->stop_requested, .pause = pause_locked(h),
        .terminate = h->terminate, .volume = h->status.volume, .eq = h->status.eq,
        .seek_revision = h->seek_revision, .seek_ms = h->seek_ms};
    portEXIT_CRITICAL(&h->state_lock);
    return c;
}

void aether_audio_player_set_persistence_guard(aether_audio_player_handle_t h,
                                               bool (*busy)(void *), void *context)
{
    if (!h) return;
    portENTER_CRITICAL(&h->state_lock);
    h->persistence_busy = busy;
    h->persistence_context = context;
    portEXIT_CRITICAL(&h->state_lock);
    if (h->task) xTaskNotifyGive(h->task);
}

/* eq_v1 stores five biased four-bit band gains, a four-bit output gain,
 * and one enable bit. Older settings have no eq_v1 key and start flat. */
static uint32_t pack_eq(const aether_audio_eq_t *eq)
{
    uint32_t packed = ((uint32_t)eq->enabled << 24) | ((uint32_t)eq->gain_db << 20);
    for (unsigned i = 0; i < AETHER_AUDIO_EQ_BANDS; ++i) {
        packed |= (uint32_t)(eq->bands_db[i] + AETHER_AUDIO_EQ_MAX_DB) << (4 * i);
    }
    return packed;
}

static bool valid_eq(const aether_audio_eq_t *eq)
{
    if (eq->gain_db > AETHER_AUDIO_EQ_MAX_GAIN_DB) return false;
    for (unsigned i = 0; i < AETHER_AUDIO_EQ_BANDS; ++i) {
        const int db = eq->bands_db[i];
        if (db < -AETHER_AUDIO_EQ_MAX_DB || db > AETHER_AUDIO_EQ_MAX_DB) return false;
    }
    return true;
}

static bool recording_path(const char *path)
{
    /* Recorder auditions also use this player, including from Files and USB. */
    static const char recordings[] = "/Recordings";
    const size_t length = sizeof(recordings) - 1;
    const char *directory = path[0] == '/' ? strchr(path + 1, '/') : NULL;
    return directory && !strncasecmp(directory, recordings, length) &&
           (directory[length] == '/' || !directory[length]);
}

static bool music_resume_path(const char *path)
{
    return aether_media_fs_safe_path(path) && !recording_path(path);
}

void aether_audio_player_set_screen_suspended(aether_audio_player_handle_t h, bool suspended)
{
    if (!h) return;
    portENTER_CRITICAL(&h->state_lock);
    h->screen_suspended = suspended;
    if (suspended && h->pause_requested) h->standby_music = false;
    portEXIT_CRITICAL(&h->state_lock);
    if (h->task) xTaskNotifyGive(h->task);
}

static void load_preferences(aether_audio_player_handle_t h)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("music", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) { h->status.settings_error = err; return; }
    uint8_t value;
    if (nvs_get_u8(nvs, "volume", &value) == ESP_OK && value <= 100) h->status.volume = value;
    if (nvs_get_u8(nvs, "repeat", &value) == ESP_OK && value <= AETHER_AUDIO_SHUFFLE) {
        h->status.repeat = (aether_audio_repeat_t)value;
    }
    uint32_t packed;
    if (nvs_get_u32(nvs, "eq_v1", &packed) == ESP_OK && (packed >> 25) == 0) {
        aether_audio_eq_t eq = {.enabled = (packed & (1UL << 24)) != 0, .gain_db = (packed >> 20) & 15U};
        for (unsigned i = 0; i < AETHER_AUDIO_EQ_BANDS; ++i) {
            eq.bands_db[i] = (int)((packed >> (4 * i)) & 15U) - AETHER_AUDIO_EQ_MAX_DB;
        }
        if (valid_eq(&eq)) h->status.eq = eq;
    }
    music_resume_t resume;
    size_t bytes = sizeof(resume);
    if (nvs_get_blob(nvs, resume_key, &resume, &bytes) == ESP_OK && bytes == sizeof(resume) &&
        resume.position_ms < resume.duration_ms && memchr(resume.path, '\0', sizeof(resume.path)) &&
        music_resume_path(resume.path)) h->resume = resume;
    nvs_close(nvs);
}

static void save_preferences(aether_audio_player_handle_t h, bool force)
{
    const TickType_t delay = pdMS_TO_TICKS(MUSIC_SAVE_DELAY_MS);
    portENTER_CRITICAL(&h->state_lock);
    const TickType_t now = xTaskGetTickCount();
    const bool save_settings = h->preferences_dirty && (force || now - h->preference_changed >= delay);
    const bool save_resume = h->resume_dirty && (force || now - h->resume_changed >= delay);
    const uint8_t volume = h->status.volume, repeat = h->status.repeat;
    const aether_audio_eq_t eq = h->status.eq;
    const uint32_t revision = h->preference_revision;
    const bool active = h->status.active && h->status.state != AETHER_AUDIO_PAUSED;
    bool (*busy)(void *) = h->persistence_busy;
    void *busy_context = h->persistence_context;
    portEXIT_CRITICAL(&h->state_lock);
    if (!save_settings && !save_resume) return;
    /* A next track may already be queued when the previous stream ends.
     * The idle music worker can also overlap video/NES after a page change.
     * Keep preferences pending until the active media allows Flash writes.
     * Explicit player teardown still flushes its final preferences. */
    if (!force && (active || (busy && busy(busy_context)))) return;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("music", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        if (save_settings) {
            err = nvs_set_u8(nvs, "volume", volume);
            if (err == ESP_OK) err = nvs_set_u8(nvs, "repeat", repeat);
            if (err == ESP_OK) err = nvs_set_u32(nvs, "eq_v1", pack_eq(&eq));
        }
        /* Only the player worker updates the cached record after startup. */
        if (err == ESP_OK && save_resume) err = nvs_set_blob(nvs, resume_key, &h->resume, sizeof(h->resume));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    portENTER_CRITICAL(&h->state_lock);
    h->status.settings_error = err;
    if (save_settings && revision == h->preference_revision) {
        h->preferences_dirty = err != ESP_OK;
        /* Retry a failed transaction with the same bounded debounce, rather
         * than losing the preference or spinning on flash every 20ms. */
        if (err != ESP_OK) h->preference_changed = xTaskGetTickCount();
    }
    if (save_resume) {
        h->resume_dirty = err != ESP_OK;
        if (err != ESP_OK) h->resume_changed = xTaskGetTickCount();
    }
    portEXIT_CRITICAL(&h->state_lock);
}

static void remember_position_locked(aether_audio_player_handle_t h)
{
    const uint32_t duration = h->status.duration_ms;
    if (!duration || !music_resume_path(h->status.path)) return;
    const uint32_t position = h->status.position_ms < duration ? h->status.position_ms : 0;
    if (h->resume.position_ms == position && h->resume.duration_ms == duration &&
        !strcmp(h->resume.path, h->status.path)) return;

    h->resume = (music_resume_t){.position_ms = position, .duration_ms = duration};
    strcpy(h->resume.path, h->status.path);
    h->resume_dirty = true;
    /* First commit is due immediately after release. Failures back off. */
    h->resume_changed = xTaskGetTickCount() - pdMS_TO_TICKS(MUSIC_SAVE_DELAY_MS);
}

static bool mp3_canceled(void *context)
{
    aether_audio_player_handle_t h = context;
    const controls_t c = controls(h);
    return c.stop || c.terminate || !aether_media_fs_available(&h->media_lease);
}

static void report_stream_failure(FILE *file, const char *path, const char *stage,
                                   esp_err_t error, bool card_inserted)
{
    char signature[33] = "unavailable";
    int64_t file_size = -1;
    if (file && card_inserted &&
        (error == ESP_ERR_NOT_SUPPORTED || error == ESP_ERR_INVALID_RESPONSE)) {
        struct stat info;
        if (fstat(fileno(file), &info) == 0) file_size = (int64_t)info.st_size;
        /* Only read the binary file signature on format rejection. Do not
         * delay hot-unplug/error cleanup with diagnostic card accesses. */
        if (fseek(file, 0, SEEK_SET) == 0) {
            unsigned char bytes[16];
            const size_t count = fread(bytes, 1, sizeof(bytes), file);
            static const char hex[] = "0123456789abcdef";
            for (size_t i = 0; i < count; ++i) {
                signature[i * 2] = hex[bytes[i] >> 4];
                signature[i * 2 + 1] = hex[bytes[i] & 15];
            }
            signature[count * 2] = 0;
        }
    }
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    ESP_LOGW(TAG, "playback file=%s stage=%s error=%s bytes=%lld header=%s", name,
             stage, esp_err_to_name(error), (long long)file_size, signature);
    /* Also compile cleanly when warning logging is disabled. */
    (void)name; (void)stage; (void)error; (void)file_size; (void)signature;
}

static esp_err_t play_stream(aether_audio_player_handle_t h, const char *path, bool *finished)
{
    *finished = false;
    esp_err_t err = aether_media_fs_acquire(h->storage, path, &h->media_lease);
    if (err != ESP_OK) {
        report_stream_failure(NULL, path, "storage_acquire", err, false);
        return err;
    }
    const char *stage = "open_file";
    FILE *file = fopen(path, "rb");
    aether_wav_stream_t *stream = NULL;
    aether_mp3_stream_t *mp3 = NULL;
    aether_mp4_audio_handle_t aac = NULL;
    aether_mp4_audio_info_t aac_info = {0};
    char detail[112] = "";
    int16_t *output = NULL;
    char *file_buffer = NULL;
    bool codec_started = false;
    int64_t previous_write_end=0;
    if (!file) { err = ESP_ERR_NOT_FOUND; goto done; }
    file_buffer = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file_buffer) { err = ESP_ERR_NO_MEM; stage = "allocate_file_cache"; goto done; }
    if (setvbuf(file, file_buffer, _IOFBF, 8192)) { err = ESP_FAIL; stage = "set_file_cache"; goto done; }
    stage = "allocate_pcm";
    const size_t pcm_bytes = 1024 * 2 * sizeof(int16_t);
    /* Resampling, EQ and I2S all touch this block. Keep it off the external
     * memory bus where possible while LVGL is composing and flushing a list. */
    output = heap_caps_malloc(pcm_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!output) output = heap_caps_malloc(pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) { err = ESP_ERR_NO_MEM; goto done; }
    /* Decode the actual container, including MP4/AAC files named .mp3. */
    unsigned char magic[16];
    const size_t magic_size = fread(magic, 1, sizeof(magic), file);
    if (ferror(file) || fseek(file, 0, SEEK_SET)) { err = ESP_FAIL; stage = "read_signature"; goto done; }
    if (magic_size >= 8 && !memcmp(magic + 4, "ftyp", 4)) {
        stage = "mp4_audio_open";
        err = aether_mp4_audio_open(file, mp3_canceled, h, &aac, detail, sizeof(detail));
        if (err != ESP_OK) goto done;
        err = aether_mp4_audio_get_info(aac, &aac_info);
        if (err != ESP_OK) goto done;
    } else if (!(magic_size >= 12 && !memcmp(magic, "RIFF", 4) && !memcmp(magic + 8, "WAVE", 4))) {
        stage = "mp3_open";
        err = aether_mp3_open(file, mp3_canceled, h, &mp3);
        if (err != ESP_OK) goto done;
    } else {
        stage = "allocate_wav";
        stream = calloc(1, sizeof(*stream));
        if (!stream) { err = ESP_ERR_NO_MEM; goto done; }
        stream->file = file;
        stage = "wav_parse";
        err = aether_wav_parse(file, &stream->info);
        if (err != ESP_OK) goto done;
        if (!stream->info.frames) { err = ESP_ERR_INVALID_SIZE; goto done; }
    }
    portENTER_CRITICAL(&h->state_lock);
    h->status.sample_rate = aac ? aac_info.sample_rate : mp3 ? aether_mp3_rate(mp3) : stream->info.sample_rate;
    h->status.channels = aac ? aac_info.channels : mp3 ? aether_mp3_channels(mp3) : stream->info.channels;
    h->status.duration_ms = aac ? aac_info.duration_ms : mp3 ? aether_mp3_duration_ms(mp3) :
        (uint32_t)((uint64_t)stream->info.frames * 1000 / stream->info.sample_rate);
    /* A restored position is a startup seek. Wait for the actual duration
     * and let a newer user seek take precedence. A replaced, shorter file
     * starts from the beginning instead of immediately advancing at EOF. */
    if (!h->seek_revision && h->seek_ms && h->seek_ms < h->status.duration_ms) {
        h->seek_revision = 1;
        h->status.seek_requested_ms = h->seek_ms;
        h->status.seeking = true;
    }
    h->status.mp3 = mp3 != NULL;
    h->status.aac = aac != NULL;
    h->status.detail[0] = 0;
    portEXIT_CRITICAL(&h->state_lock);
    if(mp3) aether_mp3_clear_metrics(mp3); /* Startup scan is not a playback stall. */
    aether_mp3_metrics_t startup_io={0};
    if(mp3) aether_mp3_get_metrics(mp3,&startup_io);
    ESP_LOGI(TAG,"start path=%s codec=%s source_hz=%lu channels=%u duration_ms=%lu output=48000/stereo io_buffer=%lu priority=%u",
        path,aac ? "AAC-LC" : mp3 ? "MP3" : "PCM16",
        (unsigned long)(aac ? aac_info.sample_rate : mp3 ? aether_mp3_rate(mp3) : stream->info.sample_rate),
        aac ? aac_info.channels : mp3 ? aether_mp3_channels(mp3) : stream->info.channels,
        (unsigned long)h->status.duration_ms,(unsigned long)startup_io.io_buffer_bytes,(unsigned)MUSIC_TASK_PRIORITY);
    music_eq_t eq = {0};
    uint32_t applied_seek = 0;
    while (true) {
        const controls_t c = controls(h);
        if (c.stop || c.terminate) break;
        if (!aether_media_fs_available(&h->media_lease)) { err = ESP_ERR_INVALID_STATE; stage = "card_removed"; break; }
        if (c.seek_revision != applied_seek) {
            previous_write_end=0;
            if (codec_started) {
                stage = "codec_stop_seek";
                err = ac101s_audio_stop(h->audio);
                if (err != ESP_OK) break;
                codec_started = false;
            }
            stage = aac ? "aac_seek" : mp3 ? "mp3_seek" : "wav_seek";
            err = aac ? aether_mp4_audio_seek(aac, c.seek_ms) : mp3 ? aether_mp3_seek(mp3, c.seek_ms) : aether_wav_seek(stream, c.seek_ms);
            if (err != ESP_OK) break;
            applied_seek = c.seek_revision;
            music_eq_reset(&eq);
            if(mp3) aether_mp3_clear_metrics(mp3);
            if (aac) aether_mp4_audio_get_info(aac, &aac_info);
            portENTER_CRITICAL(&h->state_lock);
            if (h->seek_revision == applied_seek) {
                h->status.position_ms = aac ? aac_info.position_ms : mp3 ? aether_mp3_position_ms(mp3) : aether_wav_position_ms(stream);
                h->status.seeking = false;
            }
            portEXIT_CRITICAL(&h->state_lock);
            /* Catch up to requests made during decoding before starting the
             * transport or writing a block from a superseded destination. */
            continue;
        }
        if (c.pause) {
            previous_write_end=0; /* Intentional pause/seek is not starvation. */
            if (codec_started) {
                stage = "codec_stop_pause";
                err = ac101s_audio_stop(h->audio);
                if (err != ESP_OK) break;
                codec_started = false;
            }
            portENTER_CRITICAL(&h->state_lock);
            h->status.state = AETHER_AUDIO_PAUSED;
            h->standby_music = false;
            portEXIT_CRITICAL(&h->state_lock);
            save_preferences(h, false);
            /* Keep the lease, but observe removal every 20ms. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        if (!codec_started) {
            /* Software scaling guarantees zero PCM when volume is zero. */
            stage = "codec_start";
            err = ac101s_audio_start(h->audio, 100, 24.0f);
            if (err != ESP_OK) break;
            codec_started = true;
            portENTER_CRITICAL(&h->state_lock);
            const bool ready = !pause_locked(h) && !h->stop_requested && !h->terminate;
            if (ready) {
                h->status.state = AETHER_AUDIO_PLAYING;
                h->standby_music = !recording_path(path);
            }
            portEXIT_CRITICAL(&h->state_lock);
            if (!ready) continue;
        }
        stage = aac ? "aac_decode" : mp3 ? "mp3_decode" : "wav_read";
        aether_mp3_metrics_t io_before={0},io_after={0};
        if(mp3) aether_mp3_get_metrics(mp3,&io_before);
        const int64_t pull_begin=esp_timer_get_time();
        const size_t frames = aac ? aether_mp4_audio_read(aac, output, 1024, &err) : mp3 ? aether_mp3_read(mp3, output, 1024, &err) :
            aether_wav_read(stream, output, 1024, &err);
        const int64_t pull_end=esp_timer_get_time();
        if(mp3) aether_mp3_get_metrics(mp3,&io_after);
        const uint32_t pull_us=(uint32_t)(pull_end-pull_begin);
        const uint32_t read_us=mp3 ? (uint32_t)(io_after.read_total_us-io_before.read_total_us) : 0;
        const uint32_t decode_us=mp3 && pull_us>read_us ? pull_us-read_us : 0;
        if (err != ESP_OK) break;
        const controls_t latest = controls(h);
        if (latest.stop || latest.terminate || latest.seek_revision != applied_seek) continue;
        if (!aether_media_fs_available(&h->media_lease)) { err = ESP_ERR_INVALID_STATE; stage = "card_removed"; break; }
        if (!frames) { *finished = true; break; }
        music_eq_process(&eq, &c.eq, output, frames);
        aether_pcm_gain(output, frames * 2, c.volume);
        stage = "codec_write";
        const int64_t write_begin=esp_timer_get_time();
        const uint32_t gap_us=previous_write_end ? (uint32_t)(write_begin-previous_write_end) : 0;
        err = ac101s_audio_write(h->audio, output, frames * 4);
        const int64_t write_end=esp_timer_get_time();
        const uint32_t write_us=(uint32_t)(write_end-write_begin);
        previous_write_end=write_end;
        portENTER_CRITICAL(&h->state_lock);
        h->status.io_profile_valid=mp3!=NULL;
        h->status.pull_us=pull_us; h->status.read_us=read_us; h->status.decode_no_io_us=decode_us;
        h->status.write_us=write_us; h->status.supply_gap_us=gap_us;
        if(pull_us>h->status.pull_max_us) h->status.pull_max_us=pull_us;
        if(read_us>h->status.read_max_us) h->status.read_max_us=read_us;
        if(decode_us>h->status.decode_max_us) h->status.decode_max_us=decode_us;
        if(write_us>h->status.write_max_us) h->status.write_max_us=write_us;
        if(gap_us>h->status.supply_gap_max_us) h->status.supply_gap_max_us=gap_us;
        if(mp3) { h->status.refill_max_bytes=io_after.refill_max_bytes; h->status.refill_max_us=io_after.refill_max_us; }
        portEXIT_CRITICAL(&h->state_lock);
        if (err != ESP_OK) break;
        if (aac) aether_mp4_audio_get_info(aac, &aac_info);
        portENTER_CRITICAL(&h->state_lock);
        h->status.position_ms = aac ? aac_info.position_ms : mp3 ? aether_mp3_position_ms(mp3) : aether_wav_position_ms(stream);
        portEXIT_CRITICAL(&h->state_lock);
        taskYIELD();
    }
done:
    if (codec_started) {
        /* A failed close may still own live DMA/USB transport. Keep this worker
         * active until the codec confirms release; bounded stop_wait callers
         * can time out without handing ownership to a new player. */
        unsigned retries = 0;
        for (;;) {
            const esp_err_t stopped = ac101s_audio_stop(h->audio);
            if (stopped == ESP_OK) break;
            if (err == ESP_OK) { err = stopped; stage = "codec_stop"; }
            portENTER_CRITICAL(&h->state_lock);
            h->status.last_error = stopped;
            portEXIT_CRITICAL(&h->state_lock);
            if ((retries++ % 10U) == 0)
                ESP_LOGW(TAG, "audio cleanup pending: %s", esp_err_to_name(stopped));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!aether_media_fs_available(&h->media_lease) && !controls(h).stop) {
        err = ESP_ERR_INVALID_STATE; stage = "card_removed";
    }
    if (err != ESP_OK && !controls(h).stop)
        report_stream_failure(file, path, stage, err, aether_media_fs_available(&h->media_lease));
    if (err != ESP_OK && !controls(h).stop && detail[0]) {
        ESP_LOGW(TAG, "audio format: %s", detail);
        portENTER_CRITICAL(&h->state_lock);
        memcpy(h->status.detail, detail, sizeof(h->status.detail));
        portEXIT_CRITICAL(&h->state_lock);
    }
    free(stream);
    aether_mp3_close(mp3);
    aether_mp4_audio_close(aac);
    free(output);
    if (file) fclose(file);
    free(file_buffer); /* FILE owns neither the caller-provided buffer nor its lifetime. */
    aether_media_fs_release(&h->media_lease);
    return err;
}

static void finish_session_release(aether_audio_player_handle_t h)
{
    /* play/set_playlist scan paths under command_lock. Never free a list while
     * a foreground caller still owns that lock, or from an ISR/LVGL callback. */
    portENTER_CRITICAL(&h->state_lock);
    const bool ready_to_release = h->status.closing && !h->status.active;
    portEXIT_CRITICAL(&h->state_lock);
    if (!ready_to_release) return;
    if (xSemaphoreTake(h->command_lock, 0) != pdTRUE) return;
    portENTER_CRITICAL(&h->state_lock);
    if (!h->status.closing || h->status.active) {
        portEXIT_CRITICAL(&h->state_lock);
        xSemaphoreGive(h->command_lock);
        return;
    }
    const uint32_t release_serial = h->release_serial;
    char (*old)[192] = h->playlist;
    h->playlist = NULL;
    h->status.track_count = h->status.track_index = 0;
    h->status.position_ms = h->status.duration_ms = 0;
    h->status.path[0] = 0; h->status.mp3 = h->status.aac = false;
    h->status.state = AETHER_AUDIO_STOPPED;
    h->resume_selected = false;
    portEXIT_CRITICAL(&h->state_lock);
    free(old);
    xSemaphoreGive(h->command_lock);
    /* A page polling closing=false must already be able to submit commands. */
    portENTER_CRITICAL(&h->state_lock);
    if (h->release_serial == release_serial) h->status.closing = false;
    portEXIT_CRITICAL(&h->state_lock);
}

static void player_task(void *argument)
{
    aether_audio_player_handle_t h = argument;
    while (true) {
        portENTER_CRITICAL(&h->state_lock);
        const bool terminate = h->terminate;
        const bool active = h->status.active;
        char path[192];
        memcpy(path, h->status.path, sizeof(path));
        portEXIT_CRITICAL(&h->state_lock);
        if (terminate) break;
        if (active) {
            bool finished = false;
            const esp_err_t err = play_stream(h, path, &finished);
            const bool inserted = aether_media_fs_path_ready(h->storage, path);
            const uint32_t shuffle = esp_random();
            portENTER_CRITICAL(&h->state_lock);
            const bool position_known = h->status.position_ms || (err == ESP_OK &&
                (finished || h->status.state == AETHER_AUDIO_PLAYING || h->status.state == AETHER_AUDIO_PAUSED));
            h->status.last_error = h->stop_requested ? ESP_OK : err;
            h->status.state = h->stop_requested ? AETHER_AUDIO_STOPPED :
                err != ESP_OK ? AETHER_AUDIO_ERROR :
                finished ? AETHER_AUDIO_FINISHED : AETHER_AUDIO_STOPPED;
            h->status.active = false; /* Codec/file/lease cleanup is complete. */
            h->status.seeking = false;
            h->status.seek_requested_ms = 0;
            const bool requested = h->advance_requested;
            bool next = requested || (finished && h->autoplay && err == ESP_OK);
            if (h->terminate || !inserted || !h->playlist || !h->status.track_count) next = false;
            if (!next && position_known) remember_position_locked(h);
            if (next) {
                uint16_t index = h->requested_track;
                if (!requested) {
                    index = h->status.track_index;
                    if (h->status.repeat == AETHER_AUDIO_REPEAT_LIST) index = (index + 1) % h->status.track_count;
                    else if (h->status.repeat == AETHER_AUDIO_SHUFFLE && h->status.track_count > 1) {
                        index = (index + 1 + shuffle % (h->status.track_count - 1)) % h->status.track_count;
                    }
                }
                h->status.track_index = index;
                memcpy(h->status.path, h->playlist[index], sizeof(h->status.path));
                h->standby_music = h->standby_music && !recording_path(h->status.path);
                h->status.position_ms = h->status.duration_ms = 0;
                clear_profile_locked(h);
                h->status.mp3 = h->status.aac = false;
                h->status.detail[0] = 0;
                h->status.state = AETHER_AUDIO_STARTING;
                h->status.active = true;
                h->status.last_error = ESP_OK;
                h->seek_ms = h->seek_revision = 0;
            } else h->standby_music = false;
            h->advance_requested = false;
            if (!inserted || err != ESP_OK) h->autoplay = false;
            h->stop_requested = false;
            /* An EOF must not erase a user pause queued during cleanup. */
            if (requested || !next) h->pause_requested = false;
            portEXIT_CRITICAL(&h->state_lock);
        }
        finish_session_release(h);
        /* Never commit NVS during streamed output. */
        save_preferences(h, false);
        portENTER_CRITICAL(&h->state_lock);
        const bool again = h->status.active;
        const bool closing = h->status.closing, dirty = h->preferences_dirty || h->resume_dirty;
        portEXIT_CRITICAL(&h->state_lock);
        if (again) continue;
        /* Idle management owns no media hardware and need not wake five times
         * per second. Only deferred preferences/close contention need a timer. */
        ulTaskNotifyTake(pdTRUE, closing ? pdMS_TO_TICKS(10) :
                        dirty ? pdMS_TO_TICKS(200) : portMAX_DELAY);
    }
    save_preferences(h, true);
    portENTER_CRITICAL(&h->state_lock);
    h->exited = true;
    portEXIT_CRITICAL(&h->state_lock);
    vTaskDelete(NULL); /* del may now free h. */
}

esp_err_t aether_audio_player_new(ac101s_audio_handle_t audio,
                                  aether_storage_handle_t storage,
                                  aether_audio_player_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(audio && storage && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    aether_audio_player_handle_t h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    h->audio = audio;
    h->storage = storage;
    h->state_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    h->command_lock = xSemaphoreCreateMutex();
    h->status.volume = 65;
    h->status.repeat = AETHER_AUDIO_REPEAT_LIST;
    load_preferences(h); /* Called at startup, outside LVGL. */
    if (!h->command_lock) {
        free(h);
        return ESP_ERR_NO_MEM;
    }
    /* S31 GCC -O2 -fstack-usage measures mp3dec_decode_frame at 16656 bytes.
     * 24KiB leaves >7KiB for its call chain, stdio and FreeRTOS context. */
#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    /* MP3 and EQ use the FPU, which pins a task to its current S31 core.
     * Choose core 1 before decoding, away from the core 0 LVGL worker. */
    const BaseType_t created = xTaskCreatePinnedToCore(player_task, "music_player", 24576, h,
                                                     MUSIC_TASK_PRIORITY, &h->task, 1);
#else
    const BaseType_t created = xTaskCreate(player_task, "music_player", 24576, h, MUSIC_TASK_PRIORITY, &h->task);
#endif
    if (created != pdPASS) {
        vSemaphoreDelete(h->command_lock);
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = h;
    return ESP_OK;
}

esp_err_t aether_audio_player_play(aether_audio_player_handle_t h, const char *path, int volume)
{
    if (!h || !path || !path[0] || volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    if (strlen(path) >= sizeof(h->status.path)) return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(h->command_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    /* command_lock keeps the playlist alive; scanning PSRAM paths must not
     * hold the interrupt-disabling state lock for thousands of comparisons. */
    uint16_t track_index = 0;
    for (size_t i = 0; i < h->status.track_count; ++i) {
        if (!strcmp(path, h->playlist[i])) { track_index = (uint16_t)i; break; }
    }
    portENTER_CRITICAL(&h->state_lock);
    if (h->status.active || h->terminate || h->status.closing) {
        portEXIT_CRITICAL(&h->state_lock);
        xSemaphoreGive(h->command_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t position = h->resume_selected && !strcmp(path, h->status.path) ? h->status.position_ms : 0;
    h->resume_selected = false;
    strcpy(h->status.path, path);
    h->status.mp3 = h->status.aac = false;
    h->status.detail[0] = 0;
    if (h->status.volume != volume) {
        h->status.volume = (uint8_t)volume;
        h->preferences_dirty = true;
        ++h->preference_revision;
        h->preference_changed = xTaskGetTickCount();
    }
    h->status.last_error = ESP_OK;
    h->status.position_ms = h->status.duration_ms = 0;
    clear_profile_locked(h);
    h->status.sample_rate = h->status.channels = 0;
    h->status.state = AETHER_AUDIO_STARTING;
    h->status.active = true;
    h->autoplay = true;
    h->standby_music = false;
    h->advance_requested = false;
    h->status.track_index = track_index;
    h->stop_requested = h->pause_requested = false;
    h->seek_ms = position;
    h->seek_revision = 0;
    h->status.seeking = false;
    h->status.seek_requested_ms = 0;
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    xSemaphoreGive(h->command_lock);
    return ESP_OK;
}

esp_err_t aether_audio_player_stop(aether_audio_player_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    h->autoplay = false;
    h->standby_music = false;
    h->advance_requested = false;
    if (h->status.active) h->stop_requested = true;
    else h->status.state = AETHER_AUDIO_STOPPED;
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    return ESP_OK;
}

esp_err_t aether_audio_player_release_session(aether_audio_player_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    ++h->release_serial;
    h->status.closing = true;
    h->autoplay = false; h->advance_requested = false;
    h->standby_music = false;
    if (h->status.active) h->stop_requested = true;
    else h->status.state = AETHER_AUDIO_STOPPED;
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    return ESP_OK;
}

esp_err_t aether_audio_player_set_paused(aether_audio_player_handle_t h, bool paused)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    if (!h->status.active || h->stop_requested) {
        portEXIT_CRITICAL(&h->state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    h->pause_requested = paused;
    if (paused && h->screen_suspended) h->standby_music = false;
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    return ESP_OK;
}

esp_err_t aether_audio_player_seek(aether_audio_player_handle_t h, uint32_t position_ms)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    if (!h->status.active || h->stop_requested) {
        portEXIT_CRITICAL(&h->state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    h->seek_ms = position_ms > h->status.duration_ms ? h->status.duration_ms : position_ms;
    ++h->seek_revision;
    h->status.seek_requested_ms = h->seek_ms;
    h->status.seeking = true;
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    return ESP_OK;
}

esp_err_t aether_audio_player_set_volume(aether_audio_player_handle_t h, int volume)
{
    if (!h || volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    if (h->status.volume != volume) {
        h->status.volume = (uint8_t)volume;
        h->preferences_dirty = true;
        ++h->preference_revision;
        h->preference_changed = xTaskGetTickCount();
    }
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    return ESP_OK;
}

esp_err_t aether_audio_player_set_repeat(aether_audio_player_handle_t h, aether_audio_repeat_t repeat)
{
    if (!h || repeat > AETHER_AUDIO_SHUFFLE || repeat < AETHER_AUDIO_REPEAT_LIST) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    if (h->status.repeat != repeat) {
        h->status.repeat = repeat;
        h->preferences_dirty = true;
        ++h->preference_revision;
        h->preference_changed = xTaskGetTickCount();
    }
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    return ESP_OK;
}

esp_err_t aether_audio_player_set_eq(aether_audio_player_handle_t h, const aether_audio_eq_t *eq)
{
    if (!h || !eq || !valid_eq(eq)) return ESP_ERR_INVALID_ARG;
    const uint32_t packed = pack_eq(eq);
    portENTER_CRITICAL(&h->state_lock);
    const bool changed = packed != pack_eq(&h->status.eq);
    if (changed) {
        h->status.eq = *eq;
        h->preferences_dirty = true;
        ++h->preference_revision;
        h->preference_changed = xTaskGetTickCount();
    }
    portEXIT_CRITICAL(&h->state_lock);
    if (changed && h->task) xTaskNotifyGive(h->task);
    return ESP_OK;
}

esp_err_t aether_audio_player_set_playlist(aether_audio_player_handle_t h,
                                           const char *const *paths, size_t count)
{
    if (!h || (count && !paths) || count > 4096) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->command_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->state_lock);
    const uint32_t release_serial = h->release_serial;
    const bool closing = h->status.closing;
    portEXIT_CRITICAL(&h->state_lock);
    if (closing) { xSemaphoreGive(h->command_lock); return ESP_ERR_INVALID_STATE; }
    char (*copy)[192] = count ? heap_caps_malloc(count * sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (count && !copy) { xSemaphoreGive(h->command_lock); return ESP_ERR_NO_MEM; }
    for (size_t i = 0; i < count; ++i) {
        if (!paths[i] || !paths[i][0] || strlen(paths[i]) >= 192) {
            free(copy);
            xSemaphoreGive(h->command_lock);
            return ESP_ERR_INVALID_ARG;
        }
        strcpy(copy[i], paths[i]);
    }
    char current_path[192];
    portENTER_CRITICAL(&h->state_lock);
    memcpy(current_path, h->status.path, sizeof(current_path));
    portEXIT_CRITICAL(&h->state_lock);
    uint16_t track_index = 0;
    bool selected_found = false;
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(current_path, copy[i])) {
            track_index = (uint16_t)i;
            selected_found = true;
            break;
        }
    }
    portENTER_CRITICAL(&h->state_lock);
    /* A queued selection refers to the existing list. Replacing it here would
     * either cancel the command or reinterpret its index as a different song. */
    if (h->terminate || h->status.closing || release_serial != h->release_serial ||
        h->advance_requested || strcmp(current_path, h->status.path)) {
        portEXIT_CRITICAL(&h->state_lock);
        free(copy);
        xSemaphoreGive(h->command_lock);
        return ESP_ERR_INVALID_STATE;
    }
    char (*old)[192] = h->playlist;
    h->playlist = copy;
    h->status.track_count = (uint16_t)count;
    h->status.track_index = track_index;
    if (h->resume_selected && !selected_found) {
        h->resume_selected = false;
        h->status.path[0] = 0;
        h->status.position_ms = h->status.duration_ms = 0;
    }
    portEXIT_CRITICAL(&h->state_lock);
    free(old);
    xSemaphoreGive(h->command_lock);
    return ESP_OK;
}

static void select_locked(aether_audio_player_handle_t h, uint16_t index)
{
    h->requested_track = index;
    h->autoplay = true;
    h->resume_selected = false;
    if (h->status.active) {
        h->advance_requested = true;
        h->stop_requested = true;
    } else {
        h->standby_music = false;
        h->status.track_index = index;
        memcpy(h->status.path, h->playlist[index], sizeof(h->status.path));
        h->status.position_ms = h->status.duration_ms = 0;
        h->status.last_error = ESP_OK;
        h->status.state = AETHER_AUDIO_STARTING;
        h->status.active = true;
        h->stop_requested = h->pause_requested = false;
        h->seek_ms = h->seek_revision = 0;
    }
}

esp_err_t aether_audio_player_select(aether_audio_player_handle_t h, size_t index)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->command_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->state_lock);
    if (h->terminate || h->status.closing || !h->playlist || index >= h->status.track_count) {
        portEXIT_CRITICAL(&h->state_lock);
        xSemaphoreGive(h->command_lock);
        return ESP_ERR_INVALID_ARG;
    }
    select_locked(h, (uint16_t)index);
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    xSemaphoreGive(h->command_lock);
    return ESP_OK;
}

esp_err_t aether_audio_player_next(aether_audio_player_handle_t h, int direction)
{
    if (!h || (direction != -1 && direction != 1)) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->command_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->state_lock);
    if (h->terminate || h->status.closing) {
        portEXIT_CRITICAL(&h->state_lock);
        xSemaphoreGive(h->command_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!h->playlist || !h->status.track_count) {
        portEXIT_CRITICAL(&h->state_lock);
        xSemaphoreGive(h->command_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const int current = h->advance_requested ? h->requested_track : h->status.track_index;
    select_locked(h, (uint16_t)((current + direction + h->status.track_count) % h->status.track_count));
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    xSemaphoreGive(h->command_lock);
    return ESP_OK;
}

esp_err_t aether_audio_player_get_resume_path(aether_audio_player_handle_t h, char *path, size_t size)
{
    if (!h || !path || !size) return ESP_ERR_INVALID_ARG;
    path[0] = '\0';
    portENTER_CRITICAL(&h->state_lock);
    const size_t length = strlen(h->resume.path);
    if (length && length < size) memcpy(path, h->resume.path, length + 1);
    portEXIT_CRITICAL(&h->state_lock);
    if (!length) return ESP_ERR_NOT_FOUND;
    return length < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t aether_audio_player_restore(aether_audio_player_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->command_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->state_lock);
    const bool idle = !h->status.active && !h->status.closing && !h->status.path[0] && !h->terminate;
    const music_resume_t resume = h->resume;
    const size_t count = h->status.track_count;
    portEXIT_CRITICAL(&h->state_lock);

    esp_err_t error = idle ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (idle && resume.path[0]) {
        for (size_t index = 0; index < count; ++index) {
            if (strcasecmp(resume.path, h->playlist[index])) continue;
            portENTER_CRITICAL(&h->state_lock);
            if (h->status.closing || h->terminate) error = ESP_ERR_INVALID_STATE;
            else {
                memcpy(h->status.path, h->playlist[index], sizeof(h->status.path));
                h->status.track_index = (uint16_t)index;
                h->status.position_ms = resume.position_ms;
                h->status.duration_ms = resume.duration_ms;
                h->resume_selected = true;
                error = ESP_OK;
            }
            portEXIT_CRITICAL(&h->state_lock);
            break;
        }
    }
    xSemaphoreGive(h->command_lock);
    return error;
}

esp_err_t aether_audio_player_get_status(aether_audio_player_handle_t h, aether_audio_player_status_t *status)
{
    if (!h || !status) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    *status = h->status;
    status->preferences_pending = h->preferences_dirty || h->resume_dirty;
    portEXIT_CRITICAL(&h->state_lock);
    return ESP_OK;
}

/* This API is deliberately separate from the supplying worker: stdout may
 * block on a USB host. The owner schedules it only after all media is idle.
 * Never retain a player lock while formatting or writing console output. */
void aether_audio_player_log_profile(aether_audio_player_handle_t h)
{
    aether_audio_player_status_t s;
    if (aether_audio_player_get_status(h, &s) != ESP_OK || !s.pull_max_us ||
        s.state == AETHER_AUDIO_STARTING ||
        (s.active && s.state != AETHER_AUDIO_PAUSED)) return;
    ESP_LOGI(TAG, "profile path=%s codec=%s io_split=%u pull_max_us=%lu read_max_us=%lu decode_no_io_max_us=%lu write_max_us=%lu supply_gap_max_us=%lu refill_max_bytes=%lu refill_max_us=%lu priority=%u",
        s.path, s.aac ? "AAC-LC" : s.mp3 ? "MP3" : "PCM16",
        (unsigned)s.io_profile_valid, (unsigned long)s.pull_max_us,
        (unsigned long)s.read_max_us, (unsigned long)s.decode_max_us,
        (unsigned long)s.write_max_us, (unsigned long)s.supply_gap_max_us,
        (unsigned long)s.refill_max_bytes, (unsigned long)s.refill_max_us,
        (unsigned)MUSIC_TASK_PRIORITY);
}

esp_err_t aether_audio_player_stop_wait(aether_audio_player_handle_t h, uint32_t timeout_ms)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    const TickType_t timeout = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    const TickType_t started = xTaskGetTickCount();
    if (xSemaphoreTake(h->command_lock, timeout) != pdTRUE) return ESP_ERR_TIMEOUT;
    aether_audio_player_stop(h);
    esp_err_t err = ESP_OK;
    while (true) {
        aether_audio_player_status_t status;
        aether_audio_player_get_status(h, &status);
        if (!status.active) break;
        if (timeout != portMAX_DELAY && xTaskGetTickCount() - started >= timeout) { err = ESP_ERR_TIMEOUT; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreGive(h->command_lock);
    return err;
}

esp_err_t aether_audio_player_del(aether_audio_player_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(aether_audio_player_stop_wait(h, 2000), TAG, "stop player");
    portENTER_CRITICAL(&h->state_lock);
    h->terminate = true;
    portEXIT_CRITICAL(&h->state_lock);
    xTaskNotifyGive(h->task);
    const TickType_t started = xTaskGetTickCount();
    while (true) {
        portENTER_CRITICAL(&h->state_lock);
        const bool exited = h->exited;
        portEXIT_CRITICAL(&h->state_lock);
        if (exited) break;
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(2000)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vSemaphoreDelete(h->command_lock);
    free(h->playlist);
    const esp_err_t settings_error = h->status.settings_error;
    free(h);
    return settings_error;
}
