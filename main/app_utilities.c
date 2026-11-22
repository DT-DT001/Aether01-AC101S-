#include "app_utilities.h"
#include "app_telemetry.h"
#include "app_tools_math.h"
#include "aether_wifi.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

enum {
    WEATHER_BODY = 4096,
    WEATHER_REQUEST_TIMEOUT_MS = 8000,
    WEATHER_BODY_TIMEOUT_MS = 5000,
    WEATHER_BODY_READ_TIMEOUT_MS = 1500,
    ALARM_TONE_INTERVAL_MS = 2000,
    ALARM_TONE_RETRY_MS = 500,
    ALARM_RING_MAX_MS = 60000,
    ALARM_SLOT_BYTES = 4,
    ALARM_FLAG_CONFIGURED = 1,
    ALARM_FLAG_ENABLED = 2,
    NVS_RETRY_MS = 3000,
};
static const char s_namespace[] = "utilities";
static const char *const s_note_keys[APP_NOTE_COUNT] = {"note0", "note1", "note2", "note3"};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_service;
static aether_board_t *s_board;
static app_power_t *s_power;
static bool (*s_storage_busy)(void *);
static void *s_storage_context;
static EXT_RAM_BSS_ATTR char s_notes[APP_NOTE_COUNT][APP_NOTE_CAPACITY];
static app_note_status_t s_note_status[APP_NOTE_COUNT];
static uint32_t s_note_revision[APP_NOTE_COUNT];
static app_alarm_snapshot_t s_alarm = {.hour = 7, .ringing_slot = APP_ALARM_NO_SLOT};
static app_alarm_slot_t s_alarm_slots[APP_ALARM_SLOTS];
static uint64_t s_alarm_last_minute[APP_ALARM_SLOTS];
static uint64_t s_ringing_minute;
static uint32_t s_ring_revision, s_preview_revision;
static uint8_t s_ringing_mask, s_preview_tone = APP_ALARM_TONE_COUNT;
static uint32_t s_alarm_revision, s_city_revision;
static bool s_city_pending, s_weather_running;
static esp_err_t s_city_error;
static int64_t s_weather_due;
static app_weather_snapshot_t s_weather;

static bool storage_busy(void)
{
    return s_storage_busy && s_storage_busy(s_storage_context);
}

static bool power_running(void)
{
    app_power_status_t status;
    return s_power && app_power_get_status(s_power, &status) == ESP_OK &&
        status.shutdown_state == APP_POWER_SHUTDOWN_IDLE;
}

static esp_err_t load_value_error(esp_err_t error)
{
    return error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
}

typedef uint8_t alarm_record_t[APP_ALARM_SLOTS][ALARM_SLOT_BYTES];

static void update_ringing_summary_locked(void)
{
    s_alarm.ringing = s_ringing_mask != 0;
    s_alarm.ringing_count = 0;
    s_alarm.ringing_slot = APP_ALARM_NO_SLOT;
    for (unsigned i = 0; i < APP_ALARM_SLOTS; ++i) {
        if (!(s_ringing_mask & (1U << i))) continue;
        if (!s_alarm.ringing_count) s_alarm.ringing_slot = i;
        ++s_alarm.ringing_count;
    }
    const app_alarm_slot_t *slot = &s_alarm_slots[s_alarm.ringing ? s_alarm.ringing_slot : 0];
    s_alarm.enabled = slot->enabled;
    s_alarm.hour = slot->configured ? slot->hour : 7;
    s_alarm.minute = slot->minute;
    s_alarm.tone = slot->tone;
}

static void encode_alarms(alarm_record_t record)
{
    for (unsigned i = 0; i < APP_ALARM_SLOTS; ++i) {
        const app_alarm_slot_t *slot = &s_alarm_slots[i];
        record[i][0] = (slot->configured ? ALARM_FLAG_CONFIGURED : 0) |
            (slot->enabled ? ALARM_FLAG_ENABLED : 0);
        record[i][1] = slot->hour;
        record[i][2] = slot->minute;
        record[i][3] = slot->tone;
    }
}

static esp_err_t load_alarms(nvs_handle_t handle, app_alarm_slot_t *slots, bool *migrate)
{
    alarm_record_t record;
    size_t length = sizeof(record);
    esp_err_t error = nvs_get_blob(handle, "alarms_v2", record, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        uint16_t legacy;
        error = nvs_get_u16(handle, "alarm_v1", &legacy);
        if (error != ESP_OK) return error;
        const unsigned hour = (legacy >> 8) & 0x7f;
        const unsigned minute = legacy & 0xff;
        if (hour >= 24 || minute >= 60) return ESP_ERR_INVALID_RESPONSE;
        slots[0] = (app_alarm_slot_t){true, (legacy & 0x8000) != 0, hour, minute, 0};
        *migrate = true;
        return ESP_OK;
    }
    if (error != ESP_OK) return error;
    if (length != sizeof(record)) return ESP_ERR_INVALID_SIZE;

    for (unsigned i = 0; i < APP_ALARM_SLOTS; ++i) {
        const uint8_t flags = record[i][0];
        if (!flags) continue;
        if (!(flags & ALARM_FLAG_CONFIGURED) || (flags & ~(ALARM_FLAG_CONFIGURED | ALARM_FLAG_ENABLED)) ||
            record[i][1] >= 24 || record[i][2] >= 60 || record[i][3] >= APP_ALARM_TONE_COUNT) {
            error = ESP_ERR_INVALID_RESPONSE;
            continue;
        }
        slots[i] = (app_alarm_slot_t){true, (flags & ALARM_FLAG_ENABLED) != 0,
                                      record[i][1], record[i][2], record[i][3]};
    }
    return error;
}

static void load_preferences(void)
{
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(s_namespace, NVS_READONLY, &handle);
    app_alarm_slot_t alarms[APP_ALARM_SLOTS] = {0};
    bool migrate_alarms = false;
    uint8_t city = 0;
    const esp_err_t alarm_error = opened == ESP_OK ? load_alarms(handle, alarms, &migrate_alarms) : opened;
    const esp_err_t city_error = opened == ESP_OK ? nvs_get_u8(handle, "city_v1", &city) : opened;
    esp_err_t note_errors[APP_NOTE_COUNT];
    for (unsigned note = 0; note < APP_NOTE_COUNT; ++note) {
        size_t length = sizeof(s_notes[note]);
        note_errors[note] = opened == ESP_OK ?
            nvs_get_str(handle, s_note_keys[note], s_notes[note], &length) : opened;
        if (note_errors[note] != ESP_OK) s_notes[note][0] = '\0';
    }
    if (opened == ESP_OK) nvs_close(handle);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_alarm_slots, alarms, sizeof(alarms));
    update_ringing_summary_locked();
    s_alarm.pending = migrate_alarms;
    s_alarm.save_error = load_value_error(alarm_error);
    s_alarm.ready = true;
    if (!s_city_revision && city < app_weather_city_count()) s_weather.city = city;
    s_city_error = load_value_error(city_error);
    for (unsigned note = 0; note < APP_NOTE_COUNT; ++note) {
        s_note_status[note].ready = true;
        s_note_status[note].save_error = load_value_error(note_errors[note]);
    }
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t save_preferences(void)
{
    alarm_record_t alarms;
    unsigned city;
    bool city_pending, alarm_pending;
    uint32_t alarm_revision, city_revision;
    portENTER_CRITICAL(&s_lock);
    alarm_pending = s_alarm.pending;
    if (alarm_pending) encode_alarms(alarms);
    alarm_revision = s_alarm_revision;
    city = s_weather.city;
    city_pending = s_city_pending;
    city_revision = s_city_revision;
    portEXIT_CRITICAL(&s_lock);
    if (!alarm_pending && !city_pending) return ESP_OK;

    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(s_namespace, NVS_READWRITE, &handle);
    esp_err_t alarm_error = opened, city_error = opened;
    if (opened == ESP_OK) {
        if (alarm_pending) alarm_error = nvs_set_blob(handle, "alarms_v2", alarms, sizeof(alarms));
        if (city_pending)
            city_error = nvs_set_u8(handle, "city_v1", city);
        const esp_err_t committed = nvs_commit(handle);
        if (alarm_error == ESP_OK) alarm_error = committed;
        if (city_error == ESP_OK) city_error = committed;
        nvs_close(handle);
    }
    portENTER_CRITICAL(&s_lock);
    if (alarm_pending && alarm_revision == s_alarm_revision) {
        s_alarm.save_error = alarm_error;
        s_alarm.pending = alarm_error != ESP_OK;
    }
    if (city_pending && city_revision == s_city_revision) {
        s_city_error = city_error;
        s_city_pending = city_error != ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return alarm_error != ESP_OK ? alarm_error : city_error;
}

static esp_err_t save_note(unsigned note)
{
    uint32_t revision;
    portENTER_CRITICAL(&s_lock);
    const bool pending = s_note_status[note].pending;
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return ESP_OK;
    char *copy = malloc(APP_NOTE_CAPACITY);
    if (!copy) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    revision = s_note_revision[note];
    memcpy(copy, s_notes[note], APP_NOTE_CAPACITY);
    portEXIT_CRITICAL(&s_lock);
    nvs_handle_t handle;
    esp_err_t error = nvs_open(s_namespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, s_note_keys[note], copy);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    free(copy);
    portENTER_CRITICAL(&s_lock);
    if (revision == s_note_revision[note]) {
        s_note_status[note].save_error = error;
        s_note_status[note].pending = error != ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

static bool collect_due_alarms(const pcf85063_time_t *rtc)
{
    if (rtc->oscillator_stopped) return false;
    const uint64_t seconds = app_calendar_seconds(rtc->year, rtc->month, rtc->day,
                                                  rtc->hour, rtc->minute, rtc->second);
    if (seconds == UINT64_MAX) return false;
    const uint64_t minute = seconds / 60 + 1; /* Keep zero for never fired. */
    uint8_t due = 0;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < APP_ALARM_SLOTS; ++i) {
        const app_alarm_slot_t *slot = &s_alarm_slots[i];
        if (!slot->configured || !slot->enabled || slot->hour != rtc->hour ||
            slot->minute != rtc->minute || s_alarm_last_minute[i] == minute) continue;
        s_alarm_last_minute[i] = minute;
        due |= 1U << i;
    }
    if (due) {
        /* Simultaneous alarms share one wake and dismissal. An alarm in the
         * next minute starts a new occurrence even if the old one is active. */
        s_ringing_mask = s_ringing_minute == minute ? s_ringing_mask | due : due;
        s_ringing_minute = minute;
        ++s_ring_revision;
        s_preview_tone = APP_ALARM_TONE_COUNT;
        ++s_preview_revision;
        update_ringing_summary_locked();
    }
    portEXIT_CRITICAL(&s_lock);
    return due != 0;
}

typedef struct {
    uint32_t revision;
    uint8_t slot, tone;
    bool preview;
} alarm_audio_request_t;

static bool alarm_audio_current(void *context)
{
    const alarm_audio_request_t *request = context;
    portENTER_CRITICAL(&s_lock);
    const bool current = request->preview ? !s_alarm.ringing && request->revision == s_preview_revision :
        s_alarm.ringing && request->slot == s_alarm.ringing_slot && request->revision == s_ring_revision;
    portEXIT_CRITICAL(&s_lock);
    return current && power_running();
}

static void service(void *unused)
{
    (void)unused;
    load_preferences();
    int64_t ring_started = 0, next_tone = 0, next_save = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(450));
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const bool running = power_running();
        app_telemetry_snapshot_t telemetry;
        app_telemetry_get(&telemetry);
        if (running && telemetry.rtc_error == ESP_OK && collect_due_alarms(&telemetry.rtc)) {
            ring_started = now_ms;
            next_tone = now_ms;
            (void)app_power_wake(s_power);
        }

        portENTER_CRITICAL(&s_lock);
        const bool ringing = s_alarm.ringing;
        alarm_audio_request_t audio = ringing ? (alarm_audio_request_t){
            .revision = s_ring_revision, .slot = s_alarm.ringing_slot, .tone = s_alarm.tone,
        } : (alarm_audio_request_t){
            .revision = s_preview_revision, .tone = s_preview_tone, .preview = true,
        };
        s_preview_tone = APP_ALARM_TONE_COUNT;
        portEXIT_CRITICAL(&s_lock);

        if (ringing && (!running || now_ms - ring_started >= ALARM_RING_MAX_MS)) {
            app_alarm_dismiss();
        } else if (running && s_board->audio_ready && audio.tone < APP_ALARM_TONE_COUNT &&
                   (!ringing || now_ms >= next_tone)) {
            const esp_err_t error = app_alarm_audio_play(s_board->audio, audio.tone,
                                                         alarm_audio_current, &audio);
            if (ringing) next_tone = now_ms + (error == ESP_OK ? ALARM_TONE_INTERVAL_MS : ALARM_TONE_RETRY_MS);
        }
        /* Allow the shutdown coordinator to flush already admitted saves. */
        if (now_ms >= next_save && !storage_busy()) {
            esp_err_t error = save_preferences();
            for (unsigned note = 0; note < APP_NOTE_COUNT && error == ESP_OK; ++note)
                error = save_note(note);
            if (error != ESP_OK) next_save = now_ms + NVS_RETRY_MS;
        }
    }
}

esp_err_t app_utilities_init(aether_board_t *board, app_power_t *power,
                            bool (*busy)(void *), void *context)
{
    if (!board || !power) return ESP_ERR_INVALID_ARG;
    if (s_service) return ESP_ERR_INVALID_STATE;
    s_board = board;
    s_power = power;
    s_storage_busy = busy;
    s_storage_context = context;
    return xTaskCreate(service, "app_utilities", 4608, NULL, 2, &s_service) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}

void app_alarm_get(app_alarm_snapshot_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_alarm;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t app_alarm_get_slot(unsigned slot, app_alarm_slot_t *out)
{
    if (slot >= APP_ALARM_SLOTS || !out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    const bool ready = s_alarm.ready;
    *out = s_alarm_slots[slot];
    portEXIT_CRITICAL(&s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t update_alarm_slot(unsigned slot, app_alarm_slot_t value)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_alarm.ready) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const app_alarm_slot_t old = s_alarm_slots[slot];
    const bool changed = old.configured != value.configured || old.enabled != value.enabled ||
        old.hour != value.hour || old.minute != value.minute || old.tone != value.tone;
    if (changed) {
        s_alarm_slots[slot] = value;
        if (s_ringing_mask & (1U << slot)) {
            s_ringing_mask &= ~(1U << slot);
            if (s_alarm.ringing_slot == slot) ++s_ring_revision;
        }
        update_ringing_summary_locked();
        s_alarm.pending = true;
        s_alarm.save_error = ESP_OK;
        ++s_alarm_revision;
    }
    portEXIT_CRITICAL(&s_lock);
    if (changed) xTaskNotifyGive(s_service);
    return ESP_OK;
}

esp_err_t app_alarm_set_slot(unsigned slot, unsigned hour, unsigned minute, bool enabled, unsigned tone)
{
    if (slot >= APP_ALARM_SLOTS || hour >= 24 || minute >= 60 || tone >= APP_ALARM_TONE_COUNT)
        return ESP_ERR_INVALID_ARG;
    return update_alarm_slot(slot, (app_alarm_slot_t){true, enabled, hour, minute, tone});
}

esp_err_t app_alarm_remove_slot(unsigned slot)
{
    if (slot >= APP_ALARM_SLOTS) return ESP_ERR_INVALID_ARG;
    return update_alarm_slot(slot, (app_alarm_slot_t){0});
}

esp_err_t app_alarm_set(unsigned hour, unsigned minute, bool enabled)
{
    return app_alarm_set_slot(0, hour, minute, enabled, 0);
}

esp_err_t app_alarm_preview(unsigned tone)
{
    if (tone >= APP_ALARM_TONE_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_service || !s_board->audio_ready || !power_running()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    const bool allowed = s_alarm.ready && !s_alarm.ringing;
    if (allowed) {
        s_preview_tone = tone;
        ++s_preview_revision;
    }
    portEXIT_CRITICAL(&s_lock);
    if (allowed) xTaskNotifyGive(s_service);
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void app_alarm_cancel_preview(void)
{
    portENTER_CRITICAL(&s_lock);
    s_preview_tone = APP_ALARM_TONE_COUNT;
    ++s_preview_revision;
    portEXIT_CRITICAL(&s_lock);
}

void app_alarm_dismiss(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_ringing_mask) {
        s_ringing_mask = 0;
        ++s_ring_revision;
        update_ringing_summary_locked();
    }
    portEXIT_CRITICAL(&s_lock);
}

void app_note_get_status(unsigned note, app_note_status_t *out)
{
    if (note >= APP_NOTE_COUNT) {
        *out = (app_note_status_t){.save_error = ESP_ERR_INVALID_ARG};
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_note_status[note];
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t app_note_read(unsigned note, char out[APP_NOTE_CAPACITY])
{
    if (note >= APP_NOTE_COUNT || !out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    const bool ready = s_note_status[note].ready;
    if (ready) memcpy(out, s_notes[note], APP_NOTE_CAPACITY);
    portEXIT_CRITICAL(&s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t app_note_save(unsigned note, const char *text)
{
    if (note >= APP_NOTE_COUNT || !text) return ESP_ERR_INVALID_ARG;
    const size_t length = strnlen(text, APP_NOTE_CAPACITY);
    if (length >= APP_NOTE_CAPACITY) return ESP_ERR_INVALID_SIZE;
    portENTER_CRITICAL(&s_lock);
    if (!s_note_status[note].ready) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(s_notes[note], text) != 0) {
        memcpy(s_notes[note], text, length + 1);
        s_note_status[note].pending = true;
        s_note_status[note].save_error = ESP_OK;
        ++s_note_revision[note];
    }
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_service);
    return ESP_OK;
}

bool app_utilities_save_pending(void)
{
    portENTER_CRITICAL(&s_lock);
    bool pending = s_alarm.pending || s_city_pending;
    for (unsigned note = 0; note < APP_NOTE_COUNT; ++note) pending |= s_note_status[note].pending;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

esp_err_t app_utilities_save_error(void)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = s_alarm.pending ? s_alarm.save_error : ESP_OK;
    if (error == ESP_OK && s_city_pending) error = s_city_error;
    for (unsigned note = 0; note < APP_NOTE_COUNT && error == ESP_OK; ++note)
        if (s_note_status[note].pending) error = s_note_status[note].save_error;
    portEXIT_CRITICAL(&s_lock);
    return error;
}

static bool wifi_connected(void)
{
    aether_wifi_status_t status;
    aether_wifi_handle_t wifi = aether_wifi_get_default();
    return wifi && aether_wifi_get_status(wifi, &status) == ESP_OK && status.connected;
}

static esp_err_t read_weather_body(esp_http_client_handle_t client, char *body, int64_t length,
                                   int *received)
{
    const int64_t deadline = esp_timer_get_time() + WEATHER_BODY_TIMEOUT_MS * 1000;
    const int limit = length > 0 ? (int)length : WEATHER_BODY - 1;
    while (*received < limit) {
        const int64_t remaining_ms = (deadline - esp_timer_get_time()) / 1000;
        if (!power_running()) return ESP_ERR_INVALID_STATE;
        if (remaining_ms <= 0) return ESP_ERR_TIMEOUT;
        esp_http_client_set_timeout_ms(client, remaining_ms < WEATHER_BODY_READ_TIMEOUT_MS ?
                                       (int)remaining_ms : WEATHER_BODY_READ_TIMEOUT_MS);
        const int left = limit - *received;
        const int read = esp_http_client_read(client, body + *received, left < 128 ? left : 128);
        if (read == -ESP_ERR_HTTP_EAGAIN) continue;
        if (read < 0) return ESP_ERR_INVALID_RESPONSE;
        if (!read) break;
        *received += read;
    }
    if (!*received || (length > 0 && *received != length) ||
        !esp_http_client_is_complete_data_received(client) || memchr(body, 0, *received))
        return ESP_ERR_INVALID_RESPONSE;
    body[*received] = '\0';
    return ESP_OK;
}

static esp_err_t download_weather(app_weather_snapshot_t *result)
{
    char url[320];
    if (!app_weather_forecast_url(result->city, url, sizeof(url))) return ESP_ERR_INVALID_ARG;
    char *body = malloc(WEATHER_BODY);
    if (!body) {
        ESP_LOGW("weather", "stage=buffer error=%s", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    const esp_http_client_config_t config = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET,
        .timeout_ms = WEATHER_REQUEST_TIMEOUT_MS, .buffer_size = 768, .buffer_size_tx = 768,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    const char *stage = "client-init";
    int64_t length = -1;
    int status = 0, received = 0;
    bool attempted_open = false;
    esp_err_t error = ESP_ERR_NO_MEM;
    if (client) {
        stage = "request";
        error = esp_http_client_set_header(client, "Accept", "application/json");
        if (error == ESP_OK) error = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    }
    if (error == ESP_OK) {
        stage = "open";
        attempted_open = true;
        error = esp_http_client_open(client, 0);
        if (error == ESP_OK) {
            stage = "headers";
            length = esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);
            if (length == -ESP_ERR_HTTP_EAGAIN) error = ESP_ERR_TIMEOUT;
            else if (length < 0) error = ESP_ERR_HTTP_FETCH_HEADER;
            else if (status != 200) error = ESP_ERR_INVALID_RESPONSE;
            else if (length >= WEATHER_BODY) error = ESP_ERR_INVALID_SIZE;
        }
        if (error == ESP_OK) {
            stage = "body";
            error = read_weather_body(client, body, length, &received);
        }
        if (error == ESP_OK) {
            stage = "parse";
            if (!app_weather_parse(body, result)) error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (error != ESP_OK) {
        int tls_code = 0, tls_flags = 0, socket_errno = 0;
        esp_err_t tls_error = ESP_OK;
        if (attempted_open) {
            socket_errno = esp_http_client_get_errno(client);
            tls_error = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
        }
        ESP_LOGW("weather", "stage=%s error=%s status=%d length=%" PRId64
                 " received=%d tls_error=0x%x mbedtls=%d verify=0x%x socket_errno=%d",
                 stage, esp_err_to_name(error), status, length, received, (unsigned)tls_error,
                 tls_code, (unsigned)tls_flags, socket_errno);
    } else {
        ESP_LOGI("weather", "forecast updated status=%d bytes=%d", status, received);
    }
    if (client) esp_http_client_cleanup(client);
    free(body);
    return error;
}

static void weather_task(void *context)
{
    app_weather_snapshot_t result = {.city = (unsigned)(uintptr_t)context};
    esp_err_t error = download_weather(&result);
    if (error == ESP_ERR_HTTP_CONNECT && power_running() && wifi_connected()) {
        ESP_LOGI("weather", "retrying forecast connection");
        error = download_weather(&result);
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_weather.city == result.city) {
        if (error == ESP_OK) s_weather = result;
        s_weather.error = error;
        s_weather_due = now + (error == ESP_OK ? 900000000 : 60000000);
    }
    s_weather.fetching = false;
    s_weather_running = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

void app_weather_get(app_weather_snapshot_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_weather;
    portEXIT_CRITICAL(&s_lock);
}

void app_weather_refresh(bool manual)
{
    const int64_t now = esp_timer_get_time();
    if (!power_running() || !wifi_connected() || (storage_busy() && !manual)) return;
    portENTER_CRITICAL(&s_lock);
    if (!s_alarm.ready || s_weather_running || (!manual && now < s_weather_due)) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    const unsigned city = s_weather.city;
    s_weather_running = s_weather.fetching = true;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(weather_task, "weather", 7168, (void *)(uintptr_t)city, 2, NULL) == pdPASS)
        return;
    portENTER_CRITICAL(&s_lock);
    s_weather_running = s_weather.fetching = false;
    s_weather_due = now + 60000000;
    s_weather.error = ESP_ERR_NO_MEM;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t app_weather_select_city(unsigned city)
{
    if (city >= app_weather_city_count()) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (!s_alarm.ready) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_weather.city != city) {
        s_weather = (app_weather_snapshot_t){.city = city, .fetching = s_weather_running};
        s_weather_due = 0;
        s_city_pending = true;
        s_city_error = ESP_OK;
        ++s_city_revision;
    }
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_service);
    app_weather_refresh(true);
    return ESP_OK;
}
