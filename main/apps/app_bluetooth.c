#include "app_ui_internal.h"
#include "app_audio_system.h"
#include "aether_bluetooth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    app_ui_t *ui;
    aether_bluetooth_handle_t bluetooth;
    lv_obj_t *status, *message, *toggle, *toggle_label, *scan, *disconnect, *route;
    lv_obj_t *volume, *volume_label, *list, *rows[AETHER_BLUETOOTH_MAX_DEVICES];
    lv_obj_t *names[AETHER_BLUETOOTH_MAX_DEVICES];
    aether_bluetooth_device_t devices[AETHER_BLUETOOTH_MAX_DEVICES];
    aether_bluetooth_device_t pressed_device;
    bool valid[AETHER_BLUETOOTH_MAX_DEVICES], selecting, selection_armed;
    uint8_t pressed_index;
    uint8_t count;
} bluetooth_page_t;

static const char *state_name(aether_bluetooth_state_t state)
{
    switch (state) {
    case AETHER_BT_OFF: return "蓝牙已关闭";
    case AETHER_BT_STARTING: return "正在开启蓝牙";
    case AETHER_BT_IDLE: return "蓝牙已开启，未连接";
    case AETHER_BT_SCANNING: return "正在搜索附近设备";
    case AETHER_BT_CONNECTING: return "正在配对并连接耳机";
    case AETHER_BT_CONNECTED: return "耳机已连接";
    case AETHER_BT_DISCONNECTING: return "正在断开耳机";
    case AETHER_BT_STOPPING: return "正在关闭蓝牙";
    default: return "蓝牙启动失败，可关闭后重试";
    }
}
static void availability(lv_obj_t *object, bool enabled)
{ if (enabled) lv_obj_remove_state(object,LV_STATE_DISABLED); else lv_obj_add_state(object,LV_STATE_DISABLED); }
static void show_error(bluetooth_page_t *page, esp_err_t error)
{
    if (error != ESP_OK) app_ui_label_fmt(page->message,"操作未受理：%s",esp_err_to_name(error));
    else app_ui_label_text(page->message,"");
}
static void action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    bluetooth_page_t *page = ui ? ui->page_context : NULL;
    if (!page || !page->bluetooth) return;
    const intptr_t action = (intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
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
    lv_obj_set_style_radius(object,12,0);
    lv_obj_set_style_bg_color(object,lv_color_hex(0x2768D8),0);
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
    if (error == ESP_OK) app_ui_label_fmt(page->message,"已请求连接：%s",
        page->pressed_device.name[0] ? page->pressed_device.name : "未命名设备");
}
static void volume_changed(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    bluetooth_page_t *page = ui ? ui->page_context : NULL;
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
    const char *peer = status.peer.name[0] ? status.peer.name : "未命名耳机";
    if (status.connected) app_ui_label_fmt(page->status,"%s · %s\n%lu Hz / %s · %s\n缓冲 %u 帧 · 欠载 %lu",
        state_name(status.state),peer,(unsigned long)status.sample_rate,status.channels == 1 ? "单声道" : "立体声",
        audio.applied_output == APP_AUDIO_OUTPUT_BLUETOOTH ? "声音输出到耳机" : "声音输出到扬声器",
        (unsigned)status.queued_frames,(unsigned long)status.underruns);
    else if (status.state == AETHER_BT_CONNECTING) app_ui_label_fmt(page->status,"%s\n%s\n第 %lu 次连接，请稍候",
        state_name(status.state),peer,(unsigned long)status.connection_attempts);
    else app_ui_label_fmt(page->status,"%s\n搜索前请将耳机设为配对模式\n已发现 %u 个设备%s",state_name(status.state),status.device_count,
        status.scanning ? "，搜索约13秒" : "");
    if (!status.connected && audio.applied_output == APP_AUDIO_OUTPUT_BLUETOOTH)
        app_ui_label_text(page->message,"耳机已断开，播放已停止。重连后重新播放，或手动切回扬声器。");
    if (status.last_error != ESP_OK) {
        if (status.acl_status > 0) app_ui_label_fmt(page->message,"ACL 连接失败：0x%02X\n未完成连接，可关闭蓝牙后重试",(unsigned)status.acl_status);
        else if (status.auth_status > 0) app_ui_label_fmt(page->message,"配对认证失败：0x%02X\n请将耳机重新设为配对模式",(unsigned)status.auth_status);
        else if (status.acl_disconnect_reason >= 0) app_ui_label_fmt(page->message,"蓝牙链路已断开：0x%02X\n%s",(unsigned)status.acl_disconnect_reason,esp_err_to_name(status.last_error));
        else if (status.a2dp_disconnect_reason >= 0) app_ui_label_fmt(page->message,"耳机音频连接失败：0x%02X\n%s",(unsigned)status.a2dp_disconnect_reason,esp_err_to_name(status.last_error));
        else app_ui_label_fmt(page->message,"蓝牙：%s",esp_err_to_name(status.last_error));
    } else if (status.state == AETHER_BT_CONNECTING) {
        app_ui_label_text(page->message,status.acl_status < 0 ? "正在建立蓝牙链路…" :
            status.auth_status == 0 ? "配对已完成，正在连接音频服务…" : "链路已建立，正在配对并连接音频…");
    } else if (status.connected) app_ui_label_text(page->message,
        audio.applied_output == APP_AUDIO_OUTPUT_BLUETOOTH ? "声音输出到耳机" : "耳机已连接，点击“使用耳机”切换输出");
    app_ui_label_text(page->toggle_label,status.audio_requested ? "关闭蓝牙" : "开启蓝牙");
    const bool can_select = status.audio_requested && status.enabled &&
        (status.state == AETHER_BT_IDLE || status.state == AETHER_BT_SCANNING);
    availability(page->scan,status.audio_requested && status.enabled && status.state == AETHER_BT_IDLE && !status.scanning);
    availability(page->disconnect,status.connected || status.state == AETHER_BT_CONNECTING);
    availability(page->route,status.state == AETHER_BT_CONNECTED && status.connected && status.sample_rate);
    if (!lv_obj_has_state(page->volume,LV_STATE_PRESSED)) lv_slider_set_value(page->volume,audio.master_volume,LV_ANIM_OFF);
    app_ui_label_fmt(page->volume_label,"系统音量 %u%%",audio.master_volume);
    /* Discovery can replace a non-audio slot when its list is full. Keep both
     * the visible name and address stable throughout this physical gesture. */
    if (page->selecting) return;
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
        app_ui_label_fmt(page->names[i],"%s%s\n%02X:%02X:%02X:%02X:%02X:%02X  %d dBm",
            device.audio_device ? "耳机/音响 · " : "", device.name[0] ? device.name : "未命名设备",
            device.address[0],device.address[1],device.address[2],device.address[3],device.address[4],device.address[5],device.rssi);
        lv_obj_remove_flag(page->rows[i],LV_OBJ_FLAG_HIDDEN);
        availability(page->rows[i],can_select && !status.connected);
    }
    page->count = status.device_count > AETHER_BLUETOOTH_MAX_DEVICES ?
        AETHER_BLUETOOTH_MAX_DEVICES : status.device_count;
}
static void cleanup(app_ui_t *ui) { free(ui->page_context); ui->page_context = NULL; }

void app_open_bluetooth(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui,"蓝牙耳机");
    bluetooth_page_t *page = calloc(1,sizeof(*page));
    if (!page) { app_ui_add_text(content,"内存不足",LV_ALIGN_TOP_LEFT,0,0,0xFFB17A,app_ui_chinese_font()); return; }
    page->ui = ui; page->bluetooth = app_audio_system_bluetooth();
    ui->page_context = page; ui->page_cleanup = cleanup;
    if (!page->bluetooth) { app_ui_add_text(content,"蓝牙服务未初始化",LV_ALIGN_TOP_LEFT,0,0,0xFFB17A,app_ui_chinese_font()); return; }
    page->status = app_ui_add_text(content,"",LV_ALIGN_TOP_LEFT,0,0,0xD9E8FF,app_ui_chinese_font());
    lv_obj_set_width(page->status,402); lv_obj_set_height(page->status,70);
    lv_label_set_long_mode(page->status,LV_LABEL_LONG_DOT);
    page->toggle = button(page,content,"开启蓝牙",0,80,128,1);
    page->scan = button(page,content,"搜索耳机",138,80,128,2);
    page->disconnect = button(page,content,"断开连接",276,80,128,3);
    page->route = button(page,content,"使用耳机",0,134,194,4);
    button(page,content,"使用扬声器",210,134,194,5);
    page->volume_label = app_ui_add_text(content,"",LV_ALIGN_TOP_LEFT,0,188,0xC3D8FF,app_ui_chinese_font());
    page->volume = lv_slider_create(content); lv_obj_set_pos(page->volume,0,219); lv_obj_set_size(page->volume,402,18);
    lv_obj_set_ext_click_area(page->volume,14);
    lv_slider_set_range(page->volume,0,100);
    lv_obj_add_event_cb(page->volume,volume_changed,LV_EVENT_VALUE_CHANGED,ui);
    lv_obj_add_event_cb(page->volume,volume_changed,LV_EVENT_RELEASED,ui);
    page->message = app_ui_add_text(content,"",LV_ALIGN_TOP_LEFT,0,258,0xFFB17A,app_ui_chinese_font());
    lv_obj_set_width(page->message,402); lv_obj_set_height(page->message,36);
    lv_label_set_long_mode(page->message,LV_LABEL_LONG_DOT);
    page->list = lv_obj_create(content); lv_obj_set_pos(page->list,0,302); lv_obj_set_size(page->list,404,300);
    lv_obj_set_style_bg_opa(page->list,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(page->list,0,0);
    lv_obj_set_style_pad_all(page->list,0,0); lv_obj_set_style_pad_row(page->list,8,0);
    lv_obj_set_flex_flow(page->list,LV_FLEX_FLOW_COLUMN);
    for (size_t i = 0; i < AETHER_BLUETOOTH_MAX_DEVICES; ++i) {
        lv_obj_t *row = lv_button_create(page->list); page->rows[i] = row;
        lv_obj_set_size(row,382,76); lv_obj_set_flex_grow(row,0);
        lv_obj_set_style_bg_color(row,lv_color_hex(0x182B48),0);
        page->names[i] = app_ui_add_text(row,"",LV_ALIGN_LEFT_MID,0,0,0xE4EDFF,app_ui_chinese_font());
        lv_obj_set_width(page->names[i],350); lv_obj_set_height(page->names[i],44);
        lv_label_set_long_mode(page->names[i],LV_LABEL_LONG_DOT);
        lv_obj_set_user_data(row,(void *)(uintptr_t)i);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_PRESSED,ui);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_PRESS_LOST,ui);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_INDEV_RESET,ui);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_RELEASED,ui);
        lv_obj_add_event_cb(row,device_clicked,LV_EVENT_CLICKED,ui); lv_obj_add_flag(row,LV_OBJ_FLAG_HIDDEN);
    }
    app_ui_set_timer(ui,update,300); update(ui->timer);
}
