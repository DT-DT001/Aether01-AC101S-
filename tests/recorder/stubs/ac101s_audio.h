#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef void *ac101s_audio_handle_t;
esp_err_t ac101s_audio_capture_start(ac101s_audio_handle_t,float);
esp_err_t ac101s_audio_capture_read(ac101s_audio_handle_t,void *,size_t);
esp_err_t ac101s_audio_capture_stop(ac101s_audio_handle_t);
