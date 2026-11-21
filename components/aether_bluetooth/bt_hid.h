#pragma once

#include "aether_bluetooth.h"
#include "bt_hid_reports.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd_api.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    BT_HID_OFF, BT_HID_INITIALIZING, BT_HID_INITIALIZED,
    BT_HID_REGISTERING, BT_HID_REGISTERED, BT_HID_UNREGISTERING, BT_HID_DEINITIALIZING,
} bt_hid_phase_t;

typedef enum { BT_HID_SEND_NEUTRAL, BT_HID_SEND_ACTION, BT_HID_SEND_MOTION } bt_hid_send_t;

typedef struct {
    portMUX_TYPE lock;
    aether_bluetooth_hid_status_t status;
    bt_hid_phase_t phase;
    bool connectable, discoverable, io_display, cod_changed, link, authorized, reset_link, disconnecting;
    bool reply_pending, reply_accepted, approved_loaded, save_approved;
    bool approved_valid, candidate_valid, sending, motion_pending, stop_pending;
    bool approved_bonded, pairing_fallback, reconnect_in_flight;
    uint8_t prompt_address[6], candidate[6], approved[6], link_address[6];
    esp_bt_cod_t original_cod;
    int64_t window_until, prompt_until, candidate_until, phase_until, disconnect_until;
    int64_t scan_retry_at, profile_retry_at;
    int64_t reconnect_at, reconnect_until;
    int64_t report_at, report_until;
    uint32_t generation, sending_generation, prompt_code;
    uint8_t neutral, retries;
    uint8_t reconnect_attempts;
    uint16_t keyboard_release_gap_us;
    bt_hid_send_t sending_kind;
    bt_hid_report_t sending_report;
    bt_hid_report_t queue[BT_HID_QUEUE_MAX];
    bt_hid_report_t motion;
    size_t read, count;
} bt_hid_t;

void bt_hid_init(bt_hid_t *hid);
void bt_hid_request(bt_hid_t *hid, bool enabled);
void bt_hid_error(bt_hid_t *hid, esp_err_t error);
esp_err_t bt_hid_confirm(bt_hid_t *hid, bool accepted);
esp_err_t bt_hid_enqueue(bt_hid_t *hid, const bt_hid_report_t *reports, size_t count);
void bt_hid_cancel(bt_hid_t *hid);
void bt_hid_cancel_motion(bt_hid_t *hid);
void bt_hid_status(bt_hid_t *hid, aether_bluetooth_hid_status_t *status);
bool bt_hid_offer_confirmation(bt_hid_t *hid, const uint8_t address[6], uint32_t code);
void bt_hid_auth_failed(bt_hid_t *hid, const uint8_t address[6], bool key_rejected);
void bt_hid_event(bt_hid_t *hid, esp_hidd_cb_event_t event, const esp_hidd_cb_param_t *data);
void bt_hid_service(bt_hid_t *hid, esp_hd_cb_t callback, bool audio_busy);
bool bt_hid_quiesced(bt_hid_t *hid);
void bt_hid_host_disabled(bt_hid_t *hid);
