#include "bt_hid.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bt_hid_t device;
static esp_hd_cb_t registered_callback;
static int64_t now;
static bool page_scan_on, scan_on, display_io, reply_accepted, store_valid, hold_ack;
static bool sdk_initialized, sdk_registered, sdk_connected, hold_profile_stop, hold_disconnect;
static bool unregister_pending, deinit_pending, sdk_bonded, sdk_pinned, sdk_connect_pending;
static unsigned replies, saves, sent, disconnects, initializations, unregistrations, deinitializations;
static unsigned reconnects;
static uint8_t reply_address[6], store_address[6];
static uint8_t sdk_peer[6];
static const uint8_t pc[6] = {1, 2, 3, 4, 5, 6};
static const uint8_t other[6] = {6, 5, 4, 3, 2, 1};
static esp_bt_cod_t class_of_device;
static bt_hid_report_t last_report;
static esp_hidd_status_t report_status;
static bool report_without_id;
static uint8_t report_reason;
static esp_err_t submit_status;

int64_t esp_timer_get_time(void) { return now; }
static void callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *data)
{ bt_hid_event(&device, event, data); }
esp_err_t esp_bt_hid_device_register_callback(esp_hd_cb_t cb)
{ registered_callback = cb; return ESP_OK; }
esp_err_t esp_bt_hid_device_init(void)
{
    assert(!sdk_initialized);
    sdk_initialized = true;
    ++initializations;
    esp_hidd_cb_param_t e = {.init.status=ESP_HIDD_SUCCESS};
    registered_callback(ESP_HIDD_INIT_EVT, &e);
    return ESP_OK;
}
esp_err_t esp_bt_hid_device_register_app(esp_hidd_app_param_t *config, esp_hidd_qos_param_t *in, esp_hidd_qos_param_t *out)
{
    assert(sdk_initialized && !sdk_registered);
    assert(config->subclass == ESP_HID_CLASS_COM && config->desc_list == bt_hid_descriptor);
    assert(config->desc_list_len == (int)bt_hid_descriptor_size && in && out);
    sdk_registered = true;
    esp_hidd_cb_param_t e = {.register_app.status=ESP_HIDD_SUCCESS};
    registered_callback(ESP_HIDD_REGISTER_APP_EVT, &e);
    return ESP_OK;
}
static void close_peer(void)
{
    assert(sdk_connected);
    sdk_connected = false;
    esp_hidd_cb_param_t e = {.close={.status=ESP_HIDD_SUCCESS,.conn_status=ESP_HIDD_CONN_STATE_DISCONNECTED}};
    registered_callback(ESP_HIDD_CLOSE_EVT, &e);
}
static void finish_unregister(void)
{
    assert(unregister_pending);
    unregister_pending = false;
    /* ESP-IDF closes the connected peer before emitting UNREGISTER_APP. */
    if (sdk_connected) close_peer();
    sdk_registered = false;
    esp_hidd_cb_param_t e = {.unregister_app.status=ESP_HIDD_SUCCESS};
    registered_callback(ESP_HIDD_UNREGISTER_APP_EVT, &e);
}
esp_err_t esp_bt_hid_device_unregister_app(void)
{
    assert(sdk_initialized && sdk_registered && !unregister_pending);
    ++unregistrations;
    unregister_pending = true;
    if (!hold_profile_stop) finish_unregister();
    return ESP_OK;
}
static void finish_deinit(esp_hidd_status_t result)
{
    assert(deinit_pending);
    deinit_pending = false;
    if (result == ESP_HIDD_SUCCESS) sdk_initialized = sdk_pinned = sdk_connect_pending = false;
    esp_hidd_cb_param_t e = {.deinit.status=result};
    registered_callback(ESP_HIDD_DEINIT_EVT, &e);
}
esp_err_t esp_bt_hid_device_deinit(void)
{
    assert(sdk_initialized && !sdk_registered && !sdk_connected && !deinit_pending);
    ++deinitializations;
    deinit_pending = true;
    if (!hold_profile_stop) finish_deinit(ESP_HIDD_SUCCESS);
    return ESP_OK;
}
esp_err_t esp_bt_hid_device_disconnect(void)
{
    ++disconnects;
    if (!hold_disconnect) close_peer();
    return ESP_OK;
}
esp_err_t esp_bt_hid_device_connect(esp_bd_addr_t peer)
{
    assert(sdk_bonded && !memcmp(peer, pc, 6));
    assert(sdk_registered && !sdk_connected && !sdk_connect_pending && page_scan_on && !scan_on);
    sdk_pinned = sdk_connect_pending = true;
    memcpy(sdk_peer, peer, sizeof(sdk_peer));
    ++reconnects;
    return ESP_OK;
}
static void fail_reconnect(void)
{
    assert(sdk_connect_pending);
    sdk_connect_pending = false;
    esp_hidd_cb_param_t e = {.open={.status=ESP_HIDD_ERROR,.conn_status=ESP_HIDD_CONN_STATE_DISCONNECTED}};
    memcpy(e.open.bd_addr, sdk_peer, sizeof(sdk_peer));
    registered_callback(ESP_HIDD_OPEN_EVT, &e);
}
static void ack(void)
{
    esp_hidd_cb_param_t e = {.send_report={.status=report_status,.reason=report_reason,
        .report_id=report_without_id ? 0 : last_report.id,
        .report_type=report_without_id ? ESP_HIDD_REPORT_TYPE_OTHER : ESP_HIDD_REPORT_TYPE_INPUT}};
    registered_callback(ESP_HIDD_SEND_REPORT_EVT, &e);
}
esp_err_t esp_bt_hid_device_send_report(esp_hidd_report_type_t type, uint8_t id, uint16_t length, uint8_t *data)
{
    assert(type == ESP_HIDD_REPORT_TYPE_INTRDATA && length == bt_hid_report_size(id));
    last_report = (bt_hid_report_t){.id=id};
    memcpy(last_report.bytes, data, length);
    ++sent;
    if (submit_status != ESP_OK) return submit_status;
    if (!hold_ack) ack();
    return ESP_OK;
}
esp_err_t esp_bt_hid_device_report_error(esp_hidd_handshake_error_t error) { (void)error; return ESP_OK; }
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t connection, esp_bt_discovery_mode_t discovery)
{
    page_scan_on = connection == ESP_BT_CONNECTABLE;
    scan_on = discovery == ESP_BT_GENERAL_DISCOVERABLE;
    assert(!scan_on || page_scan_on);
    if (page_scan_on) assert(display_io && class_of_device.major == ESP_BT_COD_MAJOR_DEV_PERIPHERAL);
    return ESP_OK;
}
esp_err_t esp_bt_gap_set_security_param(esp_bt_sp_param_t type, void *value, uint8_t len)
{
    assert(type == ESP_BT_SP_IOCAP_MODE && len == 1);
    display_io = *(esp_bt_io_cap_t *)value == ESP_BT_IO_CAP_IO;
    return ESP_OK;
}
esp_err_t esp_bt_gap_get_cod(esp_bt_cod_t *cod) { *cod = class_of_device; return ESP_OK; }
esp_err_t esp_bt_gap_set_cod(esp_bt_cod_t cod, esp_bt_cod_mode_t mode)
{ assert(mode == ESP_BT_SET_COD_MAJOR_MINOR); class_of_device = cod; return ESP_OK; }
esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t address, bool accepted)
{ ++replies; reply_accepted = accepted; memcpy(reply_address,address,6); return ESP_OK; }
esp_err_t esp_bt_gap_get_bond_device_list(int *count, esp_bd_addr_t *peers)
{
    assert(*count >= 1);
    *count = sdk_bonded;
    if (sdk_bonded) memcpy(peers[0], pc, sizeof(peers[0]));
    return ESP_OK;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{ assert(!strcmp(ns,"pc_hid")); *out = mode; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    assert(h == NVS_READONLY && !strcmp(key,"approved") && *size == 6);
    if (!store_valid) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out,store_address,6);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t size)
{ assert(h == NVS_READWRITE && !strcmp(key,"approved") && size == 6); memcpy(store_address,value,6); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { assert(h==NVS_READWRITE); store_valid=true; ++saves; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static void advance_by(int64_t elapsed, bool audio_busy)
{ now += elapsed; bt_hid_service(&device, callback, audio_busy); }
static void advance(bool audio_busy)
{ advance_by(20000, audio_busy); }
static aether_bluetooth_hid_status_t status(void)
{ aether_bluetooth_hid_status_t s; bt_hid_status(&device,&s); return s; }
static void open_peer(const uint8_t address[6])
{
    assert(sdk_registered && !sdk_connected);
    assert(!sdk_pinned || !memcmp(sdk_peer, address, sizeof(sdk_peer)));
    sdk_connect_pending = false;
    sdk_connected = sdk_pinned = true;
    memcpy(sdk_peer, address, sizeof(sdk_peer));
    esp_hidd_cb_param_t e = {.open={.status=ESP_HIDD_SUCCESS,.conn_status=ESP_HIDD_CONN_STATE_CONNECTED}};
    memcpy(e.open.bd_addr,address,6);
    callback(ESP_HIDD_OPEN_EVT,&e);
}
static void service_enable(void)
{
    bt_hid_request(&device,true);
    advance(false);
    advance(false);
    advance(false);
}

static void enable(void)
{
    service_enable();
    assert(status().discoverable && scan_on);
}

static void enable_known(void)
{
    service_enable();
    assert(status().reconnecting && page_scan_on && !scan_on && !sdk_connect_pending);
    advance_by(1000000, false);
    assert(sdk_connect_pending);
}

static void verify_action_timing(void)
{
    bt_hid_report_t scene = bt_hid_keyboard(5, 0x42);
    scene.hold_ms = 75;
    assert(bt_hid_enqueue(&device, &scene, 1) == ESP_OK);
    assert(bt_hid_enqueue(&device, &scene, 1) == ESP_OK);
    hold_ack = true;
    advance(false);
    assert(last_report.bytes[0] == 5 && last_report.bytes[2] == 0x42);
    now += 23000;
    ack();
    hold_ack = false;
    unsigned before = sent;
    advance_by(74000, false);
    assert(sent == before);
    advance_by(1000, false);
    assert(sent == before + 1 && last_report.id == BT_HID_KEYBOARD && !last_report.bytes[2]);
    advance_by(31000, false);
    assert(sent == before + 1);
    advance_by(1000, false);
    assert(sent == before + 2 && last_report.bytes[2] == 0x42);
    advance_by(75000, false);
    assert(sent == before + 3 && !last_report.bytes[2]);
    advance_by(32000, false);
    assert(sent == before + 3);

    bt_hid_report_t click = bt_hid_mouse(1, 0, 0);
    click.hold_ms = 34;
    assert(bt_hid_enqueue(&device, &click, 1) == ESP_OK);
    advance(false);
    assert(last_report.id == BT_HID_MOUSE && last_report.bytes[0] == 1);
    before = sent;
    advance_by(33000, false);
    assert(sent == before);
    advance_by(1000, false);
    assert(sent == before + 1 && last_report.id == BT_HID_MOUSE && !last_report.bytes[0]);
    advance(false);
}

static void verify_action_retries(void)
{
    const bt_hid_report_t first = bt_hid_keyboard(0, 0x42);
    const bt_hid_report_t second = bt_hid_mouse(2, 0, 0);
    assert(bt_hid_enqueue(&device, &first, 1) == ESP_OK);
    assert(bt_hid_enqueue(&device, &second, 1) == ESP_OK);
    const unsigned before = sent;
    report_status = ESP_HIDD_BUSY;
    report_without_id = true;
    advance(false);
    assert(sent == before + 1 && last_report.bytes[2] == 0x42);
    report_status = ESP_HIDD_ERROR;
    report_without_id = false;
    report_reason = 8;   /* Bluedroid HID_ERR_CONGESTED, before L2CAP accepts data. */
    advance(false);
    assert(sent == before + 2 && last_report.bytes[2] == 0x42);
    report_status = ESP_HIDD_SUCCESS;
    report_reason = 0;
    advance(false);
    assert(sent == before + 3 && last_report.bytes[2] == 0x42);
    advance(false);
    assert(sent == before + 4 && last_report.id == BT_HID_KEYBOARD && !last_report.bytes[2]);
    advance(false);
    assert(sent == before + 5 && last_report.id == BT_HID_MOUSE && last_report.bytes[0] == 2);
    advance(false);
    assert(sent == before + 6 && last_report.id == BT_HID_MOUSE && !last_report.bytes[0]);
    assert(status().last_error == ESP_OK);

    assert(bt_hid_enqueue(&device, &first, 1) == ESP_OK);
    assert(bt_hid_enqueue(&device, &second, 1) == ESP_OK);
    submit_status = ESP_FAIL;
    const unsigned before_rejection = sent;
    for (unsigned retry = 0; retry < 3; ++retry) {
        advance(false);
        assert(last_report.id == BT_HID_KEYBOARD && last_report.bytes[2] == 0x42);
    }
    assert(sent == before_rejection + 3 && status().last_error == ESP_FAIL);
    submit_status = ESP_OK;
    advance(false);
    assert(sent == before_rejection + 4 && last_report.id == BT_HID_MOUSE && last_report.bytes[0] == 2);
    advance(false);

    assert(bt_hid_enqueue(&device, &first, 1) == ESP_OK);
    hold_ack = true;
    advance(false);
    assert(last_report.id == BT_HID_KEYBOARD && last_report.bytes[2] == 0x42);
    bt_hid_cancel(&device);
    assert(bt_hid_enqueue(&device, &second, 1) == ESP_OK);
    ack();
    hold_ack = false;
    advance(false);
    assert(last_report.id == BT_HID_KEYBOARD && !last_report.bytes[2]);
    advance(false);
    assert(last_report.id == BT_HID_MOUSE && !last_report.bytes[0]);
    advance(false);
    assert(last_report.id == BT_HID_MOUSE && last_report.bytes[0] == 2);
    advance(false);
}

static void verify_reopen_during_teardown(void)
{
    enable_known();
    open_peer(pc);
    advance(false);
    advance(false);
    const unsigned starts_before = initializations;
    const unsigned stops_before = unregistrations;
    const bt_hid_report_t key = bt_hid_keyboard(2, 4);
    assert(bt_hid_enqueue(&device, &key, 1) == ESP_OK);

    hold_profile_stop = true;
    bt_hid_request(&device, false);
    bt_hid_request(&device, false);
    bt_hid_request(&device, true);
    assert(status().requested && !status().connected && !status().discoverable);
    advance(false);
    assert(last_report.id == BT_HID_KEYBOARD && !last_report.bytes[0] && !last_report.bytes[2]);
    advance(false);
    assert(last_report.id == BT_HID_MOUSE && !last_report.bytes[0]);
    assert(unregister_pending && unregistrations == stops_before + 1 && !deinit_pending);
    assert(!scan_on && !display_io && class_of_device.major == ESP_BT_COD_MAJOR_DEV_AV);

    advance_by(61000000, false);
    assert(status().requested && status().last_error == ESP_ERR_TIMEOUT);
    assert(initializations == starts_before && !bt_hid_quiesced(&device));
    finish_unregister();
    assert(status().requested && !status().connected);
    advance(false);
    assert(deinit_pending && initializations == starts_before && !bt_hid_quiesced(&device));
    finish_deinit(ESP_HIDD_BUSY);
    assert(status().requested && !bt_hid_quiesced(&device));
    advance(false);
    assert(!deinit_pending && initializations == starts_before);
    advance_by(100000, false);
    assert(deinit_pending && initializations == starts_before);
    finish_deinit(ESP_HIDD_SUCCESS);
    assert(bt_hid_quiesced(&device) && status().requested);

    hold_profile_stop = false;
    enable_known();
    assert(initializations == starts_before + 1 && status().last_error == ESP_OK);
    open_peer(pc);
    advance(false);
    assert(status().connected && saves == 1);
    close_peer();
    assert(status().requested && status().reconnecting);
    const unsigned reconnects_before = reconnects;
    bt_hid_request(&device, false);
    advance_by(1500000, false);
    advance(false);
    assert(bt_hid_quiesced(&device) && reconnects == reconnects_before);
}

static void verify_reopen_during_disconnect_timeout(void)
{
    sdk_bonded = false;
    enable();
    hold_disconnect = true;
    hold_profile_stop = true;
    open_peer(other);
    const unsigned disconnects_before = disconnects;
    advance(false);
    assert(disconnects == disconnects_before + 1 && sdk_connected);
    bt_hid_request(&device, false);
    bt_hid_request(&device, true);
    advance(false);
    assert(unregister_pending && !deinit_pending);
    advance_by(3100000, false);
    assert(status().requested && !status().connected);

    hold_disconnect = false;
    hold_profile_stop = false;
    finish_unregister();
    advance(false);
    enable();
    assert(status().last_error == ESP_OK);
    bt_hid_request(&device, false);
    advance(false);
    advance(false);
    assert(bt_hid_quiesced(&device));
    sdk_bonded = true;
}

static void verify_reconnect_fallback(void)
{
    const unsigned reconnects_before = reconnects;
    sdk_bonded = true;
    enable_known();
    open_peer(pc);
    close_peer();
    assert(status().requested && status().reconnecting && !status().connected);
    advance_by(600000, true);
    assert(reconnects == reconnects_before + 1);
    advance(false);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        advance_by(attempt == 0 ? 1000000 : 500000, false);
        assert(sdk_connect_pending && reconnects == reconnects_before + 2 + attempt);
        bt_hid_auth_failed(&device, pc, false); /* Link timed out, bond remains valid. */
        fail_reconnect();
    }
    assert(status().requested && status().reconnecting && sdk_pinned && page_scan_on);
    advance_by(3000000, false);
    assert(status().reconnecting && page_scan_on && !scan_on && sdk_pinned);
    advance_by(1000000, false); /* Unregister the exhausted profile. */
    advance(false); /* Deinit clears the virtual cable. */
    enable();
    assert(!sdk_pinned && reconnects == reconnects_before + 4);
    assert(bt_hid_offer_confirmation(&device, other, 456789));
    assert(bt_hid_confirm(&device, false) == ESP_OK);
    advance_by(59000000, false);
    advance(false);
    enable();
    advance_by(1100000, false);
    advance(false);
    assert(!status().requested && bt_hid_quiesced(&device));
}

static void verify_passive_reconnect(void)
{
    const unsigned reconnects_before = reconnects;
    bt_hid_request(&device, true);
    advance(true);
    advance(true);
    advance_by(1500000, true);
    assert(status().reconnecting && !page_scan_on && !sdk_connect_pending);
    advance(false);
    advance_by(500000, false);
    assert(status().reconnecting && page_scan_on && !scan_on && !sdk_connect_pending);
    assert(!bt_hid_offer_confirmation(&device, other, 456789));
    open_peer(pc);
    assert(status().connected && reconnects == reconnects_before);
    bt_hid_request(&device, false);
    advance(false);
    advance(false);
    advance(false);
    assert(bt_hid_quiesced(&device) && !page_scan_on);

    enable_known();
    fail_reconnect(); /* Windows can still open the HID channels after the outbound failure. */
    advance_by(3000000, false);
    assert(status().reconnecting && page_scan_on && !scan_on);
    open_peer(pc);
    assert(status().connected && reconnects == reconnects_before + 1);
    bt_hid_request(&device, false);
    advance(false);
    advance(false);
    advance(false);
    assert(bt_hid_quiesced(&device) && !page_scan_on);
}

static void verify_reconnect_timeout_and_stale_bond(void)
{
    enable_known();
    const unsigned reconnects_before = reconnects;
    advance_by(7100000, false); /* The SDK never reports a terminal OPEN. */
    advance(false);
    enable();
    assert(!sdk_pinned && !sdk_connect_pending && reconnects == reconnects_before);
    bt_hid_request(&device, false);
    advance(false);
    advance(false);

    enable_known();
    bt_hid_auth_failed(&device, pc, true);
    advance(false);
    advance(false);
    enable(); /* SDK still has an entry, but auth failure retired the approval. */
    open_peer(pc);
    assert(!status().connected);
    advance(false);
    advance(false);
    advance(false);
    enable();
    assert(bt_hid_offer_confirmation(&device, pc, 567890));
    assert(bt_hid_confirm(&device, true) == ESP_OK);
    advance(false);
    open_peer(pc);
    advance(false);
    assert(status().connected && saves == 2);
    bt_hid_request(&device, false);
    advance(false);
    advance(false);
    advance(false);
    assert(bt_hid_quiesced(&device));
}

int main(void)
{
    bt_hid_init(&device);
    class_of_device.major = ESP_BT_COD_MAJOR_DEV_AV;
    bt_hid_report_t letters[BT_HID_TEXT_MAX];
    size_t length;
    for (unsigned c = 32; c < 127; ++c) {
        const char text[] = {(char)c,0};
        assert(bt_hid_ascii(text,letters,&length) && length == 1 && letters[0].bytes[2]);
    }
    assert(bt_hid_ascii("aA0?",letters,&length) && length==4);
    assert(letters[0].bytes[2]==4 && letters[1].bytes[0]==2 && letters[2].bytes[2]==0x27 && letters[3].bytes[2]==0x38);
    assert(!bt_hid_ascii("ASCII\xc3\xa9",letters,&length) && !length);
    char too_long[50]; memset(too_long,'x',49); too_long[49]=0;
    assert(!bt_hid_ascii(too_long,letters,&length) && !length);
    too_long[48]=0;
    assert(bt_hid_ascii(too_long,letters,&length) && length==48);

    assert(!bt_hid_offer_confirmation(&device,pc,111111));
    enable();
    advance(true); /* Audio inquiry/connect suspends discoverability. */
    assert(!scan_on && !status().discoverable && !deinitializations);
    advance(false);
    assert(scan_on);
    open_peer(other); /* No user-approved SSP or stored peer. */
    assert(!status().connected && !saves);
    advance(false);
    assert(disconnects==1 && !sent);
    advance(false);
    advance(false);
    enable();
    assert(bt_hid_offer_confirmation(&device,pc,123456));
    assert(!bt_hid_offer_confirmation(&device,other,222222));
    assert(status().confirm_pending && status().confirm_code==123456 && !replies);
    now += 26000000;
    advance(false);
    assert(replies==1 && !reply_accepted && !status().confirm_pending);
    advance(false);
    enable();
    assert(bt_hid_offer_confirmation(&device,pc,654321));
    assert(bt_hid_confirm(&device,true)==ESP_OK && !saves);
    advance(false);
    assert(replies==2 && reply_accepted && !memcmp(reply_address,pc,6));
    open_peer(pc);
    sdk_bonded = true;
    advance(false);
    assert(status().connected && !scan_on && saves==1 && !memcmp(store_address,pc,6));
    advance(false); /* Initial neutral mouse. */

    const bt_hid_report_t key = bt_hid_keyboard(2,4);
    const bt_hid_report_t m1 = bt_hid_mouse(0,8,9), m2 = bt_hid_mouse(0,-7,2);
    assert(bt_hid_enqueue(&device,&m1,1)==ESP_OK && bt_hid_enqueue(&device,&m2,1)==ESP_OK);
    assert(bt_hid_enqueue(&device,&key,1)==ESP_OK);
    advance(false);
    assert(last_report.id==BT_HID_KEYBOARD && last_report.bytes[0]==2 && last_report.bytes[2]==4);
    advance(false);
    assert(last_report.id==BT_HID_KEYBOARD && !last_report.bytes[0] && !last_report.bytes[2]);
    advance(false);
    assert(last_report.id==BT_HID_MOUSE && last_report.bytes[1]==(uint8_t)-7);
    const unsigned before = sent;
    advance(false);
    assert(sent==before);

    verify_action_timing();
    verify_action_retries();

    assert(bt_hid_enqueue(&device,&key,1)==ESP_OK);
    hold_ack=true;
    advance(false);
    assert(last_report.bytes[0]==2);
    bt_hid_cancel(&device);
    assert(bt_hid_enqueue(&device,&m2,1)==ESP_OK);
    ack(); hold_ack=false;
    bt_hid_request(&device,false);
    assert(!status().connected && bt_hid_enqueue(&device,&key,1)==ESP_ERR_INVALID_STATE);
    advance(false);
    assert(last_report.id==BT_HID_KEYBOARD && !last_report.bytes[0]);
    advance(false);
    assert(last_report.id==BT_HID_MOUSE && !last_report.bytes[1] && !last_report.bytes[2]);
    advance(false);
    assert(bt_hid_quiesced(&device) && deinitializations==3 && !display_io);
    assert(class_of_device.major == ESP_BT_COD_MAJOR_DEV_AV);

    enable_known();
    open_peer(pc); /* User reopened HID for the already approved peer. */
    advance(false); advance(false);
    assert(status().connected && saves==1);
    hold_ack=true;
    assert(bt_hid_enqueue(&device,&key,1)==ESP_OK);
    advance(false);
    now += 300000;
    advance(false);
    assert(!status().connected && disconnects==2 && status().last_error==ESP_ERR_TIMEOUT);
    advance(false);
    advance(false);
    assert(!status().requested && bt_hid_quiesced(&device));

    sdk_bonded = false;
    enable();
    assert(bt_hid_offer_confirmation(&device,other,777777));
    const unsigned replies_before_expiry = replies;
    now += 61000000;
    advance(false);
    advance(false);
    assert(!status().requested && !status().discoverable && !status().confirm_pending);
    assert(status().last_error==ESP_ERR_TIMEOUT && bt_hid_quiesced(&device));
    assert(replies==replies_before_expiry+1 && !reply_accepted && !scan_on && !display_io);

    hold_ack=false;
    sdk_bonded = true;
    enable_known();
    open_peer(pc);
    advance(false);
    assert(status().connected);
    close_peer();
    assert(!status().connected && status().requested && status().reconnecting);
    advance(false);
    advance_by(1000000, false);
    assert(sdk_connect_pending && !status().discoverable);
    open_peer(pc);
    assert(status().connected && !status().reconnecting);
    close_peer();
    bt_hid_request(&device, false);
    advance(false);
    advance(false);
    assert(bt_hid_quiesced(&device) && status().last_error==ESP_OK);
    verify_reopen_during_teardown();
    verify_reopen_during_disconnect_timeout();
    verify_reconnect_fallback();
    verify_passive_reconnect();
    verify_reconnect_timeout_and_stale_bond();
    pthread_mutex_destroy(&device.lock);
    puts("Classic HID pairing, reconnect, asynchronous teardown and report timing passed");
    return 0;
}
