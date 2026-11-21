#pragma once
#include "../../player/stubs/nvs.h"
#include <stddef.h>
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
