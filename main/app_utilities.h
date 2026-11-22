#pragma once

#include "app_alarm_audio.h"
#include "app_power.h"
#include "app_ui.h"
#include "app_weather_data.h"

enum { APP_NOTE_COUNT = 4, APP_NOTE_CHARACTERS = 300, APP_NOTE_CAPACITY = 1201 };
enum { APP_ALARM_SLOTS = 6, APP_ALARM_NO_SLOT = APP_ALARM_SLOTS };

typedef struct {
    bool configured, enabled;
    uint8_t hour, minute, tone;
} app_alarm_slot_t;

typedef struct {
    bool ready, pending, ringing;
    bool enabled;
    uint8_t hour, minute, tone;
    uint8_t ringing_slot, ringing_count;
    esp_err_t save_error;
} app_alarm_snapshot_t;

typedef struct {
    bool ready, pending;
    esp_err_t save_error;
} app_note_status_t;

esp_err_t app_utilities_init(aether_board_t *board, app_power_t *power,
                            bool (*storage_busy)(void *), void *context);
/* Fast cached reads. Preference writes and HTTPS happen away from LVGL. */
/* Daily alarms use the RTC while firmware is running. One dismissal covers a shared minute. */
void app_alarm_get(app_alarm_snapshot_t *out);
esp_err_t app_alarm_set(unsigned hour, unsigned minute, bool enabled);
esp_err_t app_alarm_get_slot(unsigned slot, app_alarm_slot_t *out);
esp_err_t app_alarm_set_slot(unsigned slot, unsigned hour, unsigned minute, bool enabled, unsigned tone);
esp_err_t app_alarm_remove_slot(unsigned slot);
esp_err_t app_alarm_preview(unsigned tone);
void app_alarm_cancel_preview(void);
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
