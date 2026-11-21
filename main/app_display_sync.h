#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"

/* CO5300 CASET/RASET require even starts and even width/height. Assign this
 * to esp_lvgl_port's rounder_cb before adding the display; never expand only
 * at flush time, when the pixel buffer has already been rendered. */
void app_display_rounder(lv_area_t *area);
/* LVGL owner only. Does not replace the port flush or IO completion callback. */
esp_err_t app_display_sync_attach(lv_display_t *display, esp_lcd_panel_io_handle_t io);
/* Call from the existing background telemetry task. Rate-limited TE frequency
 * and cached fallback errors are printed here, never in the GUI flush path.
 * Reads only driver/POD snapshots and does not acquire the LVGL lock.
 * AETHER_VIDEO_DIAGNOSTIC also reports sampled post-render core/affinity;
 * affinity=-1 means unpinned, and coremask is not a CPU-load measurement. */
void app_display_sync_poll_diagnostics(esp_lcd_panel_io_handle_t io);
