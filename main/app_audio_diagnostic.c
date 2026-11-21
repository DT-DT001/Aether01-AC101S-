#include "app_audio_diagnostic.h"
#include <math.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t app_audio_diagnostic_run(ac101s_audio_handle_t audio)
{
    if (!audio) return ESP_ERR_INVALID_ARG;
    uint8_t previous_volume;
    ac101s_audio_output_t previous_output;
    esp_err_t error = ac101s_audio_get_master_volume(audio, &previous_volume);
    if (error != ESP_OK) return error;
    error = ac101s_audio_get_output(audio, &previous_output);
    if (error != ESP_OK) return error;
    int16_t *pcm = heap_caps_malloc(480 * 4 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) return ESP_ERR_NO_MEM;
    int16_t *feedback = pcm + 480 * 2;
    int16_t sine[48], cosine[48];
    for (unsigned i = 0; i < 48; ++i) {
        sine[i] = (int16_t)(12000 * sinf(6.28318530718f * i / 48));
        cosine[i] = (int16_t)(12000 * cosf(6.28318530718f * i / 48));
    }
    ESP_LOGI("audio_test", "BEGIN: 1s 1kHz stereo sine, peak12000, speaker/master70; original master=%u", previous_volume);
    error = ac101s_audio_set_output(audio, AC101S_OUTPUT_SPEAKER);
    if (error == ESP_OK) error = ac101s_audio_set_master_volume(audio, 70);
    if (error == ESP_OK) error = ac101s_audio_start(audio, 100, 0.0f);
    const bool started = error == ESP_OK;
    esp_err_t feedback_error = ESP_OK;
    uint32_t peaks[2] = {0}, feedback_frames = 0;
    int64_t correlation_s[2] = {0}, correlation_c[2] = {0};
    if (started) {
        /* AW8010B's documented turn-on transient is approximately 40ms. */
        vTaskDelay(pdMS_TO_TICKS(50));
        for (unsigned block = 0; block < 100 && error == ESP_OK; ++block) {
            for (unsigned frame = 0; frame < 480; ++frame) {
                float gain = block == 0 ? (float)frame / 480 : block == 99 ? (float)(479 - frame) / 480 : 1.0f;
                int16_t sample = (int16_t)(gain * sine[frame % 48]);
                pcm[frame * 2] = pcm[frame * 2 + 1] = sample;
            }
            error = ac101s_audio_write(audio, pcm, 480 * 4);
            /* Sample the live board microphone while the speaker emits the tone. */
            if (error == ESP_OK && feedback_error == ESP_OK && (block == 30 || block == 50 || block == 70)) {
                feedback_error = ac101s_audio_read(audio, feedback, 480 * 4);
                if (feedback_error == ESP_OK) {
                    for (unsigned frame = 0; frame < 480; ++frame) for (unsigned channel = 0; channel < 2; ++channel) {
                        int32_t value = feedback[frame * 2 + channel];
                        uint32_t amplitude = (uint32_t)(value < 0 ? -value : value);
                        if (amplitude > peaks[channel]) peaks[channel] = amplitude;
                        correlation_s[channel] += (int64_t)value * sine[frame % 48];
                        correlation_c[channel] += (int64_t)value * cosine[frame % 48];
                    }
                    feedback_frames += 480;
                }
            }
        }
        /* Drain the final DMA blocks before asserting PA shutdown. */
        if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(50));
        const esp_err_t stopped = ac101s_audio_stop(audio);
        if (error == ESP_OK) error = stopped;
    }
    const esp_err_t restored_volume = ac101s_audio_set_master_volume(audio, previous_volume);
    const esp_err_t restored_output = ac101s_audio_set_output(audio, previous_output);
    if (error == ESP_OK) error = restored_volume != ESP_OK ? restored_volume : restored_output;
    free(pcm);
    double tone_amplitude[2] = {0};
    if (feedback_frames) for (unsigned channel = 0; channel < 2; ++channel) {
        double s = (double)correlation_s[channel], c = (double)correlation_c[channel];
        tone_amplitude[channel] = 2 * sqrt(s * s + c * c) / (12000 * (double)feedback_frames);
    }
    ESP_LOGI("audio_test", "microphone frames=%lu result=%d peakL=%lu peakR=%lu tone1k_L=%d tone1k_R=%d",
        (unsigned long)feedback_frames, feedback_error, (unsigned long)peaks[0], (unsigned long)peaks[1],
        (int)tone_amplitude[0], (int)tone_amplitude[1]);
    (void)tone_amplitude;
    ESP_LOGI("audio_test", "END: %s; settings restored (volume=%d route=%d), no NVS writes",
        esp_err_to_name(error), restored_volume, restored_output);
    return error;
}
