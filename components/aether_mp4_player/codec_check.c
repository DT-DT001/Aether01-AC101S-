#include "aether_codec_check.h"
#include "mp4_aac.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_dec_param.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "diagnostics/aac_fixture.h"
#include "diagnostics/h264_fixture.h"
#include <string.h>

static esp_err_t silent_pcm(const int16_t *pcm, size_t frames, void *context)
{
    if (!frames || frames > 1024) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < frames * 2; ++i) if (pcm[i]) return ESP_ERR_INVALID_RESPONSE;
    *(uint64_t *)context += frames;
    return ESP_OK;
}

static esp_err_t check_aac(void)
{
    for (unsigned channels = 1; channels <= 2; ++channels) {
        const uint8_t *asc = channels == 1 ? AAC_FIXTURE_MONO_ASC : AAC_FIXTURE_STEREO_ASC;
        const uint8_t *au = channels == 1 ? AAC_FIXTURE_MONO_AU : AAC_FIXTURE_STEREO_AU;
        const size_t length = channels == 1 ? sizeof(AAC_FIXTURE_MONO_AU) : sizeof(AAC_FIXTURE_STEREO_AU);
        mp4_aac_handle_t decoder = NULL;
        esp_err_t error = mp4_aac_new(asc, 2, NULL, NULL, &decoder);
        if (error != ESP_OK) return error;
        for (unsigned pass = 0; pass < 2 && error == ESP_OK; ++pass) {
            uint64_t frames = 0;
            for (unsigned frame = 0; frame < AAC_FIXTURE_AUS && error == ESP_OK; ++frame) {
                error = mp4_aac_decode(decoder, au, length, silent_pcm, &frames);
                vTaskDelay(1);
            }
            if (error == ESP_OK) error = mp4_aac_finish(decoder, silent_pcm, &frames);
            const uint64_t expected = ((uint64_t)AAC_FIXTURE_AUS * 1024 * 48000 + 44100 - 1) / 44100;
            if (error == ESP_OK && frames != expected) error = ESP_ERR_INVALID_SIZE;
            if (error == ESP_OK) error = mp4_aac_reset(decoder);
        }
        mp4_aac_del(decoder);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}

static esp_err_t check_h264(void)
{
    esp_h264_dec_handle_t decoder = NULL;
    esp_h264_dec_cfg_sw_t config = {.pic_type = ESP_H264_RAW_FMT_I420};
    esp_h264_err_t result = esp_h264_dec_sw_new(&config, &decoder);
    if (result != ESP_H264_ERR_OK) return ESP_FAIL;
    result = esp_h264_dec_open(decoder);
    const bool opened = result == ESP_H264_ERR_OK;
    esp_err_t error = opened ? ESP_OK : ESP_FAIL;
    for (unsigned pass = 0; pass < 3 && error == ESP_OK; ++pass) {
        /* H264 may remove emulation-prevention bytes in place. Start every
         * repetition with fresh writable, aligned Annex-B input. */
        uint8_t packet[sizeof(MP4_H264_FIXTURE)] __attribute__((aligned(16)));
        memcpy(packet, MP4_H264_FIXTURE, sizeof(packet));
        size_t used = 0; unsigned pictures = 0;
        while (used < sizeof(MP4_H264_FIXTURE) && error == ESP_OK) {
            esp_h264_dec_in_frame_t in = {.raw_data = {.buffer = packet + used, .len = sizeof(packet) - used}};
            esp_h264_dec_out_frame_t out = {0};
            result = esp_h264_dec_process(decoder, &in, &out);
            if (result != ESP_H264_ERR_OK || !in.consume || in.consume > sizeof(MP4_H264_FIXTURE) - used) { error = ESP_ERR_INVALID_RESPONSE; break; }
            used += in.consume;
            if (out.out_size) {
                if (!out.outbuf || out.out_size != 32 * 32 * 3 / 2) { error = ESP_ERR_INVALID_SIZE; break; }
                ++pictures;
                for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x)
                    if (out.outbuf[y * 32 + x] != 16 + ((y / 16) * 2 + x / 16) * 3) error = ESP_ERR_INVALID_RESPONSE;
                for (unsigned i = 32 * 32; i < 32 * 32 * 3 / 2; ++i) if (out.outbuf[i] != 128) error = ESP_ERR_INVALID_RESPONSE;
            }
            vTaskDelay(1);
        }
        if (error == ESP_OK && pictures != 1) error = ESP_ERR_INVALID_SIZE;
    }
    if (opened) esp_h264_dec_close(decoder);
    esp_h264_dec_del(decoder);
    return error;
}

static void check_task(void *context)
{
    (void)context;
    const esp_err_t aac = check_aac();
    ESP_LOGI("codec_check", "S31 official AAC-LC mono/stereo, resample/reset: %s", esp_err_to_name(aac));
    const esp_err_t h264 = check_h264();
    ESP_LOGI("codec_check", "S31 official H264 baseline I420 pixels/restart: %s", esp_err_to_name(h264));
    vTaskDelete(NULL);
}
esp_err_t aether_codec_check_start(void)
{
    return xTaskCreatePinnedToCore(check_task, "codec_check", 24576, NULL, 3, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
