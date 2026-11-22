/* SPDX-License-Identifier: Apache-2.0 */

#include "aether_usb_camera.h"
#include "camera_uvc_rx.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/uvc_host.h"

enum {
    CLIENT_CORE = 0,
    QUEUED_FRAMES = 2,
    FRAME_BUFFERS = QUEUED_FRAMES + 2,
    PENDING_DEVICES = 8,
    MAX_DESCRIPTORS = 192,
    MJPEG_PADDING_LIMIT = 1024,
    FRAME_TIMEOUT_US = 3000000,
    INTERVALS_PER_SECOND = 10000000,
    MAX_FRAME_INTERVAL = INTERVALS_PER_SECOND,
};

typedef struct {
    uint8_t address, stream_index;
    size_t count;
} camera_interface_t;

typedef struct {
    const uvc_host_frame_t *frame;
    size_t size;
    int64_t time_us;
} queued_frame_t;

static const char *TAG = "usb_camera";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    _Atomic(SemaphoreHandle_t) wake, frame_ready;
    StaticSemaphore_t wake_storage, frame_ready_storage;
    bool worker_alive, start_complete, accepting, leased;
    aether_usb_camera_status_t status;
    camera_interface_t selected, pending[PENDING_DEVICES];
    unsigned pending_count;
    uvc_host_stream_hdl_t stream;
    uint32_t command;
    queued_frame_t frames[QUEUED_FRAMES];
    unsigned frame_read, frame_count;
    int64_t last_frame_time;
    esp_err_t cleanup_error;
} s = {
    .status.requested_mode = AETHER_USB_CAMERA_NO_MODE,
    .status.active_mode = AETHER_USB_CAMERA_NO_MODE,
};

static void wake_worker(void)
{
    SemaphoreHandle_t wake = atomic_load(&s.wake);
    if (wake) {
        xSemaphoreGive(wake);
    }
}

static void wake_consumer(void)
{
    SemaphoreHandle_t ready = atomic_load(&s.frame_ready);
    if (ready) {
        xSemaphoreGive(ready);
    }
}

/* Called with s_lock held. Outstanding leases still belong to the stream. */
static void invalidate_stream(void)
{
    s.accepting = false;
    s.status.streaming = false;
    s.status.active_mode = AETHER_USB_CAMERA_NO_MODE;
}

static void reset_stream_diagnostics(void)
{
    s.status.failure = AETHER_USB_CAMERA_FAILURE_NONE;
    s.status.valid_frames = 0;
    s.status.rejected_frames = 0;
    s.status.buffer_errors = 0;
    s.status.rx = (aether_usb_camera_rx_status_t){0};
}

void aether_uvc_rx_observe(const void *stream, const aether_usb_camera_rx_status_t *sample)
{
    portENTER_CRITICAL(&s_lock);
    if (stream == s.stream && !s.status.stopping && s.status.failure != AETHER_USB_CAMERA_FAILURE_FRAMES) {
        aether_usb_camera_rx_status_t *rx = &s.status.rx;
        if (!rx->packets && sample->packets) {
            memcpy(rx->first_header, sample->first_header, sizeof(rx->first_header));
        }
        rx->transfers += sample->transfers;
        rx->packets += sample->packets;
        rx->empty += sample->empty;
        rx->skipped += sample->skipped;
        rx->errors += sample->errors;
        rx->invalid_headers += sample->invalid_headers;
        rx->frame_ids += sample->frame_ids;
        rx->frame_ends += sample->frame_ends;
        rx->invalid_soi += sample->invalid_soi;
        if (sample->max_packet_bytes > rx->max_packet_bytes) {
            rx->max_packet_bytes = sample->max_packet_bytes;
        }
        if (sample->transfer_status) {
            rx->transfer_status = sample->transfer_status;
        }
        if (sample->submit_error != ESP_OK) {
            rx->submit_error = sample->submit_error;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static void clear_selection(void)
{
    invalidate_stream();
    reset_stream_diagnostics();
    s.selected = (camera_interface_t){0};
    s.status.connected = false;
    s.status.mode_count = 0;
    s.status.requested_mode = AETHER_USB_CAMERA_NO_MODE;
    ++s.status.generation;
    ++s.command;
}

static void driver_event(const uvc_host_driver_event_data_t *event, void *context)
{
    (void)context;
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        return;
    }

    const camera_interface_t found = {
        .address = event->device_connected.dev_addr,
        .stream_index = event->device_connected.uvc_stream_index,
        .count = event->device_connected.frame_info_num,
    };
    bool posted = false;
    portENTER_CRITICAL(&s_lock);
    if (!s.status.stopping) {
        if (s.pending_count == PENDING_DEVICES) {
            --s.pending_count;
            memmove(s.pending, s.pending + 1, s.pending_count * sizeof(s.pending[0]));
        }
        s.pending[s.pending_count++] = found;
        posted = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (posted) {
        wake_worker();
    }
}

static void stream_event(const uvc_host_stream_event_data_t *event, void *context)
{
    (void)context;
    switch (event->type) {
    case UVC_HOST_DEVICE_DISCONNECTED:
        portENTER_CRITICAL(&s_lock);
        clear_selection();
        s.status.error = ESP_ERR_NOT_FOUND;
        portEXIT_CRITICAL(&s_lock);
        wake_worker();
        wake_consumer();
        break;
    case UVC_HOST_TRANSFER_ERROR:
        /* The standard driver may recover the transport. A frame deadline
         * handles a dead stream without restarting on each transient error. */
        ESP_LOGD(TAG, "USB transfer: %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        portENTER_CRITICAL(&s_lock);
        if (s.accepting) ++s.status.buffer_errors;
        portEXIT_CRITICAL(&s_lock);
        break;
    default:
        break;
    }
}

static aether_usb_camera_format_t frame_format(enum uvc_host_stream_format format)
{
    return format == UVC_VS_FORMAT_YUY2 ? AETHER_USB_CAMERA_YUY2 : AETHER_USB_CAMERA_MJPEG;
}

static bool reject_frame(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s.accepting) ++s.status.rejected_frames;
    portEXIT_CRITICAL(&s_lock);
    return true; /* Return the frame to the UVC pool. */
}

static bool frame_received(const uvc_host_frame_t *frame, void *context)
{
    (void)context;
    const uvc_host_stream_format_t *format = &frame->vs_format;
    size_t length = frame->data_len;
    if (length > frame->data_buffer_len || !frame->data) {
        return reject_frame();
    }
    if (format->format == UVC_VS_FORMAT_MJPEG) {
        if (length < 4 || frame->data[0] != 0xff || frame->data[1] != 0xd8) {
            return reject_frame();
        }
        /* Trim camera padding after EOI and reject incomplete frames. */
        const size_t floor = length > MJPEG_PADDING_LIMIT + 4 ? length - MJPEG_PADDING_LIMIT : 4;
        while (length >= floor && !(frame->data[length - 2] == 0xff && frame->data[length - 1] == 0xd9)) {
            --length;
        }
        if (length < floor) {
            return reject_frame();
        }
    } else if (format->format != UVC_VS_FORMAT_YUY2 ||
               length != (size_t)format->h_res * format->v_res * 2) {
        return reject_frame();
    }

    const int64_t now = esp_timer_get_time();
    bool retained = false;
    portENTER_CRITICAL(&s_lock);
    if (s.accepting) {
        ++s.status.valid_frames;
        s.last_frame_time = now;
        if (s.frame_count < QUEUED_FRAMES) {
            unsigned slot = (s.frame_read + s.frame_count) % QUEUED_FRAMES;
            s.frames[slot] = (queued_frame_t){ .frame = frame, .size = length, .time_us = now };
            ++s.frame_count;
            retained = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (retained) {
        wake_consumer();
    }
    return !retained;
}

static uint32_t preferred_interval(const uvc_host_frame_info_t *info)
{
    /* Use an advertised interval near or below 30 fps for MJPEG and 15 fps
     * for YUY2 where possible. The raw stream also needs CPU time to encode. */
    const uint32_t target = info->format == UVC_VS_FORMAT_YUY2 ? 666666 : 333333;
    if (info->interval_type) {
        uint32_t preferred = UINT32_MAX;
        uint32_t slowest = 0;
        const size_t count = info->interval_type < CONFIG_UVC_INTERVAL_ARRAY_SIZE
            ? info->interval_type : CONFIG_UVC_INTERVAL_ARRAY_SIZE;
        for (size_t i = 0; i < count; ++i) {
            uint32_t interval = info->interval[i];
            if (interval > MAX_FRAME_INTERVAL) {
                continue;
            }
            if (target <= interval && interval < preferred) {
                preferred = interval;
            }
            if (slowest < interval) {
                slowest = interval;
            }
        }
        if (preferred < UINT32_MAX) {
            return preferred;
        }
        if (slowest) {
            return slowest;
        }
    } else if (info->interval_min && info->interval_max >= info->interval_min) {
        const uint32_t min = info->interval_min;
        const uint32_t max = info->interval_max;
        const uint32_t step = info->interval_step;
        if (target <= min) {
            return min;
        }
        if (step) {
            uint64_t rounded = (uint64_t)min + ((uint64_t)target - min + step - 1) / step * step;
            return rounded <= max ? (uint32_t)rounded : min + (max - min) / step * step;
        }
        /* The driver accepts only the minimum when no interval step exists. */
        return min;
    }
    return info->default_interval;
}

static bool make_mode(const uvc_host_frame_info_t *info, aether_usb_camera_mode_t *mode)
{
    if (info->format != UVC_VS_FORMAT_MJPEG && info->format != UVC_VS_FORMAT_YUY2) {
        return false;
    }
    const bool raw = info->format == UVC_VS_FORMAT_YUY2;
    const unsigned width = info->h_res;
    const unsigned height = info->v_res;
    if (!width || !height || width > 1920 || height > 1920 ||
        width * height > (raw ? 1280 * 720 : 1920 * 1080) || (raw && (width & 1))) {
        return false;
    }
    const uint32_t interval = preferred_interval(info);
    if (!interval || interval > MAX_FRAME_INTERVAL) {
        return false;
    }
    *mode = (aether_usb_camera_mode_t){
        .width = width,
        .height = height,
        .interval_100ns = interval,
        .format = frame_format(info->format),
    };
    return true;
}

static int compare_modes(const void *left, const void *right)
{
    const aether_usb_camera_mode_t *a = left;
    const aether_usb_camera_mode_t *b = right;
    const int pixels_a = (int)a->width * a->height;
    const int pixels_b = (int)b->width * b->height;
    if (pixels_a != pixels_b) {
        return pixels_a - pixels_b;
    }
    if (a->width != b->width) {
        return (int)a->width - b->width;
    }
    return (int)a->format - b->format;
}

static bool address_present(uint8_t address)
{
    uint8_t addresses[16];
    int count;
    if (usb_host_device_addr_list_fill(sizeof(addresses), addresses, &count) != ESP_OK) {
        return false;
    }
    return count > 0 && memchr(addresses, address, (size_t)count) != NULL;
}

static void enumerate_pending(void)
{
    camera_interface_t found = {0};
    portENTER_CRITICAL(&s_lock);
    if (s.pending_count && !s.status.stopping && !s.stream && !s.status.mode_count) {
        found = s.pending[0];
        --s.pending_count;
        memmove(s.pending, s.pending + 1, s.pending_count * sizeof(s.pending[0]));
    }
    portEXIT_CRITICAL(&s_lock);
    if (!found.address || !address_present(found.address)) {
        return;
    }

    esp_err_t error = ESP_ERR_NOT_SUPPORTED;
    size_t count = found.count < MAX_DESCRIPTORS ? found.count : MAX_DESCRIPTORS;
    aether_usb_camera_mode_t modes[AETHER_USB_CAMERA_MAX_MODES];
    uint8_t mode_count = 0;
    if (count) {
        uvc_host_frame_info_t *descriptors = calloc(count, sizeof(*descriptors));
        error = descriptors ? uvc_host_get_frame_list(found.address, found.stream_index,
                                   (uvc_host_frame_info_t (*)[])descriptors, &count) : ESP_ERR_NO_MEM;
        if (error == ESP_OK) {
            for (size_t i = 0; i < count && mode_count < AETHER_USB_CAMERA_MAX_MODES; ++i) {
                aether_usb_camera_mode_t mode;
                if (!make_mode(&descriptors[i], &mode)) {
                    continue;
                }
                uint8_t j = 0;
                while (j < mode_count && compare_modes(&mode, &modes[j])) {
                    ++j;
                }
                if (j == mode_count) {
                    modes[mode_count++] = mode;
                }
            }
            qsort(modes, mode_count, sizeof(modes[0]), compare_modes);
            error = mode_count ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
        }
        free(descriptors);
    }
    if (!address_present(found.address)) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    if (!s.status.stopping) {
        s.selected = found;
        s.status.connected = true;
        s.status.mode_count = mode_count;
        s.status.error = error;
        memcpy(s.status.modes, modes, mode_count * sizeof(modes[0]));
        ++s.status.generation;
    }
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "UVC address %u, stream %u, %u usable modes (%s)",
             found.address, found.stream_index, mode_count, esp_err_to_name(error));
}

/* The worker is the only opener/closer. Callbacks publish events and frames
 * without waiting on an application task or any USB API. */
static void start_stream(uint32_t command, uint8_t index,
                         const camera_interface_t *camera, const aether_usb_camera_mode_t *mode)
{
    portENTER_CRITICAL(&s_lock);
    reset_stream_diagnostics();
    portEXIT_CRITICAL(&s_lock);
    const uvc_host_stream_config_t configuration = {
        .event_cb = stream_event,
        .frame_cb = frame_received,
        .usb = {
            .dev_addr = camera->address,
            .uvc_stream_index = camera->stream_index,
        },
        .vs_format = {
            .h_res = mode->width,
            .v_res = mode->height,
            .fps = (float)INTERVALS_PER_SECOND / mode->interval_100ns,
            .format = mode->format == AETHER_USB_CAMERA_YUY2 ? UVC_VS_FORMAT_YUY2 : UVC_VS_FORMAT_MJPEG,
        },
        .advanced = {
            .number_of_frame_buffers = FRAME_BUFFERS,
            /* Bounded even for devices that report an incorrect maximum MJPEG size. */
            .frame_size = (size_t)mode->width * mode->height * 2,
            .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            .number_of_urbs = 3,
        },
    };
    uvc_host_stream_hdl_t stream = NULL;
    aether_usb_camera_failure_t failure = AETHER_USB_CAMERA_FAILURE_OPEN;
    esp_err_t error = uvc_host_stream_open(&configuration, pdMS_TO_TICKS(250), &stream);
    portENTER_CRITICAL(&s_lock);
    s.stream = stream;
    const bool still_requested = !s.status.stopping && s.command == command;
    portEXIT_CRITICAL(&s_lock);

    if (error == ESP_OK && still_requested) {
        uvc_host_stream_format_t actual;
        failure = AETHER_USB_CAMERA_FAILURE_FORMAT;
        error = uvc_host_stream_format_get(stream, &actual);
        if (error == ESP_OK && (actual.h_res != mode->width || actual.v_res != mode->height ||
                               actual.format != configuration.vs_format.format)) {
            error = ESP_ERR_INVALID_RESPONSE;
        }
        if (error == ESP_OK) {
            failure = AETHER_USB_CAMERA_FAILURE_START;
            error = uvc_host_stream_start(stream);
        }
    }

    portENTER_CRITICAL(&s_lock);
    if (!s.status.stopping && s.command == command) {
        s.status.error = error;
        s.status.failure = error == ESP_OK ? AETHER_USB_CAMERA_FAILURE_NONE : failure;
        if (error == ESP_OK && stream) {
            s.status.active_mode = index;
            s.status.streaming = true;
            s.accepting = true;
            s.last_frame_time = esp_timer_get_time();
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "UVC %ux%u failed at stage %u: %s",
                 mode->width, mode->height, (unsigned)failure, esp_err_to_name(error));
    }
}

static bool close_stream(void)
{
    queued_frame_t queued[QUEUED_FRAMES];
    portENTER_CRITICAL(&s_lock);
    uvc_host_stream_hdl_t stream = s.stream;
    if (!stream || s.accepting) {
        portEXIT_CRITICAL(&s_lock);
        return stream == NULL;
    }
    const unsigned count = s.frame_count;
    for (unsigned i = 0; i < count; ++i) {
        queued[i] = s.frames[(s.frame_read + i) % QUEUED_FRAMES];
    }
    s.frame_read = s.frame_count = 0;
    const bool leased = s.leased;
    portEXIT_CRITICAL(&s_lock);

    for (unsigned i = 0; i < count; ++i) {
        esp_err_t error = uvc_host_frame_return(stream, (uvc_host_frame_t *)queued[i].frame);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Returning queued UVC frame: %s", esp_err_to_name(error));
        }
    }
    if (leased) {
        return false;
    }

    const esp_err_t error = uvc_host_stream_close(stream);
    portENTER_CRITICAL(&s_lock);
    if (error == ESP_OK) {
        s.stream = NULL;
    } else if (s.status.error == ESP_OK) {
        s.status.error = error;
    }
    portEXIT_CRITICAL(&s_lock);
    return error == ESP_OK;
}

static void camera_worker(void *unused)
{
    (void)unused;
    const SemaphoreHandle_t wake = atomic_load(&s.wake);
    bool ready;
    do {
        portENTER_CRITICAL(&s_lock);
        ready = s.start_complete;
        portEXIT_CRITICAL(&s_lock);
        if (!ready) {
            xSemaphoreTake(wake, portMAX_DELAY);
        }
    } while (!ready);

    uint32_t attempted_command = 0;
    int64_t address_check = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        const bool installed = s.status.installed;
        const bool stopping = s.status.stopping;
        const bool timed_out = s.accepting && now - s.last_frame_time > FRAME_TIMEOUT_US;
        aether_usb_camera_rx_status_t rx;
        if (timed_out) {
            invalidate_stream();
            s.status.error = ESP_ERR_TIMEOUT;
            s.status.failure = AETHER_USB_CAMERA_FAILURE_FRAMES;
            rx = s.status.rx;
        }
        const bool closing = s.stream && !s.accepting;
        const bool no_stream = !s.stream;
        const camera_interface_t selected = s.selected;
        portEXIT_CRITICAL(&s_lock);

        if (timed_out) {
            ESP_LOGW(TAG, "UVC RX: URB=%lu data=%lu empty=%lu skip=%lu error=%lu max=%lu "
                     "header=%lu FID=%lu EOF=%lu SOI error=%lu first=%02x%02x%02x%02x transport=%u submit=%s",
                     (unsigned long)rx.transfers, (unsigned long)rx.packets, (unsigned long)rx.empty,
                     (unsigned long)rx.skipped, (unsigned long)rx.errors, (unsigned long)rx.max_packet_bytes,
                     (unsigned long)rx.invalid_headers, (unsigned long)rx.frame_ids, (unsigned long)rx.frame_ends,
                     (unsigned long)rx.invalid_soi, rx.first_header[0], rx.first_header[1],
                     rx.first_header[2], rx.first_header[3], rx.transfer_status, esp_err_to_name(rx.submit_error));
        }

        if (!installed || (stopping && close_stream())) {
            break;
        }
        if (closing && !stopping) {
            (void)close_stream();
        }
        if (!stopping && now >= address_check) {
            address_check = now + 250000;
            usb_host_lib_info_t usb;
            if (usb_host_lib_info(&usb) == ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                s.status.usb_enumerated = usb.num_devices > 0;
                portEXIT_CRITICAL(&s_lock);
            }
            if (no_stream && selected.address && !address_present(selected.address)) {
                portENTER_CRITICAL(&s_lock);
                clear_selection();
                s.status.error = ESP_ERR_NOT_FOUND;
                portEXIT_CRITICAL(&s_lock);
            }
        }
        if (!stopping && no_stream) {
            enumerate_pending();

            portENTER_CRITICAL(&s_lock);
            const uint32_t command = s.command;
            const uint8_t index = s.status.requested_mode;
            const bool open = !s.status.stopping && s.selected.address &&
                              index < s.status.mode_count && command != attempted_command;
            aether_usb_camera_mode_t mode = {0};
            const camera_interface_t camera = s.selected;
            if (open) {
                mode = s.status.modes[index];
            }
            portEXIT_CRITICAL(&s_lock);
            if (open) {
                start_stream(command, index, &camera, &mode);
                attempted_command = command;
            }
        }
        xSemaphoreTake(wake, pdMS_TO_TICKS(30));
    }

    const esp_err_t error = s.status.installed ? uvc_host_uninstall() : ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (error == ESP_OK) {
        s.status.installed = false;
        s.status.stopping = false;
    } else {
        /* A client whose teardown was not confirmed must still hold the host
         * session. In particular, do not equate a timeout with deregistration. */
        s.status.error = error;
    }
    s.cleanup_error = error;
    s.worker_alive = false;
    portEXIT_CRITICAL(&s_lock);
    wake_consumer();
    vTaskDelete(NULL);
}

esp_err_t aether_usb_camera_class_start(void *context)
{
    (void)context;
    if (!atomic_load(&s.wake)) {
        atomic_store(&s.frame_ready, xSemaphoreCreateBinaryStatic(&s.frame_ready_storage));
        atomic_store(&s.wake, xSemaphoreCreateBinaryStatic(&s.wake_storage));
    }

    portENTER_CRITICAL(&s_lock);
    if (s.worker_alive || s.status.installed) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    clear_selection();
    s.pending_count = 0;
    s.frame_count = s.frame_read = 0;
    s.start_complete = false;
    s.status.stopping = false;
    s.status.usb_enumerated = false;
    s.status.error = ESP_OK;
    s.cleanup_error = ESP_OK;
    s.worker_alive = true;
    portEXIT_CRITICAL(&s_lock);

    if (xTaskCreatePinnedToCore(camera_worker, "usb_camera", 4096, NULL, 4, NULL,
                                CLIENT_CORE) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s.worker_alive = false;
        s.status.error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "UVC worker allocation failed");
        return ESP_OK;
    }
    const uvc_host_driver_config_t configuration = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 6,
        .xCoreID = CLIENT_CORE,
        .create_background_task = true,
        .event_cb = driver_event,
    };
    const esp_err_t error = uvc_host_install(&configuration);
    portENTER_CRITICAL(&s_lock);
    s.status.installed = error == ESP_OK;
    s.status.error = error;
    s.start_complete = true;
    portEXIT_CRITICAL(&s_lock);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "UVC client unavailable: %s", esp_err_to_name(error));
    }
    wake_worker();
    return ESP_OK;
}

void aether_usb_camera_class_request_stop(void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_lock);
    if (!s.status.stopping) {
        s.status.stopping = s.worker_alive || s.status.installed;
        s.pending_count = 0;
        clear_selection();
    }
    portEXIT_CRITICAL(&s_lock);
    wake_worker();
    wake_consumer();
}

esp_err_t aether_usb_camera_class_poll_stopped(void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_lock);
    const esp_err_t error = s.worker_alive ? ESP_ERR_NOT_FINISHED : s.cleanup_error;
    portEXIT_CRITICAL(&s_lock);
    return error;
}

esp_err_t aether_usb_camera_get_status(aether_usb_camera_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s.status;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t aether_usb_camera_select_mode(uint32_t generation, uint8_t index)
{
    esp_err_t error = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (!s.status.installed || s.status.stopping || generation != s.status.generation) {
        error = ESP_ERR_INVALID_STATE;
    } else if (index >= s.status.mode_count) {
        error = ESP_ERR_INVALID_ARG;
    } else if (s.status.active_mode != index || !s.status.streaming) {
        invalidate_stream();
        s.status.requested_mode = index;
        s.status.error = ESP_OK;
        ++s.command;
    }
    portEXIT_CRITICAL(&s_lock);
    if (error == ESP_OK) {
        wake_worker();
        wake_consumer();
    }
    return error;
}

void aether_usb_camera_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    invalidate_stream();
    s.status.requested_mode = AETHER_USB_CAMERA_NO_MODE;
    ++s.command;
    portEXIT_CRITICAL(&s_lock);
    wake_worker();
    wake_consumer();
}

esp_err_t aether_usb_camera_frame_acquire(aether_usb_camera_frame_t *frame, uint32_t timeout_ms)
{
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        if (!s.accepting || s.leased) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (s.frame_count) {
            const queued_frame_t item = s.frames[s.frame_read];
            s.frame_read = (s.frame_read + 1) % QUEUED_FRAMES;
            --s.frame_count;
            s.leased = true;
            const unsigned index = s.status.active_mode;
            const aether_usb_camera_mode_t nominal = s.status.modes[index];
            *frame = (aether_usb_camera_frame_t){
                .data = item.frame->data,
                .size = item.size,
                .time_us = item.time_us,
                .generation = s.status.generation,
                .mode = nominal,
                .private_frame = (uvc_host_frame_t *)item.frame,
                .private_stream = s.stream,
            };
            const float fps = item.frame->vs_format.fps;
            portEXIT_CRITICAL(&s_lock);
            if (fps >= 1.0f && fps <= 1000.0f) {
                frame->mode.interval_100ns = (uint32_t)((float)INTERVALS_PER_SECOND / fps + 0.5f);
            }
            return ESP_OK;
        }
        portEXIT_CRITICAL(&s_lock);

        const int64_t remaining = deadline - esp_timer_get_time();
        const SemaphoreHandle_t ready = atomic_load(&s.frame_ready);
        if (remaining <= 0 ||
            xSemaphoreTake(ready, pdMS_TO_TICKS((uint64_t)(remaining + 999) / 1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }
}

void aether_usb_camera_frame_release(aether_usb_camera_frame_t *frame)
{
    if (!frame || !frame->private_frame) {
        return;
    }
    const esp_err_t error = uvc_host_frame_return(frame->private_stream, frame->private_frame);
    *frame = (aether_usb_camera_frame_t){0};

    portENTER_CRITICAL(&s_lock);
    s.leased = false;
    const bool closing = !s.accepting;
    if (error != ESP_OK) {
        s.status.error = error;
    }
    portEXIT_CRITICAL(&s_lock);
    if (closing) {
        wake_worker();
    }
}
