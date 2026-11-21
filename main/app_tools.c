#include "app_tools.h"
#include "app_tools_math.h"

#include <math.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TOOLS_MAGIC 0x41545433U
#define TOOLS_V2_MAGIC 0x41545432U

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
        if (snapshot.clock_pending) {
            esp_err_t err = !s_board->rtc_ready || !s_board->rtc ? ESP_ERR_NOT_FOUND :
                pcf85063_set_time(s_board->rtc, &request);
            if (err == ESP_OK) {
                pcf85063_time_t actual;
                err = pcf85063_get_time(s_board->rtc, &actual);
                if (err == ESP_OK) {
                    const uint64_t expected_seconds = clock_seconds(&request);
                    const uint64_t actual_seconds = clock_seconds(&actual);
                    if (actual.oscillator_stopped || actual_seconds == UINT64_MAX ||
                        actual_seconds < expected_seconds || actual_seconds - expected_seconds > 2) {
                        err = ESP_ERR_INVALID_RESPONSE;
                    }
                }
            }
            portENTER_CRITICAL(&s_lock);
            s_snapshot.clock_error = err;
            s_snapshot.clock_pending = false;
            ++s_snapshot.clock_revision;
            portEXIT_CRITICAL(&s_lock);
        }
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
    if (!s_snapshot.ready || s_snapshot.clock_pending) {
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
