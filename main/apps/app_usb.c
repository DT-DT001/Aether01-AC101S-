#include "app_otg_control.h"
#include "app_ui_modal.h"
#include "app_ui_internal.h"

#include <stdlib.h>
#include <stdio.h>

typedef struct {
    lv_obj_t *state, *card, *help, *enter, *leave;
    esp_err_t submit_error;
} usb_page_t;

typedef struct {
    app_ui_t *ui;
    lv_obj_t *box, *body;
    uint32_t request_id;
} usb_eject_dialog_t;

static esp_err_t submit_usb(app_ui_t *ui, bool enabled, bool confirmed, uint32_t request_id)
{
    /* Invalidate earlier queued headset actions before publishing ownership. */
    app_ui_media_control_gate(ui, true, true);
    const esp_err_t error = confirmed ? app_io_usb_exit_confirmed(ui->io, request_id) :
                                      app_io_usb_set_enabled(ui->io, enabled);
    app_ui_media_control_gate(ui, false, false);
    return error;
}

static void eject_dialog_deleted(lv_event_t *event)
{
    usb_eject_dialog_t *dialog = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) == dialog->box) free(dialog);
}

static void eject_dialog_cancel(lv_event_t *event)
{
    usb_eject_dialog_t *dialog = lv_event_get_user_data(event);
    lv_msgbox_close(dialog->box);
}

static void eject_dialog_confirm(lv_event_t *event)
{
    usb_eject_dialog_t *dialog = lv_event_get_user_data(event);
    const esp_err_t error = submit_usb(dialog->ui, false, true, dialog->request_id);
    if (error == ESP_OK) lv_msgbox_close(dialog->box);
    else {
        app_ui_label_text(dialog->body, "USB 状态已改变，请返回后重新操作。\n确认前仍需在电脑安全弹出。");
        lv_obj_add_state(lv_event_get_target_obj(event), LV_STATE_DISABLED);
    }
}

static esp_err_t confirm_ejected_on_pc(app_ui_t *ui, uint32_t request_id)
{
    if (app_ui_modal_present(ui->screen)) return ESP_OK;
    usb_eject_dialog_t *dialog = calloc(1, sizeof(*dialog));
    if (!dialog) return ESP_ERR_NO_MEM;
    dialog->ui = ui;
    dialog->request_id = request_id;
    dialog->box = app_ui_modal_msgbox(ui->screen);
    lv_obj_set_width(dialog->box, 408);
    lv_obj_center(dialog->box);
    lv_obj_set_style_text_font(dialog->box, app_ui_chinese_font(), 0);
    lv_obj_add_event_cb(dialog->box, eject_dialog_deleted, LV_EVENT_DELETE, dialog);
    lv_obj_t *title = lv_msgbox_add_title(dialog->box, "电脑已安全弹出？");
    lv_obj_set_style_text_font(title, app_ui_chinese_font_size(24), 0);
    dialog->body = lv_msgbox_add_text(dialog->box,
        "请先在电脑安全弹出 U 盘。\n电脑已提示可以移除后，确认退出。\n尚未弹出就退出可能损坏文件。");
    lv_obj_t *cancel = lv_msgbox_add_footer_button(dialog->box, "返回");
    lv_obj_t *confirm = lv_msgbox_add_footer_button(dialog->box, "已安全弹出，退出");
    lv_obj_add_event_cb(cancel, eject_dialog_cancel, LV_EVENT_CLICKED, dialog);
    lv_obj_add_event_cb(confirm, eject_dialog_confirm, LV_EVENT_CLICKED, dialog);
    app_ui_modal_focus(dialog->box, cancel);
    return ESP_OK;
}

esp_err_t app_ui_usb_set_enabled(app_ui_t *ui, bool enabled)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    if (!enabled) {
        app_io_usb_status_t status;
        const esp_err_t error = app_io_usb_get_status(ui->io, &status);
        if (error != ESP_OK) return error;
        if (status.pending || app_io_is_quiescing(ui->io)) return ESP_ERR_INVALID_STATE;
        /* Some host safe-removal paths stop the disk without a SCSI LOEJ.
         * Neither suspend nor reset proves that host caches have been flushed.
         * Accept only this explicit user acknowledgment, bound to this request. */
        if (status.active && !status.host_ejected)
            return confirm_ejected_on_pc(ui, status.request_id);
    }
    return submit_usb(ui, enabled, false, 0);
}

static void notice_close(lv_event_t *event)
{
    lv_msgbox_close(lv_event_get_user_data(event));
}

void app_usb_storage_notice(app_ui_t *ui)
{
    if (!ui || !ui->screen) return;
    if (app_ui_modal_present(ui->screen)) return;
    lv_obj_t *notice = app_ui_modal_msgbox(ui->screen);
    lv_obj_set_width(notice, 390);
    lv_obj_center(notice);
    lv_obj_set_style_text_font(notice, app_ui_chinese_font(), 0);
    const bool shutdown = app_io_is_quiescing(ui->io);
    lv_obj_t *title = lv_msgbox_add_title(notice, shutdown ? "正在保存" : "TF 卡由电脑占用");
    lv_obj_set_style_text_font(title, app_ui_chinese_font_size(24), 0);
    lv_msgbox_add_text(notice, shutdown ? "系统正在保存并准备关机，请稍候。" :
        "电脑正在使用 TF 卡，本地文件和媒体暂不可用。\n请先在电脑安全弹出，再到设置里的 USB 文件传输页退出。");
    lv_obj_t *close = lv_msgbox_add_footer_button(notice, "知道了");
    lv_obj_add_event_cb(close, notice_close, LV_EVENT_CLICKED, notice);
    app_ui_modal_focus(notice, close);
}

static void usb_cleanup(app_ui_t *ui)
{
    /* Leaving this page does not implicitly disconnect the computer. */
    free(ui->page_context);
    ui->page_context = NULL;
}

static void enabled(lv_obj_t *object, bool value)
{
    if (value) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void update_usb(app_ui_t *ui)
{
    if (ui->page_cleanup != usb_cleanup || !ui->page_context) return;
    usb_page_t *page = ui->page_context;
    app_io_usb_status_t status = {0};
    const esp_err_t read_error = app_io_usb_get_status(ui->io, &status);
    const esp_err_t error = page->submit_error != ESP_OK ? page->submit_error :
                            read_error != ESP_OK ? read_error : status.last_error;
    if (status.pending) app_ui_label_text(page->state, status.requested_enabled ?
        "正在停止媒体并准备传输…" : "正在退出并恢复 TF…");
    else if (error == ESP_ERR_INVALID_STATE && status.active &&
             status.host_connected && !status.host_ejected)
        app_ui_label_text(page->state, "未确认电脑已弹出，请先安全弹出后再点退出。");
    else if (error != ESP_OK) app_ui_label_fmt(page->state, "USB 操作失败：%s", esp_err_to_name(error));
    else app_ui_label_text(page->state, status.active ? "电脑传输模式" : "设备本地模式");
    const char *link = status.usb_speed_mbps == 480 ? "高速480Mb/s" :
                       status.usb_speed_mbps == 12 ? "全速12Mb/s" : "速率待确认";
    char clock[32];
    if (status.usb_clock_khz)
        snprintf(clock, sizeof(clock), "%lu.%luMHz%s",
            (unsigned long)(status.usb_clock_khz / 1000),
            (unsigned long)(status.usb_clock_khz % 1000 / 100),
            status.usb_high_speed_fallback ? " 已降频" : "");
    else snprintf(clock, sizeof(clock), "%s", status.active ? "时钟待确认" : "本地模式");
    app_ui_label_fmt(page->card, "USB：%s / %s\nTF：%s / %s\n容量：%llu MiB",
        status.host_ejected ? "电脑已弹出" : status.host_connected ? "电脑已连接" : "等待数据连接", link,
        status.media_present ? "已就绪" : "未就绪", clock,
        (unsigned long long)(status.capacity_bytes / (1024ULL * 1024ULL)));
    aether_usb_host_status_t host = {0};
    (void)app_otg_get_status(&host);
    const bool host_busy = host.enabled || host.active || host.vbus_enabled || host.cleanup_pending ||
        host.state == AETHER_OTG_STARTING || host.state == AETHER_OTG_STOPPING;
    enabled(page->enter, read_error == ESP_OK && !status.pending && !status.blocked && !host_busy);
    if (host_busy && !status.blocked) app_ui_label_text(page->state, "请先关闭 OTG 并等待外设释放，再开启电脑传输");
    enabled(page->leave, read_error == ESP_OK && !status.pending && status.blocked &&
                         !app_io_is_quiescing(ui->io));
    app_ui_label_fmt(page->help, "阶段 %u / 路径 %u\n%s",
        status.diagnostic_stage, status.selected_route,
        status.blocked ? "传输中；本地文件暂不可用。" : "关闭后恢复本地 TF 和下载串口。");
}

static void usb_timer(lv_timer_t *timer)
{
    update_usb(lv_timer_get_user_data(timer));
}

static void usb_command(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || ui->page_cleanup != usb_cleanup || !ui->page_context) return;
    usb_page_t *page = ui->page_context;
    page->submit_error = app_ui_usb_set_enabled(ui, lv_event_get_target_obj(event) == page->enter);
    update_usb(ui);
}

static lv_obj_t *text(app_ui_t *ui, int y, int height, uint32_t color)
{
    lv_obj_t *label = app_ui_add_text(ui->content, "", LV_ALIGN_TOP_LEFT, 0, y,
                                     color, app_ui_chinese_font());
    lv_obj_set_size(label, 408, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *button(app_ui_t *ui, const char *name, int x)
{
    lv_obj_t *object = lv_button_create(ui->content);
    lv_obj_set_pos(object, x, 254);
    lv_obj_set_size(object, 188, 48);
    lv_obj_set_style_radius(object, 14, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x2768D8), 0);
    lv_obj_add_event_cb(object, usb_command, LV_EVENT_CLICKED, ui);
    app_ui_add_text(object, name, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
    return object;
}

void app_open_usb(app_ui_t *ui)
{
    app_ui_begin_page(ui, "USB 文件传输");
    usb_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(ui->content, "内存不足，请返回后重试", LV_ALIGN_CENTER, 0, 0,
                        0xFF8C8C, app_ui_chinese_font());
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = usb_cleanup;
    page->state = text(ui, 0, 44, 0xC9E8FF);
    page->card = text(ui, 56, 66, 0xB5C8DF);
    lv_obj_t *notes = text(ui, 130, 112, 0x91A8C6);
    app_ui_label_text(notes,
        "开启后，电脑可直接读写 TF 卡。\n设备会停止播放并保存录音。\n请先在电脑安全弹出，再点退出。\n连接后串口可能暂时消失。");
    page->enter = button(ui, "开启传输", 0);
    page->leave = button(ui, "退出并恢复", 220);
    page->help = text(ui, 314, 40, 0x91A8C6);
    app_ui_set_timer(ui, usb_timer, 300);
    update_usb(ui);
}
