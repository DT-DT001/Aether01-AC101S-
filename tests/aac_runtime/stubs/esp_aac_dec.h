/* API-shaped test double only; declarations follow official esp_audio_codec
 * 2.6.2 headers. It does not contain or execute an AAC decoder. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    ESP_AUDIO_ERR_CONTINUE = 1, ESP_AUDIO_ERR_OK = 0, ESP_AUDIO_ERR_FAIL = -1,
    ESP_AUDIO_ERR_MEM_LACK = -2, ESP_AUDIO_ERR_DATA_LACK = -3,
    ESP_AUDIO_ERR_HEADER_PARSE = -4, ESP_AUDIO_ERR_INVALID_PARAMETER = -5,
    ESP_AUDIO_ERR_ALREADY_EXIST = -6, ESP_AUDIO_ERR_NOT_SUPPORT = -7,
    ESP_AUDIO_ERR_BUFF_NOT_ENOUGH = -8, ESP_AUDIO_ERR_NOT_FOUND = -9
} esp_audio_err_t;
typedef struct {
    int32_t sample_rate;
    uint8_t channel, bits_per_sample;
    bool no_adts_header, aac_plus_enable;
} esp_aac_dec_cfg_t;
typedef struct { uint8_t *buffer; uint32_t len, consumed; int frame_recover; } esp_audio_dec_in_raw_t;
typedef struct { uint8_t *buffer; uint32_t len, needed_size, decoded_size; } esp_audio_dec_out_frame_t;
typedef struct { uint32_t sample_rate; uint8_t bits_per_sample, channel; uint32_t bitrate, frame_size; } esp_audio_dec_info_t;
esp_audio_err_t esp_aac_dec_open(void *cfg, uint32_t cfg_size, void **decoder);
esp_audio_err_t esp_aac_dec_decode(void *, esp_audio_dec_in_raw_t *, esp_audio_dec_out_frame_t *, esp_audio_dec_info_t *);
esp_audio_err_t esp_aac_dec_reset(void *);
esp_audio_err_t esp_aac_dec_close(void *);
