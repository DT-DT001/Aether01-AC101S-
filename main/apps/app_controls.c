#include "app_connection_style.h"
#include "app_nes_controls.h"
#include "ui/app_ui_modal.h"
#include "app_usb_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    lv_obj_t *keys[APP_NES_CONTROL_COUNT][APP_NES_CONTROL_SLOTS];
    lv_obj_t *captions[APP_NES_CONTROL_COUNT][APP_NES_CONTROL_SLOTS];
    lv_obj_t *status, *editor, *editor_hint;
    app_ui_t *ui;
    uint32_t revision;
    esp_err_t submit_error;
    unsigned conflict_action, editing;
} controls_page_t;

static void controls_refresh(app_ui_t *ui)
{
    controls_page_t *p=ui->page_context;if(!p)return;
    app_nes_controls_snapshot_t view;app_nes_controls_get_snapshot(&view);
    if(p->revision!=view.revision){
        for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a)for(unsigned k=0;k<APP_NES_CONTROL_SLOTS;++k){
            const unsigned choice=app_nes_controls_key_index(view.keys[a][k]);
            app_ui_label_text(p->captions[a][k],choice?app_nes_controls_key_name(choice):"未设置");
            if(view.ready)lv_obj_remove_state(p->keys[a][k],LV_STATE_DISABLED);
            else lv_obj_add_state(p->keys[a][k],LV_STATE_DISABLED);
        }
        p->revision=view.revision;
    }
    if(p->submit_error!=ESP_OK&&p->conflict_action<APP_NES_CONTROL_COUNT)
        app_ui_label_fmt(p->status,"此键已用于%s，请先清除原键位",app_nes_controls_action_name(p->conflict_action));
    else if(p->submit_error!=ESP_OK)app_ui_label_fmt(p->status,"映射未应用：%s",esp_err_to_name(p->submit_error));
    else if(!view.ready)app_ui_label_text(p->status,"键位服务未就绪，使用默认配置");
    else if(view.pending&&view.deferred)app_ui_label_text(p->status,"退出媒体后自动保存");
    else if(view.pending&&view.error!=ESP_OK)app_ui_label_fmt(p->status,"保存失败，后台重试：%s",esp_err_to_name(view.error));
    else if(view.pending)app_ui_label_text(p->status,"正在保存…");
    else if(view.error!=ESP_OK)app_ui_label_fmt(p->status,"读取失败：%s。可恢复默认。",esp_err_to_name(view.error));
    else app_ui_label_text(p->status,"已保存");
}

static void editor_deleted(lv_event_t *event)
{
    controls_page_t *page=lv_event_get_user_data(event);
    page->editor=page->editor_hint=NULL;
    (void)app_usb_input_capture_keys(false);
    if(page->ui->page_context==page&&page->ui->timer)lv_timer_set_period(page->ui->timer,250);
}

static void select_key(controls_page_t *page,uint8_t usage)
{
    if(!page->editor)return;
    const unsigned action=page->editing/APP_NES_CONTROL_SLOTS,slot=page->editing%APP_NES_CONTROL_SLOTS;
    page->submit_error=app_nes_controls_set((app_nes_control_t)action,slot,usage,&page->conflict_action);
    if(page->submit_error==ESP_OK){
        lv_msgbox_close(page->editor);
    }else if(page->conflict_action<APP_NES_CONTROL_COUNT){
        app_ui_label_fmt(page->editor_hint,"此键已用于%s，试试另一个键",app_nes_controls_action_name(page->conflict_action));
    }else{
        app_ui_label_text(page->editor_hint,"无法绑定这个键，请改用列表中的键");
    }
    controls_refresh(page->ui);
}

static void pick_button(lv_event_t *event)
{
    lv_obj_t *button=lv_event_get_current_target_obj(event);
    select_key(lv_event_get_user_data(event),(uint8_t)(uintptr_t)lv_obj_get_user_data(button));
}

static void pick_dropdown(lv_event_t *event)
{
    lv_obj_t *drop=lv_event_get_current_target_obj(event);
    const unsigned choice=lv_dropdown_get_selected(drop);
    if(choice<app_nes_controls_key_count())
        select_key(lv_event_get_user_data(event),app_nes_controls_key_usage(choice));
}

static void set_choices(lv_obj_t *drop)
{
    char options[1536]={0};
    size_t used=0;
    for(unsigned i=0;i<app_nes_controls_key_count();++i){
        const int n=snprintf(options+used,sizeof(options)-used,"%s%s",i?"\n":"",app_nes_controls_key_name(i));
        if(n<0||(size_t)n>=sizeof(options)-used)break;
        used+=(size_t)n;
    }
    lv_dropdown_set_options(drop,options);
}

static void edit_key(lv_event_t *event)
{
    static const uint8_t common[]={0x52,0x51,0x50,0x4f,0x1a,0x04,0x16,0x07,0x1d,0x1b,0x28,0x29};
    controls_page_t *page=lv_event_get_user_data(event);
    if(page->editor)return;
    lv_obj_t *button=lv_event_get_current_target_obj(event);
    const unsigned binding=(unsigned)(uintptr_t)lv_obj_get_user_data(button);
    if(!binding)return;
    page->editing=binding-1;
    const unsigned action=page->editing/APP_NES_CONTROL_SLOTS, slot=page->editing%APP_NES_CONTROL_SLOTS;
    if(action>=APP_NES_CONTROL_COUNT)return;
    lv_obj_t *box=app_ui_modal_msgbox(page->ui->screen);
    if(!box)return;
    page->editor=box;
    lv_obj_add_event_cb(box,editor_deleted,LV_EVENT_DELETE,page);
    lv_obj_set_width(box,408);
    lv_obj_set_style_text_font(box,app_ui_chinese_font(),0);
    lv_obj_set_style_bg_color(box,lv_color_hex(CONNECTION_PANEL),0);
    lv_obj_set_style_text_color(box,lv_color_hex(CONNECTION_TEXT),0);
    lv_obj_set_style_border_color(box,lv_color_hex(CONNECTION_BORDER),0);
    lv_obj_set_style_radius(box,20,0);
    lv_obj_set_style_bg_color(lv_obj_get_parent(box),lv_color_hex(0x050913),0);
    lv_obj_set_style_bg_opa(lv_obj_get_parent(box),LV_OPA_70,0);
    lv_obj_center(box);
    char title[56];
    snprintf(title,sizeof(title),"%s · %s",app_nes_controls_action_name(action),slot?"备用键":"主键");
    lv_obj_t *heading=lv_msgbox_add_title(box,title);
    lv_obj_set_style_text_font(heading,app_ui_chinese_font_size(24),0);
    lv_obj_set_style_text_color(heading,lv_color_hex(CONNECTION_TEXT),0);
    lv_obj_set_style_bg_color(lv_msgbox_get_header(box),lv_color_hex(CONNECTION_PANEL),0);
    lv_obj_t *close=lv_msgbox_add_close_button(box);
    lv_obj_set_size(close,46,42);
    app_connection_button_style(close,false);
    const bool capture=app_usb_input_capture_keys(true);
    page->editor_hint=lv_msgbox_add_text(box,capture?"按下 USB 键盘上的键，或点选下方按键":"请选择下方按键");
    lv_obj_set_style_text_color(page->editor_hint,lv_color_hex(CONNECTION_ACCENT),0);
    lv_obj_t *panel=lv_obj_create(lv_msgbox_get_content(box));
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel,lv_pct(100),126);
    lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
    for(unsigned i=0;i<sizeof(common)/sizeof(*common);++i){
        lv_obj_t *key=lv_button_create(panel);
        lv_obj_set_pos(key,(int)(i%4)*91,(int)(i/4)*42);lv_obj_set_size(key,85,38);
        app_connection_button_style(key,false);
        app_ui_add_text(key,app_nes_controls_key_name(app_nes_controls_key_index(common[i])),
                        LV_ALIGN_CENTER,0,0,CONNECTION_TEXT,&lv_font_montserrat_16);
        lv_obj_set_user_data(key,(void*)(uintptr_t)common[i]);
        lv_obj_add_event_cb(key,pick_button,LV_EVENT_CLICKED,page);
    }
    lv_obj_t *drop=lv_dropdown_create(lv_msgbox_get_content(box));
    lv_obj_set_size(drop,lv_pct(100),44);
    lv_obj_set_style_bg_color(drop,lv_color_hex(CONNECTION_CONTROL),0);
    lv_obj_set_style_border_color(drop,lv_color_hex(CONNECTION_BORDER),0);
    lv_obj_set_style_text_color(drop,lv_color_hex(CONNECTION_TEXT),0);
    lv_obj_set_style_text_color(drop,lv_color_hex(CONNECTION_ACCENT),LV_PART_INDICATOR);
    lv_obj_t *choices=lv_dropdown_get_list(drop);
    lv_obj_set_style_bg_color(choices,lv_color_hex(CONNECTION_PANEL),0);
    lv_obj_set_style_border_color(choices,lv_color_hex(CONNECTION_BORDER),0);
    lv_obj_set_style_text_color(choices,lv_color_hex(CONNECTION_TEXT),0);
    lv_obj_set_style_bg_color(choices,lv_color_hex(CONNECTION_BLUE),LV_PART_SELECTED);
    set_choices(drop);
    lv_dropdown_set_text(drop,"更多按键");
    lv_obj_add_event_cb(drop,pick_dropdown,LV_EVENT_VALUE_CHANGED,page);
    lv_obj_t *clear=lv_msgbox_add_footer_button(box,"清除键位");
    lv_obj_set_size(clear,128,42);
    app_connection_button_style(clear,false);
    lv_obj_set_user_data(clear,NULL);
    lv_obj_add_event_cb(clear,pick_button,LV_EVENT_CLICKED,page);
    app_ui_modal_focus(box,NULL);
    if(page->ui->timer)lv_timer_set_period(page->ui->timer,60);
}
static void controls_reset(lv_event_t *event)
{
    app_ui_t *ui=lv_event_get_user_data(event);controls_page_t *p=ui->page_context;if(!p)return;
    p->submit_error=app_nes_controls_reset();p->conflict_action=APP_NES_CONTROL_COUNT;
    p->revision=0;controls_refresh(ui);
}
static void controls_timer(lv_timer_t *timer)
{
    app_ui_t *ui=lv_timer_get_user_data(timer);
    controls_page_t *p=ui->page_context;
    if(p&&p->editor){
        uint8_t usage=app_usb_input_take_captured_usage();
        if(usage)select_key(p,usage);
    }
    controls_refresh(ui);
}

static void controls_cleanup(app_ui_t *ui)
{
    controls_page_t *p=ui->page_context;
    ui->page_context=NULL;
    if(p&&p->editor)lv_msgbox_close(p->editor);
    free(p);
}

void app_open_nes_controls(app_ui_t *ui)
{
    lv_obj_t *content=app_ui_begin_page(ui,"NES 键盘映射");
    controls_page_t *p=calloc(1,sizeof(*p));
    if(!p){app_ui_add_text(content,"内存不足",LV_ALIGN_CENTER,0,0,0xFF8888,NULL);return;}
    ui->page_context=p;ui->page_cleanup=controls_cleanup;p->ui=ui;p->conflict_action=APP_NES_CONTROL_COUNT;
    lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *header=lv_obj_create(content);lv_obj_set_size(header,404,54);
    app_connection_card_style(header);
    app_ui_add_text(header,"点击任意键位修改",LV_ALIGN_LEFT_MID,10,0,CONNECTION_TEXT,app_ui_chinese_font());
    lv_obj_t *reset=lv_button_create(header);lv_obj_set_pos(reset,260,5);lv_obj_set_size(reset,132,44);
    app_connection_button_style(reset,false);
    app_ui_add_text(reset,"恢复默认",LV_ALIGN_CENTER,0,0,0xEAF2FF,app_ui_chinese_font());
    lv_obj_add_event_cb(reset,controls_reset,LV_EVENT_CLICKED,ui);
    app_ui_add_text(content,"操作",LV_ALIGN_TOP_LEFT,9,62,CONNECTION_DETAIL,app_ui_chinese_font());
    app_ui_add_text(content,"主键",LV_ALIGN_TOP_LEFT,85,62,CONNECTION_DETAIL,app_ui_chinese_font());
    app_ui_add_text(content,"备用键",LV_ALIGN_TOP_LEFT,244,62,CONNECTION_DETAIL,app_ui_chinese_font());
    lv_obj_t *viewport=lv_obj_create(content);
    lv_obj_set_pos(viewport,0,84);lv_obj_set_size(viewport,404,239);
    lv_obj_set_style_bg_opa(viewport,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(viewport,0,0);lv_obj_set_style_pad_all(viewport,0,0);
    lv_obj_set_scroll_dir(viewport,LV_DIR_VER);
    for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a){
        lv_obj_t *row=lv_obj_create(viewport);
        lv_obj_set_pos(row,0,(int)a*59);lv_obj_set_size(row,400,54);
        app_connection_card_style(row);
        app_ui_add_text(row,app_nes_controls_action_name(a),LV_ALIGN_LEFT_MID,8,0,
                        CONNECTION_TEXT,app_ui_chinese_font());
        for(unsigned k=0;k<APP_NES_CONTROL_SLOTS;++k){
            lv_obj_t *d=lv_button_create(row);p->keys[a][k]=d;
            lv_obj_set_pos(d,k?238:78,4);lv_obj_set_size(d,148,44);
            app_connection_button_style(d,false);
            p->captions[a][k]=app_ui_add_text(d,"",LV_ALIGN_CENTER,0,0,CONNECTION_TEXT,app_ui_chinese_font());
            lv_obj_set_user_data(d,(void*)(uintptr_t)(a*APP_NES_CONTROL_SLOTS+k+1));
            lv_obj_add_event_cb(d,edit_key,LV_EVENT_CLICKED,p);
        }
    }
    p->status=app_ui_add_text(content,"",LV_ALIGN_TOP_LEFT,5,328,CONNECTION_ACCENT,app_ui_chinese_font());
    lv_obj_set_size(p->status,396,22);lv_label_set_long_mode(p->status,LV_LABEL_LONG_DOT);
    app_ui_set_timer(ui,controls_timer,250);controls_refresh(ui);
}
