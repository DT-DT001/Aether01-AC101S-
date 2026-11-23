#include "app_audio_system.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "aether_usb_audio.h"

static struct {
    portMUX_TYPE lock;
    ac101s_audio_handle_t audio;
    aether_bluetooth_handle_t bluetooth;
    bool bluetooth_volume_dirty;
    bool usb_initialized;
    TaskHandle_t task;
    app_audio_system_snapshot_t view;
    uint8_t last_nonzero;
    uint32_t revision, applied_revision, save_revision;
    TickType_t changed;
    bool (*busy)(void *);
    void *busy_context;
    void (*control)(aether_bluetooth_remote_action_t, void *);
    void *control_context;
    bool retry_apply;
    bool quiesce_requested;
    uint32_t retry_revision;
    app_audio_output_t retry_output;
    TickType_t retry_changed;
} s = {.lock = portMUX_INITIALIZER_UNLOCKED};

static void notify(void) { if (s.task) xTaskNotifyGive(s.task); }

static esp_err_t usb_start(void *unused) { (void)unused; return aether_usb_audio_playback_start(1000); }
static esp_err_t usb_stop(void *unused) { (void)unused; return aether_usb_audio_playback_stop(500); }
static esp_err_t usb_write(void *unused, const void *pcm, size_t bytes, uint32_t timeout)
{ (void)unused; return aether_usb_audio_write(pcm, bytes, timeout); }
static bool usb_connected(void *unused) { (void)unused; return aether_usb_audio_playback_connected(); }
static esp_err_t usb_capture_start(void *unused) { (void)unused; return aether_usb_audio_capture_start(1000); }
static esp_err_t usb_capture_stop(void *unused) { (void)unused; return aether_usb_audio_capture_stop(500); }
static esp_err_t usb_read(void *unused, void *pcm, size_t bytes, uint32_t timeout)
{ (void)unused; return aether_usb_audio_capture_read(pcm, bytes, timeout); }
static bool usb_mic_connected(void *unused) { (void)unused; return aether_usb_audio_capture_connected(); }

static app_audio_output_t public_output(ac101s_audio_output_t output)
{
    return output == AC101S_OUTPUT_EXTERNAL ? APP_AUDIO_OUTPUT_BLUETOOTH :
           output == AC101S_OUTPUT_USB ? APP_AUDIO_OUTPUT_USB : APP_AUDIO_OUTPUT_SPEAKER;
}

static ac101s_audio_output_t codec_output(app_audio_output_t output)
{
    return output == APP_AUDIO_OUTPUT_BLUETOOTH ? AC101S_OUTPUT_EXTERNAL :
           output == APP_AUDIO_OUTPUT_USB ? AC101S_OUTPUT_USB : AC101S_OUTPUT_SPEAKER;
}

static void service_once(void)
{
        portENTER_CRITICAL(&s.lock);
        const bool usb_initialized = s.usb_initialized;
        portEXIT_CRITICAL(&s.lock);
        const bool usb_out = usb_initialized && aether_usb_audio_playback_connected();
        const bool usb_in = usb_initialized && aether_usb_audio_capture_connected();
        portENTER_CRITICAL(&s.lock);
        s.view.usb_playback_connected = usb_out;
        s.view.usb_capture_connected = usb_in;
        const bool quiesced = s.quiesce_requested;
        /* Output removal falls back to the speaker. USB microphone selection
         * stays explicit: substituting board MIC while an asynchronous record
         * start is preparing would silently record the wrong source. */
        if (!quiesced && !usb_out && s.view.output == APP_AUDIO_OUTPUT_USB) {
            if (!usb_out && s.view.output == APP_AUDIO_OUTPUT_USB) s.view.output = APP_AUDIO_OUTPUT_SPEAKER;
            ++s.revision; s.view.pending = true;
        }
        s.view.controls_quiesced = quiesced;
        const uint32_t revision = s.revision;
        const bool pending = revision != s.applied_revision;
        const uint8_t volume = s.view.applied_volume;
        const app_audio_output_t output = s.view.output;
        const app_audio_input_t input = s.view.input, previous_input = s.view.applied_input;
        aether_bluetooth_handle_t bluetooth = s.bluetooth;
        const bool announce_volume = !quiesced && bluetooth && s.bluetooth_volume_dirty;
        if (announce_volume) s.bluetooth_volume_dirty = false;
        const app_audio_output_t previous_output = s.view.applied_output;
        portEXIT_CRITICAL(&s.lock);
        aether_bluetooth_status_t bt = {0};
        const bool connected = bluetooth && aether_bluetooth_get_status(bluetooth, &bt) == ESP_OK &&
            bt.enabled && bt.connected && bt.state == AETHER_BT_CONNECTED && bt.sample_rate && bt.channels;
        /* Losing a headphone must never start unexpected speaker playback.
         * Preserve the user's route; the media worker stops on write error. */
        const app_audio_output_t target = output;
        const bool retry_deferred = s.retry_apply && s.retry_revision == revision && s.retry_output == target &&
            (TickType_t)(xTaskGetTickCount() - s.retry_changed) < pdMS_TO_TICKS(250);
        if (!quiesced && (pending || target != previous_output || input != previous_input) && !retry_deferred) {
            esp_err_t error = ESP_OK;
            if (input != previous_input)
                error = ac101s_audio_set_input(s.audio, input == APP_AUDIO_INPUT_USB ? AC101S_INPUT_USB : AC101S_INPUT_BOARD);
            if (error == ESP_OK) {
                if (target == APP_AUDIO_OUTPUT_BLUETOOTH && !connected && previous_output != APP_AUDIO_OUTPUT_BLUETOOTH)
                    error = ESP_ERR_INVALID_STATE;
                else if (target == APP_AUDIO_OUTPUT_USB && !usb_out) error = ESP_ERR_INVALID_STATE;
                else error = ac101s_audio_set_output(s.audio, codec_output(target));
            }
            ac101s_audio_output_t applied_output;
            ac101s_audio_input_t applied_input;
            const esp_err_t output_error = ac101s_audio_get_output(s.audio, &applied_output);
            const esp_err_t input_error = ac101s_audio_get_input(s.audio, &applied_input);
            if (error == ESP_OK) error = output_error;
            if (error == ESP_OK) error = input_error;
            s.retry_apply = error != ESP_OK;
            s.retry_revision = revision; s.retry_output = target; s.retry_changed = xTaskGetTickCount();
            portENTER_CRITICAL(&s.lock);
            if (error == ESP_OK) s.applied_revision = revision;
            if (output_error == ESP_OK) s.view.applied_output = public_output(applied_output);
            if (input_error == ESP_OK) s.view.applied_input = applied_input == AC101S_INPUT_USB ? APP_AUDIO_INPUT_USB : APP_AUDIO_INPUT_BOARD;
            s.view.error = error;
            s.view.pending = s.revision != s.applied_revision;
            portEXIT_CRITICAL(&s.lock);
        }
        if (announce_volume && aether_bluetooth_set_volume(bluetooth, volume) != ESP_OK) {
            portENTER_CRITICAL(&s.lock);
            s.bluetooth_volume_dirty = true;
            portEXIT_CRITICAL(&s.lock);
        }
        portENTER_CRITICAL(&s.lock);
        if (s.view.output == APP_AUDIO_OUTPUT_BLUETOOTH && s.view.applied_output == APP_AUDIO_OUTPUT_BLUETOOTH) {
            if (!connected) s.view.error = ESP_ERR_INVALID_STATE;
            else if (!s.retry_apply && s.applied_revision == s.revision && s.view.error == ESP_ERR_INVALID_STATE)
                s.view.error = ESP_OK;
        }
        portEXIT_CRITICAL(&s.lock);
        if (bluetooth) {
            aether_bluetooth_remote_event_t event;
            /* Bound each pass so an event flood cannot starve settings. */
            for (unsigned i = 0; i < 16 && aether_bluetooth_poll_remote(bluetooth, &event); ++i) {
                if (quiesced) continue;
                if (event.action == AETHER_BT_REMOTE_VOLUME_UP || event.action == AETHER_BT_REMOTE_VOLUME_DOWN) {
                    app_audio_system_snapshot_t view;
                    app_audio_system_get_snapshot(&view);
                    int value = (int)view.master_volume + (event.action == AETHER_BT_REMOTE_VOLUME_UP ? 5 : -5);
                    app_audio_system_set_volume((uint8_t)(value < 0 ? 0 : value > 100 ? 100 : value), true);
                } else if (event.action != AETHER_BT_REMOTE_VOLUME) {
                    portENTER_CRITICAL(&s.lock);
                    void (*control)(aether_bluetooth_remote_action_t, void *) = s.control;
                    void *argument = s.control_context;
                    portEXIT_CRITICAL(&s.lock);
                    if (control) control(event.action, argument);
                }
            }
        }
        portENTER_CRITICAL(&s.lock);
        const bool dirty = s.view.persistence_pending;
        const uint32_t save_revision = s.save_revision;
        const TickType_t changed = s.changed;
        const uint8_t saved_volume = s.view.master_volume, saved_nonzero = s.last_nonzero;
        bool (*busy)(void *) = s.busy;
        void *argument = s.busy_context;
        portEXIT_CRITICAL(&s.lock);
        const bool deferred = dirty && busy && busy(argument);
        portENTER_CRITICAL(&s.lock);
        s.view.persistence_deferred = deferred;
        portEXIT_CRITICAL(&s.lock);
        if (dirty && !deferred && xTaskGetTickCount() - changed >= pdMS_TO_TICKS(1000)) {
            nvs_handle_t nvs;
            esp_err_t error = nvs_open("sys_audio", NVS_READWRITE, &nvs);
            if (error == ESP_OK) {
                error = nvs_set_u8(nvs, "master", saved_volume);
                if (error == ESP_OK) error = nvs_set_u8(nvs, "unmute", saved_nonzero);
                if (error == ESP_OK) error = nvs_commit(nvs);
                nvs_close(nvs);
            }
            portENTER_CRITICAL(&s.lock);
            s.view.persistence_error = error;
            if (s.save_revision == save_revision) {
                s.view.persistence_pending = error != ESP_OK;
                if (error != ESP_OK) s.changed = xTaskGetTickCount();
            }
            portEXIT_CRITICAL(&s.lock);
        }
}

static void service(void *context)
{
    (void)context;
    for (;;) {
        service_once();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
    }
}

esp_err_t app_audio_system_init(ac101s_audio_handle_t audio)
{
    if (!audio) return ESP_ERR_INVALID_ARG;
    if (s.audio) return ESP_ERR_INVALID_STATE;
    uint8_t volume = 70, last = 70;
    nvs_handle_t nvs;
    esp_err_t error = nvs_open("sys_audio", NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        uint8_t value;
        if (nvs_get_u8(nvs, "master", &value) == ESP_OK && value <= 100) volume = value;
        if (nvs_get_u8(nvs, "unmute", &value) == ESP_OK && value && value <= 100) last = value;
        nvs_close(nvs);
    }
    const esp_err_t volume_error = ac101s_audio_set_master_volume(audio, volume);
    if (volume_error != ESP_OK) return volume_error;
    s.audio = audio;
    s.last_nonzero = volume ? volume : last;
    s.view = (app_audio_system_snapshot_t){.master_volume = volume, .applied_volume = volume,
        .persistence_error = error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error};
    if (xTaskCreate(service, "audio_system", 4096, NULL, 4, &s.task) != pdPASS) {
        s.audio = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s.lock);
    s.view.ready = true;
    portEXIT_CRITICAL(&s.lock);
    return ESP_OK;
}

void app_audio_system_get_snapshot(app_audio_system_snapshot_t *view)
{
    if (!view) return;
    portENTER_CRITICAL(&s.lock); *view = s.view; portEXIT_CRITICAL(&s.lock);
}

esp_err_t app_audio_system_set_volume(uint8_t volume, bool persist)
{
    if (volume > 100) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s.lock);
    if (!s.view.ready || s.quiesce_requested) { portEXIT_CRITICAL(&s.lock); return ESP_ERR_INVALID_STATE; }
    const bool changed = volume != s.view.master_volume;
    if (changed) {
        /* This setter only publishes an atomic target. The PCM writer slews
         * to it while route changes and radio metadata stay on the service. */
        const esp_err_t result = ac101s_audio_set_master_volume(s.audio, volume);
        if (result != ESP_OK) { portEXIT_CRITICAL(&s.lock); return result; }
        s.view.master_volume = s.view.applied_volume = volume;
        s.bluetooth_volume_dirty = true;
    }
    if (volume) s.last_nonzero = volume;
    s.changed = xTaskGetTickCount();
    if (persist) { s.view.persistence_pending = true; ++s.save_revision; }
    /* A preview may supersede a value already awaiting persistence. */
    else if (s.view.persistence_pending) ++s.save_revision;
    const bool wake_service = persist || (changed && s.bluetooth);
    portEXIT_CRITICAL(&s.lock);
    if (wake_service) notify();
    return ESP_OK;
}

esp_err_t app_audio_system_toggle_mute(bool persist)
{
    portENTER_CRITICAL(&s.lock);
    const uint8_t volume = s.view.master_volume ? 0 : s.last_nonzero;
    portEXIT_CRITICAL(&s.lock);
    return app_audio_system_set_volume(volume, persist);
}

esp_err_t app_audio_system_set_output(app_audio_output_t output)
{
    if (output != APP_AUDIO_OUTPUT_SPEAKER && output != APP_AUDIO_OUTPUT_BLUETOOTH && output != APP_AUDIO_OUTPUT_USB) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s.lock);
    if (!s.view.ready || s.quiesce_requested) { portEXIT_CRITICAL(&s.lock); return ESP_ERR_INVALID_STATE; }
    if (output == APP_AUDIO_OUTPUT_USB && !s.view.usb_playback_connected) { portEXIT_CRITICAL(&s.lock); return ESP_ERR_INVALID_STATE; }
    s.view.output = output; ++s.revision; s.view.pending = true;
    portEXIT_CRITICAL(&s.lock);
    notify();
    return ESP_OK;
}

esp_err_t app_audio_system_set_input(app_audio_input_t input)
{
    if (input != APP_AUDIO_INPUT_BOARD && input != APP_AUDIO_INPUT_USB) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s.lock);
    if (!s.view.ready || s.quiesce_requested || (input == APP_AUDIO_INPUT_USB && !s.view.usb_capture_connected)) {
        portEXIT_CRITICAL(&s.lock); return ESP_ERR_INVALID_STATE;
    }
    s.view.input = input; ++s.revision; s.view.pending = true;
    portEXIT_CRITICAL(&s.lock);
    notify();
    return ESP_OK;
}

esp_err_t app_audio_system_set_usb_audio(bool initialized)
{
    if (!s.audio) return ESP_ERR_INVALID_STATE;
    const ac101s_audio_sink_t sink = {.start = usb_start, .stop = usb_stop, .write = usb_write, .connected = usb_connected};
    const ac101s_audio_source_t source = {.start = usb_capture_start, .stop = usb_capture_stop,
        .read = usb_read, .connected = usb_mic_connected};
    esp_err_t error = ac101s_audio_set_usb_sink(s.audio, initialized ? &sink : NULL);
    if (error == ESP_OK) error = ac101s_audio_set_usb_source(s.audio, initialized ? &source : NULL);
    portENTER_CRITICAL(&s.lock);
    if (error == ESP_OK) s.usb_initialized = initialized;
    else s.view.error = error;
    portEXIT_CRITICAL(&s.lock);
    notify();
    return error;
}

void app_audio_system_set_persistence_guard(bool (*busy)(void *), void *argument)
{
    portENTER_CRITICAL(&s.lock); s.busy = busy; s.busy_context = argument; portEXIT_CRITICAL(&s.lock);
    notify();
}

void app_audio_system_set_bluetooth(aether_bluetooth_handle_t bluetooth)
{
    if (!s.audio) return;
    ac101s_audio_sink_t sink;
    esp_err_t error = bluetooth ? aether_bluetooth_get_sink(bluetooth, &sink) : ESP_OK;
    if (error == ESP_OK) error = ac101s_audio_set_external_sink(s.audio, bluetooth ? &sink : NULL);
    portENTER_CRITICAL(&s.lock);
    if (error == ESP_OK) {
        s.bluetooth = bluetooth;
        s.bluetooth_volume_dirty = bluetooth != NULL;
    }
    else s.view.error = error;
    portEXIT_CRITICAL(&s.lock);
    notify();
}

aether_bluetooth_handle_t app_audio_system_bluetooth(void)
{
    portENTER_CRITICAL(&s.lock); aether_bluetooth_handle_t bluetooth = s.bluetooth; portEXIT_CRITICAL(&s.lock);
    return bluetooth;
}

void app_audio_system_set_media_control(void (*control)(aether_bluetooth_remote_action_t, void *), void *argument)
{
    portENTER_CRITICAL(&s.lock); s.control = control; s.control_context = argument; portEXIT_CRITICAL(&s.lock);
}

void app_audio_system_quiesce_controls(bool quiesce) {
    portENTER_CRITICAL(&s.lock);
    aether_bluetooth_handle_t bluetooth = s.bluetooth;
    if (quiesce) s.quiesce_requested = true;
    portEXIT_CRITICAL(&s.lock);
    /* Keep the gate closed while discarding events from the prior session.
     * An owner scheduled during the drain must still discard, never dispatch. */
    if (!quiesce && bluetooth) {
        aether_bluetooth_remote_event_t event;
        for (unsigned i=0; i<64 && aether_bluetooth_poll_remote(bluetooth,&event); ++i) { }
    }
    if (!quiesce) {
        portENTER_CRITICAL(&s.lock);
        s.quiesce_requested = false;
        s.view.controls_quiesced = false;
        portEXIT_CRITICAL(&s.lock);
    }
    notify();
}
