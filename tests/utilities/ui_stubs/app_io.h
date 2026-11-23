#pragma once
#include "esp_err.h"

typedef void *app_io_handle_t;
esp_err_t app_io_release_media(app_io_handle_t io);
