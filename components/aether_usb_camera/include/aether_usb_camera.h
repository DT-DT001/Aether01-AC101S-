/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AETHER_USB_CAMERA_MAX_MODES 32
#define AETHER_USB_CAMERA_NO_MODE UINT8_MAX

typedef enum {
    AETHER_USB_CAMERA_MJPEG,
    AETHER_USB_CAMERA_YUY2,
} aether_usb_camera_format_t;

typedef struct {
    uint16_t width, height;
    uint32_t interval_100ns;
    aether_usb_camera_format_t format;
} aether_usb_camera_mode_t;

typedef enum {
    AETHER_USB_CAMERA_FAILURE_NONE,
    AETHER_USB_CAMERA_FAILURE_OPEN,
    AETHER_USB_CAMERA_FAILURE_FORMAT,
    AETHER_USB_CAMERA_FAILURE_START,
    AETHER_USB_CAMERA_FAILURE_FRAMES,
} aether_usb_camera_failure_t;

typedef struct {
    uint32_t transfers, packets, empty, skipped, errors;
    uint32_t invalid_headers, frame_ids, frame_ends, invalid_soi;
    uint32_t max_packet_bytes;
    uint8_t first_header[4];
    unsigned transfer_status;
    esp_err_t submit_error;
} aether_usb_camera_rx_status_t;

typedef struct {
    bool installed, stopping, usb_enumerated, connected, streaming;
    uint32_t generation;
    uint8_t mode_count, requested_mode, active_mode;
    esp_err_t error;
    aether_usb_camera_failure_t failure;
    uint32_t valid_frames, rejected_frames, buffer_errors;
    aether_usb_camera_rx_status_t rx;
    aether_usb_camera_mode_t modes[AETHER_USB_CAMERA_MAX_MODES];
} aether_usb_camera_status_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    int64_t time_us;
    uint32_t generation;
    aether_usb_camera_mode_t mode;
    void *private_frame;
    void *private_stream;
} aether_usb_camera_frame_t;

/* The shared USB Host owns the root port and its power policy. */
esp_err_t aether_usb_camera_class_start(void *context);
void aether_usb_camera_class_request_stop(void *context);
esp_err_t aether_usb_camera_class_poll_stopped(void *context);

esp_err_t aether_usb_camera_get_status(aether_usb_camera_status_t *status);
/* Nonblocking single-camera commands. The generation belongs to the mode
 * list returned with status and prevents stale selection after hot removal. */
esp_err_t aether_usb_camera_select_mode(uint32_t generation, uint8_t index);
void aether_usb_camera_stop(void);

/* Single consumer. Successful calls lease the UVC buffer until release. The
 * consumer must release each frame exactly once before waiting for stop. */
esp_err_t aether_usb_camera_frame_acquire(aether_usb_camera_frame_t *frame, uint32_t timeout_ms);
void aether_usb_camera_frame_release(aether_usb_camera_frame_t *frame);

#ifdef __cplusplus
}
#endif
