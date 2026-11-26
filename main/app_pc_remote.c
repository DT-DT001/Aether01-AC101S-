#include "app_pc_remote.h"
#include "app_air_mouse.h"
#include "app_telemetry.h"
#include "app_text.h"

#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#define MOTION_LEASE_US 200000
#define IMU_MAX_AGE_MS 100U
#define SHORTCUT_VERSION 3U
#define SHORTCUT_RETRY_US 2000000
#define MOUSE_SAVE_DELAY_US 400000

typedef struct {
    uint32_t version;
    app_pc_shortcut_t shortcut[APP_PC_REMOTE_SCENES];
    char scene_names[APP_PC_REMOTE_SCENES][APP_PC_REMOTE_SCENE_BYTES];
    uint8_t sensitivity;
} shortcut_settings_t;

static shortcut_settings_t s_shortcuts;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static aether_bluetooth_handle_t s_bluetooth;
static app_power_t *s_power;
static bool s_imu_ready;
static bool (*s_flash_busy)(void *);
static bool s_air_mode;
static int64_t s_motion_until;
static uint32_t s_motion_epoch, s_save_revision;
static bool s_save_pending;
static esp_err_t s_save_error;
static int64_t s_save_retry_at;
/* The telemetry task is the only owner of the motion filter. */
static app_air_mouse_t s_mouse;
static uint32_t s_mouse_epoch;

static bool power_allows_input(void)
{
    app_power_status_t power;
    return s_power && app_power_get_status(s_power, &power) == ESP_OK &&
           !power.sleeping && !power.shutdown_recovering &&
           power.shutdown_state != APP_POWER_SHUTDOWN_PREPARING &&
           power.shutdown_state != APP_POWER_SHUTDOWN_REQUESTED;
}

static bool fresh_gyro(const app_telemetry_snapshot_t *sample, uint32_t now)
{
    return sample && !sample->gyro_calibrating &&
           sample->imu_error == ESP_OK && sample->imu.gyro_bias_valid &&
           now - sample->imu_updated_ms <= IMU_MAX_AGE_MS;
}

static bool supported_key(uint8_t key)
{
    return key >= 0x04 && key <= 0x73;
}

static bool scene_name_copy(char stored[APP_PC_REMOTE_SCENE_BYTES], const char *name)
{
    if (!name) return false;
    size_t length = 0;
    while (length < APP_PC_REMOTE_SCENE_BYTES && name[length]) ++length;
    if (length == APP_PC_REMOTE_SCENE_BYTES) return false;
    while (length && *name == ' ') { ++name; --length; }
    while (length && name[length - 1] == ' ') --length;
    if (!length) return false;
    unsigned characters = 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = (unsigned char)name[i];
        if (byte < 0x20 || byte == 0x7f) return false;
        if ((byte & 0xc0) != 0x80) ++characters;
    }
    if (characters > APP_PC_REMOTE_SCENE_CHARACTERS) return false;
    memcpy(stored, name, length);
    return app_text_sanitize_utf8(stored, length) == length && !memcmp(stored, name, length);
}

static void service_shortcuts(int64_t now)
{
    portENTER_CRITICAL(&s_lock);
    /* NVS writes run in the telemetry task. Defer them while the mouse
     * movement lease needs an uninterrupted IMU stream. */
    const bool pending = s_save_pending && now >= s_save_retry_at && s_motion_until <= now;
    const uint32_t revision = s_save_revision;
    const shortcut_settings_t settings = s_shortcuts;
    portEXIT_CRITICAL(&s_lock);
    if (!pending || (s_flash_busy && s_flash_busy(NULL))) return;

    nvs_handle_t nvs;
    esp_err_t error = nvs_open("pc_remote", NVS_READWRITE, &nvs);
    if (error == ESP_OK) {
        error = nvs_set_blob(nvs, "shortcuts", &settings, sizeof(settings));
        if (error == ESP_OK) error = nvs_commit(nvs);
        nvs_close(nvs);
    }
    portENTER_CRITICAL(&s_lock);
    if (s_save_revision == revision) {
        s_save_error = error;
        s_save_pending = error != ESP_OK;
        s_save_retry_at = error == ESP_OK ? 0 : now + SHORTCUT_RETRY_US;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void stop_motion_locked(void)
{
    s_motion_until = 0;
    ++s_motion_epoch;
    /* Motion queue operations take the HID spinlock only. The HID worker
     * and callbacks never take s_lock while holding that lock. */
    if (s_bluetooth) (void)aether_bluetooth_hid_cancel_motion(s_bluetooth);
}

static void service_remote(const app_telemetry_snapshot_t *sample)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_motion_until && (s_motion_until <= now || (sample && sample->gyro_calibrating)))
        stop_motion_locked();
    const bool moving = s_air_mode && s_motion_until > now;
    const uint32_t epoch = s_motion_epoch;
    const uint8_t sensitivity = s_shortcuts.sensitivity;
    portEXIT_CRITICAL(&s_lock);
    if (!moving || !fresh_gyro(sample, (uint32_t)(now / 1000)) || !power_allows_input()) {
        s_mouse = (app_air_mouse_t){0};
    } else {
        if (s_mouse_epoch != epoch) s_mouse = (app_air_mouse_t){0};
        s_mouse_epoch = epoch;
        int8_t dx, dy;
        if (app_air_mouse_update(&s_mouse, sample->imu.accel_mps2, sample->imu.gyro_dps,
                                 sample->imu.gyro_stationary, sample->imu_updated_ms,
                                 sensitivity, &dx, &dy)) {
            const int64_t send_now = esp_timer_get_time();
            portENTER_CRITICAL(&s_lock);
            /* A release/recenter that wins the lock invalidates this sample.
             * Otherwise it clears the movement just queued under the same lock. */
            if (s_motion_epoch == epoch && s_motion_until > send_now)
                (void)aether_bluetooth_hid_move(s_bluetooth, dx, dy);
            portEXIT_CRITICAL(&s_lock);
        }
    }
    service_shortcuts(now);
}

void app_pc_remote_init(aether_bluetooth_handle_t bluetooth, app_power_t *power,
                        bool imu_ready, bool (*flash_busy)(void *))
{
    s_bluetooth = bluetooth;
    s_power = power;
    s_imu_ready = imu_ready;
    s_flash_busy = flash_busy;
    s_shortcuts = (shortcut_settings_t){
        .version = SHORTCUT_VERSION,
        .shortcut = {{0x05, 0x42}, {0x05, 0x43}, {0x05, 0x44}, {0x05, 0x45}},
        .sensitivity = APP_PC_MOUSE_DEFAULT,
    };
    s_save_pending = false;
    s_save_error = ESP_OK;
    s_save_retry_at = 0;
    nvs_handle_t nvs;
    if (nvs_open("pc_remote", NVS_READONLY, &nvs) == ESP_OK) {
        shortcut_settings_t stored = {0};
        size_t size = sizeof(stored);
        if (nvs_get_blob(nvs, "shortcuts", &stored, &size) == ESP_OK) {
            const bool legacy = stored.version == 1 && size == offsetof(shortcut_settings_t, scene_names);
            const bool named = stored.version == 2 && size == offsetof(shortcut_settings_t, sensitivity);
            const bool current = stored.version == SHORTCUT_VERSION && size == sizeof(stored);
            if (current && stored.sensitivity >= APP_PC_MOUSE_MIN && stored.sensitivity <= APP_PC_MOUSE_MAX)
                s_shortcuts.sensitivity = stored.sensitivity;
            for (unsigned i = 0; (legacy || named || current) && i < APP_PC_REMOTE_SCENES; ++i) {
                if (supported_key(stored.shortcut[i].key)) s_shortcuts.shortcut[i] = stored.shortcut[i];
                char name[APP_PC_REMOTE_SCENE_BYTES] = {0};
                if ((named || current) && scene_name_copy(name, stored.scene_names[i]))
                    memcpy(s_shortcuts.scene_names[i], name, sizeof(name));
            }
        }
        nvs_close(nvs);
    }
    app_telemetry_set_consumer(service_remote);
}

void app_pc_remote_get_status(app_pc_remote_status_t *status)
{
    if (!status) return;
    aether_bluetooth_hid_status_t hid = {0};
    const esp_err_t error = s_bluetooth ? aether_bluetooth_hid_get_status(s_bluetooth, &hid) :
                                        ESP_ERR_NOT_SUPPORTED;
    app_telemetry_snapshot_t sample;
    app_telemetry_get(&sample);
    *status = (app_pc_remote_status_t){
        .available = error == ESP_OK,
        .requested = hid.requested, .discoverable = hid.discoverable,
        .connected = hid.connected, .reconnecting = hid.reconnecting,
        .confirm_pending = hid.confirm_pending,
        .confirm_code = hid.confirm_code,
        .gyro_ready = s_imu_ready,
        .gyro_calibrated = fresh_gyro(&sample, (uint32_t)(esp_timer_get_time() / 1000)),
        .last_error = error == ESP_OK ? hid.last_error : error,
    };
    portENTER_CRITICAL(&s_lock);
    status->save_pending = s_save_pending;
    status->save_error = s_save_error;
    status->sensitivity = s_shortcuts.sensitivity;
    portEXIT_CRITICAL(&s_lock);
}

void app_pc_remote_recenter(void)
{
    portENTER_CRITICAL(&s_lock);
    stop_motion_locked();
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t app_pc_remote_set_enabled(bool enabled)
{
    if (!s_bluetooth) return ESP_ERR_NOT_SUPPORTED;
    if (enabled && !power_allows_input()) return ESP_ERR_INVALID_STATE;
    if (!enabled) app_pc_remote_recenter();
    return aether_bluetooth_hid_set_enabled(s_bluetooth, enabled);
}

esp_err_t app_pc_remote_confirm(bool accept)
{
    if (!s_bluetooth || (accept && !power_allows_input())) return ESP_ERR_INVALID_STATE;
    return aether_bluetooth_hid_confirm(s_bluetooth, accept);
}

esp_err_t app_pc_remote_key(uint8_t modifiers, uint8_t key)
{
    if (!s_bluetooth || !power_allows_input()) return ESP_ERR_INVALID_STATE;
    return aether_bluetooth_hid_key(s_bluetooth, modifiers, key);
}

esp_err_t app_pc_remote_click(uint8_t buttons)
{
    if (!s_bluetooth || !power_allows_input()) return ESP_ERR_INVALID_STATE;
    return aether_bluetooth_hid_click(s_bluetooth, buttons);
}

void app_pc_remote_set_air_mode(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    stop_motion_locked();
    s_air_mode = enabled && s_imu_ready && power_allows_input();
    /* Publish the sensor subscription before a concurrent shutdown can leave. */
    app_telemetry_set_interest(s_air_mode ? APP_TELEMETRY_IMU : 0);
    portEXIT_CRITICAL(&s_lock);
}

void app_pc_remote_set_motion(bool enabled)
{
    if (!enabled || !s_bluetooth || !power_allows_input()) {
        app_pc_remote_recenter();
        return;
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_air_mode) {
        if (s_motion_until <= now) stop_motion_locked();
        s_motion_until = now + MOTION_LEASE_US;
    }
    portEXIT_CRITICAL(&s_lock);
}

void app_pc_remote_release_controls(void)
{
    app_pc_remote_set_air_mode(false);
    if (s_bluetooth) (void)aether_bluetooth_hid_cancel_input(s_bluetooth);
}

void app_pc_remote_shutdown(void)
{
    app_pc_remote_release_controls();
    if (s_bluetooth) (void)aether_bluetooth_hid_set_enabled(s_bluetooth, false);
}

esp_err_t app_pc_remote_get_shortcut(unsigned slot, app_pc_shortcut_t *shortcut)
{
    if (slot >= APP_PC_REMOTE_SCENES || !shortcut) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    *shortcut = s_shortcuts.shortcut[slot];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

/* Called with s_lock held so both settings share the same NVS transaction. */
static void queue_settings_save(void)
{
    ++s_save_revision;
    s_save_pending = true;
    s_save_error = ESP_OK;
    s_save_retry_at = 0;
}

esp_err_t app_pc_remote_set_sensitivity(unsigned percent)
{
    if (percent < APP_PC_MOUSE_MIN || percent > APP_PC_MOUSE_MAX) return ESP_ERR_INVALID_ARG;
    if (!power_allows_input()) return ESP_ERR_INVALID_STATE;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_shortcuts.sensitivity != percent) {
        s_shortcuts.sensitivity = percent;
        stop_motion_locked();
        queue_settings_save();
        s_save_retry_at = now + MOUSE_SAVE_DELAY_US;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t app_pc_remote_set_shortcut(unsigned slot, app_pc_shortcut_t shortcut)
{
    if (slot >= APP_PC_REMOTE_SCENES || !supported_key(shortcut.key)) return ESP_ERR_INVALID_ARG;
    if (!power_allows_input()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    s_shortcuts.shortcut[slot] = shortcut;
    queue_settings_save();
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t app_pc_remote_get_scene_name(unsigned slot, char name[APP_PC_REMOTE_SCENE_BYTES])
{
    if (slot >= APP_PC_REMOTE_SCENES || !name) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    memcpy(name, s_shortcuts.scene_names[slot], APP_PC_REMOTE_SCENE_BYTES);
    portEXIT_CRITICAL(&s_lock);
    if (!name[0]) snprintf(name, APP_PC_REMOTE_SCENE_BYTES, "场景 %u", slot + 1);
    return ESP_OK;
}

esp_err_t app_pc_remote_set_scene_name(unsigned slot, const char *name)
{
    char stored[APP_PC_REMOTE_SCENE_BYTES] = {0};
    if (slot >= APP_PC_REMOTE_SCENES || !scene_name_copy(stored, name)) return ESP_ERR_INVALID_ARG;
    if (!power_allows_input()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if (strcmp(s_shortcuts.scene_names[slot], stored)) {
        memcpy(s_shortcuts.scene_names[slot], stored, sizeof(stored));
        queue_settings_save();
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t app_pc_remote_trigger(unsigned slot)
{
    app_pc_shortcut_t shortcut;
    const esp_err_t error = app_pc_remote_get_shortcut(slot, &shortcut);
    return error == ESP_OK ? app_pc_remote_key(shortcut.modifiers, shortcut.key) : error;
}

bool app_pc_remote_save_pending(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool pending = s_save_pending;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}
