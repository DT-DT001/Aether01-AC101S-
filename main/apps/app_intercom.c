#include "app_intercom.h"
#include "app_audio_system.h"
#include "app_ui_internal.h"
#include <stdlib.h>

typedef struct {
    lv_obj_t *badge, *headline, *detail, *peer, *pair, *pair_label, *forget;
    esp_err_t request_error;
    uint32_t badge_color;
} intercom_page_t;

static void set_enabled(lv_obj_t *object, bool enabled)
{
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void update(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    intercom_page_t *page = ui->page_context;
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
    const char *detail = "搜索结束。把两台机器靠近后，点「搜索附近」重试。";
    const char *icon = LV_SYMBOL_WIFI;
    uint32_t color = 0x233956;

    if (status.transmitting) {
        headline = "正在发送";
        detail = "对着板载麦克风讲话，松开用户键结束。";
        icon = LV_SYMBOL_UPLOAD;
        color = 0x275D55;
    } else if (status.receiving) {
        headline = "对方正在讲话";
        detail = "对方说完后，按住用户键即可回复。";
        icon = LV_SYMBOL_VOLUME_MAX;
        color = 0x274F78;
    } else if (starting) {
        headline = "正在启动";
        detail = "正在准备无线和音频资源。";
    } else if (retry) {
        headline = "对讲未启动";
        detail = "点「重试启动」再试一次。";
    } else if (searching) {
        headline = "正在搜索附近设备";
        detail = "请把两台机器放到一起，并打开对方的无线对讲。";
    } else if (paired) {
        headline = radio->peer_online ? "对方在线" : "已配对 · 等待对方";
        detail = radio->peer_online ? "按住用户键讲话，松开后收听。" : "请让对方也打开对讲页面。";
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
    if (error != ESP_OK) app_ui_label_fmt(page->detail, "%s\n%s", detail, esp_err_to_name(error));
    else if (paired && audio.ready && !audio.master_volume)
        app_ui_label_fmt(page->detail, "%s\n扬声器已静音，请在设置中调高音量。", detail);
    else app_ui_label_text(page->detail, detail);

    if (paired && radio->peer_online)
        app_ui_label_fmt(page->peer, "设备 %02X:%02X:%02X  ·  %d dBm",
            radio->peer_mac[3], radio->peer_mac[4], radio->peer_mac[5], radio->rssi);
    else if (paired)
        app_ui_label_fmt(page->peer, "设备 %02X:%02X:%02X",
            radio->peer_mac[3], radio->peer_mac[4], radio->peer_mac[5]);
    else app_ui_label_text(page->peer, "ESP-NOW · 双机直连");

    app_ui_label_text(page->pair_label, retry ? "重试启动" : searching ? "搜索中" : "搜索附近");
    set_enabled(page->pair, !speaking && !starting && !searching && (retry || !paired));
    set_enabled(page->forget, paired && !speaking);
    if (speaking && !app_power_is_sleeping(ui->power))
        lv_display_trigger_activity(lv_obj_get_display(ui->screen));
}

static void pair_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    intercom_page_t *page = ui->page_context;
    app_intercom_status_t status;
    app_intercom_get_status(&status);
    if (!status.active || status.radio.state == AETHER_INTERCOM_RADIO_STOPPED ||
        status.radio.state == AETHER_INTERCOM_RADIO_ERROR) {
        if (app_ui_input_blocked(ui)) return;
        page->request_error = app_intercom_enter(ui->board->audio, ui->io);
    } else {
        page->request_error = app_intercom_search();
    }
    update(ui->timer);
}

static void forget_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    intercom_page_t *page = ui->page_context;
    page->request_error = app_intercom_forget();
    update(ui->timer);
}

static void cleanup(app_ui_t *ui)
{
    app_intercom_leave();
    (void)app_io_release_media(ui->io);
    free(ui->page_context);
    ui->page_context = NULL;
}

static lv_obj_t *button(app_ui_t *ui, const char *text, int x, uint32_t color,
                        lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(ui->content);
    lv_obj_set_pos(object, x, 253);
    lv_obj_set_size(object, 198, 50);
    lv_obj_set_style_radius(object, 14, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x202B3B), LV_STATE_DISABLED);
    lv_obj_set_style_opa(object, LV_OPA_60, LV_STATE_DISABLED);
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
    app_ui_media_control_gate(ui, true, true);

    page->badge = lv_obj_create(content);
    lv_obj_set_size(page->badge, 80, 80);
    lv_obj_align(page->badge, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_remove_flag(page->badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(page->badge, 26, 0);
    lv_obj_set_style_border_width(page->badge, 0, 0);
    app_ui_add_text(page->badge, LV_SYMBOL_WIFI, LV_ALIGN_CENTER, 0, 0, 0xEAF2FF,
                    &lv_font_montserrat_28);
    page->headline = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 106, 0xF2F6FF,
                                     app_ui_chinese_font_size(24));
    page->detail = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 151, 0xC3D0E5, NULL);
    lv_obj_set_width(page->detail, 404);
    lv_obj_set_style_text_align(page->detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(page->detail, LV_LABEL_LONG_WRAP);
    page->peer = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 221, 0x9FB5D2, NULL);
    page->pair = button(ui, "搜索附近", 0, 0x286BC0, pair_clicked);
    page->pair_label = lv_obj_get_child(page->pair, 0);
    page->forget = button(ui, "解除配对", 210, 0x2E415B, forget_clicked);
    app_ui_add_text(content, "对讲期间暂停 Wi-Fi 联网", LV_ALIGN_BOTTOM_MID, 0, -12, 0x8299B8, NULL);
    page->request_error = app_intercom_enter(ui->board->audio, ui->io);
    app_ui_set_timer(ui, update, 100);
    update(ui->timer);
}
