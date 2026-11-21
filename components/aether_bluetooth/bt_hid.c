#include "sdkconfig.h"
#if CONFIG_BT_HID_DEVICE_ENABLED
#include "bt_hid.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#define WINDOW_US 60000000LL
#define CONFIRM_US 25000000LL
#define PROFILE_US 3000000LL
#define REPORT_US 16000LL
#define KEY_RELEASE_GAP_US 32000
#define ACK_US 250000LL
#define RECONNECT_PASSIVE_US 1000000LL
#define RECONNECT_DELAY_US 500000LL
#define RECONNECT_TIMEOUT_US 7000000LL
#define RECONNECT_FALLBACK_US 4000000LL
#define RECONNECT_AFTER_LOSS_ATTEMPTS 3
#define REPORT_RETRIES 2
#define BOTH_NEUTRAL ((1U << BT_HID_KEYBOARD) | (1U << BT_HID_MOUSE))

static const char *const TAG = "bt_hid";

static void cancel_locked(bt_hid_t *hid)
{
    hid->read = hid->count = 0;
    hid->motion_pending = false;
    ++hid->generation;
    hid->keyboard_release_gap_us = 0;
    hid->retries = 0;
    if (hid->link && hid->authorized) hid->neutral |= BOTH_NEUTRAL;
}

static void link_closed_locked(bt_hid_t *hid)
{
    cancel_locked(hid);
    hid->link = hid->authorized = hid->status.connected = false;
    hid->sending = hid->disconnecting = hid->reset_link = false;
    hid->neutral = hid->retries = 0;
    hid->report_at = 0;
}

static void stop_locked(bt_hid_t *hid)
{
    /* Keep retiring this profile even if the caller immediately enables a new session. */
    if (!hid->stop_pending) cancel_locked(hid);
    hid->stop_pending = hid->phase != BT_HID_OFF || hid->connectable || hid->io_display ||
                        hid->cod_changed || hid->reply_pending || hid->status.confirm_pending;
    hid->window_until = 0;
    hid->status.discoverable = hid->status.connected = false;
    hid->status.reconnecting = hid->reconnect_in_flight = false;
    hid->candidate_valid = false;
    if (hid->status.confirm_pending || hid->reply_pending) {
        hid->reply_pending = true;
        hid->reply_accepted = false;
    }
    hid->status.confirm_pending = false;
    hid->status.confirm_code = 0;
}

static void disable_locked(bt_hid_t *hid)
{
    stop_locked(hid);
    hid->status.requested = false;
}

static void open_pairing_after_stop_locked(bt_hid_t *hid)
{
    /* An attempted host can remain pinned in Bluedroid until HID deinit. */
    const int64_t deadline = hid->window_until;
    stop_locked(hid);
    hid->window_until = deadline;
    hid->pairing_fallback = true;
}

static void reconnect_locked(bt_hid_t *hid, uint8_t attempts)
{
    hid->status.reconnecting = true;
    hid->reconnect_in_flight = false;
    hid->reconnect_at = 0;
    hid->reconnect_attempts = attempts;
}

void bt_hid_init(bt_hid_t *hid)
{
    *hid = (bt_hid_t){.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED};
}

void bt_hid_request(bt_hid_t *hid, bool enabled)
{
    portENTER_CRITICAL(&hid->lock);
    if (enabled) {
        if (!hid->status.requested) {
            hid->status.last_error = ESP_OK;
            hid->pairing_fallback = false;
        }
        hid->status.requested = true;
    } else {
        disable_locked(hid);
    }
    portEXIT_CRITICAL(&hid->lock);
}

void bt_hid_error(bt_hid_t *hid, esp_err_t error)
{
    if (error == ESP_OK) return;
    portENTER_CRITICAL(&hid->lock);
    hid->status.last_error = error;
    portEXIT_CRITICAL(&hid->lock);
}

esp_err_t bt_hid_confirm(bt_hid_t *hid, bool accepted)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&hid->lock);
    const bool valid = hid->status.requested && hid->status.confirm_pending &&
                       !hid->reply_pending && now < hid->prompt_until && now < hid->window_until;
    if (valid) {
        hid->reply_pending = true;
        hid->reply_accepted = accepted;
        hid->status.confirm_pending = false;
        hid->status.confirm_code = 0;
    }
    portEXIT_CRITICAL(&hid->lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t bt_hid_enqueue(bt_hid_t *hid, const bt_hid_report_t *reports, size_t count)
{
    portENTER_CRITICAL(&hid->lock);
    esp_err_t result = ESP_OK;
    if (!hid->status.requested || !hid->status.connected || hid->reset_link) {
        result = ESP_ERR_INVALID_STATE;
    } else if (count == 1 && reports[0].id == BT_HID_MOUSE && !reports[0].bytes[0]) {
        /* Relative motion is time-sensitive. Keep the newest sample rather
         * than replaying a queue of stale motion after congestion clears. */
        hid->motion = reports[0];
        hid->motion_pending = true;
    } else if (count > BT_HID_QUEUE_MAX - hid->count) {
        result = ESP_ERR_TIMEOUT;
    } else {
        for (size_t i = 0; i < count; ++i) {
            hid->queue[(hid->read + hid->count) % BT_HID_QUEUE_MAX] = reports[i];
            ++hid->count;
        }
    }
    portEXIT_CRITICAL(&hid->lock);
    return result;
}

void bt_hid_cancel(bt_hid_t *hid)
{
    portENTER_CRITICAL(&hid->lock);
    cancel_locked(hid);
    portEXIT_CRITICAL(&hid->lock);
}

void bt_hid_cancel_motion(bt_hid_t *hid)
{
    portENTER_CRITICAL(&hid->lock);
    hid->motion_pending = false;
    portEXIT_CRITICAL(&hid->lock);
}

void bt_hid_status(bt_hid_t *hid, aether_bluetooth_hid_status_t *status)
{
    portENTER_CRITICAL(&hid->lock);
    *status = hid->status;
    portEXIT_CRITICAL(&hid->lock);
}

bool bt_hid_offer_confirmation(bt_hid_t *hid, const uint8_t address[6], uint32_t code)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&hid->lock);
    const bool valid = hid->status.requested && hid->discoverable && !hid->link &&
                       now < hid->window_until && !hid->reply_pending && !hid->candidate_valid &&
                       !hid->status.confirm_pending && code <= 999999U;
    if (valid) {
        memcpy(hid->prompt_address, address, 6);
        hid->prompt_until = now + CONFIRM_US;
        hid->prompt_code = code;
        hid->status.confirm_code = code;
        hid->status.confirm_pending = true;
    }
    /* An identical callback cannot replace the address or reset its deadline. */
    const bool duplicate = (hid->status.confirm_pending || hid->reply_pending) && hid->prompt_code == code &&
                           !memcmp(hid->prompt_address, address, 6);
    portEXIT_CRITICAL(&hid->lock);
    return valid || duplicate;
}

void bt_hid_auth_failed(bt_hid_t *hid, const uint8_t address[6], bool key_rejected)
{
    portENTER_CRITICAL(&hid->lock);
    const bool candidate = hid->candidate_valid && !memcmp(hid->candidate, address, 6);
    const bool rejected_bond = key_rejected && hid->status.requested && !hid->status.connected &&
                               hid->approved_bonded && !memcmp(hid->approved, address, 6);
    if (rejected_bond) hid->approved_valid = hid->approved_bonded = hid->save_approved = false;
    if (candidate || rejected_bond) {
        open_pairing_after_stop_locked(hid);
        hid->status.last_error = ESP_ERR_INVALID_RESPONSE;
    }
    portEXIT_CRITICAL(&hid->lock);
}

static esp_err_t sdk_status(esp_hidd_status_t status)
{
    if (status == ESP_HIDD_SUCCESS) return ESP_OK;
    if (status == ESP_HIDD_NO_RES) return ESP_ERR_NO_MEM;
    if (status == ESP_HIDD_BUSY) return ESP_ERR_TIMEOUT;
    return ESP_FAIL;
}

static void reconnect_failed_locked(bt_hid_t *hid, int64_t now, esp_err_t error)
{
    hid->status.last_error = error;
    hid->reconnect_in_flight = false;
    hid->reconnect_at = now + (hid->reconnect_attempts ? RECONNECT_DELAY_US : RECONNECT_FALLBACK_US);
}

static bool report_retryable(const struct hidd_send_report_evt_param *report)
{
    if (report->status == ESP_HIDD_BUSY || report->status == ESP_HIDD_NO_RES) return true;
    /* The callback's documented Bluedroid hiddefs.h reason codes. These are
     * returned before L2CAP accepts the data, so retrying cannot duplicate it. */
    enum { HID_REASON_NO_RESOURCES = 3, HID_REASON_CONGESTED = 8 };
    return report->status == ESP_HIDD_ERROR &&
           (report->reason == HID_REASON_NO_RESOURCES || report->reason == HID_REASON_CONGESTED);
}

static void report_done_locked(bt_hid_t *hid, esp_err_t result, bool retryable,
                               bool rejected, int64_t now)
{
    const bt_hid_report_t *report = &hid->sending_report;
    const bt_hid_send_t kind = hid->sending_kind;
    hid->sending = false;
    hid->report_at = now + REPORT_US;

    /* A full cancellation replaces the queue and schedules both releases.
     * A callback already in flight may never consume a newer action. */
    if (hid->sending_generation != hid->generation) return;
    if (result != ESP_OK && kind != BT_HID_SEND_MOTION &&
        (retryable || kind == BT_HID_SEND_NEUTRAL) && hid->retries < REPORT_RETRIES) {
        ++hid->retries;
        return;
    }
    hid->retries = 0;
    if (kind == BT_HID_SEND_ACTION) {
        hid->read = (hid->read + 1) % BT_HID_QUEUE_MAX;
        --hid->count;
        if ((result == ESP_OK || !rejected) && bt_hid_needs_release(report)) {
            hid->neutral |= 1U << report->id;
            if (result == ESP_OK && report->hold_ms) {
                const int64_t hold_us = (int64_t)report->hold_ms * 1000;
                if (hold_us > REPORT_US) hid->report_at = now + hold_us;
                if (report->id == BT_HID_KEYBOARD) hid->keyboard_release_gap_us = KEY_RELEASE_GAP_US;
            }
        }
    } else if (kind == BT_HID_SEND_NEUTRAL && result == ESP_OK) {
        hid->neutral &= (uint8_t)~(1U << report->id);
        if (report->id == BT_HID_KEYBOARD && hid->keyboard_release_gap_us) {
            hid->report_at = now + hid->keyboard_release_gap_us;
            hid->keyboard_release_gap_us = 0;
        }
    } else if (kind == BT_HID_SEND_NEUTRAL) {
        /* A release is safe to retry, but never leave a key held indefinitely. */
        hid->reset_link = true;
    }
    if (result != ESP_OK) hid->status.last_error = result;
}

void bt_hid_event(bt_hid_t *hid, esp_hidd_cb_event_t event, const esp_hidd_cb_param_t *data)
{
    const int64_t now = esp_timer_get_time();
    if (event == ESP_HIDD_REGISTER_APP_EVT) {
        ESP_LOGI(TAG, "HID registration status=%d", data->register_app.status);
    } else if (event == ESP_HIDD_OPEN_EVT) {
        ESP_LOGI(TAG, "HID open status=%d connection=%d", data->open.status, data->open.conn_status);
    } else if (event == ESP_HIDD_CLOSE_EVT) {
        ESP_LOGI(TAG, "HID close status=%d connection=%d", data->close.status, data->close.conn_status);
    }
    portENTER_CRITICAL(&hid->lock);
    if (event == ESP_HIDD_INIT_EVT && hid->phase == BT_HID_INITIALIZING) {
        const esp_err_t result = sdk_status(data->init.status);
        hid->phase = result == ESP_OK ? BT_HID_INITIALIZED : BT_HID_OFF;
        if (result != ESP_OK && !hid->stop_pending) {
            disable_locked(hid);
            hid->status.last_error = result;
        }
    } else if (event == ESP_HIDD_REGISTER_APP_EVT && hid->phase == BT_HID_REGISTERING) {
        const esp_err_t result = sdk_status(data->register_app.status);
        hid->phase = result == ESP_OK ? BT_HID_REGISTERED : BT_HID_INITIALIZED;
        if (!hid->stop_pending) {
            if (result == ESP_OK) {
                hid->status.last_error = ESP_OK;
                if (hid->approved_bonded && !hid->pairing_fallback) {
                    reconnect_locked(hid, 1);
                } else if (!hid->window_until) {
                    hid->window_until = now + WINDOW_US;
                }
            } else {
                disable_locked(hid);
                hid->status.last_error = result;
            }
        }
    } else if (event == ESP_HIDD_UNREGISTER_APP_EVT && hid->phase == BT_HID_UNREGISTERING) {
        if (data->unregister_app.status == ESP_HIDD_SUCCESS || data->unregister_app.status == ESP_HIDD_NEED_REG) {
            hid->phase = BT_HID_INITIALIZED;
            link_closed_locked(hid);
        } else if (data->unregister_app.status == ESP_HIDD_NEED_INIT) {
            hid->phase = BT_HID_OFF;
            link_closed_locked(hid);
        } else {
            hid->status.last_error = sdk_status(data->unregister_app.status);
            hid->phase = BT_HID_REGISTERED;
            hid->profile_retry_at = now + 100000;
        }
    } else if (event == ESP_HIDD_DEINIT_EVT && hid->phase == BT_HID_DEINITIALIZING) {
        if (data->deinit.status == ESP_HIDD_SUCCESS || data->deinit.status == ESP_HIDD_NEED_INIT) {
            hid->phase = BT_HID_OFF;
            link_closed_locked(hid);
        } else {
            hid->status.last_error = sdk_status(data->deinit.status);
            /* Allow the next service pass to retry a rejected deinit. */
            hid->phase = BT_HID_INITIALIZED;
            hid->profile_retry_at = now + 100000;
        }
    } else if (event == ESP_HIDD_OPEN_EVT && hid->phase == BT_HID_REGISTERED && !hid->stop_pending &&
               data->open.status == ESP_HIDD_SUCCESS &&
               data->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED) {
        if (hid->link && !memcmp(hid->link_address, data->open.bd_addr, 6)) {
            portEXIT_CRITICAL(&hid->lock);
            return;
        }
        const bool new_peer = hid->candidate_valid && now < hid->candidate_until &&
                              !memcmp(hid->candidate, data->open.bd_addr, 6);
        const bool known = hid->approved_bonded && !memcmp(hid->approved, data->open.bd_addr, 6);
        const bool pairing = now < hid->window_until;
        cancel_locked(hid);
        hid->link = true;
        memcpy(hid->link_address, data->open.bd_addr, 6);
        hid->authorized = hid->status.requested && ((pairing && new_peer) ||
                          (known && (pairing || hid->status.reconnecting)));
        hid->status.connected = hid->authorized;
        if (hid->authorized) {
            hid->window_until = 0;
            hid->status.reconnecting = hid->reconnect_in_flight = false;
            hid->neutral = BOTH_NEUTRAL;
            hid->status.last_error = ESP_OK;
            if (new_peer) {
                memcpy(hid->approved, data->open.bd_addr, 6);
                hid->approved_valid = hid->approved_bonded = hid->save_approved = true;
            }
        } else {
            hid->reset_link = true;
            hid->status.last_error = ESP_ERR_INVALID_STATE;
        }
        hid->candidate_valid = false;
    } else if (event == ESP_HIDD_OPEN_EVT && hid->reconnect_in_flight &&
               data->open.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED &&
               !memcmp(hid->approved, data->open.bd_addr, 6)) {
        const esp_err_t result = sdk_status(data->open.status);
        reconnect_failed_locked(hid, now, result == ESP_OK ? ESP_FAIL : result);
    } else if (event == ESP_HIDD_CLOSE_EVT && hid->link &&
               (data->close.status == ESP_HIDD_NO_CONNECTION ||
                (data->close.status == ESP_HIDD_SUCCESS &&
                 data->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED))) {
        const bool authorized_link = hid->authorized;
        const bool input_failed = hid->reset_link;
        link_closed_locked(hid);
        if (hid->status.requested && !hid->stop_pending) {
            if (!authorized_link && !hid->status.reconnecting) {
                open_pairing_after_stop_locked(hid);
            } else if (authorized_link && input_failed) {
                disable_locked(hid);
            } else if (authorized_link) {
                reconnect_locked(hid, RECONNECT_AFTER_LOSS_ATTEMPTS);
            }
        }
    } else if (event == ESP_HIDD_VC_UNPLUG_EVT && data->vc_unplug.status == ESP_HIDD_SUCCESS &&
               data->vc_unplug.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED &&
               hid->phase == BT_HID_REGISTERED && hid->status.requested && !hid->stop_pending) {
        if (!memcmp(hid->approved, hid->link_address, 6)) {
            hid->approved_valid = hid->approved_bonded = hid->save_approved = false;
        }
        link_closed_locked(hid);
        open_pairing_after_stop_locked(hid);
    } else if (event == ESP_HIDD_CLOSE_EVT && data->close.status != ESP_HIDD_SUCCESS) {
        hid->disconnecting = false;
    } else if (event == ESP_HIDD_SEND_REPORT_EVT && hid->sending) {
        const struct hidd_send_report_evt_param *report = &data->send_report;
        /* BTC preflight can reject before it copies the report ID/type. Zero
         * in an unsuccessful OTHER report is that rejected submission. */
        const bool preflight_rejected = report->report_id == 0 && report->reason == 0 &&
            report->report_type == ESP_HIDD_REPORT_TYPE_OTHER && report->status != ESP_HIDD_SUCCESS;
        if (report->report_id == hid->sending_report.id || preflight_rejected) {
            const bool retryable = report_retryable(report);
            report_done_locked(hid, sdk_status(report->status), retryable,
                               retryable || preflight_rejected, now);
        }
    }
    portEXIT_CRITICAL(&hid->lock);

    if (event == ESP_HIDD_GET_REPORT_EVT) {
        esp_bt_hid_device_report_error(ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ);
    }
}

static void load_approved(bt_hid_t *hid)
{
    nvs_handle_t nvs;
    uint8_t address[6];
    size_t length = sizeof(address);
    bool found = false;
    if (nvs_open("pc_hid", NVS_READONLY, &nvs) == ESP_OK) {
        found = nvs_get_blob(nvs, "approved", address, &length) == ESP_OK && length == sizeof(address);
        nvs_close(nvs);
    }
    portENTER_CRITICAL(&hid->lock);
    hid->approved_loaded = true;
    if (found) {
        memcpy(hid->approved, address, 6);
        hid->approved_valid = true;
    }
    portEXIT_CRITICAL(&hid->lock);
}

static void save_approved(bt_hid_t *hid, const uint8_t address[6])
{
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("pc_hid", NVS_READWRITE, &nvs);
    if (result == ESP_OK) {
        result = nvs_set_blob(nvs, "approved", address, 6);
        if (result == ESP_OK) result = nvs_commit(nvs);
        nvs_close(nvs);
    }
    bt_hid_error(hid, result);
}

static bool peer_is_bonded(const uint8_t address[6])
{
    esp_bd_addr_t peers[CONFIG_BT_SMP_MAX_BONDS];
    int count = CONFIG_BT_SMP_MAX_BONDS;
    if (esp_bt_gap_get_bond_device_list(&count, peers) != ESP_OK) return false;
    for (int i = 0; i < count; ++i) {
        if (!memcmp(address, peers[i], sizeof(peers[i]))) return true;
    }
    return false;
}

static void check_approved_bond(bt_hid_t *hid)
{
    uint8_t approved[6];
    portENTER_CRITICAL(&hid->lock);
    const bool approved_valid = hid->approved_valid;
    memcpy(approved, hid->approved, sizeof(approved));
    portEXIT_CRITICAL(&hid->lock);
    const bool bonded = approved_valid && peer_is_bonded(approved);
    portENTER_CRITICAL(&hid->lock);
    hid->approved_bonded = bonded;
    portEXIT_CRITICAL(&hid->lock);
}

static void service_reconnect(bt_hid_t *hid, int64_t now, bool audio_busy)
{
    uint8_t address[6];
    portENTER_CRITICAL(&hid->lock);
    const bool timed_out = hid->reconnect_in_flight && now >= hid->reconnect_until;
    if (timed_out) {
        /* The stack can still be connecting. Retire that profile before waiting for a new host. */
        hid->status.last_error = ESP_ERR_TIMEOUT;
        open_pairing_after_stop_locked(hid);
    }
    const bool waiting = hid->phase == BT_HID_REGISTERED && hid->status.reconnecting &&
                         !hid->reconnect_in_flight && !hid->link;
    /* Initial grace starts once the owner successfully submits page scan. */
    if (waiting && hid->connectable && !hid->reconnect_at) hid->reconnect_at = now + RECONNECT_PASSIVE_US;
    const bool ready = waiting && hid->reconnect_at && now >= hid->reconnect_at;
    if (ready && !hid->reconnect_attempts) open_pairing_after_stop_locked(hid);
    const bool attempt = ready && hid->reconnect_attempts && hid->connectable && !audio_busy;
    if (attempt) {
        memcpy(address, hid->approved, sizeof(address));
        --hid->reconnect_attempts;
        hid->reconnect_in_flight = true;
        hid->reconnect_until = now + RECONNECT_TIMEOUT_US;
    }
    portEXIT_CRITICAL(&hid->lock);
    if (timed_out) ESP_LOGW(TAG, "HID reconnect timed out");
    if (!attempt) return;

    const bool bonded = peer_is_bonded(address);
    portENTER_CRITICAL(&hid->lock);
    const bool still_pending = hid->reconnect_in_flight;
    if (still_pending && !bonded) {
        hid->approved_bonded = false;
        hid->status.last_error = ESP_ERR_NOT_FOUND;
        open_pairing_after_stop_locked(hid);
    }
    portEXIT_CRITICAL(&hid->lock);
    if (!still_pending || !bonded) return;

    const esp_err_t result = esp_bt_hid_device_connect(address);
    ESP_LOGI(TAG, "HID reconnect submit status=%d", result);
    if (result != ESP_OK) {
        portENTER_CRITICAL(&hid->lock);
        if (hid->reconnect_in_flight) reconnect_failed_locked(hid, now, result);
        portEXIT_CRITICAL(&hid->lock);
    }
}

static void service_scan(bt_hid_t *hid, int64_t now, bool audio_busy)
{
    portENTER_CRITICAL(&hid->lock);
    const bool available = hid->status.requested && !hid->stop_pending &&
                           hid->phase == BT_HID_REGISTERED && !hid->link;
    const bool discoverable = available && !hid->status.reconnecting && !audio_busy && now < hid->window_until;
    const bool connectable = discoverable || (available && hid->status.reconnecting &&
                                              (!audio_busy || hid->reconnect_in_flight));
    const bool change_scan = hid->connectable != connectable || hid->discoverable != discoverable;
    const bool change_io = hid->io_display != connectable;
    const bool set_cod = connectable && !hid->cod_changed;
    const bool restore_cod = hid->stop_pending && hid->cod_changed;
    const esp_bt_cod_t saved_cod = hid->original_cod;
    const bool retry = now >= hid->scan_retry_at;
    portEXIT_CRITICAL(&hid->lock);
    if (!retry || (!change_scan && !change_io && !set_cod && !restore_cod)) return;
    esp_err_t result;

    /* Close the inbound window before restoring the audio pairing IO policy. */
    if (change_scan && !connectable) {
        result = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        if (result != ESP_OK) goto failure;
        portENTER_CRITICAL(&hid->lock);
        hid->connectable = hid->discoverable = hid->status.discoverable = false;
        portEXIT_CRITICAL(&hid->lock);
    }
    if (set_cod) {
        esp_bt_cod_t original;
        result = esp_bt_gap_get_cod(&original);
        if (result != ESP_OK) goto failure;
        const esp_bt_cod_t peripheral = {.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL,
                                         .minor = ESP_HID_CLASS_COM >> 2};
        result = esp_bt_gap_set_cod(peripheral, ESP_BT_SET_COD_MAJOR_MINOR);
        if (result != ESP_OK) goto failure;
        portENTER_CRITICAL(&hid->lock);
        hid->original_cod = original;
        hid->cod_changed = true;
        portEXIT_CRITICAL(&hid->lock);
    } else if (restore_cod) {
        result = esp_bt_gap_set_cod(saved_cod, ESP_BT_SET_COD_MAJOR_MINOR);
        if (result != ESP_OK) goto failure;
        portENTER_CRITICAL(&hid->lock);
        hid->cod_changed = false;
        portEXIT_CRITICAL(&hid->lock);
    }
    if (change_io) {
        esp_bt_io_cap_t cap = connectable ? ESP_BT_IO_CAP_IO : ESP_BT_IO_CAP_NONE;
        result = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &cap, sizeof(cap));
        if (result != ESP_OK) goto failure;
        portENTER_CRITICAL(&hid->lock);
        hid->io_display = connectable;
        portEXIT_CRITICAL(&hid->lock);
    }
    if (change_scan && connectable) {
        result = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                  discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
        if (result != ESP_OK) goto failure;
        if (!discoverable) ESP_LOGI(TAG, "HID paired reconnect available");
    }
    portENTER_CRITICAL(&hid->lock);
    hid->connectable = connectable;
    hid->discoverable = discoverable;
    hid->status.discoverable = discoverable && hid->status.requested && !hid->stop_pending;
    portEXIT_CRITICAL(&hid->lock);
    return;

failure:
    portENTER_CRITICAL(&hid->lock);
    hid->status.last_error = result;
    hid->scan_retry_at = now + 100000;
    portEXIT_CRITICAL(&hid->lock);
}

static void service_pairing(bt_hid_t *hid, int64_t now)
{
    uint8_t address[6];
    bool accept = false;
    bool send = false;
    portENTER_CRITICAL(&hid->lock);
    const bool window_expired = hid->status.requested && !hid->status.connected &&
                                hid->window_until > 0 && now >= hid->window_until;
    if (window_expired) {
        disable_locked(hid);
        hid->status.last_error = ESP_ERR_TIMEOUT;
    }
    if (hid->candidate_valid && now >= hid->candidate_until) open_pairing_after_stop_locked(hid);
    if (hid->status.confirm_pending && (now >= hid->prompt_until || now >= hid->window_until)) {
        hid->reply_pending = true;
        hid->reply_accepted = false;
        hid->status.confirm_pending = false;
        hid->status.confirm_code = 0;
        hid->status.last_error = ESP_ERR_TIMEOUT;
    }
    if (hid->reply_pending) {
        send = true;
        memcpy(address, hid->prompt_address, sizeof(address));
        accept = hid->reply_accepted && hid->status.requested &&
                 now < hid->prompt_until && now < hid->window_until;
        hid->reply_pending = false;
        if (accept) {
            memcpy(hid->candidate, address, sizeof(address));
            hid->candidate_valid = true;
            hid->candidate_until = now + CONFIRM_US;
            /* Pairing accepted near the end of discovery gets time to finish. */
            if (hid->window_until < hid->candidate_until) hid->window_until = hid->candidate_until;
        }
    }
    portEXIT_CRITICAL(&hid->lock);
    if (send) {
        const esp_err_t result = esp_bt_gap_ssp_confirm_reply(address, accept);
        if (!accept) {
            portENTER_CRITICAL(&hid->lock);
            if (hid->status.requested && !hid->stop_pending && !hid->link) {
                open_pairing_after_stop_locked(hid);
            }
            portEXIT_CRITICAL(&hid->lock);
        }
        if (result != ESP_OK && !window_expired) {
            bt_hid_auth_failed(hid, address, false);
            bt_hid_error(hid, result);
        }
    }
}

static void service_reports(bt_hid_t *hid, int64_t now)
{
    bt_hid_report_t report = {0};
    portENTER_CRITICAL(&hid->lock);
    if (hid->sending && now >= hid->report_until) {
        /* An unacknowledged report may already have reached the host. Close
         * the link instead of replaying a press or guessing at stale ACKs. */
        cancel_locked(hid);
        hid->sending = false;
        hid->reset_link = true;
        hid->status.last_error = ESP_ERR_TIMEOUT;
    }
    if (hid->phase == BT_HID_REGISTERED && hid->link && hid->authorized &&
        !hid->sending && !hid->reset_link && now >= hid->report_at) {
        bt_hid_send_t kind = BT_HID_SEND_NEUTRAL;
        if (hid->neutral) {
            report.id = (hid->neutral & (1U << BT_HID_KEYBOARD)) ? BT_HID_KEYBOARD : BT_HID_MOUSE;
        } else if (!hid->stop_pending && hid->status.requested && hid->count) {
            report = hid->queue[hid->read];
            /* Keep the head until the send result; later taps retain their
             * order if a definite rejection needs a retry. */
            kind = BT_HID_SEND_ACTION;
        } else if (!hid->stop_pending && hid->status.requested && hid->motion_pending) {
            report = hid->motion;
            hid->motion_pending = false;
            kind = BT_HID_SEND_MOTION;
        }
        if (report.id) {
            hid->sending = true;
            hid->sending_kind = kind;
            hid->sending_generation = hid->generation;
            hid->sending_report = report;
            hid->report_until = now + ACK_US;
        }
    }
    portEXIT_CRITICAL(&hid->lock);
    if (!report.id) return;
    const esp_err_t result = esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA,
                                    report.id, bt_hid_report_size(report.id), report.bytes);
    if (result != ESP_OK) {
        portENTER_CRITICAL(&hid->lock);
        if (hid->sending && hid->sending_report.id == report.id) {
            /* A failure returned directly by esp_bt_hid_device_send_report
             * means BTC did not queue the request and no callback will follow. */
            const bool retryable = result == ESP_FAIL || result == ESP_ERR_NO_MEM ||
                                   result == ESP_ERR_TIMEOUT;
            report_done_locked(hid, result, retryable, true, now);
        }
        portEXIT_CRITICAL(&hid->lock);
    }
}

static bool quiesced_locked(const bt_hid_t *hid)
{
    return hid->phase == BT_HID_OFF && !hid->connectable && !hid->io_display &&
           !hid->cod_changed && !hid->reply_pending;
}

void bt_hid_service(bt_hid_t *hid, esp_hd_cb_t callback, bool audio_busy)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&hid->lock);
    const bool load = hid->status.requested && !hid->approved_loaded;
    const bool save = hid->save_approved;
    uint8_t approved[6];
    memcpy(approved, hid->approved, sizeof(approved));
    hid->save_approved = false;
    portEXIT_CRITICAL(&hid->lock);
    if (load) load_approved(hid);
    if (save) save_approved(hid, approved);

    service_pairing(hid, now);
    service_scan(hid, now, audio_busy);
    service_reconnect(hid, now, audio_busy);
    service_reports(hid, now);

    portENTER_CRITICAL(&hid->lock);
    if (hid->stop_pending && quiesced_locked(hid)) hid->stop_pending = false;
    if (hid->disconnecting && now >= hid->disconnect_until) {
        hid->disconnecting = false;
        if (!hid->stop_pending) {
            disable_locked(hid);
            hid->status.last_error = ESP_ERR_TIMEOUT;
        }
    }
    const bt_hid_phase_t previous = hid->phase;
    bt_hid_phase_t action = previous;
    const bool on = hid->status.requested && !hid->stop_pending;
    if (on && previous == BT_HID_OFF) action = BT_HID_INITIALIZING;
    else if (on && previous == BT_HID_INITIALIZED) action = BT_HID_REGISTERING;
    else if (hid->stop_pending && previous == BT_HID_REGISTERED &&
             (!hid->link || !hid->authorized || hid->reset_link || (!hid->neutral && !hid->sending))) {
        action = BT_HID_UNREGISTERING;
    } else if (hid->stop_pending && previous == BT_HID_INITIALIZED) {
        action = BT_HID_DEINITIALIZING;
    }
    if (action != previous) {
        if (now < hid->profile_retry_at) {
            portEXIT_CRITICAL(&hid->lock);
            return;
        }
        hid->phase = action;
        hid->phase_until = now + PROFILE_US;
    } else if ((previous == BT_HID_INITIALIZING || previous == BT_HID_REGISTERING ||
                previous == BT_HID_UNREGISTERING || previous == BT_HID_DEINITIALIZING) &&
               hid->phase_until && now >= hid->phase_until) {
        /* Keep waiting for the real release callback. The host shutdown
         * path may clear this state once Bluedroid is disabled. */
        if (!hid->stop_pending) disable_locked(hid);
        hid->phase_until = 0;
        hid->status.last_error = ESP_ERR_TIMEOUT;
    }
    const bool disconnect = on && action == BT_HID_REGISTERED && hid->reset_link && !hid->disconnecting;
    if (disconnect) {
        hid->disconnecting = true;
        hid->disconnect_until = now + PROFILE_US;
        hid->status.connected = false;
    }
    portEXIT_CRITICAL(&hid->lock);

    esp_err_t result = ESP_OK;
    if (action != previous) {
        if (action == BT_HID_INITIALIZING) {
            result = esp_bt_hid_device_register_callback(callback);
            if (result == ESP_OK) result = esp_bt_hid_device_init();
        } else if (action == BT_HID_REGISTERING) {
            check_approved_bond(hid);
            esp_hidd_app_param_t config = {.name = "Aether01 PC Remote", .description = "Keyboard and mouse",
                .provider = "Aether01", .subclass = ESP_HID_CLASS_COM, .desc_list = bt_hid_descriptor,
                .desc_list_len = bt_hid_descriptor_size};
            esp_hidd_qos_param_t qos = {.service_type = 1};
            result = esp_bt_hid_device_register_app(&config, &qos, &qos);
        } else if (action == BT_HID_UNREGISTERING) {
            /* ESP-IDF 6.1 closes the active link during unregister. Doing it
             * inside deinit can let CLOSE_EVT overwrite BTC_HD_DISABLING,
             * which prevents the SDK from completing deinit. */
            result = esp_bt_hid_device_unregister_app();
        } else {
            result = esp_bt_hid_device_deinit();
        }
        if (result != ESP_OK) {
            portENTER_CRITICAL(&hid->lock);
            if (hid->phase == action) hid->phase = previous;
            if (!hid->stop_pending) disable_locked(hid);
            hid->status.last_error = result;
            hid->profile_retry_at = now + 100000;
            portEXIT_CRITICAL(&hid->lock);
        }
    } else if (disconnect) {
        result = esp_bt_hid_device_disconnect();
        if (result != ESP_OK) bt_hid_error(hid, result);
    }
}

bool bt_hid_quiesced(bt_hid_t *hid)
{
    portENTER_CRITICAL(&hid->lock);
    const bool stopped = quiesced_locked(hid);
    portEXIT_CRITICAL(&hid->lock);
    return stopped;
}

void bt_hid_host_disabled(bt_hid_t *hid)
{
    portENTER_CRITICAL(&hid->lock);
    hid->phase = BT_HID_OFF;
    hid->stop_pending = false;
    hid->window_until = 0;
    hid->connectable = hid->discoverable = hid->io_display = hid->cod_changed = hid->reply_pending = false;
    hid->scan_retry_at = hid->profile_retry_at = 0;
    hid->status.discoverable = hid->status.reconnecting = hid->reconnect_in_flight = false;
    link_closed_locked(hid);
    portEXIT_CRITICAL(&hid->lock);
}
#endif
