#pragma once

#include <stdbool.h>
#include "ac101s_audio.h"

enum { APP_ALARM_TONE_COUNT = 4 };

const char *app_alarm_tone_name(unsigned tone);

/* Called by the utilities worker. The speaker is released between notes.
 * A cancellation callback can stop the melody before the next note. */
esp_err_t app_alarm_audio_play(ac101s_audio_handle_t audio, unsigned tone,
                               bool (*continue_playing)(void *), void *context);
