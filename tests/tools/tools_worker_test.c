#define _DEFAULT_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static time_t fake_time(time_t *result);
static int fake_settimeofday(const struct timeval *value, const void *zone);
#define time(...) fake_time(__VA_ARGS__)
#define settimeofday fake_settimeofday
#include "../../main/app_tools.c"
#undef settimeofday
#undef time

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

static pcf85063_time_t rtc_stored;
static unsigned rtc_writes, sntp_starts, sntp_stops;
static const char *sntp_server;
static bool sntp_live, wifi_online, sntp_has_time, fail_rtc_read, tcpip_context, defer_dns;
static time_t epoch;
static dns_found_callback pending_dns_callback;
static void *pending_dns_argument;

int64_t esp_timer_get_time(void) { return (int64_t)now_us; }
static time_t fake_time(time_t *result) { if (result) *result = epoch; return epoch; }
static int fake_settimeofday(const struct timeval *value, const void *zone)
{ assert(!zone && value->tv_usec == 0); epoch = value->tv_sec; return 0; }
aether_wifi_handle_t aether_wifi_get_default(void) { return (void *)5; }
esp_err_t aether_wifi_get_status(aether_wifi_handle_t handle, aether_wifi_status_t *status)
{
    assert(handle == (void *)5);
    *status = (aether_wifi_status_t){.connected = wifi_online, .ip = "192.0.2.1"};
    return ESP_OK;
}
esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *context)
{
    assert(!tcpip_context);
    tcpip_context = true;
    const esp_err_t error = callback(context);
    tcpip_context = false;
    return error;
}
err_t dns_gethostbyname_addrtype(const char *name, ip_addr_t *ip, dns_found_callback callback,
                                 void *argument, uint8_t address_type)
{
    assert(tcpip_context && address_type == LWIP_DNS_ADDRTYPE_IPV4);
    if (defer_dns) {
        pending_dns_callback = callback;
        pending_dns_argument = argument;
        return ERR_INPROGRESS;
    }
    ip->last_octet = !strcmp(name, "ntp.aliyun.com") ? 1 : 2;
    return ERR_OK;
}
char *ipaddr_ntoa_r(const ip_addr_t *address, char *text, int size)
{
    assert(size >= 12 && (address->last_octet == 1 || address->last_octet == 2));
    return strcpy(text, address->last_octet == 1 ? "192.0.2.1" : "192.0.2.2");
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{ assert(tcpip_context && !sntp_live); sntp_live = true; ++sntp_starts; sntp_server = config->servers[0]; return ESP_OK; }
void esp_netif_sntp_deinit(void) { assert(tcpip_context && sntp_live); sntp_live = false; ++sntp_stops; }
esp_err_t esp_netif_sntp_sync_wait(TickType_t timeout)
{ assert(!tcpip_context && sntp_live && timeout == 0); return sntp_has_time ? ESP_OK : ESP_ERR_TIMEOUT; }
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
esp_err_t pcf85063_set_time(void *h, const pcf85063_time_t *t)
{ assert(h == (void *)4 && s_snapshot.clock_pending); rtc_stored = *t; ++rtc_writes; return ESP_OK; }
esp_err_t pcf85063_get_time(void *h, pcf85063_time_t *t)
{ assert(h == (void *)4); *t = rtc_stored; return fail_rtc_read ? ESP_FAIL : ESP_OK; }
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

static void check_network_clock(aether_board_t *board)
{
    network_clock_t clock = {0};
    board->rtc_ready = true;
    board->rtc = (void *)4;
    assert(app_tools_sync_clock() == ESP_ERR_INVALID_STATE && !sntp_starts);
    wifi_online = true;
    assert(app_tools_sync_clock() == ESP_OK && app_tools_sync_clock() == ESP_ERR_INVALID_STATE);
    const pcf85063_time_t manual = {.year = 2026, .month = 11, .day = 22};
    assert(app_tools_set_clock(&manual) == ESP_ERR_INVALID_STATE);
    service_network_clock(&clock);
    assert(sntp_starts == 1 && !strcmp(sntp_server, "192.0.2.1"));
    assert(s_snapshot.network_clock_pending && !s_snapshot.clock_pending && !rtc_writes);
    now_us += (uint64_t)NTP_SERVER_TIMEOUT_MS * 1000;
    service_network_clock(&clock);
    service_network_clock(&clock);
    assert(sntp_starts == 2 && sntp_stops == 1 && !strcmp(sntp_server, "192.0.2.2"));
    epoch = 1709232123; /* 2024-02-29 18:42:03 UTC */
    sntp_has_time = true;
    service_network_clock(&clock);
    assert(!sntp_live && sntp_stops == 2 && rtc_writes == 1);
    assert(!s_snapshot.network_clock_pending && !s_snapshot.clock_pending &&
           s_snapshot.network_clock_error == ESP_OK && s_snapshot.network_clock_received);
    assert(rtc_stored.year == 2024 && rtc_stored.month == 3 && rtc_stored.day == 1 &&
           rtc_stored.hour == 2 && rtc_stored.minute == 42 && rtc_stored.second == 3 && rtc_stored.weekday == 5);
    assert(s_snapshot.clock_verified.weekday == 5);
    epoch = 0;
    restore_system_clock(&rtc_stored);
    assert(epoch == 1709232123);
    assert(!rtc_from_utc(UNIX_SECONDS_IN_2000 - RTC_UTC_OFFSET_SECONDS - 1, &rtc_stored));
    assert(!rtc_from_utc(INT64_C(4102444800) - RTC_UTC_OFFSET_SECONDS, &rtc_stored));

    assert(app_tools_sync_clock() == ESP_OK);
    sntp_has_time = false;
    service_network_clock(&clock);
    app_tools_set_clock_quiesced(true);
    app_tools_set_clock_quiesced(false);
    service_network_clock(&clock);
    assert(!sntp_live && rtc_writes == 1 && s_snapshot.network_clock_error == ESP_ERR_INVALID_STATE);

    assert(app_tools_sync_clock() == ESP_OK);
    defer_dns = true;
    service_network_clock(&clock);
    dns_found_callback stale_dns = pending_dns_callback;
    void *stale_context = pending_dns_argument;
    now_us += (uint64_t)NTP_DNS_TIMEOUT_MS * 1000;
    service_network_clock(&clock);
    service_network_clock(&clock);
    const ip_addr_t old_ip = {.last_octet = 1};
    stale_dns("first attempt", &old_ip, stale_context);
    service_network_clock(&clock);
    assert(!sntp_live && clock.phase == NTP_RESOLVING && rtc_writes == 1);
    const ip_addr_t new_ip = {.last_octet = 2};
    pending_dns_callback("second attempt", &new_ip, pending_dns_argument);
    sntp_has_time = true;
    fail_rtc_read = true;
    service_network_clock(&clock);
    assert(!sntp_live && rtc_writes == 2 && !s_snapshot.network_clock_pending &&
           s_snapshot.network_clock_received && s_snapshot.network_clock_error == ESP_FAIL);
    assert(!strcmp(sntp_server, "192.0.2.2"));
    puts("PASS manual conflict, server fallback and RTC UTC+8 rollover, shutdown cancellation, stale DNS and RTC readback failure");
}

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
    check_network_clock(&board);
    puts("PASS music profile never logs during supply/other media/unknown state, reports once after pause or stop from existing low-priority task");
    puts("PASS background battery sampling at2s independently of persistence; failed/stale samples invalidate data and uint32 tick wrap is safe");
    puts("PASS real tools worker defers/coalesces settings during media, saves on pause, and retries failed commit");
    puts("PASS settings_v2 pressure/score retained without retired sensor payload; compact settings_v3 preferred and saved; malformed data rejected without fallback");
    return 0;
}
