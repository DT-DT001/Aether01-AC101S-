#pragma once

#include <stdatomic.h>
#include "aether_nes.h"
#include "aether_media_fs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AETHER_NES_ROM_MAX_BYTES (8U * 1024U * 1024U)

struct aether_nes_t {
    aether_storage_handle_t storage;
    aether_media_fs_lease_t media_lease;
    ac101s_audio_handle_t audio;
    esp_lcd_touch_handle_t touch;
    portMUX_TYPE lock;
    bool running;
    bool stop_barrier;
    atomic_uint external_input;
    atomic_bool stop_requested;
    atomic_bool paused;
    atomic_bool screen_paused;
    atomic_int pause_request; /* -1: none; 0/1: owner-task resume/pause */
    aether_nes_state_t state;
    esp_err_t last_error;
    esp_err_t persistence_error;
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
    uint32_t core_us_per_frame, apu_us_per_frame, audio_output_us_per_frame;
    uint32_t video_convert_us_per_frame, frame_callback_us_per_frame;
    uint32_t input_us_per_frame, yield_us_per_frame;
    bool release_frame_requested;
    uint8_t *rom_data;
    size_t rom_size;
    uint16_t *frame565;
    uint16_t palette565[256];
    bool audio_started;
    bool port_cleanup_pending;
    bool (*input_gate)(void *);
    void *input_gate_context;
    aether_nes_frame_cb_t frame_cb;
    void *user_ctx;
    char path[192];
};

void aether_nes_port_set_active(aether_nes_handle_t handle);
int aether_nes_port_run(void);
esp_err_t aether_nes_port_shutdown(void);
bool aether_nes_port_mapper_supported(unsigned mapper_number);
