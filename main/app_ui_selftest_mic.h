#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "ac101s_audio.h"
#include "app_ui_internal.h"
#include "esp_log.h"

/* Call from the self-test task, after media owners have released the codec. */
static bool selftest_microphone(app_ui_t *ui)
{
    enum { FRAMES_PER_BLOCK = 512, BLOCKS = 16, LIMIT = 24000 };
    const char *const log_tag = "ui_selftest";
    if (!ui->board || !ui->board->audio_ready || !ui->board->audio) {
        ESP_LOGI(log_tag, "SKIP mic-capture audio backend unavailable");
        return true;
    }

    const ac101s_audio_handle_t audio = ui->board->audio;
    ac101s_audio_input_t input;
    const esp_err_t route = ac101s_audio_get_input(audio, &input);
    if (route != ESP_OK) {
        ESP_LOGE(log_tag, "FAIL mic-capture input-route=%s", esp_err_to_name(route));
        return false;
    }
    if (input != AC101S_INPUT_BOARD) {
        ESP_LOGI(log_tag, "SKIP mic-capture USB input selected");
        return true;
    }

    int16_t (*pcm)[2] = malloc(FRAMES_PER_BLOCK * sizeof(*pcm));
    if (!pcm) {
        ESP_LOGE(log_tag, "FAIL mic-capture buffer allocation");
        return false;
    }

    /* Match the recorder's board gain and capture-only path. */
    const esp_err_t started = ac101s_audio_capture_start(audio, 24.0f);
    if (started != ESP_OK) {
        /* INVALID_STATE can mean a different owner reached the codec first. */
        const esp_err_t rollback = started == ESP_ERR_INVALID_STATE ? ESP_OK :
            ac101s_audio_capture_stop(audio);
        free(pcm);
        ESP_LOGE(log_tag, "FAIL mic-capture start=%s cleanup=%s",
                 esp_err_to_name(started), esp_err_to_name(rollback));
        return false;
    }

    const size_t bytes = FRAMES_PER_BLOCK * sizeof(*pcm);
    unsigned peak = 0, nonzero = 0, mismatched = 0, frames = 0, magnitude_sum = 0;
    esp_err_t received = ESP_OK;
    for (unsigned block = 0; block < BLOCKS; ++block) {
        received = ac101s_audio_capture_read(audio, pcm, bytes);
        if (received != ESP_OK) break;
        frames += FRAMES_PER_BLOCK;
        for (unsigned i = 0; i < FRAMES_PER_BLOCK; ++i) {
            const int sample = pcm[i][0];
            const unsigned magnitude = sample < 0 ? (unsigned)-sample : (unsigned)sample;
            if (magnitude > peak) peak = magnitude;
            magnitude_sum += magnitude;
            if (sample != 0) ++nonzero;
            if (sample != pcm[i][1]) ++mismatched;
        }
    }

    const esp_err_t stopped = ac101s_audio_capture_stop(audio);
    if (stopped != ESP_OK) {
        const esp_err_t retry = ac101s_audio_capture_stop(audio);
        free(pcm);
        ESP_LOGE(log_tag, "FAIL mic-capture stop=%s retry=%s",
                 esp_err_to_name(stopped), esp_err_to_name(retry));
        return false;
    }
    const esp_err_t after_stop = ac101s_audio_capture_read(audio, pcm, bytes);
    free(pcm);

    if (received != ESP_OK || after_stop != ESP_ERR_INVALID_STATE || !nonzero ||
        peak > LIMIT || mismatched) {
        ESP_LOGE(log_tag, "FAIL mic-capture read=%s after_stop=%s frames=%u peak=%u nonzero=%u stereo_mismatch=%u",
                 esp_err_to_name(received), esp_err_to_name(after_stop), frames, peak, nonzero, mismatched);
        return false;
    }
    ESP_LOGI(log_tag, "PASS mic-capture board PCM16 48000Hz frames=%u peak=%u nonzero=%u stereo_mismatch=0 released=1 mean_abs=%u",
             frames, peak, nonzero, magnitude_sum / frames);
    return true;
}
