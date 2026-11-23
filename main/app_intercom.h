#pragma once

#include "aether_intercom_radio.h"
#include "app_io.h"
#include "ac101s_audio.h"

typedef struct {
    bool active;
    bool transmitting;
    bool receiving;
    esp_err_t audio_error;
    aether_intercom_radio_status_t radio;
} app_intercom_status_t;

esp_err_t app_intercom_enter(ac101s_audio_handle_t codec, app_io_handle_t io);
void app_intercom_leave(void);
bool app_intercom_busy(void);
bool app_intercom_closing(void);
/* Called while an awake, unblocked physical user-key press is still held. */
bool app_intercom_user_key(int64_t pressed_at_us);
/* The intercom page publishes its live hold state. A release cancels a
 * pending request and stops a transmission once the audio task sees it. */
void app_intercom_set_screen_talk(bool held);
void app_intercom_get_status(app_intercom_status_t *status);
esp_err_t app_intercom_search(void);
esp_err_t app_intercom_forget(void);
