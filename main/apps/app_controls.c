#include "app_connection_style.h"
#include "app_nes_controls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    lv_obj_t *keys[APP_NES_CONTROL_COUNT][APP_NES_CONTROL_SLOTS];
    lv_obj_t *status;
    uint32_t revision;
    esp_err_t submit_error;
    unsigned conflict_action;
} controls_page_t;

static void controls_refresh(app_ui_t *ui)
{
    controls_page_t *p=ui->page_context;if(!p)return;
    app_nes_controls_snapshot_t view;app_nes_controls_get_snapshot(&view);
    if(p->revision!=view.revision){
        bool open=false;
        for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a)for(unsigned k=0;k<APP_NES_CONTROL_SLOTS;++k){
            if(!lv_dropdown_is_open(p->keys[a][k]))
                lv_dropdown_set_selected(p->keys[a][k],app_nes_controls_key_index(view.keys[a][k]));
            else open=true;
            if(view.ready)lv_obj_remove_state(p->keys[a][k],LV_STATE_DISABLED);
            else lv_obj_add_state(p->keys[a][k],LV_STATE_DISABLED);
        }
        p->revision=open?0:view.revision;
    }
    if(p->submit_error!=ESP_OK&&p->conflict_action<APP_NES_CONTROL_COUNT)
        app_ui_label_fmt(p->status,"此键已用于%s。先将原键位设为 None。",app_nes_controls_action_name(p->conflict_action));
    else if(p->submit_error!=ESP_OK)app_ui_label_fmt(p->status,"映射未应用：%s",esp_err_to_name(p->submit_error));
    else if(!view.ready)app_ui_label_text(p->status,"键位服务未就绪，使用默认配置");
    else if(view.pending&&view.deferred)app_ui_label_text(p->status,"退出媒体后自动保存");
    else if(view.pending&&view.error!=ESP_OK)app_ui_label_fmt(p->status,"保存失败，后台重试：%s",esp_err_to_name(view.error));
    else if(view.pending)app_ui_label_text(p->status,"正在保存…");
    else if(view.error!=ESP_OK)app_ui_label_fmt(p->status,"读取失败：%s。可恢复默认。",esp_err_to_name(view.error));
    else app_ui_label_text(p->status,"已保存");
}
static void key_changed(lv_event_t *event)
{
    app_ui_t *ui=lv_event_get_user_data(event);controls_page_t *p=ui->page_context;if(!p)return;
    lv_obj_t *drop=lv_event_get_target_obj(event);const unsigned index=(unsigned)(uintptr_t)lv_obj_get_user_data(drop);
    const unsigned selected=lv_dropdown_get_selected(drop);
    if(index>=APP_NES_CONTROL_COUNT*APP_NES_CONTROL_SLOTS||selected>=app_nes_controls_key_count()){
        p->submit_error=ESP_ERR_INVALID_ARG;p->conflict_action=APP_NES_CONTROL_COUNT;controls_refresh(ui);return;
    }
    const uint8_t usage=app_nes_controls_key_usage(selected);
    p->submit_error=app_nes_controls_set((app_nes_control_t)(index/APP_NES_CONTROL_SLOTS),index%APP_NES_CONTROL_SLOTS,usage,&p->conflict_action);
    if(p->submit_error!=ESP_OK){
        app_nes_controls_snapshot_t view;app_nes_controls_get_snapshot(&view);
        lv_dropdown_set_selected(drop,app_nes_controls_key_index(view.keys[index/APP_NES_CONTROL_SLOTS][index%APP_NES_CONTROL_SLOTS]));
    }
    p->revision=0;controls_refresh(ui); /* Reject conflicting UI selection visibly. */
}
static void controls_reset(lv_event_t *event)
{
    app_ui_t *ui=lv_event_get_user_data(event);controls_page_t *p=ui->page_context;if(!p)return;
    p->submit_error=app_nes_controls_reset();p->conflict_action=APP_NES_CONTROL_COUNT;
    p->revision=0;controls_refresh(ui);
}
static void controls_timer(lv_timer_t *timer) { controls_refresh(lv_timer_get_user_data(timer)); }
static void controls_cleanup(app_ui_t *ui) { free(ui->page_context);ui->page_context=NULL; }

void app_open_nes_controls(app_ui_t *ui)
{
    lv_obj_t *content=app_ui_begin_page(ui,"NES 键盘映射");
    controls_page_t *p=calloc(1,sizeof(*p));
    if(!p){app_ui_add_text(content,"内存不足",LV_ALIGN_CENTER,0,0,0xFF8888,NULL);return;}
    ui->page_context=p;ui->page_cleanup=controls_cleanup;p->conflict_action=APP_NES_CONTROL_COUNT;
    lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *header=lv_obj_create(content);lv_obj_set_size(header,404,58);
    app_connection_card_style(header);
    app_ui_add_text(header,"默认 Esc 退出游戏",LV_ALIGN_TOP_LEFT,10,7,CONNECTION_TEXT,app_ui_chinese_font());
    app_ui_add_text(header,"长按用户键 2 秒回桌面",LV_ALIGN_TOP_LEFT,10,31,CONNECTION_DETAIL,app_ui_chinese_font());
    lv_obj_t *reset=lv_button_create(header);lv_obj_set_pos(reset,260,6);lv_obj_set_size(reset,132,44);
    app_connection_button_style(reset,false);
    app_ui_add_text(reset,"恢复默认",LV_ALIGN_CENTER,0,0,0xEAF2FF,app_ui_chinese_font());
    lv_obj_add_event_cb(reset,controls_reset,LV_EVENT_CLICKED,ui);
    app_ui_add_text(content,"操作",LV_ALIGN_TOP_LEFT,9,69,CONNECTION_DETAIL,app_ui_chinese_font());
    app_ui_add_text(content,"主键",LV_ALIGN_TOP_LEFT,85,69,CONNECTION_DETAIL,app_ui_chinese_font());
    app_ui_add_text(content,"副键",LV_ALIGN_TOP_LEFT,244,69,CONNECTION_DETAIL,app_ui_chinese_font());
    lv_obj_t *viewport=lv_obj_create(content);
    lv_obj_set_pos(viewport,0,93);lv_obj_set_size(viewport,404,197);
    lv_obj_set_style_bg_opa(viewport,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(viewport,0,0);lv_obj_set_style_pad_all(viewport,0,0);
    lv_obj_set_scroll_dir(viewport,LV_DIR_VER);
    char options[1536]={0};size_t used=0;
    for(unsigned i=0;i<app_nes_controls_key_count();++i){
        const int n=snprintf(options+used,sizeof(options)-used,"%s%s",i?"\n":"",app_nes_controls_key_name(i));
        if(n<0||(size_t)n>=sizeof(options)-used)break;
        used+=(size_t)n;
    }
    for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a){
        lv_obj_t *row=lv_obj_create(viewport);
        lv_obj_set_pos(row,0,(int)a*59);lv_obj_set_size(row,400,54);
        app_connection_card_style(row);
        app_ui_add_text(row,app_nes_controls_action_name(a),LV_ALIGN_LEFT_MID,8,0,
                        CONNECTION_TEXT,app_ui_chinese_font());
        for(unsigned k=0;k<APP_NES_CONTROL_SLOTS;++k){
            lv_obj_t *d=lv_dropdown_create(row);p->keys[a][k]=d;
            lv_obj_set_pos(d,k?238:78,4);lv_obj_set_size(d,148,44);
            lv_obj_set_style_text_font(d,&lv_font_montserrat_16,0);
            lv_obj_set_style_text_font(d,&lv_font_montserrat_16,LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(d,lv_color_hex(CONNECTION_CONTROL),0);lv_obj_set_style_text_color(d,lv_color_hex(CONNECTION_TEXT),0);
            lv_obj_set_style_border_width(d,0,0);lv_obj_set_style_radius(d,10,0);
            lv_obj_t *list=lv_dropdown_get_list(d);
            lv_obj_set_style_text_font(list,&lv_font_montserrat_16,0);
            lv_obj_set_style_bg_color(list,lv_color_hex(CONNECTION_PANEL),0);lv_obj_set_style_text_color(list,lv_color_hex(CONNECTION_TEXT),0);
            lv_dropdown_set_options(d,options);lv_obj_set_user_data(d,(void*)(uintptr_t)(a*APP_NES_CONTROL_SLOTS+k));
            lv_obj_add_event_cb(d,key_changed,LV_EVENT_VALUE_CHANGED,ui);
        }
    }
    p->status=app_ui_add_text(content,"",LV_ALIGN_TOP_LEFT,5,296,CONNECTION_ACCENT,app_ui_chinese_font());
    lv_obj_set_size(p->status,396,40);lv_label_set_long_mode(p->status,LV_LABEL_LONG_DOT);
    app_ui_add_text(content,"None 取消绑定",LV_ALIGN_TOP_LEFT,5,335,CONNECTION_DETAIL,app_ui_chinese_font());
    app_ui_set_timer(ui,controls_timer,250);controls_refresh(ui);
}
