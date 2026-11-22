#include "app_usb_input.h"
#include "app_ui_internal.h"
#include "app_text_input.h"
#include "app_text.h"
#include "aether_wifi.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    app_ui_t *ui;
    aether_wifi_handle_t wifi;
    lv_obj_t *status,*message,*toggle,*toggle_label,*scan,*disconnect,*forget;
    lv_obj_t *rows[AETHER_WIFI_MAX_APS],*names[AETHER_WIFI_MAX_APS];
    lv_obj_t *overlay,*panel,*password,*remember,*show_password,*password_notice;
    aether_wifi_ap_t aps[AETHER_WIFI_MAX_APS],selected;
    uint32_t generation;
    uint32_t notice_started;
    bool notice;
    uint8_t count;
} wifi_page_t;
static void availability(lv_obj_t *object,bool enabled)
{ if(enabled) lv_obj_remove_state(object,LV_STATE_DISABLED); else lv_obj_add_state(object,LV_STATE_DISABLED); }
static void show_error(wifi_page_t *p,esp_err_t error)
{
    p->notice=error!=ESP_OK; p->notice_started=lv_tick_get();
    if(error==ESP_OK) app_ui_label_text(p->message,""); else app_ui_label_fmt(p->message,"操作未受理：%s",esp_err_to_name(error));
}
static void close_password(wifi_page_t *p)
{
    if(!p->overlay) return;
    if(p->password) lv_textarea_set_text(p->password,"");
    lv_obj_delete(p->overlay);
    p->overlay=p->panel=p->password=p->remember=p->show_password=p->password_notice=NULL;
}
static void submit(wifi_page_t *p)
{
    if(!p || !p->overlay) return;
    const char *password=lv_textarea_get_text(p->password);
    esp_err_t error=aether_wifi_connect(p->wifi,&p->selected,password,lv_obj_has_state(p->remember,LV_STATE_CHECKED));
    if(error==ESP_OK) { close_password(p); app_ui_label_text(p->message,"正在连接"); }
    else if(error==ESP_ERR_INVALID_ARG && p->selected.password_required)
        app_ui_label_text(p->password_notice,"密码需要 8–63 个字符");
    else app_ui_label_fmt(p->password_notice,"连接失败：%s",esp_err_to_name(error));
}
static void password_event(lv_event_t *event)
{
    app_ui_t *ui=lv_event_get_user_data(event); wifi_page_t *p=ui ? ui->page_context : NULL;
    if(!p) return;
    intptr_t kind=(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if(lv_event_get_code(event)==LV_EVENT_READY || kind==1) submit(p);
    else if(lv_event_get_code(event)==LV_EVENT_CANCEL || kind==2) close_password(p);
    else if(kind==3 && p->password) lv_textarea_set_password_mode(p->password,!lv_obj_has_state(p->show_password,LV_STATE_CHECKED));
}

static void password_layout(void *context, bool keyboard_visible)
{
    wifi_page_t *page=context;
    lv_obj_set_y(page->panel,keyboard_visible ? 30 : 116);
}

static void password_dialog(wifi_page_t *p)
{
    close_password(p);
    lv_obj_t *overlay=lv_obj_create(p->ui->screen); p->overlay=overlay;
    lv_obj_set_pos(overlay,0,0); lv_obj_set_size(overlay,480,480);
    lv_obj_set_style_radius(overlay,0,0); lv_obj_set_style_bg_color(overlay,lv_color_hex(0x070A12),0);
    lv_obj_set_style_bg_opa(overlay,LV_OPA_COVER,0); lv_obj_set_style_border_width(overlay,0,0); lv_obj_set_style_pad_all(overlay,0,0);
    lv_obj_remove_flag(overlay,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *panel=lv_obj_create(overlay); p->panel=panel;
    lv_obj_set_pos(panel,18,116); lv_obj_set_size(panel,444,244);
    lv_obj_set_style_bg_color(panel,lv_color_hex(0x18243A),0); lv_obj_set_style_border_width(panel,0,0); lv_obj_set_style_pad_all(panel,12,0);
    lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
    app_ui_add_text(panel,"连接 Wi-Fi",LV_ALIGN_TOP_LEFT,0,0,0xEAF2FF,app_ui_chinese_font());
    char name[33]; memcpy(name,p->selected.ssid,sizeof(name)); app_text_sanitize_utf8(name,strlen(name));
    lv_obj_t *ssid=app_ui_add_text(panel,name,LV_ALIGN_TOP_LEFT,0,30,0x9BBEFF,app_ui_chinese_font());
    lv_obj_set_width(ssid,412); lv_label_set_long_mode(ssid,LV_LABEL_LONG_DOT);
    p->password=lv_textarea_create(panel); lv_obj_set_pos(p->password,0,60); lv_obj_set_size(p->password,420,44);
    lv_textarea_set_one_line(p->password,true); lv_textarea_set_password_mode(p->password,true); lv_textarea_set_max_length(p->password,63);
    lv_obj_set_style_text_font(p->password,app_ui_chinese_font(),0);
    lv_textarea_set_password_show_time(p->password,0);
    lv_textarea_set_placeholder_text(p->password,p->selected.password_required ? "网络密码" : "");
    lv_obj_add_event_cb(p->password,password_event,LV_EVENT_READY,p->ui);
    lv_obj_add_event_cb(p->password,password_event,LV_EVENT_CANCEL,p->ui);
    p->remember=lv_checkbox_create(panel); lv_obj_set_pos(p->remember,0,118); lv_checkbox_set_text(p->remember,"保存到本机"); lv_obj_add_state(p->remember,LV_STATE_CHECKED);
    p->show_password=lv_checkbox_create(panel); lv_obj_set_pos(p->show_password,230,118); lv_checkbox_set_text(p->show_password,"显示密码");
    lv_obj_set_user_data(p->show_password,(void *)3); lv_obj_add_event_cb(p->show_password,password_event,LV_EVENT_VALUE_CHANGED,p->ui);
    const char *labels[]={"连接","取消"};
    for(unsigned i=0;i<2;++i) {
        lv_obj_t *b=lv_button_create(panel); lv_obj_set_pos(b,i ? 222 : 0,158); lv_obj_set_size(b,198,38);
        lv_obj_set_style_bg_color(b,lv_color_hex(i ? 0x445268 : 0x2768D8),0); lv_obj_set_style_radius(b,12,0);
        lv_obj_set_user_data(b,(void *)(uintptr_t)(i+1)); lv_obj_add_event_cb(b,password_event,LV_EVENT_CLICKED,p->ui);
        app_ui_add_text(b,labels[i],LV_ALIGN_CENTER,0,0,0xFFFFFF,app_ui_chinese_font());
    }
    p->password_notice=app_ui_add_text(panel,"",LV_ALIGN_TOP_LEFT,0,204,0xFFB17A,app_ui_chinese_font());
    if(p->selected.password_required)
        app_text_input_show(app_text_input_create(overlay,p->password,false,password_layout,p),true);
    else {
        lv_obj_add_flag(p->password,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->show_password,LV_OBJ_FLAG_HIDDEN);
    }
    (void)app_usb_input_modal_open(overlay,p->selected.password_required?p->password:p->remember);
}
static void selected(lv_event_t *event)
{
    app_ui_t *ui=lv_event_get_user_data(event); wifi_page_t *p=ui ? ui->page_context : NULL;
    size_t index=(size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if(!p || index>=p->count || !p->aps[index].supported) return;
    p->selected=p->aps[index]; password_dialog(p);
}
static bool wifi_switch_on(const aether_wifi_status_t *status)
{
    return status->peer_radio ? status->enabled_preference :
           status->enabled || status->state==AETHER_WIFI_STARTING;
}
static void action(lv_event_t *event)
{
    app_ui_t *ui=lv_event_get_user_data(event); wifi_page_t *p=ui ? ui->page_context : NULL;
    if(!p || !p->wifi) return;
    intptr_t kind=(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    aether_wifi_status_t status; if(aether_wifi_get_status(p->wifi,&status)!=ESP_OK) return;
    esp_err_t error;
    if(kind==1) error=aether_wifi_set_enabled_preference(p->wifi,!wifi_switch_on(&status));
    else if(kind==2) error=aether_wifi_scan(p->wifi);
    else if(kind==3) error=aether_wifi_disconnect(p->wifi);
    else error=aether_wifi_forget(p->wifi);
    show_error(p,error);
}
static lv_obj_t *button(wifi_page_t *p,lv_obj_t *parent,const char *text,int x,int y,int width,intptr_t kind)
{
    lv_obj_t *b=lv_button_create(parent); lv_obj_set_pos(b,x,y); lv_obj_set_size(b,width,42);
    lv_obj_set_style_radius(b,12,0); lv_obj_set_style_bg_color(b,lv_color_hex(0x2768D8),0);
    lv_obj_set_user_data(b,(void *)kind); lv_obj_add_event_cb(b,action,LV_EVENT_CLICKED,p->ui);
    lv_obj_t *label=app_ui_add_text(b,text,LV_ALIGN_CENTER,0,0,0xFFFFFF,app_ui_chinese_font());
    if(kind==1) p->toggle_label=label;
    return b;
}
static const char *state_name(aether_wifi_state_t state)
{
    switch(state) {
    case AETHER_WIFI_OFF: return "Wi-Fi 已关闭";
    case AETHER_WIFI_STARTING: return "正在开启 Wi-Fi";
    case AETHER_WIFI_IDLE: return "Wi-Fi 已开启，未连接";
    case AETHER_WIFI_SCANNING: return "正在扫描附近网络";
    case AETHER_WIFI_CONNECTING: return "正在连接网络";
    case AETHER_WIFI_CONNECTED: return "Wi-Fi 已连接";
    case AETHER_WIFI_DISCONNECTING: return "正在切换网络";
    default: return "Wi-Fi 出错，可关闭后重试";
    }
}
static void update(lv_timer_t *timer)
{
    app_ui_t *ui=lv_timer_get_user_data(timer); wifi_page_t *p=ui->page_context;
    if(!p || !p->wifi) return;
    aether_wifi_status_t s; if(aether_wifi_get_status(p->wifi,&s)!=ESP_OK) return;
    char name[33]; memcpy(name,s.ssid,sizeof(name)); app_text_sanitize_utf8(name,strlen(name));
    if(s.connected) app_ui_label_fmt(p->status,"%s · %s\nIP %s · %d dBm",state_name(s.state),name,s.ip,s.rssi);
    else app_ui_label_fmt(p->status,"%s\n发现 %u 个网络%s · 重试 %u/5",state_name(s.state),s.ap_count,s.saved_network ? "，已保存网络" : "",s.retry_count);
    app_ui_label_text(p->toggle_label,wifi_switch_on(&s) ? "关闭 Wi-Fi" : "开启 Wi-Fi");
    bool ready=!s.peer_radio && s.enabled && s.state!=AETHER_WIFI_STARTING &&
               s.state!=AETHER_WIFI_CONNECTING && s.state!=AETHER_WIFI_DISCONNECTING;
    availability(p->scan,ready && !s.scanning);
    availability(p->disconnect,!s.peer_radio && (s.connected || s.state==AETHER_WIFI_CONNECTING));
    availability(p->forget,!s.peer_radio && s.saved_network);
    if(s.preference_error!=ESP_OK) app_ui_label_fmt(p->message,"Wi-Fi 开关设置：%s",esp_err_to_name(s.preference_error));
    else if(s.last_error!=ESP_OK) app_ui_label_fmt(p->message,"网络：%s · 原因 %u",esp_err_to_name(s.last_error),s.disconnect_reason);
    else if(s.storage_error!=ESP_OK) app_ui_label_fmt(p->message,"网络已受理，保存失败：%s",esp_err_to_name(s.storage_error));
    else if(s.persistence_pending) app_ui_label_text(p->message,"网络设置待保存，媒体空闲后自动完成");
    else if(!p->notice || lv_tick_elaps(p->notice_started)>3000) {
        p->notice=false;
        app_ui_label_text(p->message,!s.enabled ? "开启 Wi-Fi 后扫描附近网络" : s.connected ? "已取得 IP，可返回其他应用" :
            s.scanning ? "正在扫描，请稍候" : s.state==AETHER_WIFI_CONNECTING ? "正在连接，请稍候" : "点选网络后输入密码连接");
    }
    if(p->generation!=s.scan_generation) {
        p->generation=s.scan_generation; p->count=s.ap_count;
        for(unsigned i=0;i<AETHER_WIFI_MAX_APS;++i) {
            if(i>=s.ap_count) { lv_obj_add_flag(p->rows[i],LV_OBJ_FLAG_HIDDEN); continue; }
            if(aether_wifi_get_ap(p->wifi,i,&p->aps[i])!=ESP_OK) continue;
            memcpy(name,p->aps[i].ssid,sizeof(name)); app_text_sanitize_utf8(name,strlen(name));
            app_ui_label_fmt(p->names[i],"%s\n%s · %d dBm · CH%u",name[0] ? name : "隐藏网络",aether_wifi_security_name(p->aps[i].authmode),p->aps[i].rssi,p->aps[i].channel);
            lv_obj_remove_flag(p->rows[i],LV_OBJ_FLAG_HIDDEN);
        }
    }
    for(unsigned i=0;i<p->count;++i) availability(p->rows[i],ready && !s.scanning && p->aps[i].supported);
}
static void cleanup(app_ui_t *ui)
{ wifi_page_t *p=ui->page_context; if(p) close_password(p); free(p); ui->page_context=NULL; }
void app_open_wifi(app_ui_t *ui)
{
    lv_obj_t *content=app_ui_begin_page(ui,"Wi-Fi 网络");
    wifi_page_t *p=calloc(1,sizeof(*p));
    if(!p) { app_ui_add_text(content,"内存不足",LV_ALIGN_TOP_LEFT,0,0,0xFFB17A,app_ui_chinese_font()); return; }
    p->ui=ui; p->wifi=aether_wifi_get_default(); ui->page_context=p; ui->page_cleanup=cleanup;
    if(!p->wifi) { app_ui_add_text(content,"Wi-Fi 服务初始化失败",LV_ALIGN_TOP_LEFT,0,0,0xFFB17A,app_ui_chinese_font()); return; }
    p->status=app_ui_add_text(content,"",LV_ALIGN_TOP_LEFT,0,0,0xD9E8FF,app_ui_chinese_font());
    lv_obj_set_width(p->status,402); lv_label_set_long_mode(p->status,LV_LABEL_LONG_DOT); lv_obj_set_height(p->status,56);
    p->toggle=button(p,content,"开启 Wi-Fi",0,66,128,1);
    p->scan=button(p,content,"扫描网络",138,66,128,2);
    p->disconnect=button(p,content,"断开连接",276,66,128,3);
    p->forget=button(p,content,"忘记已存网络",0,118,190,4);
    app_ui_add_text(content,"点选网络连接",LV_ALIGN_TOP_LEFT,214,130,0x9EB6D8,app_ui_chinese_font());
    p->message=app_ui_add_text(content,"扫描后输入密码即可连接",LV_ALIGN_TOP_LEFT,0,170,0xFFB17A,app_ui_chinese_font());
    lv_obj_set_width(p->message,402); lv_obj_set_height(p->message,38); lv_label_set_long_mode(p->message,LV_LABEL_LONG_DOT);
    lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *list=lv_obj_create(content); lv_obj_set_pos(list,0,211); lv_obj_set_size(list,404,143);
    lv_obj_set_style_bg_opa(list,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(list,0,0); lv_obj_set_style_pad_all(list,0,0);
    lv_obj_set_style_pad_row(list,8,0); lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);
    for(unsigned i=0;i<AETHER_WIFI_MAX_APS;++i) {
        lv_obj_t *row=lv_button_create(list); p->rows[i]=row; lv_obj_set_size(row,382,72);
        lv_obj_set_style_bg_color(row,lv_color_hex(0x182B48),0); lv_obj_set_style_radius(row,12,0);
        p->names[i]=app_ui_add_text(row,"",LV_ALIGN_LEFT_MID,0,0,0xE4EDFF,app_ui_chinese_font());
        lv_obj_set_width(p->names[i],346); lv_label_set_long_mode(p->names[i],LV_LABEL_LONG_DOT); lv_obj_set_height(p->names[i],44);
        lv_obj_set_user_data(row,(void *)(uintptr_t)i); lv_obj_add_event_cb(row,selected,LV_EVENT_CLICKED,ui); lv_obj_add_flag(row,LV_OBJ_FLAG_HIDDEN);
    }
    p->generation=UINT32_MAX; app_ui_set_timer(ui,update,300); update(ui->timer);
}
