#pragma once

#include "ui/app_ui.h"

/* Temporary one-shot device diagnostic. Call once immediately after the first
 * home page is initialized. It never takes over a page the user has changed. */
/* Optional bounded radio check runs outside LVGL before video and after normal
 * media cleanup. It only submits async commands/reads cached radio state; setup
 * and persistence stay in service owners. Normal builds omit this diagnostic. */
esp_err_t app_video_probe_start(app_ui_t *ui, void (*radio_check)(void *), void *context);
