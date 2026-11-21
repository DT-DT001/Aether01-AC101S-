#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"

esp_err_t ac101s_codec_new(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *out);
/* Playback also enables the ADC for the board microphone diagnostic. */
esp_err_t ac101s_codec_start(i2c_master_dev_handle_t dev, bool capture_only, float gain_db);
esp_err_t ac101s_codec_set_speaker(i2c_master_dev_handle_t dev, unsigned level);
esp_err_t ac101s_codec_stop(i2c_master_dev_handle_t dev);
