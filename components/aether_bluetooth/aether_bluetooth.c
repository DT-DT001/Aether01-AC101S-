#include "aether_bluetooth.h"
#include "bt_pcm.h"
#include "sdkconfig.h"
#if CONFIG_BT_HID_DEVICE_ENABLED
#include "bt_hid.h"
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define FIFO_FRAMES 8192U
#define PROFILE_TIMEOUT_US 3000000LL
/* Diagnostic admission budget, not a measured SDK minimum or an OOM guarantee.
 * S31's SDK can crash while cleaning up a partially initialized controller. */
#define CONTROLLER_MIN_DMA_FREE (64U * 1024U)
#define CONTROLLER_MIN_DMA_BLOCK (24U * 1024U)
#define PEER_FORMAT "%02x:%02x:%02x:%02x:%02x:%02x"
#define PEER_ARGUMENTS(bda) (bda)[0], (bda)[1], (bda)[2], (bda)[3], (bda)[4], (bda)[5]
static const char *TAG = "aether_bluetooth";
typedef enum { CMD_SCAN, CMD_CONNECT, CMD_DISCONNECT } command_kind_t;
typedef struct { command_kind_t kind; uint8_t address[6]; } command_t;
struct aether_bluetooth {
    portMUX_TYPE lock;
    SemaphoreHandle_t producer, requests;
    QueueHandle_t commands, remote;
    atomic_bool desired, terminate, done, media_requested, task_created; /* desired is the headset request. */
    atomic_bool controller_enabled, full_stop_requested;
    atomic_int a2dp_state, ct_state, tg_state;
    aether_bluetooth_status_t status;
    aether_bluetooth_device_t devices[AETHER_BLUETOOTH_MAX_DEVICES];
    char name[AETHER_BLUETOOTH_NAME_BYTES];
    bool controller_initialized, host_initialized, host_enabled;
    bool a2dp_requested, ct_requested, tg_requested;
    bool a2dp_deinitializing, ct_deinitializing, tg_deinitializing, cleanup_pending;
    bool shutdown_disconnect_done;
    bool connecting;
    bool active, ct_connected, get_caps, register_volume, volume_supported;
    bool volume_dirty, tg_interim, tg_registered;
    uint32_t generation, ack_serial, request_revision;
    esp_a2d_media_ctrl_t ack_cmd, control;
    esp_a2d_media_ctrl_ack_t ack_status;
    int64_t control_started, connecting_started, disconnect_started, retry_at;
    int16_t *fifo;
    size_t read, count;
    bt_pcm_t converter;
#if CONFIG_BT_HID_DEVICE_ENABLED
    bt_hid_t hid;
    bool hid_requested, hid_start_pending;
#endif
};
static portMUX_TYPE s_instance_lock = portMUX_INITIALIZER_UNLOCKED;
static struct aether_bluetooth *s_instance;
static unsigned s_callbacks;
static bool s_terminal;

static TickType_t milliseconds(uint32_t ms)
{ TickType_t ticks = pdMS_TO_TICKS(ms); return ms && !ticks ? 1 : ticks; }
static void pause_ms(uint32_t ms) { vTaskDelay(milliseconds(ms)); }
static struct aether_bluetooth *callback_acquire(void)
{
    portENTER_CRITICAL(&s_instance_lock);
    struct aether_bluetooth *h = s_instance;
    if (h) ++s_callbacks;
    portEXIT_CRITICAL(&s_instance_lock);
    return h;
}
static void callback_release(void)
{
    portENTER_CRITICAL(&s_instance_lock);
    --s_callbacks;
    portEXIT_CRITICAL(&s_instance_lock);
}
#if CONFIG_BT_HID_DEVICE_ENABLED
static void hid_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    struct aether_bluetooth *h = callback_acquire();
    if (!h) return;
    bt_hid_event(&h->hid, event, param);
    callback_release();
}
#endif
static bool radio_requested_locked(struct aether_bluetooth *h)
{
#if CONFIG_BT_HID_DEVICE_ENABLED
    if (h->hid_requested) return true;
#endif
    return atomic_load(&h->desired);
}
static bool radio_requested(struct aether_bluetooth *h)
{
    portENTER_CRITICAL(&h->lock);
    const bool requested = radio_requested_locked(h);
    portEXIT_CRITICAL(&h->lock);
    return requested;
}

static void set_error(struct aether_bluetooth *h, esp_err_t error)
{
    if (error == ESP_OK) return;
    portENTER_CRITICAL(&h->lock);
    h->status.last_error = error;
    ++h->status.revision;
    portEXIT_CRITICAL(&h->lock);
}
static void cancel_stream_locked(struct aether_bluetooth *h)
{
    h->active = false;
    h->count = h->read = 0;
    ++h->generation;
}
static void remote_event(struct aether_bluetooth *h, aether_bluetooth_remote_action_t action, uint8_t volume)
{
    aether_bluetooth_remote_event_t event = {.action = action, .volume = volume};
    if (xQueueSend(h->remote, &event, 0) != pdPASS) {
        portENTER_CRITICAL(&h->lock);
        ++h->status.remote_events_dropped;
        portEXIT_CRITICAL(&h->lock);
    }
}
static uint8_t to_percent(uint8_t volume)
{ return (uint8_t)(((volume & 127U) * 100U + 63U) / 127U); }

/* Names are bounded; preserve complete valid UTF-8 and replace other bytes. */
static void copy_name(char *out, const uint8_t *in, size_t length)
{
    size_t used = 0;
    for (size_t pos = 0; pos < length && used + 1 < AETHER_BLUETOOTH_NAME_BYTES;) {
        uint8_t first = in[pos]; unsigned width = 1;
        if (!first) break;
        if (first >= 0xC2 && first <= 0xDF) width = 2;
        else if (first >= 0xE0 && first <= 0xEF) width = 3;
        else if (first >= 0xF0 && first <= 0xF4) width = 4;
        bool valid = first >= 0x20 && first != 0x7F && (first < 0x80 || width > 1);
        if (width > length - pos || used + width >= AETHER_BLUETOOTH_NAME_BYTES) break;
        for (unsigned i = 1; i < width; ++i) if ((in[pos + i] & 0xC0) != 0x80) valid = false;
        if (width == 3 && ((first == 0xE0 && in[pos+1] < 0xA0) || (first == 0xED && in[pos+1] >= 0xA0))) valid = false;
        if (width == 4 && ((first == 0xF0 && in[pos+1] < 0x90) || (first == 0xF4 && in[pos+1] >= 0x90))) valid = false;
        if (valid) { memcpy(out + used, in + pos, width); used += width; pos += width; }
        else { out[used++] = '?'; ++pos; }
    }
    out[used] = 0;
}
static void discovery(struct aether_bluetooth *h, esp_bt_gap_cb_param_t *param)
{
    portENTER_CRITICAL(&h->lock);
    const bool scanning = h->status.scanning && atomic_load(&h->desired);
    portEXIT_CRITICAL(&h->lock);
    if (!scanning || param->disc_res.num_prop < 0 || (param->disc_res.num_prop && !param->disc_res.prop)) return;
    aether_bluetooth_device_t device = {.rssi = -127};
    uint32_t cod = 0;
    memcpy(device.address, param->disc_res.bda, 6);
    for (int i = 0; i < param->disc_res.num_prop; ++i) {
        const esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
        if (!prop->val || prop->len <= 0) continue;
        if (prop->type == ESP_BT_GAP_DEV_PROP_COD && prop->len >= (int)sizeof(cod)) memcpy(&cod, prop->val, sizeof(cod));
        else if (prop->type == ESP_BT_GAP_DEV_PROP_RSSI) memcpy(&device.rssi, prop->val, 1);
        else if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME) copy_name(device.name, prop->val, (size_t)prop->len);
        else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR) {
            /* Parse bounded EIR ourselves: malformed length cannot escape prop. */
            const uint8_t *eir = prop->val;
            for (size_t p = 0; p < (size_t)prop->len;) {
                const size_t n = eir[p++];
                if (!n || n > (size_t)prop->len - p) break;
                if (eir[p] == ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME ||
                    (eir[p] == ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME && !device.name[0]))
                    copy_name(device.name, eir + p + 1, n - 1);
                p += n;
            }
        }
    }
    device.audio_device = (esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING) != 0 ||
                           esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV;
    portENTER_CRITICAL(&h->lock);
    size_t index = 0;
    while (index < h->status.device_count && memcmp(h->devices[index].address, device.address, 6)) ++index;
    if (index == AETHER_BLUETOOTH_MAX_DEVICES && device.audio_device) {
        for (size_t i = 0; i < AETHER_BLUETOOTH_MAX_DEVICES; ++i)
            if (!h->devices[i].audio_device) { index = i; break; }
    }
    if (index < AETHER_BLUETOOTH_MAX_DEVICES) {
        if (index < h->status.device_count && !memcmp(h->devices[index].address, device.address, 6)) {
            if (!device.name[0]) memcpy(device.name,h->devices[index].name,sizeof(device.name));
            device.audio_device |= h->devices[index].audio_device;
        }
        h->devices[index] = device;
        if (index == h->status.device_count) ++h->status.device_count;
        ++h->status.revision;
    }
    portEXIT_CRITICAL(&h->lock);
}
static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    struct aether_bluetooth *h = callback_acquire();
    if (!h) return;
    if (event == ESP_BT_GAP_DISC_RES_EVT) discovery(h, param);
    else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        portENTER_CRITICAL(&h->lock);
        h->status.scanning = param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
        if (h->status.state == AETHER_BT_SCANNING && !h->status.scanning) h->status.state = AETHER_BT_IDLE;
        ++h->status.revision;
        portEXIT_CRITICAL(&h->lock);
    } else if (event == ESP_BT_GAP_AUTH_CMPL_EVT) {
#if CONFIG_BT_HID_DEVICE_ENABLED
        if (param->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) {
            const bool key_rejected = param->auth_cmpl.stat == ESP_BT_STATUS_AUTH_FAILURE ||
                                      param->auth_cmpl.stat == ESP_BT_STATUS_AUTH_REJECTED ||
                                      param->auth_cmpl.stat == ESP_BT_STATUS_HCI_AUTH_FAILURE ||
                                      param->auth_cmpl.stat == ESP_BT_STATUS_HCI_KEY_MISSING;
            bt_hid_auth_failed(&h->hid, param->auth_cmpl.bda, key_rejected);
        }
#endif
        portENTER_CRITICAL(&h->lock);
        if (!memcmp(h->status.peer.address, param->auth_cmpl.bda, 6)) {
            h->status.auth_status = param->auth_cmpl.stat;
            if (param->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) h->status.last_error = ESP_ERR_INVALID_RESPONSE;
            ++h->status.revision;
        }
        portEXIT_CRITICAL(&h->lock);
        ESP_LOGI(TAG, "GAP auth peer=" PEER_FORMAT " status=0x%02x", PEER_ARGUMENTS(param->auth_cmpl.bda), param->auth_cmpl.stat);
    } else if (event == ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT) {
        portENTER_CRITICAL(&h->lock);
        if (!memcmp(h->status.peer.address, param->acl_conn_cmpl_stat.bda, 6)) {
            h->status.acl_status = param->acl_conn_cmpl_stat.stat;
            if (param->acl_conn_cmpl_stat.stat != ESP_BT_STATUS_SUCCESS) h->status.last_error = ESP_ERR_INVALID_RESPONSE;
            ++h->status.revision;
        }
        portEXIT_CRITICAL(&h->lock);
        ESP_LOGI(TAG, "GAP ACL connect peer=" PEER_FORMAT " status=0x%02x handle=0x%04x",
            PEER_ARGUMENTS(param->acl_conn_cmpl_stat.bda), param->acl_conn_cmpl_stat.stat, param->acl_conn_cmpl_stat.handle);
    } else if (event == ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT) {
        portENTER_CRITICAL(&h->lock);
        if (!memcmp(h->status.peer.address, param->acl_disconn_cmpl_stat.bda, 6)) {
            h->status.acl_disconnect_reason = param->acl_disconn_cmpl_stat.reason;
            if (h->status.connected && h->status.state != AETHER_BT_DISCONNECTING && h->status.state != AETHER_BT_STOPPING && h->status.last_error == ESP_OK)
                h->status.last_error = ESP_ERR_INVALID_STATE;
            ++h->status.revision;
        }
        portEXIT_CRITICAL(&h->lock);
        ESP_LOGI(TAG, "GAP ACL disconnect peer=" PEER_FORMAT " reason=0x%02x handle=0x%04x",
            PEER_ARGUMENTS(param->acl_disconn_cmpl_stat.bda), param->acl_disconn_cmpl_stat.reason, param->acl_disconn_cmpl_stat.handle);
    } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
        /* Standard legacy headphone PIN. A 16-digit-only device needs explicit
         * user-entry support and is rejected instead of guessed. */
        esp_bt_pin_code_t pin = {'0','0','0','0'};
        portENTER_CRITICAL(&h->lock);
        bool selected = (h->status.state == AETHER_BT_CONNECTING || h->status.connected) &&
            !memcmp(h->status.peer.address,param->pin_req.bda,6);
        portEXIT_CRITICAL(&h->lock);
        const bool accepted = selected && atomic_load(&h->desired) && !param->pin_req.min_16_digit;
        const esp_err_t reply = esp_bt_gap_pin_reply(param->pin_req.bda, accepted, 4, pin);
        ESP_LOGI(TAG, "GAP PIN peer=" PEER_FORMAT " selected=%u min16=%u accepted=%u reply=%d",
            PEER_ARGUMENTS(param->pin_req.bda), selected, param->pin_req.min_16_digit, accepted, reply);
        set_error(h, reply);
        if (param->pin_req.min_16_digit) set_error(h, ESP_ERR_NOT_SUPPORTED);
    } else if (event == ESP_BT_GAP_CFM_REQ_EVT) {
        portENTER_CRITICAL(&h->lock);
        bool selected = h->status.state == AETHER_BT_CONNECTING && !memcmp(h->status.peer.address, param->cfm_req.bda, 6);
        portEXIT_CRITICAL(&h->lock);
#if CONFIG_BT_HID_DEVICE_ENABLED
        if (!selected && !atomic_load(&h->terminate) &&
            bt_hid_offer_confirmation(&h->hid, param->cfm_req.bda, param->cfm_req.num_val)) {
            callback_release();
            return;
        }
#endif
        const bool accepted = selected && atomic_load(&h->desired);
        const esp_err_t reply = esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, accepted);
        ESP_LOGI(TAG, "GAP SSP confirm peer=" PEER_FORMAT " selected=%u accepted=%u reply=%d",
            PEER_ARGUMENTS(param->cfm_req.bda), selected, accepted, reply);
        set_error(h, reply);
#if CONFIG_BT_HID_DEVICE_ENABLED
    } else if (event == ESP_BT_GAP_KEY_REQ_EVT) {
        /* There is no app passkey-entry UI. Do not substitute a fixed PIN. */
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
#endif
    }
    callback_release();
}
static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    struct aether_bluetooth *h = callback_acquire();
    if (!h) return;
    if (event == ESP_A2D_PROF_STATE_EVT) {
        atomic_store(&h->a2dp_state, param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS ? 1 :
                     param->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS ? -1 : -2);
    } else {
        if (event == ESP_A2D_CONNECTION_STATE_EVT)
            ESP_LOGI(TAG, "A2DP state=%d peer=" PEER_FORMAT " disconnect_reason=%d",
                param->conn_stat.state, PEER_ARGUMENTS(param->conn_stat.remote_bda), param->conn_stat.disc_rsn);
        if (event == ESP_A2D_AUDIO_CFG_EVT)
            ESP_LOGI(TAG, "A2DP codec type=%u frequency_mask=0x%x channel_mask=0x%x",
                param->audio_cfg.mcc.type, param->audio_cfg.mcc.cie.sbc_info.samp_freq, param->audio_cfg.mcc.cie.sbc_info.ch_mode);
        portENTER_CRITICAL(&h->lock);
        if (event == ESP_A2D_CONNECTION_STATE_EVT) {
            bool connected = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
            if (connected) {
                h->connecting = false;
                h->status.connected = true;
                if (h->status.state != AETHER_BT_DISCONNECTING && h->status.state != AETHER_BT_STOPPING) {
                    h->status.state = AETHER_BT_CONNECTED;
                    h->status.last_error = ESP_OK;
                }
                memcpy(h->status.peer.address, param->conn_stat.remote_bda, 6);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                const bool requested = h->status.state == AETHER_BT_DISCONNECTING || h->status.state == AETHER_BT_STOPPING;
                if (!requested && h->status.last_error == ESP_OK && (h->connecting || h->status.connected || param->conn_stat.disc_rsn != ESP_A2D_DISC_RSN_NORMAL))
                    h->status.last_error = ESP_FAIL;
                h->status.a2dp_disconnect_reason = param->conn_stat.disc_rsn;
                h->connecting = false;
                h->status.connected = h->status.streaming = false;
                h->status.sample_rate = 0; h->status.channels = 0;
                if (h->status.enabled && h->status.state != AETHER_BT_STOPPING) h->status.state = AETHER_BT_IDLE;
                cancel_stream_locked(h);
                atomic_store(&h->media_requested, false);
            }
        } else if (event == ESP_A2D_AUDIO_CFG_EVT) {
            const esp_a2d_mcc_t *mcc = &param->audio_cfg.mcc;
            const unsigned frequency = mcc->cie.sbc_info.samp_freq, mode = mcc->cie.sbc_info.ch_mode;
            uint32_t rate = frequency == ESP_A2D_SBC_CIE_SF_48K ? 48000 :
                            frequency == ESP_A2D_SBC_CIE_SF_44K ? 44100 : 0;
            unsigned channels = mode == ESP_A2D_SBC_CIE_CH_MODE_MONO ? 1 :
                (mode == ESP_A2D_SBC_CIE_CH_MODE_STEREO || mode == ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO ||
                 mode == ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL) ? 2 : 0;
            if (mcc->type != ESP_A2D_MCT_SBC || !rate || !channels) {
                h->status.last_error = ESP_ERR_NOT_SUPPORTED;
                h->status.sample_rate = 0; h->status.channels = 0;
            } else {
                if (h->status.sample_rate != rate || h->status.channels != channels) cancel_stream_locked(h);
                h->status.sample_rate = rate; h->status.channels = (uint8_t)channels;
            }
        } else if (event == ESP_A2D_AUDIO_STATE_EVT) {
            h->status.streaming = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            h->ack_cmd = param->media_ctrl_stat.cmd;
            h->ack_status = param->media_ctrl_stat.status;
            ++h->ack_serial;
        }
        ++h->status.revision;
        portEXIT_CRITICAL(&h->lock);
        if (event == ESP_A2D_CONNECTION_STATE_EVT && param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) xQueueReset(h->remote);
    }
    callback_release();
}
static int32_t pcm_callback(uint8_t *data, int32_t length)
{
    struct aether_bluetooth *h = callback_acquire();
    if (length < 0) {
        if (h) {
            portENTER_CRITICAL(&h->lock);
            h->read = h->count = 0;
            portEXIT_CRITICAL(&h->lock);
            callback_release();
        }
        return 0;
    }
    if (!data || !length) { if (h) callback_release(); return 0; }
    memset(data, 0, (size_t)length);
    if (!h) return length;
    size_t position = 0;
    bool starved = false;
    /* Bounded critical sections; absolutely no wait/allocation/driver calls. */
    while (position < (size_t)length) {
        portENTER_CRITICAL(&h->lock);
        const unsigned channels = h->status.channels == 1 ? 1 : 2;
        const size_t stride = channels * 2U;
        size_t frames = ((size_t)length - position) / stride;
        if (frames > 128) frames = 128;
        if (!h->active || !h->fifo || !h->status.connected || !frames) {
            portEXIT_CRITICAL(&h->lock); break;
        }
        const size_t available = frames < h->count ? frames : h->count;
        for (size_t i = 0; i < available; ++i) {
            const int16_t *pair = h->fifo + h->read * 2;
            if (channels == 2) memcpy(data + position + i * stride, pair, 4);
            else {
                int16_t mono = (int16_t)(((int32_t)pair[0] + pair[1]) / 2);
                memcpy(data + position + i * stride, &mono, 2);
            }
            h->read = (h->read + 1) % FIFO_FRAMES;
        }
        h->count -= available;
        h->status.transmitted_frames += available;
        starved |= available < frames;
        portEXIT_CRITICAL(&h->lock);
        position += frames * stride;
    }
    if (starved) {
        portENTER_CRITICAL(&h->lock); ++h->status.underruns; portEXIT_CRITICAL(&h->lock);
    }
    callback_release();
    return length;
}
static void ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    struct aether_bluetooth *h = callback_acquire();
    if (!h) return;
    if (event == ESP_AVRC_CT_PROF_STATE_EVT) atomic_store(&h->ct_state,
        (param->avrc_ct_init_stat.state == ESP_AVRC_INIT_SUCCESS || param->avrc_ct_init_stat.state == ESP_AVRC_INIT_ALREADY) ? 1 :
        (param->avrc_ct_init_stat.state == ESP_AVRC_DEINIT_SUCCESS || param->avrc_ct_init_stat.state == ESP_AVRC_DEINIT_ALREADY) ? -1 :
        param->avrc_ct_init_stat.state == ESP_AVRC_DEINIT_FAIL ? -3 : -2);
    portENTER_CRITICAL(&h->lock);
    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        h->ct_connected = param->conn_stat.connected;
        h->get_caps = h->ct_connected;
        if (!h->ct_connected) h->volume_supported = false;
    } else if (event == ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT) {
        h->volume_supported = (param->get_rn_caps_rsp.evt_set.bits & (1U << ESP_AVRC_RN_VOLUME_CHANGE)) != 0;
        h->register_volume = h->volume_supported;
    } else if (event == ESP_AVRC_CT_CHANGE_NOTIFY_EVT && param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        h->register_volume = true;
        h->status.peer_volume = to_percent(param->change_ntf.event_parameter.volume);
        ++h->status.revision;
    } else if (event == ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT) {
        h->status.peer_volume = to_percent(param->set_volume_rsp.volume);
    }
    portEXIT_CRITICAL(&h->lock);
    callback_release();
}
static void tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    struct aether_bluetooth *h = callback_acquire();
    if (!h) return;
    if (event == ESP_AVRC_TG_PROF_STATE_EVT) atomic_store(&h->tg_state,
        (param->avrc_tg_init_stat.state == ESP_AVRC_INIT_SUCCESS || param->avrc_tg_init_stat.state == ESP_AVRC_INIT_ALREADY) ? 1 :
        (param->avrc_tg_init_stat.state == ESP_AVRC_DEINIT_SUCCESS || param->avrc_tg_init_stat.state == ESP_AVRC_DEINIT_ALREADY) ? -1 :
        param->avrc_tg_init_stat.state == ESP_AVRC_DEINIT_FAIL ? -3 : -2);
    else if (event == ESP_AVRC_TG_PASSTHROUGH_CMD_EVT && param->psth_cmd.key_state == 0) {
        switch (param->psth_cmd.key_code) {
        case ESP_AVRC_PT_CMD_PLAY: remote_event(h, AETHER_BT_REMOTE_PLAY, 0); break;
        case ESP_AVRC_PT_CMD_PAUSE: remote_event(h, AETHER_BT_REMOTE_PAUSE, 0); break;
        case ESP_AVRC_PT_CMD_STOP: remote_event(h, AETHER_BT_REMOTE_STOP, 0); break;
        case ESP_AVRC_PT_CMD_FORWARD: remote_event(h, AETHER_BT_REMOTE_NEXT, 0); break;
        case ESP_AVRC_PT_CMD_BACKWARD: remote_event(h, AETHER_BT_REMOTE_PREVIOUS, 0); break;
        case ESP_AVRC_PT_CMD_VOL_UP: remote_event(h, AETHER_BT_REMOTE_VOLUME_UP, 0); break;
        case ESP_AVRC_PT_CMD_VOL_DOWN: remote_event(h, AETHER_BT_REMOTE_VOLUME_DOWN, 0); break;
        default: break;
        }
    } else if (event == ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT) {
        portENTER_CRITICAL(&h->lock); h->status.peer_volume = to_percent(param->set_abs_vol.volume); ++h->status.revision; portEXIT_CRITICAL(&h->lock);
    }
    else if (event == ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT && param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        portENTER_CRITICAL(&h->lock); h->tg_interim = true; portEXIT_CRITICAL(&h->lock);
    }
    callback_release();
}

static bool wait_profile(struct aether_bluetooth *h, atomic_int *state, int desired, bool starting)
{
    const int64_t deadline = esp_timer_get_time() + PROFILE_TIMEOUT_US;
    while (atomic_load(state) != desired && esp_timer_get_time() < deadline) {
        if (atomic_load(state) <= -2 ||
            (starting && (atomic_load(&h->full_stop_requested) || !radio_requested(h)))) return false;
        pause_ms(10);
    }
    return atomic_load(state) == desired;
}
static esp_err_t initialize(struct aether_bluetooth *h)
{
    esp_err_t error;
    h->fifo = heap_caps_calloc(FIFO_FRAMES * 2U, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h->fifo) return ESP_ERR_NO_MEM;
    if (!h->controller_initialized) {
        const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
        const size_t available = heap_caps_get_free_size(caps);
        const size_t largest = heap_caps_get_largest_free_block(caps);
        ESP_LOGI(TAG, "controller first init DMA before free=%u largest=%u; admission=%u/%u",
            (unsigned)available, (unsigned)largest, CONTROLLER_MIN_DMA_FREE, CONTROLLER_MIN_DMA_BLOCK);
        if (available < CONTROLLER_MIN_DMA_FREE || largest < CONTROLLER_MIN_DMA_BLOCK) {
            ESP_LOGW(TAG, "controller init deferred: insufficient internal DMA budget; retry after freeing resources");
            return ESP_ERR_NO_MEM;
        }
        esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_LOGI(TAG, "controller config mode=%d max_acl=%u max_sync=%u",
            BTDM_CONTROLLER_MODE_EFF, config.bredr.max_acl_conn, config.bredr.max_sync_conn);
        error = esp_bt_controller_init(&config);
        ESP_LOGI(TAG, "controller first init DMA after free=%u largest=%u result=%d",
            (unsigned)heap_caps_get_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps), error);
        if (error != ESP_OK) return error;
        h->controller_initialized = true;
    }
    if (!atomic_load(&h->controller_enabled)) {
        error = esp_bt_controller_enable(BTDM_CONTROLLER_MODE_EFF); if (error != ESP_OK) return error;
        atomic_store(&h->controller_enabled, true);
    } else {
        ESP_LOGI(TAG, "starting Bluetooth host on retained controller");
    }
    error = esp_bluedroid_init(); if (error != ESP_OK) return error; h->host_initialized = true;
    error = esp_bluedroid_enable(); if (error != ESP_OK) return error; h->host_enabled = true;
    error = esp_bt_gap_register_callback(gap_callback); if (error != ESP_OK) return error;
    error = esp_bt_gap_set_device_name(h->name); if (error != ESP_OK) return error;
    esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
    error = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability)); if (error != ESP_OK) return error;
    error = esp_avrc_ct_register_callback(ct_callback); if (error != ESP_OK) return error;
    atomic_store(&h->ct_state, 0); error = esp_avrc_ct_init(); if (error != ESP_OK) return error; h->ct_requested = true;
    error = esp_avrc_tg_register_callback(tg_callback); if (error != ESP_OK) return error;
    atomic_store(&h->tg_state, 0); error = esp_avrc_tg_init(); if (error != ESP_OK) return error; h->tg_requested = true;
    if (!wait_profile(h, &h->ct_state, 1, true) || !wait_profile(h, &h->tg_state, 1, true)) return ESP_ERR_TIMEOUT;
    esp_avrc_psth_bit_mask_t commands = {0};
    const uint8_t codes[] = {ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STOP,
        ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_VOL_UP, ESP_AVRC_PT_CMD_VOL_DOWN};
    for (size_t i = 0; i < sizeof(codes); ++i) commands.bits[codes[i] / 16] |= 1U << (codes[i] % 16);
    error = esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &commands); if (error != ESP_OK) return error;
    esp_avrc_rn_evt_cap_mask_t events = {.bits = 1U << ESP_AVRC_RN_VOLUME_CHANGE};
    error = esp_avrc_tg_set_rn_evt_cap(&events); if (error != ESP_OK) return error;
    error = esp_a2d_register_callback(a2dp_callback); if (error != ESP_OK) return error;
    error = esp_a2d_source_register_data_callback(pcm_callback); if (error != ESP_OK) return error;
    atomic_store(&h->a2dp_state, 0); error = esp_a2d_source_init(); if (error != ESP_OK) return error; h->a2dp_requested = true;
    if (!wait_profile(h, &h->a2dp_state, 1, true)) return ESP_ERR_TIMEOUT;
    /* A source connects only to the user-selected sink. Match the official
     * source example: unsolicited inbound links must not consume ACL slots. */
    error = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE); if (error != ESP_OK) return error;
    portENTER_CRITICAL(&h->lock);
    h->status.enabled = true; h->status.state = AETHER_BT_IDLE; ++h->status.revision;
    portEXIT_CRITICAL(&h->lock);
    return ESP_OK;
}
static esp_err_t shutdown_radio(struct aether_bluetooth *h)
{
    portENTER_CRITICAL(&h->lock);
    h->status.state = AETHER_BT_STOPPING;
    cancel_stream_locked(h);
    const bool connected = h->status.connected || h->connecting, scanning = h->status.scanning;
    uint8_t address[6]; memcpy(address, h->status.peer.address, 6);
#if CONFIG_BT_HID_DEVICE_ENABLED
    bt_hid_request(&h->hid, false);
    /* Finish the old host shutdown before applying a new HID enable. */
    h->hid_start_pending = h->hid_requested;
#endif
    portEXIT_CRITICAL(&h->lock);
    atomic_store(&h->media_requested, false);
    if (h->host_enabled) {
#if CONFIG_BT_HID_DEVICE_ENABLED
        const int64_t hid_deadline = esp_timer_get_time() + PROFILE_TIMEOUT_US + 500000;
        while (!bt_hid_quiesced(&h->hid) && esp_timer_get_time() < hid_deadline) {
            bt_hid_service(&h->hid, hid_callback, true);
            pause_ms(10);
        }
        if (!bt_hid_quiesced(&h->hid)) bt_hid_error(&h->hid, ESP_ERR_TIMEOUT);
#endif
        if (scanning) {
            esp_err_t error = esp_bt_gap_cancel_discovery(); if (error != ESP_OK) return error;
            const int64_t deadline = esp_timer_get_time() + PROFILE_TIMEOUT_US;
            bool pending = true;
            while (pending && esp_timer_get_time() < deadline) {
                pause_ms(10); portENTER_CRITICAL(&h->lock); pending = h->status.scanning; portEXIT_CRITICAL(&h->lock);
            }
            if (pending) return ESP_ERR_TIMEOUT;
        }
        if (!h->shutdown_disconnect_done) {
            h->shutdown_disconnect_done = true;
            if (connected) {
                const esp_err_t error = esp_a2d_source_disconnect(address);
                if (error != ESP_OK) set_error(h, error);
                else {
                    const int64_t deadline = esp_timer_get_time() + PROFILE_TIMEOUT_US;
                    bool pending = true;
                    while (pending && esp_timer_get_time() < deadline) {
                        pause_ms(10); portENTER_CRITICAL(&h->lock); pending = h->status.connected || h->connecting; portEXIT_CRITICAL(&h->lock);
                    }
                    if (pending) set_error(h, ESP_ERR_TIMEOUT);
                }
                /* A rejected/unfinished disconnect does not prove ownership
                 * ended. Stay STOPPING and let legal profile deinit close the
                 * connection; never restart this wait on a teardown retry. */
            }
        }
        /* Bluedroid requires both AVRCP roles to be removed before A2DP. */
        if (h->tg_requested) {
            if (atomic_load(&h->tg_state) == -3) h->tg_deinitializing = false;
            if (atomic_load(&h->tg_state) != -2) {
                if (!h->tg_deinitializing) {
                    atomic_store(&h->tg_state, 0);
                    esp_err_t error = esp_avrc_tg_deinit(); if (error != ESP_OK) return error;
                    h->tg_deinitializing = true;
                }
                if (!wait_profile(h, &h->tg_state, -1, false)) return ESP_ERR_TIMEOUT;
            }
            h->tg_requested = h->tg_deinitializing = false;
        }
        if (h->ct_requested) {
            if (atomic_load(&h->ct_state) == -3) h->ct_deinitializing = false;
            if (atomic_load(&h->ct_state) != -2) {
                if (!h->ct_deinitializing) {
                    atomic_store(&h->ct_state, 0);
                    esp_err_t error = esp_avrc_ct_deinit(); if (error != ESP_OK) return error;
                    h->ct_deinitializing = true;
                }
                if (!wait_profile(h, &h->ct_state, -1, false)) return ESP_ERR_TIMEOUT;
            }
            h->ct_requested = h->ct_deinitializing = false;
        }
        if (h->a2dp_requested) {
            if (atomic_load(&h->a2dp_state) != -2) {
                if (!h->a2dp_deinitializing) {
                    atomic_store(&h->a2dp_state, 0);
                    esp_err_t error = esp_a2d_source_deinit(); if (error != ESP_OK) return error;
                    h->a2dp_deinitializing = true;
                }
                if (!wait_profile(h, &h->a2dp_state, -1, false)) return ESP_ERR_TIMEOUT;
            }
            h->a2dp_requested = h->a2dp_deinitializing = false;
        }
        esp_err_t error = esp_bluedroid_disable(); if (error != ESP_OK) return error;
        h->host_enabled = false;
#if CONFIG_BT_HID_DEVICE_ENABLED
        bt_hid_host_disabled(&h->hid);
#endif
    }
    if (h->host_initialized) {
        esp_err_t error = esp_bluedroid_deinit(); if (error != ESP_OK) return error;
        h->host_initialized = false;
    }
    if (atomic_load(&h->full_stop_requested) && atomic_load(&h->controller_enabled)) {
        esp_err_t error = esp_bt_controller_disable(); if (error != ESP_OK) return error;
        atomic_store(&h->controller_enabled, false);
    } else if (atomic_load(&h->controller_enabled)) {
        ESP_LOGI(TAG, "Bluetooth host stopped; retaining ESP32-S31 controller");
    }
    /* The writer may be leaving a bounded wait; serialize before freeing. */
    if (xSemaphoreTake(h->producer, milliseconds(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&h->lock); int16_t *fifo = h->fifo; h->fifo = NULL; portEXIT_CRITICAL(&h->lock);
    free(fifo);
    xSemaphoreGive(h->producer);
    h->control = ESP_A2D_MEDIA_CTRL_NONE;
    portENTER_CRITICAL(&h->lock);
    h->status.enabled = h->status.connected = h->status.scanning = h->status.streaming = false;
    h->connecting = false;
    if (!atomic_load(&h->controller_enabled)) atomic_store(&h->full_stop_requested, false);
    h->status.sample_rate = 0; h->status.channels = 0;
    h->status.state = atomic_load(&h->full_stop_requested) ? AETHER_BT_STOPPING : AETHER_BT_OFF;
    h->ct_connected = h->volume_supported = h->tg_registered = false;
    h->get_caps = h->register_volume = h->tg_interim = false;
    ++h->status.revision;
    portEXIT_CRITICAL(&h->lock);
    h->shutdown_disconnect_done = false;
    xQueueReset(h->remote);
    return ESP_OK;
}
static void control_command(struct aether_bluetooth *h, esp_a2d_media_ctrl_t command)
{
    esp_err_t error = esp_a2d_media_ctrl(command);
    if (error == ESP_OK) { h->control = command; h->control_started = esp_timer_get_time(); }
    else { set_error(h, error); h->retry_at = esp_timer_get_time() + 300000; }
}
static void service_media(struct aether_bluetooth *h, uint32_t *seen_ack)
{
    bool connected, streaming, want = atomic_load(&h->media_requested), caps, subscribe, dirty, interim, registered, ct, supported;
    uint32_t serial; esp_a2d_media_ctrl_t command; esp_a2d_media_ctrl_ack_t ack; uint8_t volume;
    portENTER_CRITICAL(&h->lock);
    connected = h->status.connected && h->status.state == AETHER_BT_CONNECTED; streaming = h->status.streaming;
    serial = h->ack_serial; command = h->ack_cmd; ack = h->ack_status;
    caps = h->get_caps; subscribe = h->register_volume; dirty = h->volume_dirty;
    interim = h->tg_interim; registered = h->tg_registered; ct = h->ct_connected;
    supported = h->volume_supported; volume = h->status.volume;
    h->get_caps = h->register_volume = h->volume_dirty = h->tg_interim = false;
    portEXIT_CRITICAL(&h->lock);
    if (ct && caps) esp_avrc_ct_send_get_rn_capabilities_cmd(0);
    if (ct && subscribe && supported) esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_VOLUME_CHANGE, 0);
    /* Software master is applied once by ac101s_audio before this transport.
     * Do not mirror it to the headphone's independent hardware gain. */
    if (interim || (dirty && registered)) {
        esp_avrc_rn_param_t value = {.volume = (uint8_t)((volume * 127U + 50U) / 100U)};
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
            interim ? ESP_AVRC_RN_RSP_INTERIM : ESP_AVRC_RN_RSP_CHANGED, &value);
        portENTER_CRITICAL(&h->lock); h->tg_registered = interim; portEXIT_CRITICAL(&h->lock);
    }
    if (!connected) { h->control = ESP_A2D_MEDIA_CTRL_NONE; return; }
    if (*seen_ack != serial) {
        *seen_ack = serial;
        if (command == h->control) {
            h->control = ESP_A2D_MEDIA_CTRL_NONE;
            if (ack == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                if (command == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && want) { control_command(h, ESP_A2D_MEDIA_CTRL_START); return; }
                if (command == ESP_A2D_MEDIA_CTRL_START || command == ESP_A2D_MEDIA_CTRL_SUSPEND) {
                    streaming = command == ESP_A2D_MEDIA_CTRL_START;
                    portENTER_CRITICAL(&h->lock); h->status.streaming = streaming; portEXIT_CRITICAL(&h->lock);
                }
            } else h->retry_at = esp_timer_get_time() + 300000;
        }
    }
    const int64_t now = esp_timer_get_time();
    if (h->control != ESP_A2D_MEDIA_CTRL_NONE && now - h->control_started > PROFILE_TIMEOUT_US) {
        h->control = ESP_A2D_MEDIA_CTRL_NONE; h->retry_at = now + 300000; set_error(h, ESP_ERR_TIMEOUT);
    }
    if (h->control == ESP_A2D_MEDIA_CTRL_NONE && now >= h->retry_at) {
        if (want && !streaming) control_command(h, ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        else if (!want && streaming) control_command(h, ESP_A2D_MEDIA_CTRL_SUSPEND);
    }
}
static esp_err_t disconnect_peer(struct aether_bluetooth *h, const uint8_t address[6], esp_err_t cause)
{
    atomic_store(&h->media_requested, false);
    portENTER_CRITICAL(&h->lock);
    const bool pending_connect = h->connecting;
    cancel_stream_locked(h);
    if (cause != ESP_OK) h->status.last_error = cause;
    h->status.state = AETHER_BT_DISCONNECTING;
    ++h->status.revision;
    portEXIT_CRITICAL(&h->lock);
    h->disconnect_started = esp_timer_get_time();
    uint8_t peer[6]; memcpy(peer, address, sizeof(peer));
    const esp_err_t error = esp_a2d_source_disconnect(peer);
    ESP_LOGI(TAG, "disconnect request peer=" PEER_FORMAT " cause=%d accepted=%d", PEER_ARGUMENTS(peer), cause, error);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&h->lock);
        h->status.last_error = error;
        if (h->status.state == AETHER_BT_DISCONNECTING && !pending_connect && h->status.connected)
            h->status.state = AETHER_BT_CONNECTED;
        ++h->status.revision;
        portEXIT_CRITICAL(&h->lock);
    }
    return error;
}
static void process_command(struct aether_bluetooth *h, const command_t *command)
{
    aether_bluetooth_status_t status; aether_bluetooth_get_status(h, &status);
    if (!status.enabled || !atomic_load(&h->desired)) { set_error(h, ESP_ERR_INVALID_STATE); return; }
    if (command->kind == CMD_SCAN) {
        if (status.connected || status.state != AETHER_BT_IDLE || status.scanning) { set_error(h, ESP_ERR_INVALID_STATE); return; }
        portENTER_CRITICAL(&h->lock); h->status.device_count = 0; h->status.scanning = true; h->status.state = AETHER_BT_SCANNING; ++h->status.revision; portEXIT_CRITICAL(&h->lock);
        esp_err_t error = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        if (error != ESP_OK) {
            set_error(h, error); portENTER_CRITICAL(&h->lock); h->status.scanning = false; h->status.state = AETHER_BT_IDLE; portEXIT_CRITICAL(&h->lock);
        }
    } else if (command->kind == CMD_CONNECT) {
        if (status.connected || status.state == AETHER_BT_CONNECTING || status.state == AETHER_BT_DISCONNECTING) { set_error(h, ESP_ERR_INVALID_STATE); return; }
        if (status.scanning || status.state == AETHER_BT_SCANNING) {
            esp_err_t error = esp_bt_gap_cancel_discovery(); if (error != ESP_OK) { set_error(h,error); return; }
            const int64_t deadline = esp_timer_get_time() + PROFILE_TIMEOUT_US;
            do { pause_ms(10); aether_bluetooth_get_status(h, &status); }
            while (status.scanning && atomic_load(&h->desired) && esp_timer_get_time() < deadline);
            if (status.scanning || !atomic_load(&h->desired)) { set_error(h, ESP_ERR_TIMEOUT); return; }
        }
        portENTER_CRITICAL(&h->lock);
        memset(&h->status.peer, 0, sizeof(h->status.peer)); memcpy(h->status.peer.address, command->address, 6);
        for (unsigned i = 0; i < h->status.device_count; ++i)
            if (!memcmp(h->devices[i].address, command->address, 6)) h->status.peer = h->devices[i];
        h->status.state = AETHER_BT_CONNECTING; h->status.last_error = ESP_OK; ++h->status.revision;
        h->status.auth_status = h->status.acl_status = h->status.acl_disconnect_reason = h->status.a2dp_disconnect_reason = -1;
        ++h->status.connection_attempts;
        h->connecting = true;
        portEXIT_CRITICAL(&h->lock);
        h->connecting_started = esp_timer_get_time();
        uint8_t address[6]; memcpy(address, command->address, 6);
        ESP_LOGI(TAG, "connect request peer=" PEER_FORMAT " DMA free=%u largest=%u",
            PEER_ARGUMENTS(address),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        esp_err_t error = esp_a2d_source_connect(address);
        ESP_LOGI(TAG, "connect request peer=" PEER_FORMAT " accepted=%d", PEER_ARGUMENTS(address), error);
        if (error != ESP_OK) { set_error(h,error); portENTER_CRITICAL(&h->lock); h->connecting = false; h->status.state = AETHER_BT_IDLE; portEXIT_CRITICAL(&h->lock); }
    } else {
        if (!status.connected && status.state != AETHER_BT_CONNECTING) return;
        disconnect_peer(h, status.peer.address, ESP_OK);
    }
}
#if CONFIG_BT_HID_DEVICE_ENABLED
static void service_hid(struct aether_bluetooth *h, bool audio_busy)
{
    portENTER_CRITICAL(&h->lock);
    if (h->hid_start_pending) {
        bt_hid_request(&h->hid, true);
        h->hid_start_pending = false;
    }
    portEXIT_CRITICAL(&h->lock);

    bt_hid_service(&h->hid, hid_callback, audio_busy);

    portENTER_CRITICAL(&h->lock);
    if (h->hid_requested && !h->hid_start_pending) {
        /* HID can end its own session on an expired window, failure, or
         * completed disconnect. A newer app request remains pending. */
        aether_bluetooth_hid_status_t status;
        bt_hid_status(&h->hid, &status);
        h->hid_requested = status.requested;
    }
    portEXIT_CRITICAL(&h->lock);
}
#endif

static void manager(void *argument)
{
    struct aether_bluetooth *h = argument; uint32_t seen_ack = 0;
    for (;;) {
        portENTER_CRITICAL(&h->lock);
        const bool start = radio_requested_locked(h) && !h->status.enabled &&
            !h->cleanup_pending && !atomic_load(&h->full_stop_requested);
        const uint32_t revision = h->request_revision;
        if (start) {
            h->status.state = AETHER_BT_STARTING;
            h->status.last_error = ESP_OK;
            ++h->status.revision;
        }
        portEXIT_CRITICAL(&h->lock);
        if (start) {
            esp_err_t error = initialize(h);
            if (error != ESP_OK) {
                h->cleanup_pending = true;
                portENTER_CRITICAL(&h->lock);
                h->status.last_error = error;
                h->status.state = AETHER_BT_ERROR;
                ++h->status.revision;
                if (h->request_revision == revision) {
                    atomic_store(&h->desired, false);
                    atomic_store(&h->full_stop_requested, true);
#if CONFIG_BT_HID_DEVICE_ENABLED
                    if (h->hid_requested) bt_hid_error(&h->hid, error);
                    h->hid_requested = h->hid_start_pending = false;
                    bt_hid_request(&h->hid, false);
#endif
                }
                portEXIT_CRITICAL(&h->lock);
            }
        }
        if (h->cleanup_pending || atomic_load(&h->full_stop_requested) ||
            (!radio_requested(h) && (h->host_initialized || h->fifo))) {
            esp_err_t error = shutdown_radio(h);
            if (error != ESP_OK) { set_error(h,error); h->cleanup_pending = true; pause_ms(100); continue; }
            h->cleanup_pending = false;
        }
        if (atomic_load(&h->terminate) && !h->host_initialized && !atomic_load(&h->controller_enabled)) {
            if (h->controller_initialized) {
                esp_err_t error = esp_bt_controller_deinit();
                if (error != ESP_OK) { set_error(h,error); pause_ms(100); continue; }
                h->controller_initialized = false;
                portENTER_CRITICAL(&s_instance_lock); s_terminal = true; portEXIT_CRITICAL(&s_instance_lock);
            }
            atomic_store(&h->done,true); vTaskDelete(NULL); return;
        }
        aether_bluetooth_status_t status;
        aether_bluetooth_get_status(h, &status);
        const bool audio_requested = atomic_load(&h->desired);
        if (status.enabled && audio_requested) {
            command_t command;
            if (xQueueReceive(h->commands,&command,0) == pdPASS) process_command(h,&command);
            service_media(h,&seen_ack);
            aether_bluetooth_get_status(h,&status);
            if (status.state == AETHER_BT_CONNECTING && esp_timer_get_time() - h->connecting_started > 20000000) {
                disconnect_peer(h, status.peer.address, ESP_ERR_TIMEOUT);
            } else if (status.state == AETHER_BT_DISCONNECTING && esp_timer_get_time() - h->disconnect_started > PROFILE_TIMEOUT_US) {
                /* If the SDK accepts cancellation but emits no completion,
                 * deinitialize the profiles/host before allowing a fresh try.
                 * Profile deinit is legal with an active A2DP connection. Never
                 * publish IDLE and overlap another connection on that link. */
                ESP_LOGW(TAG, "disconnect completion timeout: closing radio; explicit re-enable required");
                portENTER_CRITICAL(&h->lock);
                if (h->request_revision == revision) atomic_store(&h->desired, false);
                atomic_store(&h->full_stop_requested, true);
                h->status.last_error = ESP_ERR_TIMEOUT;
                h->status.state = AETHER_BT_STOPPING;
                h->status.connected = h->connecting = false;
                cancel_stream_locked(h);
                ++h->status.revision;
                portEXIT_CRITICAL(&h->lock);
                h->cleanup_pending = true;
            }
        } else if (!audio_requested) {
            command_t unused;
            while (xQueueReceive(h->commands,&unused,0) == pdPASS) {}
        }
#if CONFIG_BT_HID_DEVICE_ENABLED
        if (h->host_enabled && !h->cleanup_pending) {
            service_hid(h, status.scanning || status.state == AETHER_BT_CONNECTING ||
                status.state == AETHER_BT_DISCONNECTING);
        }
#endif
        pause_ms(10);
    }
}

typedef struct { struct aether_bluetooth *h; uint32_t generation; int64_t deadline; } write_context_t;
static int fifo_write(const int16_t *pcm, size_t frames, void *context)
{
    write_context_t *write = context; struct aether_bluetooth *h = write->h;
    while (frames) {
        portENTER_CRITICAL(&h->lock);
        if (!h->active || !h->status.connected || !h->fifo || write->generation != h->generation || !atomic_load(&h->desired)) {
            portEXIT_CRITICAL(&h->lock); return ESP_ERR_INVALID_STATE;
        }
        size_t count = FIFO_FRAMES - h->count; if (count > frames) count = frames;
        size_t tail = (h->read + h->count) % FIFO_FRAMES;
        size_t first = FIFO_FRAMES - tail; if (first > count) first = count;
        if (count) {
            memcpy(h->fifo + tail * 2, pcm, first * 4);
            memcpy(h->fifo, pcm + first * 2, (count - first) * 4);
            h->count += count;
        }
        portEXIT_CRITICAL(&h->lock);
        pcm += count * 2; frames -= count;
        if (frames) {
            if (esp_timer_get_time() >= write->deadline) return ESP_ERR_TIMEOUT;
            pause_ms(2);
        }
    }
    return ESP_OK;
}
static bool sink_connected(void *context)
{
    struct aether_bluetooth *h = context;
    if (!h || !atomic_load(&h->desired)) return false;
    portENTER_CRITICAL(&h->lock);
    bool ready = h->status.enabled && h->status.connected && h->status.state == AETHER_BT_CONNECTED && h->status.sample_rate && h->status.channels;
    portEXIT_CRITICAL(&h->lock);
    return ready;
}
static esp_err_t sink_stop(void *context)
{
    struct aether_bluetooth *h = context;
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    atomic_store(&h->media_requested,false);
    cancel_stream_locked(h);
    portEXIT_CRITICAL(&h->lock);
    return ESP_OK;
}
static esp_err_t sink_start(void *context)
{
    struct aether_bluetooth *h = context;
    if (!sink_connected(h)) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->lock); const uint32_t generation = h->generation; portEXIT_CRITICAL(&h->lock);
    if (xSemaphoreTake(h->producer,milliseconds(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&h->lock);
    if (generation != h->generation || !atomic_load(&h->desired) || !h->status.connected ||
        h->status.state != AETHER_BT_CONNECTED || !h->fifo) {
        portEXIT_CRITICAL(&h->lock); xSemaphoreGive(h->producer); return ESP_ERR_INVALID_STATE;
    }
    const uint32_t rate = h->status.sample_rate;
    cancel_stream_locked(h); h->active = true;
    atomic_store(&h->media_requested,true);
    portEXIT_CRITICAL(&h->lock);
    bool valid = bt_pcm_reset(&h->converter,rate);
    xSemaphoreGive(h->producer);
    if (!valid) { sink_stop(h); return ESP_ERR_NOT_SUPPORTED; }
    const int64_t deadline = esp_timer_get_time() + PROFILE_TIMEOUT_US;
    while (sink_connected(h) && atomic_load(&h->media_requested) && esp_timer_get_time() < deadline) {
        portENTER_CRITICAL(&h->lock); bool streaming = h->status.streaming; portEXIT_CRITICAL(&h->lock);
        if (streaming) return ESP_OK;
        pause_ms(10);
    }
    sink_stop(h); return sink_connected(h) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
static esp_err_t sink_write(void *context, const void *pcm, size_t bytes, uint32_t timeout_ms)
{
    struct aether_bluetooth *h = context;
    if (!h || (!pcm && bytes) || bytes % 4) return ESP_ERR_INVALID_ARG;
    if (!bytes) return ESP_OK;
    if (timeout_ms > 3000) timeout_ms = 3000;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    if (xSemaphoreTake(h->producer,milliseconds(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&h->lock);
    write_context_t write = {.h = h, .generation = h->generation, .deadline = deadline};
    bool ready = h->active && h->status.connected && atomic_load(&h->desired);
    portEXIT_CRITICAL(&h->lock);
    esp_err_t error = ready ? bt_pcm_write(&h->converter,pcm,bytes/4,fifo_write,&write) : ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->lock);
    if (error == ESP_OK && (!h->active || write.generation != h->generation || !atomic_load(&h->desired))) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) h->status.written_frames += bytes / 4;
    else { cancel_stream_locked(h); h->status.last_error = error; atomic_store(&h->media_requested,false); }
    portEXIT_CRITICAL(&h->lock);
    xSemaphoreGive(h->producer);
    return error;
}

esp_err_t aether_bluetooth_new(const aether_bluetooth_config_t *config, aether_bluetooth_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    struct aether_bluetooth *h = calloc(1,sizeof(*h)); if (!h) return ESP_ERR_NO_MEM;
    atomic_init(&h->desired,false); atomic_init(&h->terminate,false); atomic_init(&h->done,false);
    atomic_init(&h->controller_enabled,false); atomic_init(&h->full_stop_requested,false);
    atomic_init(&h->media_requested,false); atomic_init(&h->a2dp_state,0);
    atomic_init(&h->ct_state,0); atomic_init(&h->tg_state,0);
    atomic_init(&h->task_created,false);
    h->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_BT_HID_DEVICE_ENABLED
    bt_hid_init(&h->hid);
#endif
    h->status.volume = 100;
    h->status.auth_status = h->status.acl_status = h->status.acl_disconnect_reason = h->status.a2dp_disconnect_reason = -1;
    const char *name = config && config->device_name ? config->device_name : "Aether01";
    copy_name(h->name,(const uint8_t *)name,strlen(name));
    h->producer = xSemaphoreCreateMutex();
    h->requests = xSemaphoreCreateMutex();
    h->commands = xQueueCreate(8,sizeof(command_t)); h->remote = xQueueCreate(16,sizeof(aether_bluetooth_remote_event_t));
    if (!h->producer || !h->requests || !h->commands || !h->remote) {
        if (h->producer) vSemaphoreDelete(h->producer);
        if (h->requests) vSemaphoreDelete(h->requests);
        if (h->commands) vQueueDelete(h->commands);
        if (h->remote) vQueueDelete(h->remote);
        free(h); return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_instance_lock);
    bool busy = s_instance || s_terminal;
    if (!busy) s_instance = h;
    portEXIT_CRITICAL(&s_instance_lock);
    if (busy) {
        vSemaphoreDelete(h->producer); vSemaphoreDelete(h->requests);
        vQueueDelete(h->commands); vQueueDelete(h->remote); free(h);
        return ESP_ERR_INVALID_STATE;
    }
    *out = h; return ESP_OK;
}
/* Call under the request mutex so a concurrent enable cannot report success
 * while the first task creation is still in progress. */
static esp_err_t ensure_manager(struct aether_bluetooth *h)
{
    /* Controller/host setup can reach NVS/ROM and needs an internal stack. */
    if (!atomic_load(&h->task_created)) {
        if (xTaskCreate(manager,"bluetooth",6144,h,4,NULL) != pdPASS) {
            portENTER_CRITICAL(&h->lock);
            h->status.last_error = ESP_ERR_NO_MEM; h->status.state = AETHER_BT_ERROR; ++h->status.revision;
            portEXIT_CRITICAL(&h->lock);
            return ESP_ERR_NO_MEM;
        }
        atomic_store(&h->task_created,true);
    }
    return ESP_OK;
}
esp_err_t aether_bluetooth_set_enabled(aether_bluetooth_handle_t h, bool enabled)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->requests, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result = atomic_load(&h->terminate) ? ESP_ERR_INVALID_STATE :
        (enabled ? ensure_manager(h) : ESP_OK);
    if (result == ESP_OK) {
        portENTER_CRITICAL(&h->lock);
        atomic_store(&h->desired,enabled);
        ++h->request_revision;
        ++h->status.revision;
        if (!enabled) {
            if (atomic_load(&h->task_created)) atomic_store(&h->full_stop_requested, true);
            if (atomic_load(&h->controller_enabled) || h->status.state == AETHER_BT_STARTING)
                h->status.state = AETHER_BT_STOPPING;
#if CONFIG_BT_HID_DEVICE_ENABLED
            h->hid_requested = h->hid_start_pending = false;
            bt_hid_request(&h->hid, false);
#endif
        }
        portEXIT_CRITICAL(&h->lock);
        if (!enabled) sink_stop(h);
    }
    xSemaphoreGive(h->requests);
    return result;
}
static esp_err_t enqueue(struct aether_bluetooth *h, command_kind_t kind, const uint8_t *address)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&h->desired) || atomic_load(&h->terminate)) return ESP_ERR_INVALID_STATE;
    command_t command = {.kind = kind}; if (address) memcpy(command.address,address,6);
    return xQueueSend(h->commands,&command,0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}
esp_err_t aether_bluetooth_scan(aether_bluetooth_handle_t h) { return enqueue(h,CMD_SCAN,NULL); }
esp_err_t aether_bluetooth_connect(aether_bluetooth_handle_t h, const uint8_t address[6])
{ return address ? enqueue(h,CMD_CONNECT,address) : ESP_ERR_INVALID_ARG; }
esp_err_t aether_bluetooth_disconnect(aether_bluetooth_handle_t h)
{ if (h) sink_stop(h); return enqueue(h,CMD_DISCONNECT,NULL); }
esp_err_t aether_bluetooth_set_volume(aether_bluetooth_handle_t h, uint8_t volume)
{
    if (!h || volume > 100) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    if (h->status.volume != volume) { h->status.volume = volume; h->volume_dirty = true; ++h->status.revision; }
    portEXIT_CRITICAL(&h->lock);
    return ESP_OK;
}
esp_err_t aether_bluetooth_get_status(aether_bluetooth_handle_t h, aether_bluetooth_status_t *status)
{
    if (!h || !status) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    *status = h->status;
    status->audio_requested = atomic_load(&h->desired);
    status->queued_frames = h->count;
    portEXIT_CRITICAL(&h->lock);
    return ESP_OK;
}
esp_err_t aether_bluetooth_get_device(aether_bluetooth_handle_t h, size_t index, aether_bluetooth_device_t *device)
{
    if (!h || !device) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    bool valid = index < h->status.device_count; if (valid) *device = h->devices[index];
    portEXIT_CRITICAL(&h->lock);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}
bool aether_bluetooth_poll_remote(aether_bluetooth_handle_t h, aether_bluetooth_remote_event_t *event)
{ return h && event && xQueueReceive(h->remote,event,0) == pdPASS; }
esp_err_t aether_bluetooth_get_sink(aether_bluetooth_handle_t h, ac101s_audio_sink_t *sink)
{
    if (!h || !sink) return ESP_ERR_INVALID_ARG;
    *sink = (ac101s_audio_sink_t){.context = h, .start = sink_start, .stop = sink_stop, .write = sink_write, .connected = sink_connected};
    return ESP_OK;
}
esp_err_t aether_bluetooth_hid_set_enabled(aether_bluetooth_handle_t h, bool enabled)
{
    if (!h) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    if (xSemaphoreTake(h->requests, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t result = atomic_load(&h->terminate) ? ESP_ERR_INVALID_STATE :
        (enabled ? ensure_manager(h) : ESP_OK);
    if (result == ESP_OK) {
        portENTER_CRITICAL(&h->lock);
        h->hid_requested = h->hid_start_pending = enabled;
        ++h->request_revision;
        if (!enabled) bt_hid_request(&h->hid, false);
        portEXIT_CRITICAL(&h->lock);
    } else {
        bt_hid_error(&h->hid, result);
    }
    xSemaphoreGive(h->requests);
    return result;
#else
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_hid_confirm(aether_bluetooth_handle_t h, bool accepted)
{
    if (!h) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    return bt_hid_confirm(&h->hid, accepted);
#else
    (void)accepted;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_hid_key(aether_bluetooth_handle_t h, uint8_t modifiers, uint8_t key)
{
    if (!h || (!modifiers && !key) || (key && (key < 4 || key > 0x73))) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    bt_hid_report_t report = bt_hid_keyboard(modifiers, key);
    /* OBS polls key state about every 25 ms and can require two successive
     * samples of a modifier chord before it accepts the shortcut. */
    report.hold_ms = 75;
    return bt_hid_enqueue(&h->hid, &report, 1);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_hid_text(aether_bluetooth_handle_t h, const char *ascii)
{
    if (!h || !ascii) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    bt_hid_report_t reports[BT_HID_TEXT_MAX];
    size_t count;
    if (!bt_hid_ascii(ascii, reports, &count)) return ESP_ERR_INVALID_ARG;
    return bt_hid_enqueue(&h->hid, reports, count);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_hid_move(aether_bluetooth_handle_t h, int8_t dx, int8_t dy)
{
    if (!h) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    const bt_hid_report_t report = bt_hid_mouse(0, dx, dy);
    return bt_hid_enqueue(&h->hid, &report, 1);
#else
    (void)dx;
    (void)dy;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_hid_click(aether_bluetooth_handle_t h, uint8_t buttons)
{
    if (!h || !buttons || buttons > 7) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    bt_hid_report_t report = bt_hid_mouse(buttons, 0, 0);
    report.hold_ms = 34;
    return bt_hid_enqueue(&h->hid, &report, 1);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_hid_cancel_input(aether_bluetooth_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    bt_hid_cancel(&h->hid);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_hid_cancel_motion(aether_bluetooth_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    bt_hid_cancel_motion(&h->hid);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_hid_get_status(aether_bluetooth_handle_t h, aether_bluetooth_hid_status_t *status)
{
    if (!h || !status) return ESP_ERR_INVALID_ARG;
#if CONFIG_BT_HID_DEVICE_ENABLED
    portENTER_CRITICAL(&h->lock);
    bt_hid_status(&h->hid, status);
    if (h->hid_start_pending) {
        status->requested = true;
        status->last_error = ESP_OK;
    }
    portEXIT_CRITICAL(&h->lock);
    return ESP_OK;
#else
    *status = (aether_bluetooth_hid_status_t){.last_error = ESP_ERR_NOT_SUPPORTED};
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
esp_err_t aether_bluetooth_del(aether_bluetooth_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->requests, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&h->lock);
    atomic_store(&h->terminate,true); atomic_store(&h->desired,false);
    atomic_store(&h->full_stop_requested,true);
    ++h->request_revision;
#if CONFIG_BT_HID_DEVICE_ENABLED
    h->hid_requested = h->hid_start_pending = false;
    bt_hid_request(&h->hid, false);
#endif
    portEXIT_CRITICAL(&h->lock);
    sink_stop(h);
    xSemaphoreGive(h->requests);
    const int64_t deadline = esp_timer_get_time() + 15000000;
    while (atomic_load(&h->task_created) && !atomic_load(&h->done) && esp_timer_get_time() < deadline) pause_ms(10);
    if (atomic_load(&h->task_created) && !atomic_load(&h->done)) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&s_instance_lock); s_instance = NULL; unsigned callbacks = s_callbacks; portEXIT_CRITICAL(&s_instance_lock);
    while (callbacks) {
        pause_ms(1); portENTER_CRITICAL(&s_instance_lock); callbacks = s_callbacks; portEXIT_CRITICAL(&s_instance_lock);
    }
    vSemaphoreDelete(h->producer); vSemaphoreDelete(h->requests);
    vQueueDelete(h->commands); vQueueDelete(h->remote); free(h);
    return ESP_OK;
}
