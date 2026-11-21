#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ac101s_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AETHER_BLUETOOTH_MAX_DEVICES 16U
#define AETHER_BLUETOOTH_NAME_BYTES 64U

typedef struct aether_bluetooth *aether_bluetooth_handle_t;
typedef enum {
    AETHER_BT_OFF, AETHER_BT_STARTING, AETHER_BT_IDLE, AETHER_BT_SCANNING,
    AETHER_BT_CONNECTING, AETHER_BT_CONNECTED, AETHER_BT_DISCONNECTING,
    AETHER_BT_STOPPING, AETHER_BT_ERROR
} aether_bluetooth_state_t;
typedef struct {
    uint8_t address[6];
    char name[AETHER_BLUETOOTH_NAME_BYTES];
    int8_t rssi;
    bool audio_device;
} aether_bluetooth_device_t;
typedef struct {
    aether_bluetooth_state_t state;
    bool enabled, audio_requested, scanning, connected, streaming;
    uint32_t sample_rate;
    uint8_t volume, peer_volume, device_count, channels;
    uint32_t revision, underruns, remote_events_dropped;
    uint64_t written_frames, transmitted_frames;
    size_t queued_frames;
    aether_bluetooth_device_t peer;
    esp_err_t last_error;
    /* Raw SDK event values for the latest connection attempt; -1 = not seen.
     * ACL status/reason values can be HCI codes, not esp_err_t values. */
    int auth_status, acl_status, acl_disconnect_reason, a2dp_disconnect_reason;
    uint32_t connection_attempts;
} aether_bluetooth_status_t;
typedef enum {
    AETHER_BT_REMOTE_PLAY, AETHER_BT_REMOTE_PAUSE, AETHER_BT_REMOTE_STOP,
    AETHER_BT_REMOTE_NEXT, AETHER_BT_REMOTE_PREVIOUS,
    AETHER_BT_REMOTE_VOLUME, AETHER_BT_REMOTE_VOLUME_UP, AETHER_BT_REMOTE_VOLUME_DOWN
} aether_bluetooth_remote_action_t;
typedef struct {
    aether_bluetooth_remote_action_t action;
    uint8_t volume;
} aether_bluetooth_remote_event_t;
typedef struct { const char *device_name; } aether_bluetooth_config_t;
typedef struct {
    bool requested, discoverable, connected, confirm_pending, reconnecting;
    uint32_t confirm_code;
    esp_err_t last_error;
} aether_bluetooth_hid_status_t;

/* Allocates control objects only. A headset or HID enable request starts the
 * owner task, controller and host. NVS must already be ready. */
esp_err_t aether_bluetooth_new(const aether_bluetooth_config_t *config,
                               aether_bluetooth_handle_t *out_handle);
/* Enable requests the headset service. Disable shuts down both headset and
 * HID. Observe get_status.last_error/state/revision for async completion. */
esp_err_t aether_bluetooth_set_enabled(aether_bluetooth_handle_t handle, bool enabled);
esp_err_t aether_bluetooth_scan(aether_bluetooth_handle_t handle);
esp_err_t aether_bluetooth_connect(aether_bluetooth_handle_t handle, const uint8_t address[6]);
esp_err_t aether_bluetooth_disconnect(aether_bluetooth_handle_t handle);
esp_err_t aether_bluetooth_set_volume(aether_bluetooth_handle_t handle, uint8_t volume);
esp_err_t aether_bluetooth_get_status(aether_bluetooth_handle_t handle, aether_bluetooth_status_t *status);
esp_err_t aether_bluetooth_get_device(aether_bluetooth_handle_t handle, size_t index,
                                     aether_bluetooth_device_t *device);
/* Nonblocking dequeue for the app audio service. BT host callbacks never call
 * audio/player/GUI code or take those modules' mutexes. */
bool aether_bluetooth_poll_remote(aether_bluetooth_handle_t handle,
                                  aether_bluetooth_remote_event_t *event);
/* Sink fixed input 48kHz PCM16 stereo; adapter handles negotiated peer rate.
 * start/stop/write run only in app tasks. Detach sink before terminal del. */
esp_err_t aether_bluetooth_get_sink(aether_bluetooth_handle_t handle, ac101s_audio_sink_t *sink);
/* Classic HID keyboard/mouse. Disabling HID without a headset service closes
 * the profiles/host while retaining the enabled ESP32-S31 controller. Use
 * set_enabled(false) to explicitly stop the shared controller as well. */
esp_err_t aether_bluetooth_hid_set_enabled(aether_bluetooth_handle_t handle, bool enabled);
esp_err_t aether_bluetooth_hid_confirm(aether_bluetooth_handle_t handle, bool accepted);
esp_err_t aether_bluetooth_hid_key(aether_bluetooth_handle_t handle, uint8_t modifiers, uint8_t key);
/* Send at most 48 printable ASCII characters with a US keyboard layout. */
esp_err_t aether_bluetooth_hid_text(aether_bluetooth_handle_t handle, const char *ascii);
esp_err_t aether_bluetooth_hid_move(aether_bluetooth_handle_t handle, int8_t dx, int8_t dy);
esp_err_t aether_bluetooth_hid_click(aether_bluetooth_handle_t handle, uint8_t buttons);
esp_err_t aether_bluetooth_hid_cancel_input(aether_bluetooth_handle_t handle);
/* Discard a queued relative movement while keeping keyboard and button reports. */
esp_err_t aether_bluetooth_hid_cancel_motion(aether_bluetooth_handle_t handle);
esp_err_t aether_bluetooth_hid_get_status(aether_bluetooth_handle_t handle, aether_bluetooth_hid_status_t *status);
/* Terminal cleanup, not the radio switch: official S31 controller deinit
 * cannot be followed by init. Radio off retains initialized controller only. */
esp_err_t aether_bluetooth_del(aether_bluetooth_handle_t handle);

#ifdef __cplusplus
}
#endif
