#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Standard UAC1 PCM only. The pinned official driver rejects UAC2. Application
 * transport is always48kHz PCM16 stereo; mono and44.1kHz endpoints are adapted. */
typedef struct {
    bool installed, stopping, cleanup_pending;
    bool playback_connected, capture_connected;
    bool playback_ready, capture_ready;
    bool playback_active, capture_active;
    bool hardware_volume_supported;
    uint32_t generation;
    uint32_t playback_rate_hz, capture_rate_hz;
    uint8_t playback_channels, capture_channels;
    uint16_t playback_vid, playback_pid, capture_vid, capture_pid;
    uint32_t tx_fifo_bytes, rx_fifo_bytes;
    uint64_t tx_queued_bytes, tx_submitted_bytes, rx_received_bytes;
    uint32_t tx_timeouts, tx_low_watermarks, rx_overruns, transfer_errors, disconnects;
    uint32_t unsupported_interfaces;
    esp_err_t playback_error, capture_error, last_error;
} aether_usb_audio_status_t;

/* Lazy resources/worker only: no PHY, host-core, USB client or power I/O. */
esp_err_t aether_usb_audio_init(void);
/* Multi-class lifecycle hooks. start follows the shared core installation;
 * stop publication is nonblocking. poll is cache-only and retains NOT_FINISHED
 * until streams/devices and the UAC client are safely closed/uninstalled. */
esp_err_t aether_usb_audio_class_start(void *context);
void aether_usb_audio_class_request_stop(void *context);
esp_err_t aether_usb_audio_class_poll_stopped(void *context);
esp_err_t aether_usb_audio_get_status(aether_usb_audio_status_t *status);
bool aether_usb_audio_playback_connected(void);
bool aether_usb_audio_capture_connected(void);

/* Outside LVGL. Start/stop requests run on the USB-audio worker; callers wait
 * only their timeout. A timed-out start is cancelled. Stop errors preserve
 * ownership and are retryable. No callbacks call into codec/application locks. */
esp_err_t aether_usb_audio_playback_start(uint32_t timeout_ms);
esp_err_t aether_usb_audio_playback_stop(uint32_t timeout_ms);
esp_err_t aether_usb_audio_capture_start(uint32_t timeout_ms);
esp_err_t aether_usb_audio_capture_stop(uint32_t timeout_ms);
/* Whole-block FIFO contracts: ESP_OK means all bytes queued/read; error never
 * reports a partial block as success. bytes must be a nonzero multiple of4,
 * max32768. No caller buffer is retained by DMA or used after return. USB DMA
 * belongs to the official driver. Existing codec route supplies the one volume
 * gain, so this normalized transport uses unity gain. */
esp_err_t aether_usb_audio_write(const void *pcm, size_t bytes, uint32_t timeout_ms);
esp_err_t aether_usb_audio_capture_read(void *pcm, size_t bytes, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
