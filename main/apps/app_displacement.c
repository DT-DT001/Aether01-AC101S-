#include "app_ui_internal.h"
#include "app_displacement.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    DISPLACEMENT_ACTION_START,
    DISPLACEMENT_ACTION_FINISH,
    DISPLACEMENT_ACTION_RESUME,
    DISPLACEMENT_ACTION_CLEAR,
} displacement_action_t;

typedef struct {
    lv_obj_t *state, *instruction, *caption, *distance, *direction, *axes[3], *samples, *quality;
    lv_obj_t *primary, *secondary;
    esp_err_t command_error;
    uint32_t state_color;
    bool finish_pending;
    displacement_action_t primary_action, secondary_action;
} displacement_page_t;

static const float axis_display_min_m = 0.005f;

static void displacement_cleanup(app_ui_t *ui);
static void displacement_action(lv_event_t *event);

static void update_button(lv_obj_t *object, displacement_action_t *shown_action,
                          displacement_action_t next_action, const char *label, bool enabled)
{
    /* A telemetry refresh can change the available commands before release.
     * Keep the pressed button bound to the command the user saw. */
    if (lv_obj_has_state(object, LV_STATE_PRESSED)) return;
    *shown_action = next_action;
    app_ui_label_text(lv_obj_get_child(object, 0), label);
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y, int width,
                      uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = app_ui_add_text(parent, value, LV_ALIGN_TOP_LEFT, x, y, color, font);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x182B42), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    return box;
}

static lv_obj_t *button(app_ui_t *ui, const char *value, int x, uint32_t color)
{
    lv_obj_t *object = lv_button_create(ui->content);
    lv_obj_set_pos(object, x, 270);
    lv_obj_set_size(object, 196, 46);
    lv_obj_set_style_radius(object, 14, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x38699B), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_add_event_cb(object, displacement_action, LV_EVENT_CLICKED, ui);
    app_ui_add_text(object, value, LV_ALIGN_CENTER, 0, 0, 0xEAF2FF, app_ui_chinese_font());
    return object;
}

static void format_cm(char *buffer, size_t size, float meters, bool signed_axis)
{
    const double cm = (double)meters * 100.0;
    if (!signed_axis && cm < 1.0) snprintf(buffer, size, "<1 cm");
    else if (signed_axis && fabsf(meters) < axis_display_min_m) snprintf(buffer, size, "≈0 cm");
    else if (signed_axis) snprintf(buffer, size, fabs(cm) < 1.0 ? "%+.1f cm" : "%+.0f cm", cm);
    else snprintf(buffer, size, "%.0f cm", cm);
}

static void update_direction(lv_obj_t *label, const float xyz[3], bool usable)
{
    if (!usable) {
        app_ui_label_text(label, "--");
        return;
    }

    const float largest = fmaxf(fabsf(xyz[0]), fmaxf(fabsf(xyz[1]), fabsf(xyz[2])));
    if (largest < axis_display_min_m) {
        app_ui_label_text(label, "接近起点");
        return;
    }

    // Keep the dominant direction and any substantial diagonal components.
    const float threshold = fmaxf(axis_display_min_m, largest * 0.60f);
    char direction[sizeof("+X / +Y / +Z")];
    size_t length = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (fabsf(xyz[i]) < threshold) continue;
        if (length) {
            direction[length++] = ' ';
            direction[length++] = '/';
            direction[length++] = ' ';
        }
        direction[length++] = xyz[i] < 0.0f ? '-' : '+';
        direction[length++] = "XYZ"[i];
    }
    direction[length] = '\0';
    app_ui_label_text(label, direction);
}

typedef struct {
    const char *title, *instruction;
} fault_text_t;

static fault_text_t fault_text(app_displacement_fault_t fault)
{
    switch (fault) {
    case APP_DISPLACEMENT_FAULT_START_STILL:
        return (fault_text_t){"起点静止超时", "将设备放平、放稳，再重新测量。"};
    case APP_DISPLACEMENT_FAULT_WAIT_MOTION:
        return (fault_text_t){"等待移动超时", "看到「可以移动」后，平稳移动设备。"};
    case APP_DISPLACEMENT_FAULT_MOTION_TIMEOUT:
        return (fault_text_t){"未确认停稳", "请缩短移动时间，放稳后重新测量。"};
    case APP_DISPLACEMENT_FAULT_STOP_STILL:
        return (fault_text_t){"终点静止超时", "先放稳设备，再轻点结束。"};
    case APP_DISPLACEMENT_FAULT_SENSOR:
        return (fault_text_t){"传感器采样中断", "请检查六轴传感器后重试。"};
    case APP_DISPLACEMENT_FAULT_TIMEOUT:
        return (fault_text_t){"测量超时", "请缩短测量时间后重试。"};
    case APP_DISPLACEMENT_FAULT_GAP:
        return (fault_text_t){"采样间隔过长", "请静置后重试。"};
    case APP_DISPLACEMENT_FAULT_SATURATION:
        return (fault_text_t){"动作过快", "请放慢移动后重试。"};
    case APP_DISPLACEMENT_FAULT_ENDPOINT:
        return (fault_text_t){"终点校准未通过", "请保持朝向，缩短移动距离后重测。"};
    default:
        return (fault_text_t){"测量无效", "请重新测量。"};
    }
}

static void refresh(displacement_page_t *page)
{
    app_displacement_status_t status;
    app_displacement_get_status(&status);
    const app_displacement_result_t *result = &status.result;
    const bool finishing_allowed = status.active &&
        (status.state == APP_DISPLACEMENT_READY || status.state == APP_DISPLACEMENT_MOVING);
    if (!finishing_allowed) page->finish_pending = false;
    const bool terminal = status.state == APP_DISPLACEMENT_COMPLETE || status.state == APP_DISPLACEMENT_INVALID;
    const bool complete = status.available && status.state == APP_DISPLACEMENT_COMPLETE;
    const bool no_motion = complete && result->elapsed_ms == 0;
    const bool live = status.available && status.active && result->samples > 0 &&
        (status.state == APP_DISPLACEMENT_MOVING || status.state == APP_DISPLACEMENT_SETTLING);
    const bool stationary = live && status.state == APP_DISPLACEMENT_MOVING && result->stationary;
    /* Hardware motion does not count as touch activity. Hold the display
     * awake only during the bounded measurement or a requested continuation. */
    if (status.active && (!result->stationary || result->resume_pending))
        lv_display_trigger_activity(lv_obj_get_display(page->state));
    const bool preview_held = live && !stationary &&
        (result->preview_held || page->finish_pending || status.state == APP_DISPLACEMENT_SETTLING);
    const bool usable = (live || (complete && !no_motion && result->endpoint_corrected &&
        (!result->measurement_limited || result->stationary))) &&
        isfinite(result->distance_m) && result->distance_m >= 0.0f &&
        isfinite(result->xyz_m[0]) && isfinite(result->xyz_m[1]) && isfinite(result->xyz_m[2]);
    const char *state = "等待测量";
    const char *instruction = "将设备放稳并保持静止，再点开始。";
    uint32_t color = 0xAFC6E9;
    if (!status.available) {
        state = "六轴传感器不可用";
        instruction = "此功能需要加速度计和陀螺仪。";
        color = 0xFFB38C;
    } else switch (page->finish_pending ? APP_DISPLACEMENT_SETTLING : status.state) {
    case APP_DISPLACEMENT_WAITING_STILL:
        state = "正在确认起点";
        instruction = "请保持静止，等待提示可以移动。";
        break;
    case APP_DISPLACEMENT_READY:
        state = "可以移动";
        instruction = "平稳移动，尽量保持朝向，可查看实时位移。";
        color = 0x8FE0BA;
        break;
    case APP_DISPLACEMENT_MOVING:
        state = stationary ? "已停稳 · 读数锁定" : preview_held ? "正在确认停稳" : "正在测量";
        instruction = stationary ? (result->resume_pending ? "放稳片刻，再移动设备继续测量。" :
            "点继续测量后再移动，或结束测量。") : preview_held ? "读数已暂存，请保持设备静止。" :
            "看实时位移，到终点停稳后轻点结束。";
        color = 0x8FE0BA;
        break;
    case APP_DISPLACEMENT_SETTLING:
        state = "正在确认终点";
        instruction = "请保持静止，等待终点校准。";
        break;
    case APP_DISPLACEMENT_COMPLETE:
        if (result->measurement_limited) {
            state = result->fault == APP_DISPLACEMENT_FAULT_ENDPOINT ? "终点校准未通过" : "未确认停稳";
            instruction = usable ? "已保留上次停稳读数，请重新测量。" : "请缩短移动时间，放稳后重新测量。";
        } else {
            state = no_motion ? "未检测到明显移动" : usable ? "测量完成" : "结果不可用";
            instruction = no_motion ? "非常缓慢的移动可能无法检出。" : usable ?
                "位移以起点处的传感器轴为准。" : "结果数据异常，请重新测量。";
        }
        color = usable && !result->low_confidence && !result->measurement_limited ? 0x8FE0BA : 0xFFB38C;
        break;
    case APP_DISPLACEMENT_INVALID: {
        const fault_text_t fault = fault_text(result->fault);
        state = fault.title;
        instruction = fault.instruction;
        color = 0xFFB38C;
        break;
    }
    case APP_DISPLACEMENT_IDLE:
        break;
    }
    app_ui_label_text(page->state, state);
    if (page->state_color != color) {
        page->state_color = color;
        lv_obj_set_style_text_color(page->state, lv_color_hex(color), 0);
    }
    app_ui_label_text(page->instruction, instruction);
    const char *caption = "距起点（净位移）";
    if (stationary || (complete && result->measurement_limited && usable)) caption = "距起点（已锁定）";
    else if (preview_held) caption = "距起点（读数暂存）";
    else if (live) caption = "距起点（实时估算）";
    else if (usable) caption = "距起点（终点修正）";
    app_ui_label_text(page->caption, caption);
    char value[32];
    if (usable) format_cm(value, sizeof(value), result->distance_m, false);
    else snprintf(value, sizeof(value), "--");
    app_ui_label_text(page->distance, value);
    update_direction(page->direction, result->xyz_m, usable);
    for (size_t i = 0; i < 3; ++i) {
        if (usable) format_cm(value, sizeof(value), result->xyz_m[i], true);
        else snprintf(value, sizeof(value), "--");
        app_ui_label_text(page->axes[i], value);
    }
    if (result->samples) app_ui_label_fmt(page->samples, "采样 %lu  ·  耗时 %.1f s  ·  最大间隔 %lu ms",
        (unsigned long)result->samples, (double)result->elapsed_ms / 1000.0, (unsigned long)result->max_gap_ms);
    else app_ui_label_text(page->samples, "采样 —  ·  耗时 —  ·  最大间隔 —");
    if (page->command_error != ESP_OK)
        app_ui_label_fmt(page->quality, "操作未受理 %s", esp_err_to_name(page->command_error));
    else if (usable && complete && result->measurement_limited)
        app_ui_label_text(page->quality, "本段未确认终点，未计入位移。");
    else if (usable && stationary) app_ui_label_text(page->quality, result->low_confidence ?
        "估算质量偏低，三轴读数已锁定。" : "三轴读数已锁定。");
    else if (usable && preview_held)
        app_ui_label_text(page->quality, "确认停稳后更新三轴读数。");
    else if (usable && live) app_ui_label_text(page->quality, result->low_confidence ?
        "当前估算可信度偏低，可停稳后结束。" : "实时值可能漂移，结束时校准。");
    else if (usable) app_ui_label_text(page->quality, result->low_confidence ?
        "质量偏低，建议重测" : "已通过静止与采样检查");
    else app_ui_label_text(page->quality, "");

    update_button(page->primary, &page->primary_action,
        stationary ? DISPLACEMENT_ACTION_RESUME : status.active ? DISPLACEMENT_ACTION_FINISH : DISPLACEMENT_ACTION_START,
        stationary ? "继续测量" : status.active ? "结束测量" : terminal ? "重新测量" : "开始测量",
        status.available && (!status.active || (finishing_allowed && !page->finish_pending && !result->resume_pending)));
    update_button(page->secondary, &page->secondary_action, stationary ? DISPLACEMENT_ACTION_FINISH : DISPLACEMENT_ACTION_CLEAR,
        stationary ? "结束测量" : status.active ? "取消测量" : "清除结果", status.active || terminal);
}

static void displacement_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (!ui || ui->page_cleanup != displacement_cleanup || lv_obj_get_screen(target) != ui->screen) return;
    displacement_page_t *page = ui->page_context;
    if (!page) return;
    if (lv_obj_has_state(target, LV_STATE_DISABLED)) return;
    const displacement_action_t action = target == page->primary ? page->primary_action : page->secondary_action;
    switch (action) {
    case DISPLACEMENT_ACTION_FINISH:
        if (page->finish_pending) break;
        page->command_error = app_displacement_finish();
        page->finish_pending = page->command_error == ESP_OK;
        break;
    case DISPLACEMENT_ACTION_CLEAR:
        app_displacement_cancel();
        page->command_error = ESP_OK;
        page->finish_pending = false;
        break;
    case DISPLACEMENT_ACTION_START:
        page->finish_pending = false;
        page->command_error = app_displacement_start();
        break;
    case DISPLACEMENT_ACTION_RESUME:
        page->command_error = app_displacement_resume();
        break;
    }
    refresh(page);
}

static void displacement_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    if (ui->page_cleanup == displacement_cleanup && ui->page_context) refresh(ui->page_context);
}

static void displacement_cleanup(app_ui_t *ui)
{
    app_displacement_cancel();
    free(ui->page_context);
    ui->page_context = NULL;
}

void app_open_displacement(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "短程位移");
    displacement_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFFAAAA, app_ui_chinese_font());
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = displacement_cleanup;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    page->state = text(content, "", 0, 0, 404, 0xAFC6E9, app_ui_chinese_font_size(24));
    page->instruction = text(content, "", 0, 34, 404, 0xC9DCF8, app_ui_chinese_font());

    lv_obj_t *net = card(content, 0, 65, 404, 81);
    page->caption = text(net, "距起点（净位移）", 8, 9, 188, 0xAFC6E9, app_ui_chinese_font());
    lv_obj_set_style_text_align(page->caption, LV_TEXT_ALIGN_CENTER, 0);
    page->distance = text(net, "--", 8, 34, 188, 0xD4F1F5, &lv_font_montserrat_28);
    lv_obj_set_style_text_align(page->distance, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *direction_caption = text(net, "净方向", 208, 9, 188, 0xAFC6E9, app_ui_chinese_font());
    lv_obj_set_style_text_align(direction_caption, LV_TEXT_ALIGN_CENTER, 0);
    page->direction = text(net, "--", 208, 40, 188, 0xD4F1F5, app_ui_chinese_font_size(24));
    lv_obj_set_style_text_align(page->direction, LV_TEXT_ALIGN_CENTER, 0);

    static const char *const axes[] = {"X", "Y", "Z"};
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *axis = card(content, (int)i * 138, 154, 128, 62);
        lv_obj_t *caption = text(axis, axes[i], 0, 7, 128, 0x82AACF, &lv_font_montserrat_16);
        lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
        page->axes[i] = text(axis, "--", 0, 30, 128, 0xEAF2FF, app_ui_chinese_font());
        lv_obj_set_style_text_align(page->axes[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    page->samples = text(content, "", 0, 224, 404, 0xAFC6E9, app_ui_chinese_font());
    page->quality = text(content, "", 0, 245, 404, 0xFFD099, app_ui_chinese_font());
    page->primary = button(ui, "开始测量", 0, 0x2768D8);
    page->secondary = button(ui, "清除结果", 208, 0x243955);
    text(content, "方向以起点的传感器坐标轴为准。", 0, 327, 404, 0x9CB2D2, app_ui_chinese_font());
    app_ui_set_timer(ui, displacement_timer, 100);
    refresh(page);
}
