#include "app_connection_style.h"
#include "app_audio_system.h"
#include "aether_bluetooth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    app_ui_t *ui;
    aether_bluetooth_handle_t bluetooth;
    lv_obj_t *status, *detail, *message, *toggle_label, *scan, *disconnect, *route, *speaker;
    lv_obj_t *volume, *volume_label, *list, *rows[AETHER_BLUETOOTH_MAX_DEVICES];
    lv_obj_t *names[AETHER_BLUETOOTH_MAX_DEVICES], *metrics[AETHER_BLUETOOTH_MAX_DEVICES];
    lv_obj_t *icons[AETHER_BLUETOOTH_MAX_DEVICES];
    aether_bluetooth_device_t devices[AETHER_BLUETOOTH_MAX_DEVICES];
    aether_bluetooth_device_t pressed_device;
    uint32_t notice_started;
    bool notice, notice_layout;
    bool valid[AETHER_BLUETOOTH_MAX_DEVICES], selecting, selection_armed;
    uint8_t pressed_index;
    uint8_t count;
} bluetooth_page_t;

static const char *state_name(aether_bluetooth_state_t state)
{
    switch (state) {
    case AETHER_BT_OFF: return "已关闭";
    case AETHER_BT_STARTING: return "正在开启";
    case AETHER_BT_IDLE: return "未连接";
    case AETHER_BT_SCANNING: return "正在搜索";
    case AETHER_BT_CONNECTING: return "正在配对";
    case AETHER_BT_CONNECTED: return "已连接";
    case AETHER_BT_DISCONNECTING: return "正在断开";
    case AETHER_BT_STOPPING: return "正在关闭";
    default: return "连接异常";
    }
}
static void availability(lv_obj_t *object, bool enabled)
{ if (enabled) lv_obj_remove_state(object,LV_STATE_DISABLED); else lv_obj_add_state(object,LV_STATE_DISABLED); }
static void show_error(bluetooth_page_t *page, esp_err_t error)
{
    page->notice = error != ESP_OK;
    page->notice_started = lv_tick_get();
    if (error != ESP_OK) app_ui_label_fmt(page->message,"操作未受理：%s",esp_err_to_name(error));
    else app_ui_label_text(page->message,"");
}
static void action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_current_target_obj(event);
    if (!ui || lv_obj_get_screen(target) != ui->screen) return;
    bluetooth_page_t *page = ui->page_context;
    if (!page || !page->bluetooth) return;
    const intptr_t action = (intptr_t)lv_obj_get_user_data(target);
    aether_bluetooth_status_t status;
    if (aether_bluetooth_get_status(page->bluetooth,&status) != ESP_OK) return;
    esp_err_t error;
    if (action == 1) error = aether_bluetooth_set_enabled(page->bluetooth,!status.audio_requested);
    else if (action == 2) error = aether_bluetooth_scan(page->bluetooth);
    else if (action == 3) error = aether_bluetooth_disconnect(page->bluetooth);
    else error = app_audio_system_set_output(action == 4 ? APP_AUDIO_OUTPUT_BLUETOOTH : APP_AUDIO_OUTPUT_SPEAKER);
    show_error(page,error);
}
static lv_obj_t *button(bluetooth_page_t *page, lv_obj_t *parent, const char *text,
                         int x, int y, int width, intptr_t kind)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_pos(object,x,y); lv_obj_set_size(object,width,44);
    app_connection_button_style(object,kind==1);
    lv_obj_set_style_bg_color(object,lv_color_hex(CONNECTION_BLUE),LV_STATE_CHECKED);
    lv_obj_set_user_data(object,(void *)kind);
    lv_obj_add_event_cb(object,action,LV_EVENT_CLICKED,page->ui);
    lv_obj_t *label = app_ui_add_text(object,text,LV_ALIGN_CENTER,0,0,0xFFFFFF,app_ui_chinese_font());
    if (kind == 1) page->toggle_label = label;
    return object;
}
static void device_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_target_obj(event);
    /* A retired screen may receive input cancellation during navigation. */
    if (!ui || lv_obj_get_parent(lv_obj_get_parent(row)) != ui->content) return;
    bluetooth_page_t *page = ui ? ui->page_context : NULL;
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(row);
    if (!page) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        page->selecting = page->selection_armed = false;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        page->selecting = false;
        lv_indev_t *input = lv_indev_active();
        if (input && lv_indev_get_scroll_obj(input)) page->selection_armed = false;
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        page->selecting = index < page->count && page->valid[index];
        page->selection_armed = page->selecting;
        if (page->selecting) {
            page->pressed_index = (uint8_t)index;
            page->pressed_device = page->devices[index];
        }
        return;
    }
    if (!page->selection_armed || index != page->pressed_index) return;
    page->selecting = page->selection_armed = false;
    aether_bluetooth_status_t current;
    const esp_err_t state_error = aether_bluetooth_get_status(page->bluetooth,&current);
    if (state_error != ESP_OK || !current.audio_requested || !current.enabled ||
        (current.state != AETHER_BT_IDLE && current.state != AETHER_BT_SCANNING)) {
        show_error(page,state_error != ESP_OK ? state_error : ESP_ERR_INVALID_STATE);
        return;
    }
    const esp_err_t error = aether_bluetooth_connect(page->bluetooth,page->pressed_device.address);
    show_error(page,error);
}
static void volume_changed(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || lv_obj_get_screen(lv_event_get_current_target_obj(event)) != ui->screen) return;
    bluetooth_page_t *page = ui->page_context;
    if (!page) return;
    uint8_t volume = (uint8_t)lv_slider_get_value(page->volume);
    show_error(page,app_audio_system_set_volume(volume,lv_event_get_code(event)==LV_EVENT_RELEASED));
    app_ui_label_fmt(page->volume_label,"系统音量 %u%%",volume);
}
static void update(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    bluetooth_page_t *page = ui->page_context;
    if (!page || !page->bluetooth) return;
    aether_bluetooth_status_t status;
    if (aether_bluetooth_get_status(page->bluetooth,&status) != ESP_OK) return;
    app_audio_system_snapshot_t audio; app_audio_system_get_snapshot(&audio);
    const bool linked = status.state == AETHER_BT_CONNECTED && status.connected;
    const char *peer = status.peer.name[0] ? status.peer.name : "未命名音频设备";
    app_ui_label_text(page->status,linked ? peer : state_name(status.state));
    if (linked) app_ui_label_fmt(page->detail,"%lu Hz · %s",(unsigned long)status.sample_rate,
                                status.channels == 1 ? "单声道" : "立体声");
    else if (status.state == AETHER_BT_CONNECTING) app_ui_label_text(page->detail,peer);
    else if (status.scanning || status.device_count) app_ui_label_fmt(page->detail,"附近设备  %u",status.device_count);
    else app_ui_label_text(page->detail,"");
    if (status.last_error != ESP_OK) {
        if (status.acl_status > 0) app_ui_label_fmt(page->message,"ACL 连接失败：0x%02X\n未完成连接，可关闭蓝牙后重试",(unsigned)status.acl_status);
        else if (status.auth_status > 0) app_ui_label_fmt(page->message,"配对认证失败：0x%02X\n请将设备重新设为配对模式",(unsigned)status.auth_status);
        else if (status.acl_disconnect_reason >= 0) app_ui_label_fmt(page->message,"蓝牙链路已断开：0x%02X\n%s",(unsigned)status.acl_disconnect_reason,esp_err_to_name(status.last_error));
        else if (status.a2dp_disconnect_reason >= 0) app_ui_label_fmt(page->message,"蓝牙音频连接失败：0x%02X\n%s",(unsigned)status.a2dp_disconnect_reason,esp_err_to_name(status.last_error));
        else app_ui_label_fmt(page->message,"蓝牙：%s",esp_err_to_name(status.last_error));
    } else if (!page->notice || lv_tick_elaps(page->notice_started) > 3000) {
        page->notice = false;
        if (!status.connected && audio.applied_output == APP_AUDIO_OUTPUT_BLUETOOTH)
            app_ui_label_text(page->message,"蓝牙音频设备已断开，可切回扬声器。");
        else if (!status.device_count && status.state == AETHER_BT_IDLE)
            app_ui_label_text(page->message,"请将音频设备设为配对模式。");
        else app_ui_label_text(page->message,"");
    }
    app_ui_label_text(page->toggle_label,status.audio_requested ? "关闭全部蓝牙" : "开启蓝牙");
    const bool can_select = status.audio_requested && status.enabled &&
        (status.state == AETHER_BT_IDLE || status.state == AETHER_BT_SCANNING);
    availability(page->scan,status.audio_requested && status.enabled && status.state == AETHER_BT_IDLE && !status.scanning);
    availability(page->disconnect,status.connected || status.state == AETHER_BT_CONNECTING);
    availability(page->route,status.state == AETHER_BT_CONNECTED && status.connected && status.sample_rate);
    if (audio.applied_output == APP_AUDIO_OUTPUT_BLUETOOTH) lv_obj_add_state(page->route,LV_STATE_CHECKED);
    else lv_obj_remove_state(page->route,LV_STATE_CHECKED);
    if (audio.applied_output == APP_AUDIO_OUTPUT_SPEAKER) lv_obj_add_state(page->speaker,LV_STATE_CHECKED);
    else lv_obj_remove_state(page->speaker,LV_STATE_CHECKED);
    if (!lv_obj_has_state(page->volume,LV_STATE_PRESSED)) lv_slider_set_value(page->volume,audio.master_volume,LV_ANIM_OFF);
    app_ui_label_fmt(page->volume_label,"系统音量 %u%%",audio.master_volume);
    /* Discovery can replace a non-audio slot when its list is full. Keep both
     * the visible name and address stable throughout this physical gesture. */
    if (page->selecting) return;
    const bool show_notice = lv_label_get_text(page->message)[0] != '\0';
    if (page->notice_layout != show_notice) {
        page->notice_layout = show_notice;
        lv_obj_set_y(page->list,show_notice ? 273 : 229);
        lv_obj_set_height(page->list,show_notice ? 81 : 125);
    }
    for (size_t i = 0; i < AETHER_BLUETOOTH_MAX_DEVICES; ++i) {
        page->valid[i] = false;
        if (i >= status.device_count) { lv_obj_add_flag(page->rows[i],LV_OBJ_FLAG_HIDDEN); continue; }
        aether_bluetooth_device_t device;
        if (aether_bluetooth_get_device(page->bluetooth,i,&device) != ESP_OK) {
            lv_obj_add_flag(page->rows[i],LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        page->devices[i] = device;
        page->valid[i] = true;
        app_ui_label_text(page->icons[i],device.audio_device ? LV_SYMBOL_AUDIO : LV_SYMBOL_BLUETOOTH);
        app_ui_label_text(page->names[i],device.name[0] ? device.name :
                          device.audio_device ? "未命名音频设备" : "未命名设备");
        app_ui_label_fmt(page->metrics[i],"%02X:%02X:%02X:%02X:%02X:%02X · %d dBm",
            device.address[0],device.address[1],device.address[2],device.address[3],device.address[4],device.address[5],device.rssi);
        lv_obj_remove_flag(page->rows[i],LV_OBJ_FLAG_HIDDEN);
        availability(page->rows[i],can_select && !status.connected);
    }
    page->count = status.device_count > AETHER_BLUETOOTH_MAX_DEVICES ?
        AETHER_BLUETOOTH_MAX_DEVICES : status.device_count;
}
static void cleanup(app_ui_t *ui) { free(ui->page_context); ui->page_context = NULL; }

static void open_bluetooth_audio(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui,"蓝牙音频");
    bluetooth_page_t *page = calloc(1,sizeof(*page));
    if (!page) { app_ui_add_text(content,"内存不足",LV_ALIGN_TOP_LEFT,0,0,0xFFB17A,app_ui_chinese_font()); return; }
    page->ui = ui; page->bluetooth = app_audio_system_bluetooth();
    ui->page_context = page; ui->page_cleanup = cleanup;
    if (!page->bluetooth) { app_ui_add_text(content,"蓝牙服务未初始化",LV_ALIGN_TOP_LEFT,0,0,0xFFB17A,app_ui_chinese_font()); return; }
    lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *card = lv_obj_create(content);
    lv_obj_set_pos(card,0,0); lv_obj_set_size(card,404,64);
    app_connection_card_style(card);
    app_ui_add_text(card,LV_SYMBOL_BLUETOOTH,LV_ALIGN_LEFT_MID,16,0,CONNECTION_ACCENT,&lv_font_montserrat_16);
    page->status = app_ui_add_text(card,"",LV_ALIGN_TOP_LEFT,50,8,CONNECTION_TEXT,app_ui_chinese_font());
    lv_obj_set_size(page->status,214,20); lv_label_set_long_mode(page->status,LV_LABEL_LONG_DOT);
    page->detail = app_ui_add_text(card,"",LV_ALIGN_TOP_LEFT,50,35,CONNECTION_DETAIL,app_ui_chinese_font());
    lv_obj_set_size(page->detail,214,20); lv_label_set_long_mode(page->detail,LV_LABEL_LONG_DOT);
    button(page,card,"开启蓝牙",278,9,113,1);
    page->scan = button(page,content,"搜索音频设备",0,73,197,2);
    page->disconnect = button(page,content,"断开连接",207,73,197,3);
    page->route = button(page,content,"使用蓝牙输出",0,124,197,4);
    page->speaker = button(page,content,"使用扬声器",207,124,197,5);
    lv_obj_t *volume_card = lv_obj_create(content);
    lv_obj_set_pos(volume_card,0,176); lv_obj_set_size(volume_card,404,46);
    app_connection_card_style(volume_card);
    page->volume_label = app_ui_add_text(volume_card,"",LV_ALIGN_LEFT_MID,12,0,CONNECTION_TEXT,app_ui_chinese_font());
    page->volume = lv_slider_create(volume_card); lv_obj_set_pos(page->volume,149,16); lv_obj_set_size(page->volume,236,12);
    lv_obj_set_style_bg_color(page->volume,lv_color_hex(CONNECTION_CONTROL),LV_PART_MAIN);
    lv_obj_set_style_bg_color(page->volume,lv_color_hex(CONNECTION_ACCENT),LV_PART_INDICATOR);
    lv_obj_set_ext_click_area(page->volume,14);
    lv_slider_set_range(page->volume,0,100);
    lv_obj_add_event_cb(page->volume,volume_changed,LV_EVENT_VALUE_CHANGED,ui);
    lv_obj_add_event_cb(page->volume,volume_changed,LV_EVENT_RELEASED,ui);
    page->message = app_ui_add_text(content,"",LV_ALIGN_TOP_LEFT,2,228,CONNECTION_WARNING,app_ui_chinese_font());
    lv_obj_set_size(page->message,402,42);
    lv_label_set_long_mode(page->message,LV_LABEL_LONG_DOT);
    page->list = lv_obj_create(content); lv_obj_set_pos(page->list,0,229); lv_obj_set_size(page->list,404,125);
    lv_obj_set_style_bg_opa(page->list,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(page->list,0,0);
    lv_obj_set_style_pad_all(page->list,0,0); lv_obj_set_style_pad_row(page->list,8,0);
    lv_obj_set_flex_flow(page->list,LV_FLEX_FLOW_COLUMN);
    for (size_t i = 0; i < AETHER_BLUETOOTH_MAX_DEVICES; ++i) {
        lv_obj_t *row = lv_button_create(page->list); page->rows[i] = row;
        lv_obj_set_size(row,396,56); lv_obj_set_flex_grow(row,0);
        app_connection_button_style(row,false);
        page->icons[i] = app_ui_add_text(row,LV_SYMBOL_BLUETOOTH,LV_ALIGN_LEFT_MID,14,0,
                                         CONNECTION_ACCENT,&lv_font_montserrat_16);
        page->names[i] = app_ui_add_text(row,"",LV_ALIGN_TOP_LEFT,40,6,CONNECTION_TEXT,app_ui_chinese_font());
        lv_obj_set_size(page->names[i],317,21);
        lv_label_set_long_mode(page->names[i],LV_LABEL_LONG_DOT);
        page->metrics[i] = app_ui_add_text(row,"",LV_ALIGN_TOP_LEFT,40,29,CONNECTION_DETAIL,app_ui_chinese_font());
        lv_obj_set_size(page->metrics[i],317,21); lv_label_set_long_mode(page->metrics[i],LV_LABEL_LONG_DOT);
        app_ui_add_text(row,LV_SYMBOL_RIGHT,LV_ALIGN_RIGHT_MID,-14,0,CONNECTION_DETAIL,&lv_font_montserrat_16);
        lv_obj_set_user_data(row,(void *)(uintptr_t)i);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_PRESSED,ui);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_PRESS_LOST,ui);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_INDEV_RESET,ui);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_RELEASED,ui);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_CLICKED,ui); lv_obj_add_flag(row,LV_OBJ_FLAG_HIDDEN);
    }
    app_ui_set_timer(ui,update,300); update(ui->timer);
}

static void open_role(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_current_target_obj(event);
    if (!ui || lv_obj_get_screen(row) != ui->screen) return;
    if (lv_obj_get_user_data(row)) app_open_pc_remote(ui);
    else open_bluetooth_audio(ui);
    ui->page_back = app_open_bluetooth;
}

static void role_row(app_ui_t *ui, lv_obj_t *parent, int y, bool hid,
                     const char *icon, const char *title, const char *detail)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, 404, 72);
    app_connection_button_style(row, false);
    lv_obj_set_user_data(row, (void *)(uintptr_t)hid);
    lv_obj_add_event_cb(row, open_role, LV_EVENT_CLICKED, ui);
    app_ui_add_text(row, icon, LV_ALIGN_LEFT_MID, 16, 0, CONNECTION_ACCENT, &lv_font_montserrat_16);
    app_ui_add_text(row, title, LV_ALIGN_TOP_LEFT, 49, 10, CONNECTION_TEXT, app_ui_chinese_font_size(24));
    app_ui_add_text(row, detail, LV_ALIGN_TOP_LEFT, 49, 42, CONNECTION_DETAIL, app_ui_chinese_font());
    app_ui_add_text(row, LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, -14, 0,
                    CONNECTION_DETAIL, &lv_font_montserrat_16);
}

void app_open_bluetooth(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "蓝牙");
    ui->page_back = app_open_settings;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    role_row(ui, content, 0, false, LV_SYMBOL_AUDIO, "音频输出", "连接耳机、音箱播放音乐");
    role_row(ui, content, 84, true, LV_SYMBOL_KEYBOARD, "蓝牙键鼠", "供手机、平板或电脑连接");
}
