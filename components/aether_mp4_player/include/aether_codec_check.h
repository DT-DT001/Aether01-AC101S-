#pragma once
#include "esp_err.h"
/* Optional bounded on-device check of the actual S31 codec libraries.
 * Original synthetic fixtures; no TF card, display or audio output needed. */
esp_err_t aether_codec_check_start(void);
