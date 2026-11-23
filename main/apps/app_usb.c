#include "app_otg_control.h"
#include "app_ui_modal.h"
#include "app_connection_style.h"

#include <stdlib.h>
#include <stdio.h>

typedef struct {
    lv_obj_t *state, *details, *enter, *leave;
    esp_err_t submit_error;
} usb_page_t;

typedef struct {
    app_ui_t *ui;
    lv_obj_t *box, *body;
    uint32_t request_id;
    void (*destination)(app_ui_t *);
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
    if (error == ESP_OK) {
        app_ui_t *ui = dialog->ui;
        void (*destination)(app_ui_t *) = dialog->destination;
        if (destination) {
            /* The page transition retires the dialog with its screen. */
            destination(ui);
        } else {
            lv_msgbox_close_async(dialog->box);
        }
    } else {
        app_ui_label_text(dialog->body, "USB 状态已改变，请返回后重新操作。\n确认前仍需在电脑安全弹出。");
        lv_obj_add_state(lv_event_get_target_obj(event), LV_STATE_DISABLED);
    }
}

static esp_err_t confirm_ejected_on_pc(app_ui_t *ui, uint32_t request_id,
                                       void (*destination)(app_ui_t *))
{
    if (app_ui_modal_present(ui->screen)) return ESP_OK;
    usb_eject_dialog_t *dialog = calloc(1, sizeof(*dialog));
    if (!dialog) return ESP_ERR_NO_MEM;
    dialog->ui = ui;
    dialog->request_id = request_id;
    dialog->destination = destination;
    dialog->box = app_ui_modal_msgbox(ui->screen);
    if (!dialog->box) { free(dialog); return ESP_ERR_NO_MEM; }
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
    lv_obj_set_height(cancel, 44);
    lv_obj_set_height(confirm, 44);
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
            return confirm_ejected_on_pc(ui, status.request_id, NULL);
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
    lv_obj_set_height(close, 44);
    lv_obj_add_event_cb(close, notice_close, LV_EVENT_CLICKED, notice);
    app_ui_modal_focus(notice, close);
}

static void usb_cleanup(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
}

static bool usb_can_exit(app_ui_t *ui, void (*destination)(app_ui_t *))
{
    usb_page_t *page = ui->page_context;
    app_io_usb_status_t status;
    page->submit_error = app_io_usb_get_status(ui->io, &status);
    if (page->submit_error != ESP_OK) return false;
    if (status.pending) return !status.requested_enabled;
    if (!status.blocked) return true;
    if (status.active && !status.host_ejected) {
        page->submit_error = confirm_ejected_on_pc(ui, status.request_id, destination);
        return false;
    }
    page->submit_error = submit_usb(ui, false, false, 0);
    return page->submit_error == ESP_OK;
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
    app_ui_label_fmt(page->details, "USB：%s / %s\nTF：%s / %s\n容量：%llu MiB",
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

static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y,
                      int width, int height, uint32_t color)
{
    lv_obj_t *label = app_ui_add_text(parent, value, LV_ALIGN_TOP_LEFT, x, y, color, app_ui_chinese_font());
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *button(app_ui_t *ui, const char *name, int x, bool primary)
{
    lv_obj_t *object = lv_button_create(ui->content);
    lv_obj_set_pos(object, x, 270);
    lv_obj_set_size(object, 196, 48);
    app_connection_button_style(object, primary);
    lv_obj_add_event_cb(object, usb_command, LV_EVENT_CLICKED, ui);
    app_ui_add_text(object, name, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
    return object;
}

void app_open_usb(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "USB 文件传输");
    usb_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(ui->content, "内存不足，请返回后重试", LV_ALIGN_CENTER, 0, 0,
                        0xFF8C8C, app_ui_chinese_font());
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = usb_cleanup;
    ui->page_can_exit = usb_can_exit;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *status_card = lv_obj_create(content);
    lv_obj_set_pos(status_card, 0, 0);
    lv_obj_set_size(status_card, 404, 144);
    app_connection_card_style(status_card);
    app_ui_add_text(status_card, LV_SYMBOL_USB, LV_ALIGN_TOP_LEFT, 16, 16,
                    CONNECTION_ACCENT, &lv_font_montserrat_16);
    page->state = text(status_card, "", 46, 14, 338, 44, CONNECTION_TEXT);
    page->details = text(status_card, "", 46, 65, 338, 66, CONNECTION_DETAIL);

    lv_obj_t *notice = lv_obj_create(content);
    lv_obj_set_pos(notice, 0, 155);
    lv_obj_set_size(notice, 404, 100);
    app_connection_card_style(notice);
    app_ui_add_text(notice, LV_SYMBOL_WARNING, LV_ALIGN_TOP_LEFT, 16, 15,
                    CONNECTION_WARNING, &lv_font_montserrat_16);
    text(notice, "退出前请在电脑安全弹出。", 46, 13, 342, 22, CONNECTION_WARNING);
    text(notice, "传输时电脑独占 TF 卡。\n连接后下载串口可能暂不可用。", 46, 43, 342, 44, CONNECTION_DETAIL);
    page->enter = button(ui, "开启传输", 0, true);
    page->leave = button(ui, "退出并恢复", 208, false);
    app_ui_set_timer(ui, usb_timer, 300);
    update_usb(ui);
}
