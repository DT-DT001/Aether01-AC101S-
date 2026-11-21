#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef void *ac101s_audio_handle_t;
esp_err_t ac101s_audio_start(ac101s_audio_handle_t, int, float);
esp_err_t ac101s_audio_stop(ac101s_audio_handle_t);
esp_err_t ac101s_audio_write(ac101s_audio_handle_t, void *, size_t);
