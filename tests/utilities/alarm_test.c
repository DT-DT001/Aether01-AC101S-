#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_tools_math.h"
#include "app_weather_data.h"
#include "alarm_types.inc"

typedef int portMUX_TYPE;
typedef int TaskHandle_t;
typedef int nvs_handle_t;
typedef struct { bool audio_ready; void *audio; } aether_board_t;
typedef struct { esp_err_t rtc_error; pcf85063_time_t rtc; } app_telemetry_snapshot_t;

#define EXT_RAM_BSS_ATTR
#define PORTMUX_INITIALIZER 0
#define portMUX_INITIALIZER_UNLOCKED PORTMUX_INITIALIZER
#define portENTER_CRITICAL(lock) do { assert(!*(lock)); *(lock) = 1; } while (0)
#define portEXIT_CRITICAL(lock) do { assert(*(lock)); *(lock) = 0; } while (0)

enum { NVS_READONLY, NVS_READWRITE };

static const aether_board_t board = {.audio_ready = true};
static unsigned power_fixture, *s_power = &power_fixture;
static app_telemetry_snapshot_t telemetry = {.rtc_error = ESP_FAIL};
static int64_t now_us;
static unsigned wakes, playback, last_tone;
static esp_err_t playback_error;
static bool running = true;
static bool s_city_pending;
static esp_err_t s_city_error;
static app_weather_snapshot_t s_weather;
static uint8_t stored[APP_ALARM_SLOTS * 4], staged[sizeof(stored)];
static size_t stored_length;
static bool has_legacy;
static uint16_t legacy;
static esp_err_t commit_error;
static void (*during_commit)(void);
static unsigned notifications;

static bool power_running(void) { return running; }
static int64_t esp_timer_get_time(void) { return now_us; }
static void app_telemetry_get(app_telemetry_snapshot_t *out) { *out = telemetry; }
static esp_err_t app_power_wake(unsigned *power) { assert(power == s_power); ++wakes; return ESP_OK; }
static esp_err_t app_alarm_audio_play(void *audio, unsigned tone, bool (*current)(void *), void *context)
{
    (void)audio;
    assert(current(context));
    ++playback;
    last_tone = tone;
    return playback_error;
}
void app_alarm_dismiss(void);
void app_countdown_dismiss(void);
static void xTaskNotifyGive(TaskHandle_t task) { assert(task); ++notifications; }
unsigned app_weather_city_count(void) { return 1; }

static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    assert(!strcmp(name, "utilities"));
    *out = mode;
    return ESP_OK;
}

static void nvs_close(nvs_handle_t handle) { (void)handle; }

static esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *length)
{
    assert(handle == NVS_READONLY && !strcmp(key, "alarms_v2"));
    if (!stored_length) return ESP_ERR_NVS_NOT_FOUND;
    assert(*length >= stored_length);
    memcpy(data, stored, stored_length);
    *length = stored_length;
    return ESP_OK;
}

static esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    assert(handle == NVS_READONLY && !strcmp(key, "alarm_v1"));
    if (!has_legacy) return ESP_ERR_NVS_NOT_FOUND;
    *value = legacy;
    return ESP_OK;
}

static esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle == NVS_READONLY && !strcmp(key, "city_v1"));
    (void)value;
    return ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *text, size_t *length)
{
    assert(handle == NVS_READONLY && !strncmp(key, "note", 4));
    (void)text;
    (void)length;
    return ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    assert(handle == NVS_READWRITE && !strcmp(key, "alarms_v2") && length == sizeof(staged));
    memcpy(staged, data, length);
    return ESP_OK;
}

static esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == NVS_READWRITE && !strcmp(key, "city_v1"));
    (void)value;
    return ESP_OK;
}

static esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == NVS_READWRITE);
    if (during_commit) during_commit();
    if (commit_error == ESP_OK) {
        memcpy(stored, staged, sizeof(stored));
        stored_length = sizeof(stored);
    }
    return commit_error;
}

#include "alarm_service.inc"

static void expect_slot(unsigned slot, bool configured, bool enabled,
                        unsigned hour, unsigned minute, unsigned tone)
{
    app_alarm_slot_t value;
    assert(app_alarm_get_slot(slot, &value) == ESP_OK);
    assert(value.configured == configured && value.enabled == enabled);
    assert(value.hour == hour && value.minute == minute && value.tone == tone);
}

static void expect_ring(unsigned slot, unsigned count, unsigned tone)
{
    app_alarm_snapshot_t state;
    app_alarm_get(&state);
    assert(state.ringing == (count != 0));
    assert(state.ringing_slot == slot && state.ringing_count == count);
    if (count) assert(state.tone == tone);
}

static void concurrent_edit(void)
{
    assert(app_alarm_set_slot(2, 16, 5, true, 3) == ESP_OK);
}

static void test_persistence(void)
{
    assert(app_alarm_set_slot(0, 7, 9, true, 0) == ESP_ERR_INVALID_STATE);
    load_preferences();
    assert(s_alarm.ready && !s_alarm.pending && s_alarm.save_error == ESP_OK);
    for (unsigned i = 0; i < APP_ALARM_SLOTS; ++i) expect_slot(i, false, false, 0, 0, 0);

    has_legacy = true;
    legacy = (7U << 8) | 9U;
    load_preferences();
    expect_slot(0, true, false, 7, 9, 0);
    assert(s_alarm.pending);
    assert(app_alarm_set_slot(APP_ALARM_SLOTS, 7, 0, true, 0) == ESP_ERR_INVALID_ARG);
    assert(app_alarm_set_slot(1, 24, 0, true, 0) == ESP_ERR_INVALID_ARG);
    assert(app_alarm_set_slot(1, 7, 60, true, 0) == ESP_ERR_INVALID_ARG);
    assert(app_alarm_set_slot(1, 7, 0, true, APP_ALARM_TONE_COUNT) == ESP_ERR_INVALID_ARG);
    assert(app_alarm_set_slot(2, 16, 0, true, 1) == ESP_OK);
    assert(app_alarm_set_slot(5, 23, 59, true, 2) == ESP_OK);
    commit_error = ESP_FAIL;
    assert(save_preferences() == ESP_FAIL && s_alarm.pending && s_alarm.save_error == ESP_FAIL);
    assert(!stored_length);

    commit_error = ESP_OK;
    during_commit = concurrent_edit;
    assert(save_preferences() == ESP_OK && s_alarm.pending);
    expect_slot(2, true, true, 16, 5, 3);
    during_commit = NULL;
    assert(save_preferences() == ESP_OK && !s_alarm.pending);
    legacy = 0x8000 | (8U << 8) | 17U;
    load_preferences();
    expect_slot(0, true, false, 7, 9, 0);
    expect_slot(2, true, true, 16, 5, 3);
    expect_slot(5, true, true, 23, 59, 2);

    stored[2 * 4 + 3] = APP_ALARM_TONE_COUNT;
    load_preferences();
    assert(s_alarm.save_error == ESP_ERR_INVALID_RESPONSE);
    expect_slot(2, false, false, 0, 0, 0);
    expect_slot(5, true, true, 23, 59, 2);
    assert(app_alarm_remove_slot(0) == ESP_OK);
    assert(save_preferences() == ESP_OK);
    load_preferences();
    expect_slot(0, false, false, 0, 0, 0);
    assert(s_alarm.save_error == ESP_OK && !s_alarm.pending);
}

static void test_real_occurrences_and_preview(void)
{
    assert(app_alarm_set_slot(0, 23, 59, true, 1) == ESP_OK);
    assert(app_alarm_set_slot(1, 23, 59, false, 3) == ESP_OK);
    assert(app_alarm_set_slot(3, 0, 0, true, 0) == ESP_OK);
    assert(app_alarm_preview(APP_ALARM_TONE_COUNT) == ESP_ERR_INVALID_ARG);
    assert(app_alarm_preview(3) == ESP_OK);
    const alarm_audio_request_t preview = {.preview = true, .revision = s_preview_revision};
    assert(s_preview_tone == 3 && alarm_audio_current((void *)&preview));
    app_alarm_cancel_preview();
    assert(s_preview_tone == APP_ALARM_TONE_COUNT && !alarm_audio_current((void *)&preview));

    assert(app_alarm_preview(2) == ESP_OK);
    const alarm_audio_request_t next_preview = {.preview = true, .revision = s_preview_revision};
    pcf85063_time_t rtc = {.year = 2026, .month = 12, .day = 31, .hour = 23,
                           .minute = 59, .second = 58, .oscillator_stopped = true};
    assert(!collect_due_alarms(&rtc));
    rtc.oscillator_stopped = false;
    rtc.day = 32;
    assert(!collect_due_alarms(&rtc));
    rtc.day = 31;
    assert(collect_due_alarms(&rtc));
    expect_ring(0, 2, 1);
    assert(!alarm_audio_current((void *)&next_preview));
    assert(app_alarm_preview(1) == ESP_ERR_INVALID_STATE);
    const alarm_audio_request_t first = {.slot = 0, .revision = s_ring_revision};
    app_alarm_cancel_preview();
    assert(app_alarm_remove_slot(5) == ESP_OK);
    expect_ring(0, 1, 1);
    assert(alarm_audio_current((void *)&first));
    rtc.second = 59;
    assert(!collect_due_alarms(&rtc));

    rtc = (pcf85063_time_t){.year = 2027, .month = 1, .day = 1};
    assert(collect_due_alarms(&rtc));
    expect_ring(3, 1, 0);
    assert(!alarm_audio_current((void *)&first));
    const alarm_audio_request_t midnight = {.slot = 3, .revision = s_ring_revision};
    assert(alarm_audio_current((void *)&midnight));
    running = false;
    assert(!alarm_audio_current((void *)&midnight));
    running = true;
    app_alarm_dismiss();
    expect_ring(APP_ALARM_NO_SLOT, 0, 0);
    assert(!collect_due_alarms(&rtc) && !alarm_audio_current((void *)&midnight));
    rtc.day = 2;
    assert(collect_due_alarms(&rtc));
    expect_ring(3, 1, 0);
    assert(app_alarm_set_slot(3, 0, 0, false, 0) == ESP_OK);
    expect_ring(APP_ALARM_NO_SLOT, 0, 0);
    assert(!collect_due_alarms(&rtc));
}

static void expect_countdown(app_countdown_state_t state, unsigned remaining, bool ringing)
{
    app_countdown_snapshot_t snapshot;
    app_countdown_get(&snapshot);
    assert(snapshot.state == state && snapshot.remaining_seconds == remaining && snapshot.ringing == ringing);
}

static void test_countdown_and_alert(void)
{
    assert(app_countdown_start(0) == ESP_ERR_INVALID_ARG);
    assert(app_countdown_start(APP_COUNTDOWN_MAX_SECONDS + 1) == ESP_ERR_INVALID_ARG);
    expect_countdown(APP_COUNTDOWN_IDLE, 300, false);
    assert(app_countdown_start(3) == ESP_OK);
    int64_t ring_started = 0, next_tone = 0;
    now_us = 850000;
    app_countdown_pause();
    expect_countdown(APP_COUNTDOWN_PAUSED, 3, false);
    now_us = 5000000;
    process_clock_alerts(now_us, &ring_started, &next_tone);
    assert(wakes == 0 && playback == 0);
    app_countdown_resume();
    expect_countdown(APP_COUNTDOWN_RUNNING, 3, false);
    now_us = 7149999;
    process_clock_alerts(now_us, &ring_started, &next_tone);
    expect_countdown(APP_COUNTDOWN_RUNNING, 1, false);
    assert(wakes == 0 && playback == 0);
    now_us = 7150000;
    app_countdown_pause(); /* A tap just after the deadline must not lose the alert. */
    playback_error = ESP_ERR_INVALID_STATE;
    process_clock_alerts(now_us, &ring_started, &next_tone);
    assert(wakes == 1 && playback == 1 && last_tone == 1 && next_tone == 7650);
    expect_countdown(APP_COUNTDOWN_FINISHED, 0, true);
    assert(app_alarm_preview(3) == ESP_ERR_INVALID_STATE);
    now_us = 7650000;
    playback_error = ESP_OK;
    process_clock_alerts(now_us, &ring_started, &next_tone);
    assert(wakes == 1 && playback == 2 && next_tone == 9650);
    now_us = 37150000;
    process_clock_alerts(now_us, &ring_started, &next_tone);
    expect_countdown(APP_COUNTDOWN_FINISHED, 0, false);
    assert(wakes == 1 && playback == 2);
    app_countdown_reset();
    expect_countdown(APP_COUNTDOWN_IDLE, 3, false);

    assert(app_alarm_set_slot(1, 5, 10, true, 3) == ESP_OK);
    assert(app_countdown_start(1) == ESP_OK);
    telemetry = (app_telemetry_snapshot_t){ESP_OK, {.year = 2027, .month = 1, .day = 3, .hour = 5, .minute = 10}};
    now_us += 1000000;
    process_clock_alerts(now_us, &ring_started, &next_tone);
    expect_ring(1, 1, 3);
    expect_countdown(APP_COUNTDOWN_FINISHED, 0, true);
    assert(wakes == 2 && playback == 3 && last_tone == 3);
    app_alarm_dismiss();
    now_us = next_tone * 1000;
    process_clock_alerts(now_us, &ring_started, &next_tone);
    assert(wakes == 2 && playback == 4 && last_tone == 1);
    app_countdown_dismiss();
    expect_countdown(APP_COUNTDOWN_FINISHED, 0, false);
    assert(app_countdown_start(1) == ESP_OK);
    running = false;
    now_us += 1000000;
    process_clock_alerts(now_us, &ring_started, &next_tone);
    expect_countdown(APP_COUNTDOWN_FINISHED, 0, false);
    assert(wakes == 2 && playback == 4 && app_countdown_start(1) == ESP_ERR_INVALID_STATE);
    running = true;
}

int main(void)
{
    aether_board_t fixture = board;
    s_board = &fixture;
    s_service = 1;
    test_persistence();
    test_real_occurrences_and_preview();
    test_countdown_and_alert();
    assert(notifications > 0 && !s_lock);
    puts("alarm/countdown service: persistence, RTC rollover, monotonic pause/resume, wake, tone retry, coexisting alerts and shutdown passed");
    return 0;
}
