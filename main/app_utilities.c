#include "app_utilities.h"
#include "app_telemetry.h"
#include "app_tools_math.h"
#include "aether_wifi.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
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
    ALARM_FRAMES = 4320,
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
static app_alarm_snapshot_t s_alarm = {.hour = 7};
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

static void load_preferences(void)
{
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(s_namespace, NVS_READONLY, &handle);
    uint16_t alarm = 7U << 8;
    uint8_t city = 0;
    const esp_err_t alarm_error = opened == ESP_OK ? nvs_get_u16(handle, "alarm_v1", &alarm) : opened;
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
    const unsigned hour = (alarm >> 8) & 0x7f;
    const unsigned minute = alarm & 0xff;
    if (hour < 24 && minute < 60) {
        s_alarm.hour = hour;
        s_alarm.minute = minute;
        s_alarm.enabled = (alarm & 0x8000) != 0;
    }
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
    app_alarm_snapshot_t alarm;
    unsigned city;
    bool city_pending;
    uint32_t alarm_revision, city_revision;
    portENTER_CRITICAL(&s_lock);
    alarm = s_alarm;
    alarm_revision = s_alarm_revision;
    city = s_weather.city;
    city_pending = s_city_pending;
    city_revision = s_city_revision;
    portEXIT_CRITICAL(&s_lock);
    if (!alarm.pending && !city_pending) return ESP_OK;

    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(s_namespace, NVS_READWRITE, &handle);
    esp_err_t alarm_error = opened, city_error = opened;
    if (opened == ESP_OK) {
        if (alarm.pending)
            alarm_error = nvs_set_u16(handle, "alarm_v1",
                (alarm.enabled ? 0x8000 : 0) | (alarm.hour << 8) | alarm.minute);
        if (city_pending)
            city_error = nvs_set_u8(handle, "city_v1", city);
        const esp_err_t committed = nvs_commit(handle);
        if (alarm_error == ESP_OK) alarm_error = committed;
        if (city_error == ESP_OK) city_error = committed;
        nvs_close(handle);
    }
    portENTER_CRITICAL(&s_lock);
    if (alarm.pending && alarm_revision == s_alarm_revision) {
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

static void play_alarm(void)
{
    if (!s_board->audio_ready || storage_busy()) return;
    int16_t *pcm = heap_caps_malloc(ALARM_FRAMES * 2 * sizeof(*pcm),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) return;
    /* Two 35 ms notes with short ramps. The one-shot codec operation owns
     * the local speaker only for this burst and leaves existing routes intact. */
    const unsigned note_frames = 1680;
    const unsigned ramp_frames = 192;
    for (unsigned frame = 0; frame < ALARM_FRAMES; ++frame) {
        const bool second_note = frame >= ALARM_FRAMES / 2;
        const unsigned offset = frame % (ALARM_FRAMES / 2);
        int16_t sample = 0;
        if (offset < note_frames) {
            unsigned edge = offset < note_frames - offset ? offset : note_frames - offset;
            if (edge > ramp_frames) edge = ramp_frames;
            const int amplitude = (int)(5200U * edge / ramp_frames);
            const unsigned period = second_note ? 40 : 50;
            sample = offset % period < period / 2 ? amplitude : -amplitude;
        }
        pcm[2 * frame] = pcm[2 * frame + 1] = sample;
    }
    app_alarm_snapshot_t alarm;
    app_alarm_get(&alarm);
    if (alarm.ringing && power_running())
        (void)ac101s_audio_play_alert(s_board->audio, pcm, ALARM_FRAMES);
    free(pcm);
}

static void service(void *unused)
{
    (void)unused;
    load_preferences();
    uint64_t last_alarm_minute = UINT64_MAX;
    int64_t ring_started = 0, next_tone = 0, next_save = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(450));
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const bool running = power_running();
        app_alarm_snapshot_t alarm;
        app_alarm_get(&alarm);
        app_telemetry_snapshot_t telemetry;
        app_telemetry_get(&telemetry);
        const pcf85063_time_t *rtc = &telemetry.rtc;
        if (running && alarm.enabled && telemetry.rtc_error == ESP_OK && !rtc->oscillator_stopped &&
            rtc->hour == alarm.hour && rtc->minute == alarm.minute) {
            const uint64_t seconds = app_calendar_seconds(rtc->year, rtc->month, rtc->day,
                                                         rtc->hour, rtc->minute, 0);
            if (seconds != UINT64_MAX && seconds != last_alarm_minute) {
                portENTER_CRITICAL(&s_lock);
                const bool match = s_alarm.enabled && s_alarm.hour == rtc->hour &&
                                   s_alarm.minute == rtc->minute;
                if (match) s_alarm.ringing = true;
                portEXIT_CRITICAL(&s_lock);
                if (match) {
                    ring_started = now_ms;
                    next_tone = now_ms;
                    last_alarm_minute = seconds;
                    alarm.ringing = true;
                    (void)app_power_wake(s_power);
                }
            }
        }
        if (alarm.ringing) {
            if (!running || now_ms - ring_started >= 60000) app_alarm_dismiss();
            else if (now_ms >= next_tone) {
                play_alarm();
                next_tone = now_ms + 4500;
            }
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

esp_err_t app_alarm_set(unsigned hour, unsigned minute, bool enabled)
{
    if (hour >= 24 || minute >= 60) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (!s_alarm.ready) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_alarm.hour != hour || s_alarm.minute != minute || s_alarm.enabled != enabled) {
        s_alarm.hour = hour;
        s_alarm.minute = minute;
        s_alarm.enabled = enabled;
        s_alarm.ringing = false;
        s_alarm.pending = true;
        s_alarm.save_error = ESP_OK;
        ++s_alarm_revision;
    }
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_service);
    return ESP_OK;
}

void app_alarm_dismiss(void)
{
    portENTER_CRITICAL(&s_lock);
    s_alarm.ringing = false;
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
