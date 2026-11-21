#include "app_ui_internal.h"
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
        app_ui_label_fmt(p->status,"此键已用于“%s”。请先将原位置设为 None，再重新分配。",app_nes_controls_action_name(p->conflict_action));
    else if(p->submit_error!=ESP_OK)app_ui_label_fmt(p->status,"映射未应用：%s",esp_err_to_name(p->submit_error));
    else if(!view.ready)app_ui_label_text(p->status,"键位服务未就绪；游戏使用默认键位。");
    else if(view.pending&&view.deferred)app_ui_label_text(p->status,"键位已生效；退出媒体后自动保存，避免写 Flash 影响流畅度。");
    else if(view.pending&&view.error!=ESP_OK)app_ui_label_fmt(p->status,"键位已生效，保存失败：%s；后台重试。",esp_err_to_name(view.error));
    else if(view.pending)app_ui_label_text(p->status,"键位已生效，正在后台保存…");
    else if(view.error!=ESP_OK)app_ui_label_fmt(p->status,"读取配置失败：%s；已使用默认，可点“恢复默认”修复。",esp_err_to_name(view.error));
    else app_ui_label_text(p->status,"已保存；重启后保留。USB 手柄键位保持默认。");
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
    lv_obj_t *intro=app_ui_add_text(content,"每个功能可设置主键和副键；None 为不绑定。一个键只对应一个功能。",LV_ALIGN_TOP_LEFT,2,0,0xAFC6E9,app_ui_chinese_font());
    lv_obj_set_width(intro,400);lv_label_set_long_mode(intro,LV_LABEL_LONG_WRAP);
    lv_obj_t *reset=lv_button_create(content);lv_obj_set_pos(reset,230,68);lv_obj_set_size(reset,170,40);
    lv_obj_set_style_bg_color(reset,lv_color_hex(0x274769),0);lv_obj_set_style_radius(reset,12,0);
    app_ui_add_text(reset,"恢复默认",LV_ALIGN_CENTER,0,0,0xEAF2FF,app_ui_chinese_font());
    lv_obj_add_event_cb(reset,controls_reset,LV_EVENT_CLICKED,ui);
    app_ui_add_text(content,"操作",LV_ALIGN_TOP_LEFT,2,120,0x91A8C7,NULL);
    app_ui_add_text(content,"主键",LV_ALIGN_TOP_LEFT,84,120,0x91A8C7,NULL);
    app_ui_add_text(content,"副键",LV_ALIGN_TOP_LEFT,250,120,0x91A8C7,NULL);
    char options[1536]={0};size_t used=0;
    for(unsigned i=0;i<app_nes_controls_key_count();++i){
        const int n=snprintf(options+used,sizeof(options)-used,"%s%s",i?"\n":"",app_nes_controls_key_name(i));
        if(n<0||(size_t)n>=sizeof(options)-used)break;
        used+=(size_t)n;
    }
    for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a){
        const int y=152+(int)a*64;
        app_ui_add_text(content,app_nes_controls_action_name(a),LV_ALIGN_TOP_LEFT,2,y+12,0xD9E8FF,NULL);
        for(unsigned k=0;k<APP_NES_CONTROL_SLOTS;++k){
            lv_obj_t *d=lv_dropdown_create(content);p->keys[a][k]=d;
            lv_obj_set_pos(d,k?248:82,y);lv_obj_set_size(d,152,42);
            lv_obj_set_style_text_font(d,&lv_font_montserrat_16,0);
            lv_obj_set_style_bg_color(d,lv_color_hex(0x20334F),0);lv_obj_set_style_text_color(d,lv_color_hex(0xEAF2FF),0);
            lv_obj_set_style_border_width(d,0,0);lv_obj_set_style_radius(d,10,0);
            lv_obj_t *list=lv_dropdown_get_list(d);
            lv_obj_set_style_text_font(list,&lv_font_montserrat_16,0);
            lv_obj_set_style_bg_color(list,lv_color_hex(0x18243A),0);lv_obj_set_style_text_color(list,lv_color_hex(0xEAF2FF),0);
            lv_dropdown_set_options(d,options);lv_obj_set_user_data(d,(void*)(uintptr_t)(a*APP_NES_CONTROL_SLOTS+k));
            lv_obj_add_event_cb(d,key_changed,LV_EVENT_VALUE_CHANGED,ui);
        }
    }
    p->status=app_ui_add_text(content,"",LV_ALIGN_TOP_LEFT,2,864,0x9CCFBE,app_ui_chinese_font());
    lv_obj_set_width(p->status,398);lv_label_set_long_mode(p->status,LV_LABEL_LONG_WRAP);
    lv_obj_t *help=app_ui_add_text(content,"默认：方向键/WASD、Z=A、X=B、Shift=选择、Enter=开始、P=暂停、R=复位、Esc=退出。游戏中短按用户键（GPIO61）退出 NES。",LV_ALIGN_TOP_LEFT,2,970,0x8FA9AD,app_ui_chinese_font());
    lv_obj_set_width(help,398);lv_label_set_long_mode(help,LV_LABEL_LONG_WRAP);
    app_ui_set_timer(ui,controls_timer,250);controls_refresh(ui);
}
