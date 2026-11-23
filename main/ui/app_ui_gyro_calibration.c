#include "app_ui_gyro_calibration.h"
#include "app_gyro_calibration.h"
#include <app_ui_internal.h>
#include "app_ui_modal.h"

static void calibration_deleted(lv_event_t *event)
{
    app_ui_gyro_calibration_t *dialog = lv_event_get_user_data(event);
    if (dialog->started) (void)app_telemetry_gyro_calibration_cancel();
    *dialog = (app_ui_gyro_calibration_t){0};
}

void app_ui_gyro_calibration_close(app_ui_gyro_calibration_t *dialog)
{
    if (dialog->box) lv_msgbox_close(dialog->box);
}

static void calibration_action(lv_event_t *event)
{
    app_ui_gyro_calibration_close(lv_event_get_user_data(event));
}

static const char *calibration_error_text(esp_err_t error)
{
    switch (error) {
    case ESP_ERR_NOT_SUPPORTED: return "未检测到陀螺仪。";
    case ESP_ERR_INVALID_RESPONSE: return "请平放设备并保持静止，再试一次。";
    case ESP_ERR_TIMEOUT: return "陀螺仪采样中断，请重试。";
    case ESP_ERR_INVALID_STATE: return "设备忙，请稍后重试。";
    default: return "校准未完成，请重试。";
    }
}

void app_ui_gyro_calibration_refresh(app_ui_gyro_calibration_t *dialog)
{
    if (!dialog->box) return;

    app_gyro_calibration_status_t status = {
        .phase = APP_GYRO_CAL_FAILED,
        .error = dialog->start_error,
    };
    if (dialog->started) app_telemetry_gyro_calibration_get(&status);
    const unsigned seconds = (unsigned)((status.remaining_ms + 999U) / 1000U);
    const bool collecting = status.phase == APP_GYRO_CAL_COUNTDOWN ||
                            status.phase == APP_GYRO_CAL_SAMPLING;
    const bool saving = status.phase == APP_GYRO_CAL_SAVING;
    if (collecting || saving) lv_display_trigger_activity(lv_obj_get_display(dialog->box));
    app_ui_label_text(dialog->action_label, collecting ? "取消" : "关闭");
    if (saving) lv_obj_add_state(dialog->action, LV_STATE_DISABLED);
    else lv_obj_remove_state(dialog->action, LV_STATE_DISABLED);

    switch (status.phase) {
    case APP_GYRO_CAL_COUNTDOWN:
        app_ui_label_fmt(dialog->status, "屏幕朝上平放\n%u 秒后开始", seconds);
        break;
    case APP_GYRO_CAL_SAMPLING:
        app_ui_label_fmt(dialog->status, "保持静止\n还需 %u 秒", seconds);
        break;
    case APP_GYRO_CAL_SAVING:
        app_ui_label_text(dialog->status, "正在保存校准");
        break;
    case APP_GYRO_CAL_COMPLETE:
        app_ui_label_text(dialog->status, "校准完成，补偿已保存。");
        break;
    case APP_GYRO_CAL_CANCELLED:
        app_ui_label_text(dialog->status, "校准已取消。");
        break;
    default:
        app_ui_label_text(dialog->status, calibration_error_text(status.error));
        break;
    }
}

void app_ui_gyro_calibration_open(lv_obj_t *screen, app_ui_gyro_calibration_t *dialog)
{
    if (dialog->box || app_ui_modal_present(screen)) return;
    lv_obj_t *box = app_ui_modal_msgbox(screen);
    if (!box) return;
    dialog->box = box;
    lv_obj_set_width(box, 334);
    lv_obj_set_style_text_font(box, app_ui_chinese_font(), 0);
    lv_obj_add_event_cb(box, calibration_deleted, LV_EVENT_DELETE, dialog);
    lv_msgbox_add_title(box, "陀螺仪校准");
    dialog->status = lv_msgbox_add_text(box, "");
    dialog->action = lv_msgbox_add_footer_button(box, "取消");
    if (!dialog->status || !dialog->action) {
        app_ui_gyro_calibration_close(dialog);
        return;
    }
    lv_obj_set_style_text_align(dialog->status, LV_TEXT_ALIGN_CENTER, 0);
    dialog->action_label = lv_obj_get_child(dialog->action, 0);
    lv_obj_add_event_cb(dialog->action, calibration_action, LV_EVENT_CLICKED, dialog);

    dialog->start_error = app_telemetry_gyro_calibration_begin();
    dialog->started = dialog->start_error == ESP_OK;
    app_ui_gyro_calibration_refresh(dialog);
    app_ui_modal_focus(box, dialog->action);
}
