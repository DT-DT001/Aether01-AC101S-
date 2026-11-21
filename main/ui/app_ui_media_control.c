#include <app_ui_internal.h>
#include "app_media_control.h"
#include "aether_recorder.h"
#include "esp_log.h"

#define REMOTE_QUEUE_LENGTH 16U
typedef struct {
    aether_bluetooth_remote_action_t action;
    uint32_t epoch;
} remote_command_t;

static bool recorder_owns_audio(void)
{
    aether_recorder_handle_t recorder = aether_recorder_get_default();
    aether_recorder_status_t status;
    return recorder && (aether_recorder_get_status(recorder, &status) != ESP_OK || status.active);
}

esp_err_t app_ui_media_control_init(app_ui_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    ui->media_control_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    ui->media_control_queue = xQueueCreate(REMOTE_QUEUE_LENGTH, sizeof(remote_command_t));
    return ui->media_control_queue ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_ui_media_control_delete(app_ui_t *ui)
{
    if (!ui || !ui->media_control_queue) return;
    vQueueDelete(ui->media_control_queue);
    ui->media_control_queue = NULL;
}

/* Only the GUI thread changes this gate. A producer can already have copied
 * the prior epoch when a transition starts; its late queue send is rejected by
 * the consumer's epoch check even after the transition has finished. */
void app_ui_media_control_gate(app_ui_t *ui, bool blocked, bool invalidate)
{
    if (!ui || !ui->media_control_queue) return;
    portENTER_CRITICAL(&ui->media_control_lock);
    ui->media_control_blocked = blocked;
    if (invalidate) ++ui->media_control_epoch;
    portEXIT_CRITICAL(&ui->media_control_lock);
}

esp_err_t app_ui_queue_media_control(app_ui_t *ui, aether_bluetooth_remote_action_t action)
{
    if (!ui || !ui->media_control_queue || action < AETHER_BT_REMOTE_PLAY ||
        action > AETHER_BT_REMOTE_VOLUME_DOWN) return ESP_ERR_INVALID_ARG;
    if (recorder_owns_audio() || app_io_usb_storage_blocked(ui->io)) return ESP_ERR_INVALID_STATE;
    remote_command_t command = {.action = action};
    portENTER_CRITICAL(&ui->media_control_lock);
    const bool blocked = ui->media_control_blocked;
    command.epoch = ui->media_control_epoch;
    portEXIT_CRITICAL(&ui->media_control_lock);
    if (blocked) return ESP_ERR_INVALID_STATE;
    return xQueueSend(ui->media_control_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void app_ui_media_control_process(app_ui_t *ui)
{
    if (!ui || !ui->media_control_queue) return;
    remote_command_t command;
    for (unsigned i = 0; i < REMOTE_QUEUE_LENGTH &&
         xQueueReceive(ui->media_control_queue, &command, 0) == pdTRUE; ++i) {
        portENTER_CRITICAL(&ui->media_control_lock);
        const bool blocked = ui->media_control_blocked;
        const uint32_t epoch = ui->media_control_epoch;
        portEXIT_CRITICAL(&ui->media_control_lock);
        if (blocked || command.epoch != epoch || recorder_owns_audio() ||
            app_io_usb_storage_blocked(ui->io)) continue;
        const esp_err_t error = app_media_control_dispatch(ui->audio_player, ui->video_player,
                                                          ui->nes, command.action);
        if (error != ESP_OK) ESP_LOGD("ui_remote", "media action %d rejected: %s",
                                      command.action, esp_err_to_name(error));
    }
}
