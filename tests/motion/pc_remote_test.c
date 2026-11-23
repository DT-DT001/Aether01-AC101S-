#include "app_pc_remote.h"
#include "app_telemetry.h"
#include "nvs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static aether_bluetooth_handle_t bluetooth = (aether_bluetooth_handle_t)1;
static app_power_t power;
static app_power_status_t power_status;
static app_telemetry_snapshot_t imu;
static aether_bluetooth_hid_status_t hid;
static void (*consume)(const app_telemetry_snapshot_t *);
static int64_t now = 1000000;
static uint32_t interest;
static unsigned moves, keys, cancels, writes, queued_clicks;
static app_pc_shortcut_t last_key;
static bool busy, reject_commit, motion_queued;
static uint8_t stored[32], staging[32];
static size_t stored_size, staging_size;

int64_t esp_timer_get_time(void) { return now; }
static bool flash_busy(void *unused) { (void)unused; return busy; }
esp_err_t app_power_get_status(const app_power_t *handle, app_power_status_t *status)
{ assert(handle == &power); *status = power_status; return ESP_OK; }
void app_telemetry_get(app_telemetry_snapshot_t *sample) { *sample = imu; }
void app_telemetry_set_interest(uint32_t mask) { interest = mask; }
void app_telemetry_set_consumer(void (*consumer)(const app_telemetry_snapshot_t *))
{ consume = consumer; }

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *handle)
{ assert(!strcmp(ns, "pc_remote")); *handle = mode; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size)
{
    assert(handle == NVS_READONLY && !strcmp(key, "shortcuts"));
    if (!stored_size) return ESP_ERR_NOT_FOUND;
    assert(*size >= stored_size);
    memcpy(data, stored, stored_size);
    *size = stored_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == NVS_READWRITE && !strcmp(key, "shortcuts") && size <= sizeof(staging));
    memcpy(staging, data, size);
    staging_size = size;
    ++writes;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == NVS_READWRITE);
    if (reject_commit) return ESP_FAIL;
    memcpy(stored, staging, staging_size);
    stored_size = staging_size;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t aether_bluetooth_hid_get_status(aether_bluetooth_handle_t handle,
                                         aether_bluetooth_hid_status_t *status)
{ assert(handle == bluetooth); *status = hid; return ESP_OK; }
esp_err_t aether_bluetooth_hid_set_enabled(aether_bluetooth_handle_t handle, bool on)
{
    assert(handle == bluetooth);
    hid.requested = on;
    if (!on) {
        hid.connected = false;
        queued_clicks = 0;
        motion_queued = false;
    }
    return ESP_OK;
}
esp_err_t aether_bluetooth_hid_confirm(aether_bluetooth_handle_t handle, bool accept)
{ assert(handle == bluetooth); return accept && hid.confirm_pending ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t aether_bluetooth_hid_key(aether_bluetooth_handle_t handle, uint8_t modifiers, uint8_t key)
{
    assert(handle == bluetooth);
    if (!hid.connected) return ESP_ERR_INVALID_STATE;
    last_key = (app_pc_shortcut_t){modifiers, key};
    ++keys;
    return ESP_OK;
}
esp_err_t aether_bluetooth_hid_text(aether_bluetooth_handle_t handle, const char *text)
{ assert(handle == bluetooth && text); return hid.connected ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t aether_bluetooth_hid_click(aether_bluetooth_handle_t handle, uint8_t buttons)
{
    assert(handle == bluetooth && buttons);
    if (!hid.connected) return ESP_ERR_INVALID_STATE;
    ++queued_clicks;
    return ESP_OK;
}
esp_err_t aether_bluetooth_hid_move(aether_bluetooth_handle_t handle, int8_t dx, int8_t dy)
{
    assert(handle == bluetooth && dx < 0 && dy == 0);
    ++moves;
    motion_queued = true;
    return ESP_OK;
}
esp_err_t aether_bluetooth_hid_cancel_input(aether_bluetooth_handle_t handle)
{
    assert(handle == bluetooth);
    queued_clicks = 0;
    motion_queued = false;
    return ESP_OK;
}
esp_err_t aether_bluetooth_hid_cancel_motion(aether_bluetooth_handle_t handle)
{
    assert(handle == bluetooth);
    ++cancels;
    motion_queued = false;
    return ESP_OK;
}

static void sample_after(unsigned ms)
{
    now += (int64_t)ms * 1000;
    imu.imu_updated_ms = (uint32_t)(now / 1000);
    consume(&imu);
}

int main(void)
{
    app_pc_remote_init(bluetooth, &power, true, flash_busy);
    assert(consume);
    for (unsigned slot = 0; slot < 4; ++slot) {
        app_pc_shortcut_t key;
        assert(app_pc_remote_get_shortcut(slot, &key) == ESP_OK);
        assert(key.modifiers == 0x05 && key.key == 0x42 + slot);
    }
    assert(app_pc_remote_trigger(0) == ESP_ERR_INVALID_STATE && keys == 0);
    assert(app_pc_remote_set_enabled(true) == ESP_OK);
    hid.connected = true;
    const app_pc_shortcut_t replacement = {0x03, 0x11};
    assert(app_pc_remote_set_shortcut(2, replacement) == ESP_OK && app_pc_remote_save_pending());
    busy = true;
    consume(NULL);
    assert(writes == 0 && app_pc_remote_save_pending());
    busy = false;
    reject_commit = true;
    consume(NULL);
    app_pc_remote_status_t status;
    app_pc_remote_get_status(&status);
    assert(status.save_error == ESP_FAIL && !status.save_pending);
    reject_commit = false;
    assert(app_pc_remote_set_shortcut(2, replacement) == ESP_OK);
    consume(NULL);
    assert(writes == 2 && stored_size && !app_pc_remote_save_pending());
    app_pc_remote_init(bluetooth, &power, true, flash_busy);
    assert(app_pc_remote_trigger(2) == ESP_OK && keys == 1);
    assert(last_key.modifiers == replacement.modifiers && last_key.key == replacement.key);
    power_status.sleeping = true;
    assert(app_pc_remote_trigger(2) == ESP_ERR_INVALID_STATE && keys == 1);
    power_status.sleeping = false;
    puts("PASS OBS shortcut defaults, persistence guard, error/retry, reload and input gating");

    app_pc_remote_set_air_mode(true);
    assert(interest == APP_TELEMETRY_IMU);
    app_pc_remote_set_motion(true);
    imu.imu.accel_mps2[1] = 9.80665f;
    imu.imu.gyro_dps[1] = 40;
    sample_after(20);
    sample_after(20);
    assert(moves == 0);
    imu.imu.gyro_bias_valid = true;
    sample_after(20);
    sample_after(20);
    assert(moves == 1 && motion_queued);
    assert(app_pc_remote_click(1) == ESP_OK && queued_clicks == 1);
    const unsigned cancelled = cancels;
    sample_after(200);
    assert(moves == 1 && cancels == cancelled + 1 && !motion_queued && queued_clicks == 1);
    app_pc_remote_set_motion(true);
    imu.imu.gyro_stationary = true;
    sample_after(20);
    sample_after(20);
    assert(moves == 1);
    app_pc_remote_set_motion(false);
    app_pc_remote_recenter();
    assert(!motion_queued && queued_clicks == 1);
    imu.imu.gyro_stationary = false;
    app_pc_remote_set_motion(true);
    sample_after(20);
    sample_after(20);
    assert(moves == 2 && motion_queued);
    imu.gyro_calibrating = true;
    app_pc_remote_get_status(&status);
    assert(!status.gyro_calibrated);
    sample_after(20);
    assert(moves == 2 && !motion_queued && queued_clicks == 1);
    imu.gyro_calibrating = false;
    sample_after(20);
    assert(moves == 2 && !motion_queued);
    app_pc_remote_leave();
    assert(interest == 0 && !hid.requested && !hid.connected && !queued_clicks && !motion_queued);
    power_status.shutdown_state = APP_POWER_SHUTDOWN_PREPARING;
    app_pc_remote_set_air_mode(true);
    assert(interest == 0);
    power_status.shutdown_state = APP_POWER_SHUTDOWN_IDLE;
    assert(app_pc_remote_set_enabled(true) == ESP_OK);
    hid.reconnecting = true;
    app_pc_remote_get_status(&status);
    assert(status.requested && status.reconnecting && !status.connected);
    hid.reconnecting = false;
    hid.connected = true;
    app_pc_remote_set_air_mode(true);
    sample_after(20);
    sample_after(20);
    assert(moves == 2 && !motion_queued);
    app_pc_remote_set_motion(true);
    sample_after(20);
    sample_after(20);
    assert(moves == 3 && motion_queued);
    app_pc_remote_leave();
    puts("PASS air-mouse calibration, pending-motion cancellation, hold timeout and fresh input after reopening");
    return 0;
}
