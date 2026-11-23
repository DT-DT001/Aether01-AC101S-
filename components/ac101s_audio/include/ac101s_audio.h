#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ac101s_audio_dev_t *ac101s_audio_handle_t;

/* The shared audio API retains its original names. This board uses AC101S.
 * The local speaker's combined volume is capped at 70 on the 0..100 scale. */
#define AC101S_AUDIO_SPEAKER_VOLUME_MAX 70

typedef enum { AC101S_OUTPUT_SPEAKER, AC101S_OUTPUT_EXTERNAL, AC101S_OUTPUT_USB } ac101s_audio_output_t;
typedef enum { AC101S_INPUT_BOARD, AC101S_INPUT_USB } ac101s_audio_input_t;

/* Optional output transport. Callbacks execute on application tasks with the
 * audio mutex held, never on a radio callback or LVGL event. write receives
 * 48kHz PCM16 stereo and must finish within timeout_ms or return an error. */
typedef struct {
    void *context;
    esp_err_t (*start)(void *context);
    esp_err_t (*stop)(void *context);
    esp_err_t (*write)(void *context, const void *pcm, size_t bytes, uint32_t timeout_ms);
    bool (*connected)(void *context);
} ac101s_audio_sink_t;

/* Capture transport provides complete packed 48kHz PCM16 stereo blocks. The
 * USB adapter negotiates/resamples the device format; callbacks never acquire
 * the codec mutex or call GUI code. stop must be idempotent after disconnect. */
typedef struct {
    void *context;
    esp_err_t (*start)(void *context);
    esp_err_t (*stop)(void *context);
    esp_err_t (*read)(void *context, void *pcm, size_t bytes, uint32_t timeout_ms);
    bool (*connected)(void *context);
} ac101s_audio_source_t;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout; /**< ESP32-S31 TX output to AC101S SDIN. */
    gpio_num_t din;  /**< ESP32-S31 RX input from AC101S SDOUT. */
    gpio_num_t pa_enable;
    uint32_t sample_rate_hz;
} ac101s_audio_config_t;

/** Prepare the I2S and AC101S interfaces. The PA remains disabled. */
esp_err_t ac101s_audio_new(const ac101s_audio_config_t *config,
                           ac101s_audio_handle_t *out_handle);

/** Open ADC/DAC, set volume/gain, explicitly unmute, then enable the PA.
 * volume is logical 0..100; local speaker volume is capped by
 * AC101S_AUDIO_SPEAKER_VOLUME_MAX after combining it with the logical master.
 * Line-out gain and mute registers are read back before the local PA can turn on.
 * A failed setting/readback keeps it off unless restoring the previous volume
 * also passes the same hardware verification.
 * Starting an already active codec returns ESP_ERR_INVALID_STATE. */
esp_err_t ac101s_audio_start(ac101s_audio_handle_t handle, int volume, float mic_gain_db);

/** Play up to 5760 frames of 48 kHz stereo PCM16 through the local speaker.
 * Uses the master volume and never changes the selected output. Playback,
 * recording and board-owned audio have priority over the one-shot alert. */
esp_err_t ac101s_audio_play_alert(ac101s_audio_handle_t handle, const int16_t *pcm, size_t frames);

/** Mute/close the selected output and force the PA enable GPIO low. A failed
 * external stop keeps its ownership for retry; do not destroy its transport. */
esp_err_t ac101s_audio_stop(ac101s_audio_handle_t handle);
esp_err_t ac101s_audio_del(ac101s_audio_handle_t handle);

esp_err_t ac101s_audio_write(ac101s_audio_handle_t handle, void *data, size_t bytes);
typedef struct {
    uint32_t mutex_wait_us, mutex_wait_max_us;
    uint32_t output_write_us, output_write_max_us;
    uint32_t write_calls, mutex_timeouts, output_errors;
    esp_err_t last_error;
} ac101s_audio_write_metrics_t;
/** Cached write timings, accumulated for the handle lifetime. Output timing
 * covers the selected output path after acquiring the audio mutex; a mutex
 * timeout records zero output time without clearing the previous maximum.
 * Invalid write arguments are not counted. This getter only copies a short
 * snapshot under a spinlock; it never waits for the audio I/O mutex. */
esp_err_t ac101s_audio_get_write_metrics(ac101s_audio_handle_t handle,
                                        ac101s_audio_write_metrics_t *out);
esp_err_t ac101s_audio_read(ac101s_audio_handle_t handle, void *data, size_t bytes);
/** Exclusive microphone session (selected board/USB input), independent of output.
 * Requires the configured 48kHz transport. Requests up to 42 dB are accepted and clamped
 * to the AC101S analog PGA range of 0..31 dB in 1 dB steps. DAC stays muted and PA stays disabled.
 * Playback must be stopped first; playback start/write and changing the output
 * are rejected while capturing. Master-volume changes only update the saved
 * playback setting. A failed start rolls back; call capture_stop to retry any
 * failed codec close before starting another session. */
esp_err_t ac101s_audio_capture_start(ac101s_audio_handle_t handle, float gain_db);
/** Read packed 48kHz PCM16 stereo. The board mono ADC uses bounded voice gain
 * and is duplicated to both slots. USB microphone PCM keeps its original gain.
 * ESP_OK means all bytes are valid; bytes must be a nonzero multiple of four.
 * The mutex + read budget is 1s (plus RTOS tick scheduling). Discard the entire
 * buffer on error: partial reads are never reported as a successful block.
 * A USB microphone uses its normalized transport and allows up to32768 bytes;
 * removal fails the capture so its recorder can finalize the existing WAV. */
esp_err_t ac101s_audio_capture_read(ac101s_audio_handle_t handle, void *stereo_pcm, size_t bytes);
/** Stop only capture. Safe when already stopped, including after a later
 * playback session starts: this never stops playback or the external sink. */
esp_err_t ac101s_audio_capture_stop(ac101s_audio_handle_t handle);
/** Exclusive access to the built-in microphone or speaker using the current
 * master volume. The owner's identity must stay valid until stop succeeds.
 * An unsuccessful start can retain the owner for cleanup. Stop is safe for
 * another owner or an idle codec and should be retried after an I/O error. */
esp_err_t ac101s_audio_board_start(ac101s_audio_handle_t handle, const void *owner,
                                  bool capture, int volume, float gain_db);
/** Transfer a whole packed stereo 48 kHz PCM16 block in the active direction.
 * Capture applies a bounded gain suitable for real-time voice.
 * A timeout covers mutex acquisition plus DMA. Discard capture data on error. */
esp_err_t ac101s_audio_board_pcm(ac101s_audio_handle_t handle, const void *owner,
                                void *pcm, size_t bytes, uint32_t timeout_ms);
esp_err_t ac101s_audio_board_stop(ac101s_audio_handle_t handle, const void *owner);
/* Master values remain logical 0..100, independent of the speaker ceiling. */
esp_err_t ac101s_audio_set_master_volume(ac101s_audio_handle_t handle, uint8_t volume);
esp_err_t ac101s_audio_get_master_volume(ac101s_audio_handle_t handle, uint8_t *volume);
esp_err_t ac101s_audio_set_external_sink(ac101s_audio_handle_t handle, const ac101s_audio_sink_t *sink);
esp_err_t ac101s_audio_set_usb_sink(ac101s_audio_handle_t handle, const ac101s_audio_sink_t *sink);
esp_err_t ac101s_audio_set_usb_source(ac101s_audio_handle_t handle, const ac101s_audio_source_t *source);
esp_err_t ac101s_audio_set_input(ac101s_audio_handle_t handle, ac101s_audio_input_t input);
esp_err_t ac101s_audio_get_input(ac101s_audio_handle_t handle, ac101s_audio_input_t *input);
esp_err_t ac101s_audio_set_output(ac101s_audio_handle_t handle, ac101s_audio_output_t output);
esp_err_t ac101s_audio_get_output(ac101s_audio_handle_t handle, ac101s_audio_output_t *output);

#ifdef __cplusplus
}
#endif
