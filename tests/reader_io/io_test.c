#define _DEFAULT_SOURCE
static char gallery_usb_root[] = "/tmp/aether-gallery-usb-XXXXXX";
#define AETHER_USB_STORAGE_MOUNT_POINT gallery_usb_root
#include "../../main/app_io.c"
#include <assert.h>

static bool intercom_closing;
bool app_intercom_closing(void) { return intercom_closing; }

struct fake_queue { size_t capacity, size, count, head; unsigned char *data; };
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size)
{
    QueueHandle_t q = calloc(1, sizeof(*q)); assert(q);
    q->capacity = count; q->size = size; q->data = calloc(count, size); assert(q->data); return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *data, TickType_t wait)
{
    (void)wait; if (q->count == q->capacity) return pdFALSE;
    memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, data, q->size); ++q->count; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *data, TickType_t wait)
{
    (void)wait; if (!q->count) return pdFALSE;
    memcpy(data, q->data + q->head * q->size, q->size); q->head = (q->head + 1) % q->capacity; --q->count; return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *data)
{
    assert(q->capacity == 1); memcpy(q->data, data, q->size); q->head = 0; q->count = 1; return pdTRUE;
}
void vQueueDelete(QueueHandle_t q) { if (q) { free(q->data); free(q); } }
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack, void *ctx,
                        UBaseType_t priority, void *handle)
{ (void)task; (void)name; (void)stack; (void)ctx; (void)priority; (void)handle; return pdPASS; }
static TickType_t ticks;
static app_io_handle_t pump_io;
static bool pumping;
static void (*delay_hook)(void);
static app_io_handle_t acquire_requires_drained;
void vTaskDelay(TickType_t amount) {
    ticks += amount;
    if(delay_hook)delay_hook();
    if (pump_io && !pumping) {
        pumping = true;
        perform_control_request(pump_io);
        pumping = false;
    }
}
struct fake_semaphore { bool held; };
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1,sizeof(struct fake_semaphore)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) {(void)t;if(s->held)return pdFALSE;s->held=true;return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {assert(s->held);s->held=false;return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t s) {assert(!s->held);free(s);}
TickType_t xTaskGetTickCount(void) { return ticks; }
size_t strlcpy(char *to, const char *from, size_t capacity)
{
    size_t length = strlen(from);
    if (capacity) { size_t n = length < capacity - 1 ? length : capacity - 1; memcpy(to, from, n); to[n] = 0; }
    return length;
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test"; }
static unsigned storage_acquires, root_selections;
static bool switch_root_after_selection;
const char *aether_media_fs_default_root(aether_storage_handle_t h){(void)h;++root_selections;return switch_root_after_selection&&root_selections>1?"/usb":"/sdcard";}

static bool gallery_usb_ready, gallery_advance_generation_on_release;
static uint32_t gallery_usb_generation;
static char file_tf_root[] = "/tmp/aether-files-tf-XXXXXX";
static char file_usb_root[] = "/tmp/aether-files-usb-XXXXXX";
static uint32_t file_tf_generation = 3, file_usb_generation = 7;

static const char *test_path_suffix(const char *path, const char *root)
{
    const size_t length = strlen(root);
    return path && !strncmp(path, root, length) && (!path[length] || path[length] == '/') ? path + length : NULL;
}

bool aether_media_fs_safe_path(const char *path)
{
    if (tf_media_safe_path(path)) return true;
    const char *suffix = test_path_suffix(path, file_tf_root);
    if (!suffix) suffix = test_path_suffix(path, file_usb_root);
    if (!suffix) return false;
    char sandbox_path[192];
    const int length = snprintf(sandbox_path, sizeof(sandbox_path), "/sdcard%s", suffix);
    return length > 0 && (size_t)length < sizeof(sandbox_path) && tf_media_safe_path(sandbox_path);
}

esp_err_t aether_media_fs_acquire(aether_storage_handle_t storage, const char *path, aether_media_fs_lease_t *lease)
{
    const esp_err_t error = tf_media_acquire(storage, path, lease);
    if (error == ESP_OK && test_path_suffix(path, file_usb_root)) lease->tf = NULL;
    return error;
}

bool aether_media_fs_path_ready(aether_storage_handle_t storage, const char *path)
{ return storage != NULL && !strcmp(path, gallery_usb_root) && gallery_usb_ready; }
uint32_t aether_media_fs_generation(aether_storage_handle_t storage, const char *path)
{
    if (test_path_suffix(path, file_tf_root)) return file_tf_generation;
    if (test_path_suffix(path, file_usb_root)) return file_usb_generation;
    return storage != NULL && !strcmp(path, gallery_usb_root) ? gallery_usb_generation : 0;
}
bool aether_storage_card_inserted(aether_storage_handle_t storage) { (void)storage; return true; }
bool aether_storage_is_mounted(aether_storage_handle_t storage) { return storage != NULL; }
esp_err_t aether_storage_acquire(aether_storage_handle_t storage, uint32_t timeout)
{ (void)storage; (void)timeout; if(acquire_requires_drained)assert(atomic_load(&acquire_requires_drained->media_release_requested)==atomic_load(&acquire_requires_drained->media_release_completed)); ++storage_acquires; return ESP_OK; }
void aether_storage_release(aether_storage_handle_t storage)
{
    (void)storage;
    if (gallery_advance_generation_on_release) ++gallery_usb_generation;
}
static aether_audio_player_status_t audio_status;
static aether_video_status_t video_status;
static aether_nes_status_t nes_status;
static esp_err_t audio_snapshot_error, video_snapshot_error, nes_snapshot_error;
static bool recorder_present, recorder_active;
static unsigned recorder_stops, playback_stops;
static esp_err_t recorder_snapshot_error, recorder_stop_error;
static esp_err_t recorder_saved_error, playback_stop_error;
static bool cancel_usb_on_audio_stop;
static aether_usb_msc_status_t usb_status;
static esp_err_t usb_enter_error, usb_exit_error;
static unsigned usb_enters, usb_exits;
static unsigned media_release_posts, media_frame_releases, prepared_playlists;
aether_storage_handle_t usb_storage;
esp_err_t aether_usb_msc_init(aether_storage_handle_t storage) {usb_storage=storage;return ESP_OK;}
esp_err_t aether_usb_msc_get_status(aether_usb_msc_status_t *s) {*s=usb_status;return ESP_OK;}
bool aether_usb_msc_is_active(void) {return usb_status.active;}
esp_err_t aether_usb_msc_enter(uint32_t timeout) {
    assert(timeout==10000);++usb_enters;
    if(usb_enter_error!=ESP_OK)return usb_enter_error;
    assert(!recorder_active&&!audio_status.active&&video_status.state==AETHER_VIDEO_STOPPED&&nes_status.state==AETHER_NES_STOPPED);
    usb_status.active=usb_status.host_connected=usb_status.media_present=true;
    usb_status.state=AETHER_USB_MSC_ACTIVE;return ESP_OK;
}
esp_err_t aether_usb_msc_exit(uint32_t timeout) {
    assert(timeout==10000);++usb_exits;
    if(usb_exit_error!=ESP_OK)return usb_exit_error;
    usb_status=(aether_usb_msc_status_t){0};return ESP_OK;
}
static unsigned usb_confirmed_exits;
esp_err_t aether_usb_msc_exit_confirmed(uint32_t timeout) {
    assert(timeout==10000);++usb_confirmed_exits;
    usb_status=(aether_usb_msc_status_t){0};return ESP_OK;
}

aether_recorder_handle_t aether_recorder_get_default(void) { return recorder_present ? (void *)5 : NULL; }
esp_err_t aether_recorder_get_status(aether_recorder_handle_t h,aether_recorder_status_t *s)
{ assert(h==(void *)5); *s=(aether_recorder_status_t){.active=recorder_active,.last_error=recorder_saved_error}; return recorder_snapshot_error; }
esp_err_t aether_recorder_stop_wait(aether_recorder_handle_t h,uint32_t timeout)
{ assert(h==(void *)5&&(timeout==5000||timeout==0)); ++recorder_stops; if(recorder_stop_error==ESP_OK)recorder_active=false; return recorder_stop_error; }
esp_err_t aether_audio_player_get_status(aether_audio_player_handle_t p, aether_audio_player_status_t *status)
{ (void)p; *status = audio_status; return audio_snapshot_error; }
esp_err_t aether_video_player_get_status(aether_video_player_handle_t p, aether_video_status_t *status)
{ (void)p; *status = video_status; return video_snapshot_error; }
esp_err_t aether_nes_get_status(aether_nes_handle_t p, aether_nes_status_t *status)
{ (void)p; *status = nes_status; return nes_snapshot_error; }
esp_err_t aether_audio_player_release_session(aether_audio_player_handle_t h){assert(h);++media_release_posts;audio_status.closing=true;return ESP_OK;}
esp_err_t aether_audio_player_set_playlist(aether_audio_player_handle_t h,const char *const *paths,size_t count){assert(h);(void)paths;++prepared_playlists;audio_status.track_count=(uint16_t)count;return ESP_OK;}
esp_err_t aether_recorder_stop(aether_recorder_handle_t h){assert(h);return ESP_OK;}
esp_err_t aether_nes_stop(aether_nes_handle_t h){assert(h);return ESP_OK;}
esp_err_t aether_video_player_stop(aether_video_player_handle_t h){assert(h);return ESP_OK;}
void aether_nes_release_frame(aether_nes_handle_t h){assert(h);++media_frame_releases;}
void aether_video_player_release_frame(aether_video_player_handle_t h){assert(h);++media_frame_releases;}
esp_err_t aether_audio_player_stop_wait(aether_audio_player_handle_t p, uint32_t t)
{ (void)p; (void)t; ++playback_stops;if(cancel_usb_on_audio_stop){cancel_usb_on_audio_stop=false;assert(app_io_usb_set_enabled(pump_io,false)==ESP_OK);} if(playback_stop_error==ESP_OK){audio_status.active=false;audio_status.closing=false;audio_status.state=AETHER_AUDIO_STOPPED;}return playback_stop_error; }
esp_err_t aether_video_player_stop_wait(aether_video_player_handle_t p, uint32_t t)
{ (void)p; (void)t; ++playback_stops;if(playback_stop_error==ESP_OK)video_status.state=AETHER_VIDEO_STOPPED;return playback_stop_error; }
esp_err_t aether_nes_stop_wait(aether_nes_handle_t p, uint32_t t)
{ (void)p; (void)t; ++playback_stops;if(playback_stop_error==ESP_OK)nes_status.state=AETHER_NES_STOPPED;return playback_stop_error; }

static struct { char key[16]; unsigned char value[512]; size_t size; } records[128];
static size_t record_count, writes;
static esp_err_t init_error, write_error, commit_error;
static size_t commit_calls;
esp_err_t nvs_flash_init_partition(const char *name) { assert(!strcmp(name, "reader_nvs")); return init_error; }
esp_err_t nvs_open_from_partition(const char *part, const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(part, "reader_nvs") && !strcmp(name, "reader"));
    *handle = 1; return mode == NVS_READONLY && !record_count ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle == 1);
    for (size_t i = 0; i < record_count; ++i) if (!strcmp(records[i].key, key)) {
        if (*size < records[i].size) { *size = records[i].size; return ESP_ERR_NVS_INVALID_LENGTH; }
        *size = records[i].size; memcpy(value, records[i].value, *size); return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    assert(handle == 1 && size <= sizeof(records[0].value));
    if (write_error) return write_error;
    size_t i; for (i = 0; i < record_count && strcmp(records[i].key, key); ++i) {}
    if (i == record_count) { assert(record_count < 128); ++record_count; strlcpy(records[i].key, key, sizeof(records[i].key)); }
    memcpy(records[i].value, value, size); records[i].size = size; ++writes; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); ++commit_calls; return commit_error; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

static void save(app_io_handle_t io, const char *path, uint64_t offset)
{
    app_io_request_t q = {.operation = APP_IO_READER_SAVE, .offset = offset, .font_px = 16, .dark = false};
    strlcpy(q.path, path, sizeof(q.path)); assert(app_io_submit_request(io, &q) == ESP_OK);
}
static app_io_result_t load(app_io_handle_t io, const char *path)
{
    app_io_request_t q = {.operation = APP_IO_READER_LOAD, .generation = 1};
    strlcpy(q.path, path, sizeof(q.path)); assert(app_io_submit_request(io, &q) == ESP_OK);
    io_request_t item; assert(xQueueReceive(io->requests, &item, 0));
    app_io_result_t result = {.generation = item.request.generation, .request_id = item.id};
    result.error = perform_request(io, &item.request, &result); return result;
}
static void destroy_io(app_io_handle_t io)
{
    vQueueDelete(io->requests); vQueueDelete(io->results); vQueueDelete(io->saves); vQueueDelete(io->latest_save);
    vQueueDelete(io->usb_requests);vQueueDelete(io->quiesce_requests);vSemaphoreDelete(io->usb_control);
    free(io->save_batch); free(io);
}

static void finish_release_on_delay(void)
{ playback_stop_error=ESP_OK;delay_hook=NULL; }
static void test_usb_and_shutdown(app_io_handle_t io, const char *path)
{
    pump_io=io;
    io->audio=(void*)2;io->video=(void*)3;io->nes=(void*)4;
    recorder_present=recorder_active=true;
    audio_status.active=true;audio_status.state=AETHER_AUDIO_PLAYING;
    video_status.state=AETHER_VIDEO_PLAYING;nes_status.state=AETHER_NES_RUNNING;
    const unsigned acquired=storage_acquires;
    app_io_request_t files={.operation=APP_IO_FILES,.generation=70};
    assert(aether_usb_bus_claim(AETHER_USB_BUS_HOST)==ESP_OK);
    assert(app_io_usb_set_enabled(io,true)==ESP_ERR_INVALID_STATE && !app_io_usb_storage_blocked(io));
    assert(aether_usb_bus_release(AETHER_USB_BUS_HOST)==ESP_OK);
    assert(app_io_usb_set_enabled(io,true)==ESP_OK&&app_io_usb_storage_blocked(io));
    assert(app_io_submit_request(io,&files)==ESP_ERR_INVALID_STATE&&storage_acquires==acquired);
    assert(app_io_quiesce_begin(io)==ESP_ERR_INVALID_STATE);
    app_io_cancel(io,71);
    save(io,path,14000); /* Flash-only bookmarks remain owned while USB starts. */
    assert(perform_control_request(io));
    app_io_usb_status_t status;
    assert(app_io_usb_get_status(io,&status)==ESP_OK&&status.active&&status.blocked&&!status.pending);
    assert(!app_io_bookmark_pending(io)&&load(io,path).offset==14000&&storage_acquires==acquired);
    usb_status.usb_speed_mbps=480;usb_status.usb_clock_khz=32000;usb_status.usb_high_speed_fallback=true;
    assert(app_io_usb_get_status(io,&status)==ESP_OK&&status.usb_speed_mbps==480&&
        status.usb_clock_khz==32000&&status.usb_high_speed_fallback);
    usb_exit_error=ESP_ERR_INVALID_STATE;
    assert(app_io_usb_set_enabled(io,false)==ESP_OK&&perform_control_request(io));
    assert(app_io_usb_get_status(io,&status)==ESP_OK&&status.active&&status.blocked&&!status.pending&&status.last_error==ESP_ERR_INVALID_STATE);
    usb_exit_error=ESP_OK;usb_status.host_ejected=true;
    assert(app_io_usb_set_enabled(io,false)==ESP_OK&&perform_control_request(io));
    assert(!app_io_usb_storage_blocked(io));
    assert(app_io_submit_request(io,&files)==ESP_OK);
    io_request_t discarded;assert(xQueueReceive(io->requests,&discarded,0));
    puts("PASS USB gate precedes media stops, blocks TF leases, survives navigation, saves bookmarks, and retains ownership on unsafe exit");

    assert(app_io_usb_set_enabled(io,true)==ESP_OK&&perform_control_request(io));
    assert(app_io_usb_get_status(io,&status)==ESP_OK);
    const uint32_t before_exit=status.request_id;
    const unsigned confirmations=usb_confirmed_exits;
    assert(app_io_usb_exit_confirmed(io,before_exit-1)==ESP_ERR_INVALID_STATE);
    usb_exit_error=ESP_ERR_INVALID_STATE;
    assert(app_io_usb_set_enabled(io,false)==ESP_OK);
    assert(app_io_usb_exit_confirmed(io,before_exit)==ESP_ERR_INVALID_STATE); /* pending */
    assert(perform_control_request(io)&&app_io_usb_get_status(io,&status)==ESP_OK);
    assert(status.active&&status.blocked&&usb_confirmed_exits==confirmations);
    assert(app_io_usb_exit_confirmed(io,before_exit)==ESP_ERR_INVALID_STATE); /* stale */
    assert(app_io_usb_exit_confirmed(io,status.request_id)==ESP_OK);
    assert(app_io_usb_storage_blocked(io)&&perform_control_request(io));
    assert(usb_confirmed_exits==confirmations+1&&!app_io_usb_storage_blocked(io));
    assert(app_io_usb_exit_confirmed(io,status.request_id)==ESP_ERR_INVALID_STATE); /* consumed */
    usb_exit_error=ESP_OK;
    puts("PASS host-eject confirmation uses its own worker path and rejects stale/pending/already-consumed dialog tokens");

    const unsigned started=usb_enters;
    cancel_usb_on_audio_stop=true;
    assert(app_io_usb_set_enabled(io,true)==ESP_OK&&perform_control_request(io));
    assert(usb_enters==started&&app_io_usb_get_status(io,&status)==ESP_OK&&status.pending);
    assert(perform_control_request(io)&&!app_io_usb_storage_blocked(io));
    recorder_active=true;recorder_saved_error=ESP_FAIL;
    assert(app_io_usb_set_enabled(io,true)==ESP_OK&&perform_control_request(io));
    assert(usb_enters==started&&app_io_usb_get_status(io,&status)==ESP_OK&&status.last_error==ESP_FAIL&&!status.blocked);
    recorder_saved_error=ESP_OK;
    usb_enter_error=ESP_ERR_NO_MEM;usb_status.active=true;
    assert(app_io_usb_set_enabled(io,true)==ESP_OK&&perform_control_request(io));
    assert(app_io_usb_storage_blocked(io));
    usb_enter_error=ESP_OK;
    assert(app_io_usb_set_enabled(io,false)==ESP_OK&&perform_control_request(io)&&!app_io_usb_storage_blocked(io));
    puts("PASS superseded USB entry does not enter, recording save failure aborts export, and failed core cleanup keeps local TF blocked");

    const TickType_t began=ticks;
    assert(app_io_quiesce_begin(io)==ESP_OK&&ticks==began&&app_io_is_quiescing(io));
    assert(app_io_usb_set_enabled(io,true)==ESP_ERR_INVALID_STATE);
    assert(app_io_submit_request(io,&files)==ESP_ERR_INVALID_STATE);
    save(io,path,15000);app_io_cancel(io,72);
    assert(app_io_quiesce_wait(io,100)==ESP_OK&&!app_io_bookmark_pending(io));
    assert(app_io_quiesce_wait(io,100)==ESP_OK);
    assert(app_io_resume(io)==ESP_OK&&app_io_is_quiescing(io));
    assert(perform_control_request(io)&&!app_io_is_quiescing(io));
    assert(load(io,path).offset==15000);
    puts("PASS shutdown BEGIN is nonblocking and atomic against USB; WAIT closes media and drains last GUI bookmark; resume waits worker acknowledgment");

    save(io,path,15001);commit_error=ESP_FAIL;
    assert(app_io_quiesce_begin(io)==ESP_OK&&app_io_quiesce_wait(io,100)==ESP_FAIL);
    assert(app_io_is_quiescing(io)&&app_io_bookmark_pending(io));
    assert(app_io_resume(io)==ESP_OK&&perform_control_request(io));
    commit_error=ESP_OK;ticks+=2000;drain_bookmarks(io);
    assert(!app_io_bookmark_pending(io)&&load(io,path).offset==15001);
    recorder_active=true;recorder_stop_error=playback_stop_error=ESP_ERR_TIMEOUT;
    const TickType_t wait_started=ticks;
    assert(app_io_quiesce_begin(io)==ESP_OK&&ticks==wait_started);
    assert(app_io_quiesce_wait(io,100)==ESP_ERR_TIMEOUT&&ticks-wait_started<=101);
    assert(app_io_is_quiescing(io));
    assert(app_io_resume(io)==ESP_OK&&perform_control_request(io)&&!app_io_is_quiescing(io));
    recorder_stop_error=playback_stop_error=ESP_OK;recorder_active=false;
    assert(app_io_quiesce_begin(io)==ESP_OK&&app_io_resume(io)==ESP_OK);
    assert(app_io_usb_set_enabled(io,true)==ESP_ERR_INVALID_STATE);
    assert(perform_control_request(io)&&!app_io_is_quiescing(io));
    assert(app_io_usb_set_enabled(io,true)==ESP_OK&&perform_control_request(io));
    assert(app_io_usb_set_enabled(io,false)==ESP_OK&&perform_control_request(io));
    recorder_present=false;pump_io=NULL;
    puts("PASS shutdown save failure retains RAM, one shared stop deadline bounds every media owner, and cancellation cannot stop a later USB session");
}

#include "gallery_catalog_cases.h"

static app_io_result_t run_file_job(app_io_handle_t io, app_io_request_t *request)
{
    request->generation = atomic_load(&io->latest_generation) + 1U;
    assert(app_io_submit_request(io, request) == ESP_OK);
    io_request_t queued;
    assert(xQueueReceive(io->requests, &queued, 0) == pdTRUE);
    perform_queued_request(io, &queued);
    app_io_result_t result;
    assert(app_io_poll(io, &result));
    assert(result.operation == request->operation && !io->media_lease.held);
    return result;
}

static void detach_copy_target(void)
{
    ++file_usb_generation;
    delay_hook = NULL;
}

static void test_file_operations(app_io_handle_t io)
{
    assert(mkdtemp(file_tf_root) && mkdtemp(file_usb_root));
    char nested[256], original[256], local[256], remote[256];
    fixture_path(nested, file_tf_root, "旅行");
    assert(!mkdir(nested, 0700));
    fixture_path(original, nested, "作业.TXT");
    fixture_path(local, file_tf_root, "Copy.txt");
    fixture_path(remote, file_usb_root, "作业.TXT");
    assert(strlen(original) < 192 && strlen(local) < 192 && strlen(remote) < 192);
    static const char text[] = "File manager fixture\n";
    write_photo_fixture(nested, "作业.TXT", text, sizeof(text), 1234);

    app_io_request_t job = {.operation = APP_IO_SEARCH_FILES, .media_generation = file_tf_generation, .limit = 16};
    strlcpy(job.path, file_tf_root, sizeof(job.path));
    strlcpy(job.query, "作业.txt", sizeof(job.query));
    app_io_result_t result = run_file_job(io, &job);
    assert(result.error == ESP_OK && result.count == 1 && !result.truncated);
    assert(!strcmp(result.entries[0].path, original) && result.entries[0].size == sizeof(text));
    assert(result.entries[0].modified_s == 1234);
    app_io_result_free(&result);

    job.operation = APP_IO_COPY_FILE;
    job.destination_generation = file_tf_generation;
    strlcpy(job.path, original, sizeof(job.path));
    strlcpy(job.destination, local, sizeof(job.destination));
    result = run_file_job(io, &job);
    assert(result.error == ESP_OK && !result.target_exists && access(original, F_OK) == 0);
    FILE *copy = fopen(local, "rb");
    char contents[sizeof(text)];
    assert(copy && fread(contents, 1, sizeof(contents), copy) == sizeof(contents) && !fclose(copy));
    assert(memcmp(contents, text, sizeof(contents)) == 0);

    result = run_file_job(io, &job);
    assert(result.error == ESP_ERR_INVALID_STATE && result.target_exists && access(original, F_OK) == 0);
    write_photo_fixture(file_usb_root, "作业.TXT", "existing", 8, 100);
    job.operation = APP_IO_MOVE_FILE;
    job.destination_generation = file_usb_generation;
    strlcpy(job.destination, remote, sizeof(job.destination));
    result = run_file_job(io, &job);
    assert(result.target_exists && access(original, F_OK) == 0 && !unlink(remote));
    result = run_file_job(io, &job);
    assert(result.error == ESP_OK && access(original, F_OK) == -1 && access(remote, F_OK) == 0);

    job.operation = APP_IO_DELETE_FILE;
    strlcpy(job.path, nested, sizeof(job.path));
    result = run_file_job(io, &job);
    assert(result.error == ESP_ERR_INVALID_ARG && access(nested, F_OK) == 0);
    snprintf(job.path, sizeof(job.path), "%s/../retained", file_tf_root);
    assert(app_io_submit_request(io, &job) == ESP_ERR_INVALID_ARG);
    strlcpy(job.path, remote, sizeof(job.path));
    job.media_generation = file_usb_generation - 1;
    result = run_file_job(io, &job);
    assert(result.error == ESP_ERR_INVALID_STATE && access(remote, F_OK) == 0);
    job.media_generation = file_usb_generation;
    usb_status.active = true;
    assert(app_io_submit_request(io, &job) == ESP_ERR_INVALID_STATE);
    usb_status.active = false;
    result = run_file_job(io, &job);
    assert(result.error == ESP_OK && access(remote, F_OK) == -1);
    result = run_file_job(io, &job);
    assert(result.error == ESP_ERR_NOT_FOUND);

    FILE *large = fopen(original, "wb");
    assert(large);
    char block[1024] = {0};
    for (unsigned i = 0; i < 256; ++i) assert(fwrite(block, 1, sizeof(block), large) == sizeof(block));
    assert(!fclose(large));
    job.operation = APP_IO_MOVE_FILE;
    strlcpy(job.path, original, sizeof(job.path));
    job.media_generation = file_tf_generation;
    job.destination_generation = file_usb_generation;
    delay_hook = detach_copy_target;
    result = run_file_job(io, &job);
    assert(!delay_hook && result.error == ESP_ERR_INVALID_STATE && access(original, F_OK) == 0 && access(remote, F_OK) == -1);
    DIR *directory = opendir(file_usb_root);
    assert(directory);
    struct dirent *remaining;
    while ((remaining = readdir(directory)) != NULL) {
        if (!copy_temporary_name(remaining->d_name)) continue;
        char part[256];
        fixture_path(part, file_usb_root, remaining->d_name);
        assert(!unlink(part));
    }
    assert(!closedir(directory));
    assert(!unlink(original) && !unlink(local) && !rmdir(nested) && !rmdir(file_tf_root) && !rmdir(file_usb_root));
    puts("PASS Files worker recursive UTF-8/case-insensitive search, details, copy bytes, cross-volume move, collisions, disconnect, invalid paths and gated deletion");
}

int main(void)
{
    assert(safe_path("/sdcard") && safe_path("/sdcard/Books/小说.txt"));
    assert(!safe_path("/sdcardevil/a") && !safe_path("/sdcard/../secret") &&
           !safe_path("/sdcard//file") && !safe_path("/sdcard/./file") && !safe_path("/sdcard/a\\b"));
    app_io_handle_t io;
    assert(app_io_start((void *)1, NULL, NULL, NULL, &io) == ESP_OK);
    root_selections=0;switch_root_after_selection=true;
    app_io_request_t roots={.operation=APP_IO_FILES};
    app_io_result_t roots_result={.operation=APP_IO_FILES};
    (void)perform_request(io,&roots,&roots_result);
    assert(root_selections==1&&!strcmp(roots_result.path,"/sdcard"));
    app_io_result_free(&roots_result);switch_root_after_selection=false;
    puts("PASS catalog pins the acquired volume even if preferred media changes during the operation");
    app_io_request_t unsafe_save = {.operation = APP_IO_READER_SAVE};
    strlcpy(unsafe_save.path, "/sdcard/../secret.txt", sizeof(unsafe_save.path));
    assert(app_io_submit_request(io, &unsafe_save) == ESP_ERR_INVALID_ARG);
    assert(load(io, unsafe_save.path).error == ESP_ERR_INVALID_ARG);
    const char *path = "/sdcard/Books/one.txt";
    app_io_result_t result = load(io, path);
    assert(result.error == ESP_OK && result.offset == 0 && result.font_px == 24 && result.dark);
    save(io, path, 4242);
    result = load(io, path); /* No explicit drain: production LOAD must do it. */
    assert(result.error == ESP_OK && result.offset == 4242 && result.font_px == 16 && !result.dark);
    typedef struct { uint32_t version,font_px; uint64_t offset; uint8_t dark; char path[192]; } legacy_bookmark_t;
    assert(sizeof(legacy_bookmark_t) == sizeof(reader_bookmark_t));
    reader_bookmark_t *legacy = (reader_bookmark_t *)records[0].value;
    legacy->version = 1; legacy->encoding_hint = UINT32_MAX; /* Old padding is not a saved encoding. */
    legacy->turn_mode = UINT8_MAX;
    result = load(io,path);
    assert(result.offset == 4242 && result.encoding_hint == 0 && result.turn_mode == APP_READER_TURN_SWIPE_HORIZONTAL);
    legacy->version = 2;
    legacy->encoding_hint = 4;
    result = load(io,path);
    assert(result.offset == 4242 && result.encoding_hint == 4 && result.turn_mode == APP_READER_TURN_SWIPE_HORIZONTAL);
    app_io_request_t forced = {.operation=APP_IO_READER_SAVE,.offset=4242,.font_px=16,.encoding_hint=2,
                              .turn_mode=APP_READER_TURN_TAP_VERTICAL};
    strlcpy(forced.path,path,sizeof(forced.path)); assert(app_io_submit_request(io,&forced) == ESP_OK);
    result = load(io,path);
    assert(result.offset == 4242 && result.encoding_hint == 2 && result.turn_mode == APP_READER_TURN_TAP_VERTICAL);
    puts("PASS v1/v2 bookmarks upgrade and v3 page turn preference persists without a binary layout change");
    puts("PASS safe paths and immediate close/reopen SAVE-before-LOAD ordering");
    size_t before = writes;
    for (unsigned i = 0; i < 20; ++i) save(io, path, 5000 + i);
    drain_bookmarks(io); result = load(io, path);
    assert(result.offset == 5019 && writes == before + 1);
    puts("PASS saturated save queue retains latest and coalesces NVS writes");
    for (unsigned i = 0; i < 9; ++i) save(io, path, 6000 + i);
    io_save_t discarded; assert(xQueueReceive(io->saves, &discarded, 0));
    save(io, path, 6010); drain_bookmarks(io);
    result = load(io, path); assert(result.offset == 6010);
    atomic_store(&io->save_id, UINT32_MAX - 3U);
    for (unsigned i = 0; i < 12; ++i) save(io, path, 7000 + i);
    result = load(io, path); assert(result.offset == 7011);
    puts("PASS overflow slot ordering and save sequence wrap");
    save(io, path, 8000); app_io_cancel(io, 22); drain_bookmarks(io);
    result = load(io, path); assert(result.offset == 8000);
    app_io_result_t stale = {.generation = 1, .request_id = atomic_load(&io->latest_id), .text = strdup("old")};
    assert(xQueueSend(io->results, &stale, 0)); app_io_cancel(io, 23);
    assert(!app_io_poll(io, &result));
    puts("PASS navigation cancels stale results without cancelling queued bookmarks");
    write_error = ESP_ERR_NO_MEM; save(io, path, 9000); drain_bookmarks(io);
    assert(app_io_bookmark_status(io) == ESP_ERR_NO_MEM);
    assert(io->save_pending_count == 1 && app_io_bookmark_pending(io));
    write_error = ESP_OK;
    result = load(io, path); assert(result.offset == 9000); /* Retained RAM save, no new navigation required. */
    drain_bookmarks(io); assert(app_io_bookmark_status(io) == ESP_ERR_NO_MEM);
    ticks += 2000; drain_bookmarks(io);
    assert(app_io_bookmark_status(io) == ESP_OK && io->save_pending_count == 0);
    assert(load(io, path).offset == 9000);
    commit_error = ESP_FAIL; save(io, path, 9001); drain_bookmarks(io);
    const size_t failed_at = commit_calls;
    drain_bookmarks(io); assert(commit_calls == failed_at); /* No 50ms flash retry loop. */
    commit_error = ESP_OK; ticks += 2000; drain_bookmarks(io);
    assert(commit_calls == failed_at + 1 && app_io_bookmark_status(io) == ESP_OK);
    puts("PASS failed set/commit retains bookmark, resumes from RAM, and retries after backoff without a new save");
    io->audio = (void *)2; audio_status.active = true; audio_status.state = AETHER_AUDIO_PLAYING;
    before = writes; const size_t commits_before_music = commit_calls;
    save(io, path, 9100); drain_bookmarks(io); save(io, path, 9200); drain_bookmarks(io);
    assert(writes == before && commit_calls == commits_before_music && app_io_bookmark_pending(io));
    result = load(io, path); assert(result.offset == 9200 && commit_calls == commits_before_music);
    audio_status.state = AETHER_AUDIO_PAUSED; drain_bookmarks(io);
    assert(writes == before + 1 && commit_calls == commits_before_music + 1 && !app_io_bookmark_pending(io));
    assert(load(io, path).offset == 9200);
    puts("PASS background music defers/coalesces flash writes while RAM resume stays current; pause commits latest");
    io->video = (void *)3; io->nes = (void *)4;
    video_status.state = AETHER_VIDEO_PLAYING; before = writes;
    save(io, path, 9300); drain_bookmarks(io); assert(writes == before);
    video_status.state = AETHER_VIDEO_PAUSED; nes_status.state = AETHER_NES_RUNNING;
    drain_bookmarks(io); assert(writes == before);
    nes_status.state = AETHER_NES_PAUSED; drain_bookmarks(io);
    assert(writes == before + 1 && load(io, path).offset == 9300);
    puts("PASS pending book saves also avoid writes during video/NES and flush after pause");
    esp_err_t *snapshot_errors[] = {&audio_snapshot_error, &video_snapshot_error, &nes_snapshot_error};
    for (unsigned i = 0; i < 3; ++i) {
        before = writes; *snapshot_errors[i] = ESP_ERR_TIMEOUT;
        save(io, path, 9400 + i); drain_bookmarks(io);
        assert(writes == before && app_io_bookmark_pending(io));
        result = load(io, path); assert(result.offset == 9400 + i && writes == before);
        *snapshot_errors[i] = ESP_OK; drain_bookmarks(io);
        assert(writes == before + 1 && !app_io_bookmark_pending(io));
    }
    audio_status.active = false; audio_status.state = AETHER_AUDIO_STARTING;
    before = writes; save(io, path, 9500); drain_bookmarks(io); assert(writes == before);
    audio_status.state = AETHER_AUDIO_PAUSED; drain_bookmarks(io); assert(writes == before + 1);
    audio_status.active = true;
    puts("PASS unavailable media snapshots and queued STARTING defer Flash while preserving latest RAM bookmark");
    audio_status.state = AETHER_AUDIO_PLAYING; before = writes;
    for (unsigned i = 0; i < 40; ++i) {
        char other[192]; snprintf(other, sizeof(other), "/sdcard/Books/book%u.txt", i);
        save(io, other, 10000 + i); drain_bookmarks(io);
    }
    save(io, "/sdcard/Books/book0.txt", 12000);
    result = load(io, "/sdcard/Books/book0.txt");
    assert(result.offset == 12000 && writes == before && io->save_pending_count == 40);
    audio_status.state = AETHER_AUDIO_PAUSED; drain_bookmarks(io);
    assert(writes == before + 40 && !app_io_bookmark_pending(io));
    puts("PASS PSRAM pending cache grows across 40 books and keeps latest reopen position during background music");
    recorder_present=recorder_active=true;
    before=writes;save(io,path,13000);drain_bookmarks(io);
    assert(writes==before&&app_io_bookmark_pending(io));
    recorder_active=false;recorder_snapshot_error=ESP_ERR_TIMEOUT;drain_bookmarks(io);
    assert(writes==before&&app_io_bookmark_pending(io));
    recorder_snapshot_error=ESP_OK;drain_bookmarks(io);assert(writes==before+1);
    const app_io_operation_t preparations[]={APP_IO_RECORDER,APP_IO_MUSIC,APP_IO_VIDEO,APP_IO_NES};
    for(unsigned i=0;i<sizeof(preparations)/sizeof(*preparations);++i){
        app_io_request_t q={.operation=preparations[i],.generation=23};
        assert(app_io_submit_request(io,&q)==ESP_OK);
        io_request_t incoming;assert(xQueueReceive(io->requests,&incoming,0)==pdTRUE);
        app_io_result_t r={.generation=incoming.request.generation,.request_id=incoming.id};
        unsigned prior=playback_stops;recorder_stop_error=ESP_ERR_TIMEOUT;
        assert(perform_request(io,&incoming.request,&r)==ESP_ERR_TIMEOUT&&playback_stops==prior);
    }
    recorder_stop_error=ESP_OK;
    app_io_request_t q={.operation=APP_IO_RECORDER,.generation=23};
    assert(app_io_submit_request(io,&q)==ESP_OK);
    io_request_t incoming;assert(xQueueReceive(io->requests,&incoming,0)==pdTRUE);
    app_io_result_t prepared={.generation=incoming.request.generation,.request_id=incoming.id};
    assert(perform_request(io,&incoming.request,&prepared)==ESP_OK&&recorder_stops>=5);
    recorder_present=false;
    puts("PASS recording/unknown state defers bookmarks and every audio preparation waits for recorder file ownership release");
    io->audio=(void*)2;io->video=(void*)3;io->nes=(void*)4;
    audio_status.active=true;audio_status.state=AETHER_AUDIO_PLAYING;
    playback_stop_error=ESP_ERR_TIMEOUT;
    TickType_t release_at=ticks;assert(app_io_release_media(io)==ESP_OK&&ticks==release_at);
    bool release_pending=false;esp_err_t release_error=ESP_OK;
    assert(app_io_media_release_status(io,&release_pending,&release_error)==ESP_OK&&release_pending);
    intercom_closing = true;
    unsigned stops_before_intercom_release = playback_stops;
    assert(service_media_release(io) && playback_stops == stops_before_intercom_release);
    intercom_closing = false;
    assert(service_media_release(io));
    assert(app_io_media_release_status(io,&release_pending,&release_error)==ESP_OK&&release_pending&&release_error==ESP_ERR_TIMEOUT);
    unsigned before_frames=media_frame_releases;
    assert(service_media_release(io)&&media_frame_releases==before_frames);
    playback_stop_error=ESP_OK;ticks+=100;
    assert(!service_media_release(io));
    assert(app_io_media_release_status(io,&release_pending,&release_error)==ESP_OK&&!release_pending&&release_error==ESP_OK);
    assert(media_frame_releases==before_frames+2&&media_release_posts==1);
    app_io_result_t playlist_result={.generation=atomic_load(&io->latest_generation),.request_id=atomic_load(&io->latest_id)};
    app_io_request_t playlist_request={.operation=APP_IO_CATALOG,.filter=APP_IO_MUSIC};
    assert(prepare_music_playlist(io,&playlist_request,&playlist_result)==ESP_OK&&playlist_result.playlist_prepared&&prepared_playlists==1);
    app_io_cancel(io,playlist_result.generation+1);playlist_result.playlist_prepared=false;
    assert(prepare_music_playlist(io,&playlist_request,&playlist_result)==ESP_ERR_INVALID_STATE&&prepared_playlists==1);
    playback_stop_error=ESP_ERR_TIMEOUT;assert(app_io_release_media(io)==ESP_OK);
    io_request_t queued_close_window={.request={.operation=APP_IO_FILES,.generation=atomic_load(&io->latest_generation)},.id=atomic_load(&io->latest_id)};
    acquire_requires_drained=io;delay_hook=finish_release_on_delay;
    const unsigned acquire_before_release=storage_acquires;
    perform_queued_request(io,&queued_close_window);
    assert(!delay_hook&&storage_acquires==acquire_before_release+1);
    acquire_requires_drained=NULL;
    app_io_result_t drained_result;assert(app_io_poll(io,&drained_result));app_io_result_free(&drained_result);
    puts("PASS a close published while IO queue sleeps is drained again after dequeue; new-page filesystem work cannot bypass cleanup");
    puts("PASS page-exit cleanup retains owner through failure/backoff, releases frames only after drain, and prepares music playlists outside GUI with stale-generation rejection");
    test_gallery_catalog(io);
    test_file_operations(io);
    test_usb_and_shutdown(io,path);
    destroy_io(io);
    init_error = ESP_FAIL; assert(app_io_start((void *)1, NULL, NULL, NULL, &io) == ESP_OK);
    assert(app_io_bookmark_status(io) == ESP_FAIL && load(io, path).error == ESP_FAIL);
    app_io_request_t bad = {.operation = APP_IO_READER_SAVE}; strlcpy(bad.path, path, sizeof(bad.path));
    assert(app_io_submit_request(io, &bad) == ESP_FAIL); destroy_io(io);
    puts("PASS persistence failures are reported without aborting UI or erasing partitions");
    return 0;
}
