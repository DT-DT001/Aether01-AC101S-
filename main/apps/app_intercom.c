#include "app_intercom.h"
#include "app_audio_system.h"
#include "app_connection_style.h"
#include "app_power.h"
#include <stdlib.h>

typedef struct {
    lv_obj_t *badge, *headline, *detail, *peer, *talk, *talk_label, *pair, *pair_label, *forget;
    esp_err_t request_error;
    uint32_t badge_color;
    bool suspended, resume_pending;
} intercom_page_t;

static void cleanup(app_ui_t *ui);

static void set_enabled(lv_obj_t *object, bool enabled)
{
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void update(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    intercom_page_t *page = ui->page_context;
    if (page->suspended) return;
    if (page->resume_pending && !app_ui_input_blocked(ui) && !app_power_is_sleeping(ui->power)) {
        page->resume_pending = false;
        page->request_error = app_intercom_enter(ui->board->audio, ui->io);
    }
    app_intercom_status_t status;
    app_intercom_get_status(&status);
    const aether_intercom_radio_status_t *radio = &status.radio;
    const bool starting = radio->state == AETHER_INTERCOM_RADIO_STARTING ||
        (status.active && radio->state == AETHER_INTERCOM_RADIO_STOPPED && status.audio_error == ESP_OK);
    const bool searching = radio->state == AETHER_INTERCOM_RADIO_SEARCHING;
    const bool paired = radio->state == AETHER_INTERCOM_RADIO_PAIRED;
    const bool speaking = status.transmitting || status.receiving;
    const bool retry = !status.active || (!starting && radio->state == AETHER_INTERCOM_RADIO_STOPPED) ||
        radio->state == AETHER_INTERCOM_RADIO_ERROR;
    const char *headline = "等待配对";
    const char *detail = "让两台设备靠近后重新搜索。";
    const char *icon = LV_SYMBOL_WIFI;
    uint32_t color = 0x233956;

    if (status.transmitting) {
        headline = "正在发送";
        detail = "松开结束。";
        icon = LV_SYMBOL_UPLOAD;
        color = 0x275D55;
    } else if (status.receiving) {
        headline = "对方正在讲话";
        detail = "对方说完后再回复。";
        icon = LV_SYMBOL_VOLUME_MAX;
        color = 0x274F78;
    } else if (starting) {
        headline = "正在启动";
        detail = "";
    } else if (retry) {
        headline = "对讲未启动";
        detail = "";
    } else if (searching) {
        headline = "搜索附近设备";
        detail = "请在另一台设备打开无线对讲。";
    } else if (paired) {
        headline = radio->peer_online ? "对方在线" : "已配对 · 等待对方";
        detail = radio->peer_online ? "" : "请在另一台设备打开无线对讲。";
        color = radio->peer_online ? 0x265367 : 0x384051;
    }

    app_ui_label_text(lv_obj_get_child(page->badge, 0), icon);
    if (page->badge_color != color) {
        page->badge_color = color;
        lv_obj_set_style_bg_color(page->badge, lv_color_hex(color), 0);
    }
    app_ui_label_text(page->headline, headline);
    app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    const esp_err_t error = page->request_error != ESP_OK ? page->request_error :
        status.audio_error != ESP_OK ? status.audio_error : radio->last_error;
    if (error != ESP_OK && detail[0]) app_ui_label_fmt(page->detail, "%s\n%s", detail, esp_err_to_name(error));
    else if (error != ESP_OK) app_ui_label_fmt(page->detail, "操作失败：%s", esp_err_to_name(error));
    else if (paired && audio.ready && !audio.master_volume)
        app_ui_label_text(page->detail, "扬声器已静音，请在设置中调高音量。");
    else app_ui_label_text(page->detail, detail);

    if (paired && radio->peer_online)
        app_ui_label_fmt(page->peer, "设备 %02X:%02X:%02X  ·  %d dBm",
            radio->peer_mac[3], radio->peer_mac[4], radio->peer_mac[5], radio->rssi);
    else if (paired)
        app_ui_label_fmt(page->peer, "设备 %02X:%02X:%02X",
            radio->peer_mac[3], radio->peer_mac[4], radio->peer_mac[5]);
    else app_ui_label_text(page->peer, "");

    app_ui_label_text(page->pair_label, retry ? "重试启动" : searching ? "搜索中" : "搜索附近");
    set_enabled(page->pair, !speaking && !starting && !searching && (retry || !paired));
    set_enabled(page->forget, paired && !speaking);
    const bool talk_available = status.active && paired && radio->peer_online && !status.receiving;
    set_enabled(page->talk, talk_available);
    app_ui_label_text(page->talk_label, status.transmitting ? "松开结束" : "按住讲话");
    if (status.transmitting) lv_obj_add_state(page->talk, LV_STATE_CHECKED);
    else lv_obj_remove_state(page->talk, LV_STATE_CHECKED);
    if (!talk_available) app_intercom_set_screen_talk(false);
    if (speaking && !app_power_is_sleeping(ui->power))
        lv_display_trigger_activity(lv_obj_get_display(ui->screen));
}

static void pair_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || ui->page_cleanup != cleanup || lv_obj_get_screen(lv_event_get_target_obj(event)) != ui->screen)
        return;
    intercom_page_t *page = ui->page_context;
    if (page->suspended || app_ui_input_blocked(ui)) return;
    app_intercom_status_t status;
    app_intercom_get_status(&status);
    if (!status.active || status.radio.state == AETHER_INTERCOM_RADIO_STOPPED ||
        status.radio.state == AETHER_INTERCOM_RADIO_ERROR) {
        page->request_error = app_intercom_enter(ui->board->audio, ui->io);
    } else {
        page->request_error = app_intercom_search();
    }
    update(ui->timer);
}

static void forget_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || ui->page_cleanup != cleanup || lv_obj_get_screen(lv_event_get_target_obj(event)) != ui->screen)
        return;
    intercom_page_t *page = ui->page_context;
    if (page->suspended || app_ui_input_blocked(ui)) return;
    page->request_error = app_intercom_forget();
    update(ui->timer);
}

static void intercom_suspend(app_ui_t *ui, bool suspended)
{
    intercom_page_t *page = ui->page_context;
    page->suspended = suspended;
    page->resume_pending = !suspended;
    if (suspended) {
        app_intercom_set_screen_talk(false);
        app_intercom_leave();
    }
}

static void cleanup(app_ui_t *ui)
{
    app_intercom_leave();
    (void)app_io_release_media(ui->io);
    free(ui->page_context);
    ui->page_context = NULL;
}

static void talk_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || ui->page_cleanup != cleanup || lv_obj_get_screen(lv_event_get_target_obj(event)) != ui->screen)
        return;
    intercom_page_t *page = ui->page_context;
    const bool pressed = lv_event_get_code(event) == LV_EVENT_PRESSED;
    if (!pressed || (!page->suspended && !app_ui_input_blocked(ui) &&
        !lv_obj_has_state(page->talk, LV_STATE_DISABLED))) app_intercom_set_screen_talk(pressed);
}

static lv_obj_t *button(app_ui_t *ui, const char *text, int x, bool primary,
                        lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(ui->content);
    lv_obj_set_pos(object, x, 281);
    lv_obj_set_size(object, 198, 44);
    app_connection_button_style(object, primary);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, ui);
    app_ui_add_text(object, text, LV_ALIGN_CENTER, 0, 0, 0xF2F6FF, NULL);
    return object;
}

void app_open_intercom(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "无线对讲");
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    intercom_page_t *page = calloc(1, sizeof(*page));
    const bool blocked = app_ui_input_blocked(ui);
    if (!page || !ui->board->audio_ready || blocked) {
        app_ui_add_text(content, blocked ? "系统正在准备关机" : page ? "板载音频未就绪" : "内存不足",
                        LV_ALIGN_CENTER, 0, 0, 0xFFD2B1, app_ui_chinese_font_size(24));
        free(page);
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = cleanup;
    ui->page_suspend = intercom_suspend;
    app_ui_media_control_gate(ui, true, true);

    lv_obj_t *card = lv_obj_create(content);
    lv_obj_set_size(card, 408, 110);
    app_connection_card_style(card);
    page->badge = lv_obj_create(card);
    lv_obj_set_size(page->badge, 52, 52);
    lv_obj_set_pos(page->badge, 14, 12);
    lv_obj_remove_flag(page->badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(page->badge, 16, 0);
    lv_obj_set_style_pad_all(page->badge, 0, 0);
    lv_obj_set_style_border_width(page->badge, 0, 0);
    app_ui_add_text(page->badge, LV_SYMBOL_WIFI, LV_ALIGN_CENTER, 0, 0, 0xEAF2FF,
                    &lv_font_montserrat_28);
    page->headline = app_ui_add_text(card, "", LV_ALIGN_TOP_LEFT, 81, 10, CONNECTION_TEXT,
                                     app_ui_chinese_font_size(24));
    page->peer = app_ui_add_text(card, "", LV_ALIGN_TOP_LEFT, 82, 42, CONNECTION_ACCENT, NULL);
    page->detail = app_ui_add_text(card, "", LV_ALIGN_TOP_LEFT, 14, 70, CONNECTION_DETAIL, NULL);
    lv_obj_set_size(page->detail, 378, 36);
    lv_label_set_long_mode(page->detail, LV_LABEL_LONG_DOT);
    page->talk = lv_button_create(content);
    lv_obj_set_pos(page->talk, 0, 120);
    lv_obj_set_size(page->talk, 408, 151);
    app_connection_button_style(page->talk, true);
    lv_obj_set_style_radius(page->talk, 25, 0);
    lv_obj_set_style_bg_color(page->talk, lv_color_hex(0x244660), 0);
    lv_obj_set_style_bg_color(page->talk, lv_color_hex(0x296A70), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(page->talk, lv_color_hex(0x286C63), LV_STATE_CHECKED);
    lv_obj_remove_flag(page->talk, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *signal = lv_obj_create(page->talk);
    lv_obj_set_size(signal, 58, 58);
    lv_obj_align(signal, LV_ALIGN_CENTER, 0, -22);
    lv_obj_remove_flag(signal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(signal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(signal, 0, 0);
    lv_obj_set_style_bg_color(signal, lv_color_hex(0x306079), 0);
    app_ui_add_text(signal, LV_SYMBOL_UPLOAD, LV_ALIGN_CENTER, 0, 0,
                    CONNECTION_ACCENT, &lv_font_montserrat_28);
    page->talk_label = app_ui_add_text(page->talk, "按住讲话", LV_ALIGN_CENTER, 0, 36,
                    CONNECTION_TEXT, app_ui_chinese_font_size(24));
    lv_obj_add_event_cb(page->talk, talk_event, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(page->talk, talk_event, LV_EVENT_RELEASED, ui);
    lv_obj_add_event_cb(page->talk, talk_event, LV_EVENT_PRESS_LOST, ui);
    lv_obj_add_event_cb(page->talk, talk_event, LV_EVENT_INDEV_RESET, ui);
    page->pair = button(ui, "搜索附近", 0, true, pair_clicked);
    page->pair_label = lv_obj_get_child(page->pair, 0);
    page->forget = button(ui, "解除配对", 210, false, forget_clicked);
    app_ui_add_text(content, "对讲时暂停 Wi-Fi 联网", LV_ALIGN_BOTTOM_MID, 0, -3, CONNECTION_DETAIL, NULL);
    page->request_error = app_intercom_enter(ui->board->audio, ui->io);
    app_ui_set_timer(ui, update, 100);
    update(ui->timer);
}
