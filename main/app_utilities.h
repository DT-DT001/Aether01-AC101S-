#pragma once

#include "app_power.h"
#include "app_ui.h"
#include "app_weather_data.h"

enum { APP_NOTE_COUNT = 4, APP_NOTE_CHARACTERS = 300, APP_NOTE_CAPACITY = 1201 };

typedef struct {
    bool ready, pending, ringing;
    bool enabled;
    uint8_t hour, minute;
    esp_err_t save_error;
} app_alarm_snapshot_t;

typedef struct {
    bool ready, pending;
    esp_err_t save_error;
} app_note_status_t;

esp_err_t app_utilities_init(aether_board_t *board, app_power_t *power,
                            bool (*storage_busy)(void *), void *context);
/* Fast cached reads. Preference writes and HTTPS happen away from LVGL. */
void app_alarm_get(app_alarm_snapshot_t *out);
esp_err_t app_alarm_set(unsigned hour, unsigned minute, bool enabled);
void app_alarm_dismiss(void);
void app_note_get_status(unsigned note, app_note_status_t *out);
esp_err_t app_note_read(unsigned note, char out[APP_NOTE_CAPACITY]);
esp_err_t app_note_save(unsigned note, const char *text);
bool app_utilities_save_pending(void);
esp_err_t app_utilities_save_error(void);
void app_utilities_alarm_ui(app_ui_t *ui);
void app_weather_get(app_weather_snapshot_t *out);
void app_weather_refresh(bool manual);
esp_err_t app_weather_select_city(unsigned city);
void app_open_alarm(app_ui_t *ui);
void app_open_notes(app_ui_t *ui);
void app_open_calendar(app_ui_t *ui);
void app_open_weather(app_ui_t *ui);
