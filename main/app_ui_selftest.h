#pragma once

#include "app_ui.h"

/* Optional board diagnostics. Normal builds do not include this task. */
esp_err_t app_ui_selftest_start(app_ui_t *ui);
