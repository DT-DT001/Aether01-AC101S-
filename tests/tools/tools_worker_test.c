#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../../main/app_tools.c"

static jmp_buf done;
static unsigned step, commits, writes;
static uint64_t now_us;
static bool media_busy = true, fail_commit;
static persisted_tools_t saved;
static bool has_v3, has_v2;
static persisted_tools_t stored_v3;
static persisted_tools_v2_t stored_v2;
static size_t v3_size = sizeof(stored_v3), v2_size = sizeof(stored_v2);
static unsigned legacy_reads;
static const unsigned guard_context = 42;
static unsigned battery_reads;
static esp_err_t battery_read_error;
static tg29_battery_state_t battery_sample = {.presence_valid=true,.battery_present=true,
    .percentage_valid=true,.percentage=80,.voltage_valid=true,.voltage_mv=3900};

int64_t esp_timer_get_time(void) { return (int64_t)now_us; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, int priority, TaskHandle_t *out)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; *out = (void *)1; return pdPASS; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task == (void *)1); }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ assert(!strcmp(name, "tools")); *handle = 1; return mode == NVS_READONLY && !has_v3 && !has_v2 ? ESP_ERR_NVS_NOT_FOUND : ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *size)
{
    assert(h == 1);
    const void *source;
    size_t length;
    if (!strcmp(key, "settings_v3")) {
        if (!has_v3) return ESP_ERR_NVS_NOT_FOUND;
        source = &stored_v3; length = v3_size;
    } else {
        assert(!strcmp(key, "settings_v2"));
        ++legacy_reads;
        if (!has_v2) return ESP_ERR_NVS_NOT_FOUND;
        source = &stored_v2; length = v2_size;
    }
    assert(*size >= length);
    memcpy(v, source, length);
    *size = length;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t size)
{ assert(h == 1 && !strcmp(key, "settings_v3") && size == sizeof(saved)); memcpy(&saved, v, size); ++writes; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { assert(h == 1); ++commits; return fail_commit ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { assert(h == 1); }
esp_err_t pcf85063_set_time(void *h, const pcf85063_time_t *t) { (void)h; (void)t; assert(0); return ESP_FAIL; }
esp_err_t pcf85063_get_time(void *h, pcf85063_time_t *t) { (void)h; (void)t; assert(0); return ESP_FAIL; }
esp_err_t tg29_get_power_state(void *h, tg29_power_state_t *p)
{ assert(h==(void *)3); *p=(tg29_power_state_t){.dcdc2_mv=3300}; return ESP_OK; }
esp_err_t tg29_get_battery_state(void *h,tg29_battery_state_t *p)
{ assert(h==(void *)3); ++battery_reads; if(battery_read_error!=ESP_OK)return battery_read_error; *p=battery_sample;return ESP_OK; }
static aether_audio_player_status_t audio_status;
static esp_err_t snapshot_error;
esp_err_t aether_audio_player_get_status(aether_audio_player_handle_t h, aether_audio_player_status_t *status)
{ assert(h == (void *)2); *status = audio_status; return snapshot_error; }
static unsigned music_profiles;
void aether_audio_player_log_profile(aether_audio_player_handle_t h)
{ assert(h==(void *)2 && !preferences_busy()); ++music_profiles; }
static bool busy(void *context) { assert(context == &guard_context); return media_busy; }

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    assert(clear == pdTRUE && timeout == 250);
    now_us += 1000000;
    switch (step++) {
    case 0:
        assert(s_snapshot.best_score == 123 && s_snapshot.reference_pressure_pa == 99000);
        assert(!writes && !commits);
        app_tools_record_score(321); break;
    case 1:
        assert(battery_reads==1&&s_snapshot.battery.percentage==80&&s_snapshot.battery_updated_ms==1000);
        battery_sample.percentage=70;
        assert(!commits && !writes && s_snapshot.preferences_pending && s_snapshot.preferences_deferred);
        app_tools_record_score(654); media_busy = false; break;
    case 2:
        assert(commits == 1 && saved.magic == TOOLS_MAGIC && saved.best_score == 654 &&
               saved.reference_pa == 99000 && !s_snapshot.preferences_pending);
        assert(app_tools_set_reference_pressure(100000) == ESP_OK); fail_commit = true; break;
    case 3:
        assert(battery_reads==2&&s_snapshot.battery.percentage==70&&s_snapshot.battery_updated_ms==3000);
        battery_read_error=ESP_FAIL;
        assert(commits == 2 && s_snapshot.preferences_error == ESP_FAIL && s_snapshot.preferences_pending);
        media_busy = true; fail_commit = false; break;
    case 4:
        assert(commits == 2 && s_snapshot.preferences_deferred);
        media_busy = false; now_us += 3000000; break;
    case 5:
        assert(s_snapshot.battery_error==ESP_FAIL&&!s_snapshot.battery.percentage_valid&&s_snapshot.battery_updated_ms==3000);
        assert(commits == 3 && saved.reference_pa == 100000 && !s_snapshot.preferences_pending &&
               !s_snapshot.preferences_deferred && s_snapshot.preferences_error == ESP_OK);
        longjmp(done, 1);
    default: assert(0);
    }
    return 1;
}

int main(void)
{
    persisted_tools_t loaded;
    assert(load_preferences(&loaded) == ESP_ERR_NVS_NOT_FOUND);
    has_v2 = true;
    stored_v2 = (persisted_tools_v2_t){.magic=TOOLS_V2_MAGIC, .best_score=123, .reference_pa=99000};
    /* Retired payload must not invalidate otherwise healthy pressure/score data. */
    memset(stored_v2.retired_axis_data, 0xff, sizeof(stored_v2.retired_axis_data));
    stored_v2.retired_flag = UINT32_MAX;
    assert(sizeof(stored_v2)==40 && load_preferences(&loaded)==ESP_OK);
    assert(loaded.magic==TOOLS_MAGIC && loaded.best_score==123 && loaded.reference_pa==99000);
    assert(!writes && !commits);
    v2_size--;
    assert(load_preferences(&loaded)==ESP_ERR_INVALID_RESPONSE);
    v2_size=sizeof(stored_v2); stored_v2.magic=0;
    assert(load_preferences(&loaded)==ESP_ERR_INVALID_RESPONSE);
    stored_v2.magic=TOOLS_V2_MAGIC; stored_v2.reference_pa=NAN;
    assert(load_preferences(&loaded)==ESP_ERR_INVALID_RESPONSE);
    stored_v2.reference_pa=99000;
    has_v3=true;
    stored_v3=(persisted_tools_t){.magic=TOOLS_MAGIC,.best_score=456,.reference_pa=102000};
    unsigned before=legacy_reads;
    assert(load_preferences(&loaded)==ESP_OK && loaded.best_score==456 && loaded.reference_pa==102000);
    assert(legacy_reads==before);
    v3_size--;
    assert(load_preferences(&loaded)==ESP_ERR_INVALID_RESPONSE && legacy_reads==before);
    v3_size=sizeof(stored_v3); stored_v3.best_score=1000001;
    assert(load_preferences(&loaded)==ESP_ERR_INVALID_RESPONSE);
    stored_v3.best_score=456; stored_v3.magic=0;
    assert(load_preferences(&loaded)==ESP_ERR_INVALID_RESPONSE);
    has_v3=false;
    aether_board_t board = {.pmic=(void *)3};
    assert(app_tools_init(&board) == ESP_OK);
    app_tools_set_persistence_guard(busy, (void *)&guard_context);
    if (!setjmp(done)) tools_task(NULL);
    app_tools_set_persistence_guard(NULL, NULL);
    app_tools_set_audio_player((void *)2);
    audio_status.active = true; audio_status.state = AETHER_AUDIO_PLAYING; assert(preferences_busy());
    audio_status.state = AETHER_AUDIO_PAUSED; assert(!preferences_busy());
    snapshot_error = ESP_ERR_TIMEOUT; assert(preferences_busy());
    snapshot_error = ESP_OK; audio_status.active = false;
    audio_status.state = AETHER_AUDIO_STARTING; assert(preferences_busy());
    audio_status.state = AETHER_AUDIO_STOPPED; assert(!preferences_busy());
    app_tools_snapshot_t snapshot;
    s_snapshot.battery_error=ESP_OK;s_snapshot.battery_updated_ms=(uint32_t)(now_us/1000)-7001;
    app_tools_get_snapshot(&snapshot);assert(snapshot.battery_error==ESP_ERR_TIMEOUT&&s_snapshot.battery_error==ESP_OK);
    now_us=(UINT64_C(1)<<32)*1000+3000000;s_snapshot.battery_updated_ms=UINT32_MAX-2000;
    app_tools_get_snapshot(&snapshot);assert(snapshot.battery_error==ESP_OK);
    now_us+=3000000;app_tools_get_snapshot(&snapshot);assert(snapshot.battery_error==ESP_ERR_TIMEOUT);
    bool profile_pending=false;
    audio_status=(aether_audio_player_status_t){.active=true,.state=AETHER_AUDIO_PLAYING,.pull_max_us=1200};
    music_profile_service(&profile_pending);assert(profile_pending&&!music_profiles);
    audio_status.state=AETHER_AUDIO_PAUSED;
    media_busy=true;app_tools_set_persistence_guard(busy,(void *)&guard_context);
    music_profile_service(&profile_pending);assert(profile_pending&&!music_profiles);
    media_busy=false;music_profile_service(&profile_pending);
    assert(!profile_pending&&music_profiles==1);
    music_profile_service(&profile_pending);assert(music_profiles==1);
    audio_status.state=AETHER_AUDIO_PLAYING;music_profile_service(&profile_pending);
    snapshot_error=ESP_ERR_TIMEOUT;audio_status.active=false;audio_status.state=AETHER_AUDIO_STOPPED;
    music_profile_service(&profile_pending);assert(profile_pending&&music_profiles==1);
    snapshot_error=ESP_OK;music_profile_service(&profile_pending);
    assert(!profile_pending&&music_profiles==2);
    app_tools_set_persistence_guard(NULL,NULL);
    puts("PASS music profile never logs during supply/other media/unknown state, reports once after pause or stop from existing low-priority task");
    puts("PASS background battery sampling at2s independently of persistence; failed/stale samples invalidate data and uint32 tick wrap is safe");
    puts("PASS real tools worker defers/coalesces settings during media, saves on pause, and retries failed commit");
    puts("PASS settings_v2 pressure/score retained without retired sensor payload; compact settings_v3 preferred and saved; malformed data rejected without fallback");
    return 0;
}
