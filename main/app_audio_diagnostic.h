#pragma once
#include "ac101s_audio.h"
/* Boot-only test, before any media client can start. Does not persist settings. */
esp_err_t app_audio_diagnostic_run(ac101s_audio_handle_t audio);
