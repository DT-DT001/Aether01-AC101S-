#include "app_tools.h"
#include "app_tools_math.h"
#include "aether_wifi.h"

#include <math.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "nvs.h"

#define TOOLS_MAGIC 0x41545433U
#define TOOLS_V2_MAGIC 0x41545432U
#define RTC_UTC_OFFSET_SECONDS (8 * 60 * 60)
#define UNIX_SECONDS_IN_2000 INT64_C(946684800)
#define NTP_DNS_TIMEOUT_MS 7000U
#define NTP_SERVER_TIMEOUT_MS 12000U

static const char *const s_ntp_servers[] = {"ntp.aliyun.com", "pool.ntp.org"};

typedef struct {
    uint32_t magic;
    uint32_t best_score;
    float reference_pa;
} persisted_tools_t;

/* Read-only compatibility with settings_v2; retired sensor data is ignored. */
typedef struct {
    uint32_t magic;
    uint32_t best_score;
    uint32_t retired_axis_data[6];
    float reference_pa;
    uint32_t retired_flag;
} persisted_tools_v2_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static aether_board_t *s_board;
static aether_audio_player_handle_t s_audio;
static bool (*s_persistence_busy)(void *);
static void *s_persistence_context;
static bool s_started;
static bool s_clock_quiesced;
static bool s_clock_cancel_network;
static uint32_t s_revision;
static pcf85063_time_t s_clock_request;
static app_tools_snapshot_t s_snapshot = {
    .reference_pressure_pa = 101325,
    .preferences_error = ESP_ERR_NOT_FINISHED,
    .clock_error = ESP_ERR_NOT_FINISHED,
    .power_error = ESP_ERR_NOT_FINISHED,
    .battery_error = ESP_ERR_NOT_FINISHED,
};

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool preferences_busy(void)
{
    portENTER_CRITICAL(&s_lock);
    aether_audio_player_handle_t audio = s_audio;
    bool (*busy)(void *) = s_persistence_busy;
    void *context = s_persistence_context;
    portEXIT_CRITICAL(&s_lock);
    if (busy && busy(context)) return true;
    aether_audio_player_status_t status = {0};
    return audio && (aether_audio_player_get_status(audio, &status) != ESP_OK ||
        status.state == AETHER_AUDIO_STARTING ||
        (status.active && status.state != AETHER_AUDIO_PAUSED));
}

static esp_err_t load_preferences(persisted_tools_t *settings)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tools", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t length = sizeof(*settings);
    err = nvs_get_blob(nvs, "settings_v3", settings, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        persisted_tools_v2_t legacy;
        length = sizeof(legacy);
        err = nvs_get_blob(nvs, "settings_v2", &legacy, &length);
        if (err == ESP_OK) {
            if (length != sizeof(legacy) || legacy.magic != TOOLS_V2_MAGIC) {
                err = ESP_ERR_INVALID_RESPONSE;
            } else {
                *settings = (persisted_tools_t){.magic = TOOLS_MAGIC,
                    .best_score = legacy.best_score, .reference_pa = legacy.reference_pa};
                length = sizeof(*settings);
            }
        }
    }
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    if (length != sizeof(*settings) || settings->magic != TOOLS_MAGIC ||
        !isfinite(settings->reference_pa) || settings->reference_pa < 80000 ||
        settings->reference_pa > 120000 || settings->best_score > 1000000) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

static esp_err_t save_preferences(const app_tools_snapshot_t *snapshot)
{
    persisted_tools_t settings = {
        .magic = TOOLS_MAGIC, .best_score = snapshot->best_score,
        .reference_pa = snapshot->reference_pressure_pa,
    };
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tools", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "settings_v3", &settings, sizeof(settings));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static uint64_t clock_seconds(const pcf85063_time_t *time)
{
    return app_calendar_seconds(time->year, time->month, time->day,
                                time->hour, time->minute, time->second);
}

static bool rtc_from_utc(time_t utc, pcf85063_time_t *rtc)
{
    /* RTC date fields are valid only for local dates from 2000 to 2099. */
    if (utc < UNIX_SECONDS_IN_2000 - RTC_UTC_OFFSET_SECONDS ||
        utc >= INT64_C(4102444800) - RTC_UTC_OFFSET_SECONDS) return false;
    const time_t local = utc + RTC_UTC_OFFSET_SECONDS;
    struct tm fields;
    if (!gmtime_r(&local, &fields)) return false;
    *rtc = (pcf85063_time_t){
        .year = fields.tm_year + 1900, .month = fields.tm_mon + 1,
        .day = fields.tm_mday, .weekday = fields.tm_wday,
        .hour = fields.tm_hour, .minute = fields.tm_min, .second = fields.tm_sec,
    };
    return true;
}

static void restore_system_clock(const pcf85063_time_t *rtc)
{
    const uint64_t seconds = clock_seconds(rtc);
    if (rtc->oscillator_stopped || seconds == UINT64_MAX) return;
    const struct timeval utc = {
        .tv_sec = (time_t)(UNIX_SECONDS_IN_2000 + (int64_t)seconds - RTC_UTC_OFFSET_SECONDS),
    };
    if (settimeofday(&utc, NULL) != 0) ESP_LOGW("clock", "Failed to set system time from RTC");
}

static esp_err_t write_rtc_checked(const pcf85063_time_t *request, pcf85063_time_t *actual)
{
    if (!s_board->rtc_ready || !s_board->rtc) return ESP_ERR_NOT_FOUND;
    esp_err_t error = pcf85063_set_time(s_board->rtc, request);
    if (error != ESP_OK) return error;
    error = pcf85063_get_time(s_board->rtc, actual);
    if (error != ESP_OK) return error;
    const uint64_t expected = clock_seconds(request);
    const uint64_t received = clock_seconds(actual);
    return actual->oscillator_stopped || received == UINT64_MAX ||
           received < expected || received - expected > 2 ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}

static void commit_clock(const pcf85063_time_t *request, bool network)
{
    pcf85063_time_t actual;
    const esp_err_t error = write_rtc_checked(request, &actual);
    if (error == ESP_OK && !network) restore_system_clock(&actual);
    portENTER_CRITICAL(&s_lock);
    if (error == ESP_OK) s_snapshot.clock_verified = actual;
    s_snapshot.clock_error = error;
    s_snapshot.clock_pending = false;
    ++s_snapshot.clock_revision;
    if (network) {
        s_snapshot.network_clock_error = error;
        s_snapshot.network_clock_pending = false;
        ++s_snapshot.network_clock_revision;
    }
    portEXIT_CRITICAL(&s_lock);
}

bool app_tools_clock_network_ready(void)
{
    aether_wifi_handle_t wifi = aether_wifi_get_default();
    aether_wifi_status_t status;
    return wifi && aether_wifi_get_status(wifi, &status) == ESP_OK &&
           status.connected && status.ip[0] != '\0';
}

typedef struct {
    enum { NTP_IDLE, NTP_RESOLVING, NTP_WAITING } phase;
    unsigned server;
    uint32_t started_ms;
    char address[IP4ADDR_STRLEN_MAX];
} network_clock_t;

static struct {
    uintptr_t generation;
    esp_err_t error;
    ip_addr_t address;
} s_clock_dns;

typedef struct {
    const char *name;
    uintptr_t generation;
} clock_dns_request_t;

static void clock_dns_found(const char *hostname, const ip_addr_t *address, void *context)
{
    (void)hostname;
    portENTER_CRITICAL(&s_lock);
    if ((uintptr_t)context == s_clock_dns.generation) {
        s_clock_dns.error = address ? ESP_OK : ESP_ERR_NOT_FOUND;
        if (address) s_clock_dns.address = *address;
    }
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t clock_resolve_hostname(void *context)
{
    const clock_dns_request_t *request = context;
    ip_addr_t address;
    const err_t error = dns_gethostbyname_addrtype(request->name, &address,
        clock_dns_found, (void *)request->generation, LWIP_DNS_ADDRTYPE_IPV4);
    if (error == ERR_OK) clock_dns_found(request->name, &address, (void *)request->generation);
    if (error == ERR_OK || error == ERR_INPROGRESS) return ESP_OK;
    return error == ERR_MEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_FOUND;
}

static void invalidate_clock_dns(void)
{
    portENTER_CRITICAL(&s_lock);
    if (++s_clock_dns.generation == 0) ++s_clock_dns.generation;
    s_clock_dns.error = ESP_ERR_NOT_FINISHED;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t start_sntp(void *address)
{
    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG((const char *)address);
    return esp_netif_sntp_init(&config);
}

static esp_err_t stop_sntp(void *context)
{
    (void)context;
    esp_netif_sntp_deinit();
    return ESP_OK;
}

static void stop_network_clock(network_clock_t *sync)
{
    if (sync->phase == NTP_RESOLVING) invalidate_clock_dns();
    /* Disconnect/deinit must be ordered with the SNTP callback on the same
     * TCP/IP task. Numeric addresses also keep SNTP from owning a DNS callback
     * that could outlive the attempt. */
    if (sync->phase == NTP_WAITING) (void)esp_netif_tcpip_exec(stop_sntp, NULL);
    sync->phase = NTP_IDLE;
}

static void fail_network_clock(network_clock_t *sync, esp_err_t error)
{
    stop_network_clock(sync);
    sync->server = 0;
    portENTER_CRITICAL(&s_lock);
    s_snapshot.network_clock_pending = false;
    s_snapshot.network_clock_error = error;
    ++s_snapshot.network_clock_revision;
    portEXIT_CRITICAL(&s_lock);
}

static void retry_network_clock(network_clock_t *sync, esp_err_t error)
{
    stop_network_clock(sync);
    if (++sync->server >= sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0])) {
        fail_network_clock(sync, error);
    }
}

static void service_network_clock(network_clock_t *sync)
{
    portENTER_CRITICAL(&s_lock);
    const bool pending = s_snapshot.network_clock_pending;
    const bool cancelled = s_clock_cancel_network;
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return;
    if (cancelled || !app_tools_clock_network_ready()) {
        fail_network_clock(sync, ESP_ERR_INVALID_STATE);
        return;
    }

    if (sync->phase == NTP_IDLE) {
        invalidate_clock_dns();
        clock_dns_request_t request = {.name = s_ntp_servers[sync->server]};
        portENTER_CRITICAL(&s_lock);
        request.generation = s_clock_dns.generation;
        portEXIT_CRITICAL(&s_lock);
        sync->phase = NTP_RESOLVING;
        sync->started_ms = uptime_ms();
        const esp_err_t error = esp_netif_tcpip_exec(clock_resolve_hostname, &request);
        if (error != ESP_OK) {
            retry_network_clock(sync, error);
            return;
        }
    }
    if (sync->phase == NTP_RESOLVING) {
        portENTER_CRITICAL(&s_lock);
        const esp_err_t resolved = s_clock_dns.error;
        const ip_addr_t address = s_clock_dns.address;
        portEXIT_CRITICAL(&s_lock);
        if (resolved == ESP_ERR_NOT_FINISHED) {
            if ((uint32_t)(uptime_ms() - sync->started_ms) >= NTP_DNS_TIMEOUT_MS) {
                retry_network_clock(sync, ESP_ERR_TIMEOUT);
            }
            return;
        }
        if (resolved != ESP_OK || !ipaddr_ntoa_r(&address, sync->address, sizeof(sync->address))) {
            retry_network_clock(sync, resolved == ESP_OK ? ESP_ERR_INVALID_RESPONSE : resolved);
            return;
        }
        invalidate_clock_dns();
        const esp_err_t error = esp_netif_tcpip_exec(start_sntp, sync->address);
        if (error != ESP_OK) {
            fail_network_clock(sync, error);
            return;
        }
        sync->phase = NTP_WAITING;
        sync->started_ms = uptime_ms();
    }

    const esp_err_t error = esp_netif_sntp_sync_wait(0);
    if (error == ESP_ERR_TIMEOUT) {
        if ((uint32_t)(uptime_ms() - sync->started_ms) >= NTP_SERVER_TIMEOUT_MS) {
            retry_network_clock(sync, error);
        }
        return;
    }
    if (error != ESP_OK) {
        fail_network_clock(sync, error);
        return;
    }

    stop_network_clock(sync);
    sync->server = 0;
    pcf85063_time_t now;
    if (!rtc_from_utc(time(NULL), &now)) {
        fail_network_clock(sync, ESP_ERR_INVALID_RESPONSE);
        return;
    }
    /* Shutdown closes this gate before inspecting RTC write state. The
     * transaction either starts in time for shutdown to await it or is cancelled. */
    portENTER_CRITICAL(&s_lock);
    const bool commit = !s_clock_cancel_network;
    if (commit) {
        s_snapshot.network_clock_received = true;
        s_snapshot.clock_pending = true;
        s_snapshot.clock_error = ESP_ERR_NOT_FINISHED;
    }
    portEXIT_CRITICAL(&s_lock);
    if (commit) commit_clock(&now, true);
    else fail_network_clock(sync, ESP_ERR_INVALID_STATE);
}

/* Console output can block on a slow USB host. Only report cached music
 * measurements after supply has stopped and no other media owner is active. */
static void music_profile_service(bool *pending)
{
    portENTER_CRITICAL(&s_lock);
    aether_audio_player_handle_t audio = s_audio;
    portEXIT_CRITICAL(&s_lock);
    if (!audio) { *pending = false; return; }
    aether_audio_player_status_t status;
    if (aether_audio_player_get_status(audio, &status) != ESP_OK) return;
    if (status.state == AETHER_AUDIO_STARTING ||
        (status.active && status.state != AETHER_AUDIO_PAUSED)) {
        *pending = true;
        return;
    }
    if (!*pending || preferences_busy()) return;
    *pending = false;
    if (status.pull_max_us) aether_audio_player_log_profile(audio);
}

static void tools_task(void *argument)
{
    (void)argument;
    persisted_tools_t settings;
    const esp_err_t loaded = load_preferences(&settings);
    if (s_board->rtc_ready && s_board->rtc) {
        pcf85063_time_t time;
        if (pcf85063_get_time(s_board->rtc, &time) == ESP_OK) restore_system_clock(&time);
    }
    portENTER_CRITICAL(&s_lock);
    if (loaded == ESP_OK) {
        s_snapshot.best_score = settings.best_score;
        s_snapshot.reference_pressure_pa = settings.reference_pa;
    }
    s_snapshot.preferences_error = loaded == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : loaded;
    s_snapshot.ready = true;
    portEXIT_CRITICAL(&s_lock);

    uint32_t power_due = 0, battery_due = 0, save_due = 0;
    bool music_profile_pending = false;
    network_clock_t network_clock = {0};
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        app_tools_snapshot_t snapshot;
        pcf85063_time_t request;
        uint32_t revision;
        portENTER_CRITICAL(&s_lock);
        snapshot = s_snapshot;
        request = s_clock_request;
        revision = s_revision;
        portEXIT_CRITICAL(&s_lock);
        if (snapshot.clock_pending) commit_clock(&request, false);
        service_network_clock(&network_clock);
        const uint32_t now = uptime_ms();
        if ((int32_t)(now - battery_due) >= 0) {
            tg29_battery_state_t battery = {0};
            const esp_err_t err = s_board->pmic ?
                tg29_get_battery_state(s_board->pmic, &battery) : ESP_ERR_NOT_FOUND;
            const uint32_t sampled = uptime_ms();
            if (err == ESP_OK && (snapshot.battery_error != ESP_OK ||
                snapshot.battery.presence_valid != battery.presence_valid ||
                snapshot.battery.battery_present != battery.battery_present ||
                snapshot.battery.vbus_present != battery.vbus_present ||
                snapshot.battery.charging != battery.charging ||
                snapshot.battery.charge_done != battery.charge_done)) {
                ESP_LOGI("battery", "presence_valid=%u present=%u VBUS_good=%u charging=%u done=%u SOC=%u valid=%u VBAT=%u mV valid=%u phase=%u direction=%u",
                    battery.presence_valid, battery.battery_present, battery.vbus_present,
                    battery.charging, battery.charge_done, battery.percentage, battery.percentage_valid,
                    battery.voltage_mv, battery.voltage_valid, battery.charge_state, battery.current_direction);
            } else if (err != ESP_OK && snapshot.battery_error != err) {
                ESP_LOGW("battery", "battery snapshot unavailable: %s", esp_err_to_name(err));
            }
            portENTER_CRITICAL(&s_lock);
            s_snapshot.battery = battery;
            s_snapshot.battery_error = err;
            if (err == ESP_OK) s_snapshot.battery_updated_ms = sampled;
            portEXIT_CRITICAL(&s_lock);
            battery_due = now + 2000;
        }
        if ((int32_t)(now - power_due) >= 0) {
            tg29_power_state_t power = {0};
            const esp_err_t err = s_board->pmic ? tg29_get_power_state(s_board->pmic, &power) : ESP_ERR_NOT_FOUND;
            portENTER_CRITICAL(&s_lock);
            s_snapshot.power_error = err;
            s_snapshot.power = power;
            s_snapshot.power_updated_ms = uptime_ms();
            portEXIT_CRITICAL(&s_lock);
            power_due = now + 5000;
        }
        const bool deferred = snapshot.preferences_pending && preferences_busy();
        portENTER_CRITICAL(&s_lock);
        s_snapshot.preferences_deferred = deferred;
        portEXIT_CRITICAL(&s_lock);
        if (snapshot.preferences_pending && !deferred && (int32_t)(now - save_due) >= 0) {
            const esp_err_t err = save_preferences(&snapshot);
            portENTER_CRITICAL(&s_lock);
            s_snapshot.preferences_error = err;
            if (err == ESP_OK && s_revision == revision) s_snapshot.preferences_pending = false;
            portEXIT_CRITICAL(&s_lock);
            save_due = now + (err == ESP_OK ? 1000 : 3000);
        }
        music_profile_service(&music_profile_pending);
    }
}

void app_tools_set_audio_player(aether_audio_player_handle_t audio)
{
    portENTER_CRITICAL(&s_lock);
    s_audio = audio;
    portEXIT_CRITICAL(&s_lock);
    if (s_task) xTaskNotifyGive(s_task);
}

void app_tools_set_persistence_guard(bool (*busy)(void *), void *context)
{
    portENTER_CRITICAL(&s_lock);
    s_persistence_busy = busy;
    s_persistence_context = context;
    portEXIT_CRITICAL(&s_lock);
    if (s_task) xTaskNotifyGive(s_task);
}

esp_err_t app_tools_init(aether_board_t *board)
{
    if (!board) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_started) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_started = true;
    s_board = board;
    portEXIT_CRITICAL(&s_lock);
    /* FAT timestamps and other localtime consumers use the same RTC zone. */
    if (setenv("TZ", "CST-8", 1) == 0) tzset();
    if (xTaskCreate(tools_task, "app_tools", 4096, NULL, 2, &s_task) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_started = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_tools_get_snapshot(app_tools_snapshot_t *snapshot)
{
    if (!snapshot) return;
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
    if (snapshot->battery_error == ESP_OK &&
        (uint32_t)(uptime_ms() - snapshot->battery_updated_ms) > 7000U) {
        snapshot->battery_error = ESP_ERR_TIMEOUT;
    }
}

esp_err_t app_tools_set_clock(const pcf85063_time_t *time)
{
    if (!time || !app_calendar_valid(time->year, time->month, time->day,
                                     time->hour, time->minute, time->second)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (!s_snapshot.ready || s_clock_quiesced || s_snapshot.clock_pending || s_snapshot.network_clock_pending) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_clock_request = *time;
    s_clock_request.weekday = app_calendar_weekday(time->year, time->month, time->day);
    s_clock_request.oscillator_stopped = false;
    s_snapshot.clock_pending = true;
    s_snapshot.clock_error = ESP_ERR_NOT_FINISHED;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t app_tools_sync_clock(void)
{
    if (!app_tools_clock_network_ready()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (!s_snapshot.ready || s_clock_quiesced || s_snapshot.clock_pending || s_snapshot.network_clock_pending) {
        error = ESP_ERR_INVALID_STATE;
    } else if (!s_board->rtc_ready || !s_board->rtc) {
        error = ESP_ERR_NOT_FOUND;
    } else {
        s_clock_cancel_network = false;
        s_snapshot.network_clock_pending = true;
        s_snapshot.network_clock_received = false;
        s_snapshot.network_clock_error = ESP_ERR_NOT_FINISHED;
    }
    portEXIT_CRITICAL(&s_lock);
    if (error == ESP_OK) xTaskNotifyGive(s_task);
    return error;
}

void app_tools_set_clock_quiesced(bool quiesced)
{
    portENTER_CRITICAL(&s_lock);
    s_clock_quiesced = quiesced;
    if (quiesced) s_clock_cancel_network = true;
    portEXIT_CRITICAL(&s_lock);
    if (quiesced && s_task) xTaskNotifyGive(s_task);
}

static void mark_dirty(void)
{
    s_snapshot.preferences_pending = true;
    s_snapshot.preferences_error = ESP_ERR_NOT_FINISHED;
    ++s_revision;
}

esp_err_t app_tools_set_reference_pressure(float pressure_pa)
{
    if (!isfinite(pressure_pa) || pressure_pa < 80000 || pressure_pa > 120000) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (!s_snapshot.ready) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot.reference_pressure_pa = pressure_pa;
    mark_dirty();
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void app_tools_record_score(uint32_t score)
{
    portENTER_CRITICAL(&s_lock);
    if (s_snapshot.ready && score > s_snapshot.best_score && score <= 1000000) {
        s_snapshot.best_score = score;
        mark_dirty();
    }
    portEXIT_CRITICAL(&s_lock);
    if (s_task) xTaskNotifyGive(s_task);
}
