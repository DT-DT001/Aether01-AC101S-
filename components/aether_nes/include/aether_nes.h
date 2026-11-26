#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "aether_storage.h"
#include "ac101s_audio.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AETHER_NES_FRAME_WIDTH  256U
#define AETHER_NES_FRAME_HEIGHT 224U
#define AETHER_NES_OUTPUT_WIDTH 384U
#define AETHER_NES_OUTPUT_HEIGHT 336U

/* Complete external-controller state, published without touching the core. */
typedef enum {
    AETHER_NES_INPUT_UP = 1U << 0,
    AETHER_NES_INPUT_DOWN = 1U << 1,
    AETHER_NES_INPUT_LEFT = 1U << 2,
    AETHER_NES_INPUT_RIGHT = 1U << 3,
    AETHER_NES_INPUT_A = 1U << 4,
    AETHER_NES_INPUT_B = 1U << 5,
    AETHER_NES_INPUT_SELECT = 1U << 6,
    AETHER_NES_INPUT_START = 1U << 7,
    AETHER_NES_INPUT_PAUSE = 1U << 8,
    AETHER_NES_INPUT_RESET = 1U << 9,
    AETHER_NES_INPUT_EXIT = 1U << 10,
} aether_nes_input_t;

typedef struct aether_nes_t *aether_nes_handle_t;

typedef enum {
    AETHER_NES_STOPPED = 0,
    AETHER_NES_STARTING,
    AETHER_NES_RUNNING,
    AETHER_NES_PAUSED,
    AETHER_NES_ERROR,
} aether_nes_state_t;

typedef struct {
    aether_nes_state_t state;
    esp_err_t last_error;
    uint16_t external_buttons;
    uint32_t frame_number;
    uint32_t emulated_frames;
    uint32_t skipped_frames;
    uint32_t audio_errors;
    uint32_t input_errors;
    uint32_t emulation_fps_x100;
    uint32_t render_fps_x100;
    uint16_t mapper_number;
    uint32_t prg_bytes, chr_bytes;
    bool repaired_diskdude;
    char detail[128];
    /* Mean wall time per emulated frame over the latest measurement window,
     * including preemption/blocking. These are not CPU utilization metrics. */
    uint32_t core_us_per_frame, apu_us_per_frame, audio_output_us_per_frame;
    uint32_t video_convert_us_per_frame, frame_callback_us_per_frame;
    uint32_t input_us_per_frame, yield_us_per_frame;
    size_t rom_size;
    char path[192];
} aether_nes_status_t;

/** Frame memory remains valid until the callback returns. */
typedef void (*aether_nes_frame_cb_t)(const uint16_t *pixels,
                                      uint16_t width, uint16_t height,
                                      uint32_t frame_number,
                                      void *user_ctx);

esp_err_t aether_nes_new(aether_storage_handle_t storage,
                         ac101s_audio_handle_t audio,
                         esp_lcd_touch_handle_t touch,
                         aether_nes_handle_t *out_handle);
/** Supports NTSC iNES 1.0 images accepted by the bundled Nofrendo mappers.
 * PAL-only and NES 2.0 headers fail with ESP_ERR_NOT_SUPPORTED; unsupported
 * mappers fail at core initialization. Compatibility still needs ROM testing. */
esp_err_t aether_nes_start(aether_nes_handle_t handle, const char *rom_path,
                           aether_nes_frame_cb_t frame_cb, void *user_ctx);
esp_err_t aether_nes_stop(aether_nes_handle_t handle);
/** Publish all held HID buttons; send zero on disconnect. Controls above bit7
 * act on press edges. Sleep consumes a held state until it is fully released.
 * Nonzero input is rejected outside a running session; zero always releases. */
esp_err_t aether_nes_set_external_input(aether_nes_handle_t handle, uint16_t buttons);
/** Queue an idempotent pause/resume on the emulator owner task. Supports
 * STARTING/RUNNING/PAUSED; status changes after the task applies the request. */
esp_err_t aether_nes_set_paused(aether_nes_handle_t handle, bool paused);
/* The screen pause is independent of the in-game pause. The power/UI owner
 * may set it before ROM loading and must clear it on wake, even when idle. */
esp_err_t aether_nes_set_screen_paused(aether_nes_handle_t handle, bool paused);
/* Called on the emulator task for a new touch; return true to consume the
 * entire gesture (for example while requesting a screen wake). */
esp_err_t aether_nes_set_input_gate(aether_nes_handle_t handle,
                                    bool (*gate)(void *), void *context);
/* Optional board-level reader for shared touch ownership. Install while idle. */
typedef esp_err_t (*aether_nes_touch_reader_t)(void *context, esp_lcd_touch_point_data_t *points,
                                              uint8_t *count, uint8_t capacity);
esp_err_t aether_nes_set_touch_reader(aether_nes_handle_t handle,
                                      aether_nes_touch_reader_t reader, void *context);
/** Stop and wait for callbacks, raw touch and audio to finish. Zero polls. */
esp_err_t aether_nes_stop_wait(aether_nes_handle_t handle, uint32_t timeout_ms);
/** Release the retained last frame when the emulator page is closed. */
void aether_nes_release_frame(aether_nes_handle_t handle);
esp_err_t aether_nes_get_status(aether_nes_handle_t handle,
                                aether_nes_status_t *status);
esp_err_t aether_nes_del(aether_nes_handle_t handle);

#ifdef __cplusplus
}
#endif
