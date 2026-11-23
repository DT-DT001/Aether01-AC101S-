#include "app_usb_input.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static uint16_t draw_buffer[480*20];
static unsigned clicks[4];
static bool game;
static app_power_t power;
bool app_power_is_sleeping(const app_power_t *p) { return p && (p->sleeping || p->sleep_requested); }
static bool game_gate(void *context) { assert(context==&game);return game; }
static void flush(lv_display_t *d,const lv_area_t *area,uint8_t *pixels)
{ (void)area;(void)pixels;lv_display_flush_ready(d); }
static void advance(unsigned ms)
{ for(unsigned i=0;i<ms;++i){lv_tick_inc(1);lv_timer_handler();} }
static void click(lv_event_t *e) { ++*(unsigned*)lv_event_get_user_data(e); }
static void mouse(int dx,int dy,int wheel,unsigned buttons)
{ const aether_usb_input_event_t e={.type=AETHER_USB_INPUT_MOUSE,.dx=dx,.dy=dy,.wheel=wheel,.buttons=buttons};app_usb_input_publish(NULL,&e); }
static void keyboard(unsigned usage,bool shift)
{
    aether_usb_input_event_t e={.type=AETHER_USB_INPUT_KEYBOARD};
    if(usage)e.keys[usage/8]|=1U<<(usage%8);
    if(shift)e.keys[0xe1/8]|=1U<<(0xe1%8);
    app_usb_input_publish(NULL,&e);
}
static void key(unsigned usage,bool shift)
{ keyboard(usage,shift);keyboard(0,false);advance(20); }
static void reset(void)
{ const aether_usb_input_event_t e={.type=AETHER_USB_INPUT_RESET};app_usb_input_publish(NULL,&e);advance(20); }
static lv_obj_t *button(lv_obj_t *parent,int x,unsigned which)
{
    lv_obj_t *b=lv_button_create(parent);lv_obj_set_pos(b,x,25);lv_obj_set_size(b,130,60);
    lv_obj_add_event_cb(b,click,LV_EVENT_CLICKED,&clicks[which]);return b;
}
static void *producer(void *unused)
{ (void)unused;for(unsigned i=0;i<2000;++i){keyboard(4+(i%26),false);keyboard(0,false);mouse(1,-1,i%4==0?-1:0,0);}return NULL; }

int main(void)
{
    setbuf(stdout,NULL);lv_init();lv_display_t *d=lv_display_create(480,480);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    assert(app_usb_input_modal_open(lv_display_get_screen_active(d),NULL)==ESP_OK);
    assert(app_usb_input_init(&power,NULL)==ESP_ERR_INVALID_ARG);
    assert(app_usb_input_init(&power,d)==ESP_OK);
    assert(app_usb_input_init(&power,d)==ESP_ERR_INVALID_STATE);
    app_usb_input_set_gate(game_gate,&game);
    lv_obj_t *screen=lv_display_get_screen_active(d);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *one=button(screen,25,0),*two=button(screen,220,1);
    lv_obj_t *text=lv_textarea_create(screen);lv_obj_set_pos(text,25,120);lv_obj_set_size(text,400,95);
    lv_obj_t *list=lv_obj_create(screen);lv_obj_set_pos(list,25,245);lv_obj_set_size(list,400,220);
    lv_obj_set_scroll_dir(list,LV_DIR_VER);lv_obj_set_style_pad_all(list,0,0);
    lv_obj_t *long_child=lv_obj_create(list);lv_obj_set_size(long_child,360,800);
    lv_obj_remove_flag(long_child,LV_OBJ_FLAG_SCROLLABLE);
    advance(40);assert(lv_group_get_focused(lv_group_get_default())==one);
    key(40,false);assert(clicks[0]==1);
    key(43,false);assert(lv_group_get_focused(lv_group_get_default())==two);
    key(40,false);assert(clicks[1]==1);
    key(43,true);assert(lv_group_get_focused(lv_group_get_default())==one);
    puts("PASS default LVGL focus group: fast Enter tap, Tab and Shift+Tab navigate actual widgets");

    lv_group_focus_obj(text);key(4,false);key(4,true);key(30,true);key(57,false);key(5,false);key(6,true);
    assert(!strcmp(lv_textarea_get_text(text),"aA!Bc"));key(42,false);
    assert(!strcmp(lv_textarea_get_text(text),"aA!B"));
    key(74,false);key(29,false);assert(!strcmp(lv_textarea_get_text(text),"ZaA!B"));
    key(77,false);keyboard(27,false);advance(700);keyboard(0,false);advance(20);
    assert(strlen(lv_textarea_get_text(text))>6);
    puts("PASS real textarea: US ASCII, Shift/Caps, punctuation, Backspace/Home/End and held-key repeat");

    mouse(-185,-185,0,0);advance(20);mouse(0,0,0,1);mouse(0,0,0,0);advance(20);
    assert(clicks[0]==2&&lv_group_get_focused(lv_group_get_default())==one);
    mouse(80,270,-3,0);advance(20);assert(lv_obj_get_scroll_y(list)>0);
    int scroll=lv_obj_get_scroll_y(list);advance(80);assert(lv_obj_get_scroll_y(list)==scroll);
    puts("PASS relative mouse cursor and queued click, click-to-focus, wheel scroll under cursor with no delta replay");

    /* A USB release/disconnect cancels a gesture instead of clicking. */
    lv_group_focus_obj(one);keyboard(40,false);advance(16);unsigned before=clicks[0];reset();
    assert(clicks[0]==before);key(40,false);assert(clicks[0]==before+1);
    power.sleeping=true;keyboard(40,false);power.sleeping=false;advance(30);
    assert(clicks[0]==before+1);keyboard(0,false);advance(20);key(40,false);assert(clicks[0]==before+2);
    power.sleep_requested=true;mouse(-80,-270,0,1);power.sleep_requested=false;advance(20);
    mouse(0,0,0,0);advance(20);assert(clicks[0]==before+2);
    mouse(-80,-270,0,0);advance(20);
    mouse(0,0,0,1);mouse(0,0,0,0);advance(20);assert(clicks[0]==before+3);
    game=true;advance(10);keyboard(40,false);game=false;advance(20);keyboard(0,false);advance(20);
    assert(clicks[0]==before+3);key(40,false);assert(clicks[0]==before+4);
    app_usb_input_set_enabled(false);keyboard(40,false);app_usb_input_set_enabled(true);advance(20);
    keyboard(0,false);advance(20);assert(clicks[0]==before+4);
    puts("PASS reset cancels Enter gesture; sleep/queued-sleep, NES page gate and disable consume held keys/buttons through wake without injecting clicks");

    lv_group_focus_obj(one);
    const unsigned before_capture=clicks[0];
    keyboard(40,false);advance(16);
    assert(app_usb_input_capture_keys(true)&&app_usb_input_take_captured_usage()==0);
    keyboard(40,false);advance(20);assert(app_usb_input_take_captured_usage()==0);
    keyboard(0,false);keyboard(0x45,false);keyboard(0,false);
    assert(app_usb_input_take_captured_usage()==0x45&&app_usb_input_take_captured_usage()==0);
    keyboard(40,false);
    assert(app_usb_input_take_captured_usage()==40&&app_usb_input_capture_keys(false));
    advance(20);keyboard(0,false);advance(20);
    assert(clicks[0]==before_capture);key(40,false);assert(clicks[0]==before_capture+1);
    puts("PASS binding capture accepts raw F12 and Enter, ignores initially held keys and consumes through modal close");

    lv_group_t *base=lv_group_get_default();
    lv_group_focus_obj(one);
    lv_obj_t *modal=lv_obj_create(screen);lv_obj_set_size(modal,480,480);lv_obj_set_pos(modal,0,0);
    lv_obj_t *cancel=button(modal,25,2),*confirm=button(modal,220,3);
    assert(app_usb_input_modal_open(modal,confirm)==ESP_OK);advance(20);
    lv_group_t *scope=lv_obj_get_group(confirm);assert(scope!=base&&lv_group_get_default()==base);
    key(40,false);assert(clicks[3]==1);
    const unsigned behind0=clicks[0],behind1=clicks[1];
    for(unsigned i=0;i<20;++i){key(43,false);key(40,false);}
    assert(clicks[0]==behind0&&clicks[1]==behind1&&clicks[2]==10&&clicks[3]==11);
    lv_obj_t *background=button(screen,25,0);assert(lv_obj_get_group(background)==base);
    lv_obj_move_foreground(modal);key(43,true);
    assert(lv_group_get_focused(scope)==cancel);
    lv_obj_t *new_row=button(modal,25,2);assert(lv_obj_get_group(new_row)==base);
    assert(app_usb_input_modal_open(modal,NULL)==ESP_OK);
    assert(lv_obj_get_group(new_row)==scope&&lv_group_get_focused(scope)==cancel);
    /* Modal entry and deletion consume a key held across the focus change. */
    keyboard(40,false);advance(16);before=clicks[0];lv_obj_delete(modal);advance(20);
    keyboard(0,false);advance(20);assert(clicks[0]==before&&lv_group_get_focused(base)==one);
    key(40,false);assert(clicks[0]==before+1);lv_obj_delete(background);
    puts("PASS actual modal group traps Tab/ShiftTab/Enter, excludes covered/background-created controls, restores prior focus and cancels held Enter on deletion");

    modal=lv_obj_create(screen);lv_obj_t *field=lv_textarea_create(modal);
    lv_obj_t *outside=button(screen,25,0);lv_group_focus_obj(outside);
    for(unsigned repeat=0;repeat<100;++repeat){
        assert(app_usb_input_modal_open(modal,field)==ESP_OK);advance(10);
        assert(lv_group_get_focused(lv_obj_get_group(field))==field);
        app_usb_input_modal_close(modal);lv_obj_add_flag(modal,LV_OBJ_FLAG_HIDDEN);advance(10);
        assert(lv_obj_get_group(field)==base&&lv_group_get_focused(base)==outside);
        lv_obj_remove_flag(modal,LV_OBJ_FLAG_HIDDEN);
    }
    assert(app_usb_input_modal_open(modal,field)==ESP_OK);
    lv_obj_delete(outside); /* Saved focus can disappear during async refresh. */
    lv_obj_t *nested=lv_obj_create(modal);lv_obj_t *nested_button=button(nested,25,2);
    assert(app_usb_input_modal_open(nested,nested_button)==ESP_OK);
    lv_group_t *nested_group=lv_obj_get_group(nested_button);
    assert(app_usb_input_modal_open(modal,NULL)==ESP_OK&&lv_obj_get_group(nested_button)==nested_group);
    lv_obj_delete(modal);advance(20);
    assert(lv_obj_get_group(one)==base);
    puts("PASS100 modal hide/reopen cycles, preferred text focus, nested automatic deletion and stale saved-focus pointer after background refresh");

    lv_obj_t *outer=NULL,*parent=screen;
    for(unsigned level=0;level<9;++level){
        lv_obj_t *box=lv_obj_create(parent);lv_obj_set_size(box,480,480);lv_obj_set_pos(box,0,0);
        lv_obj_set_style_pad_all(box,0,0);lv_obj_remove_flag(box,LV_OBJ_FLAG_SCROLLABLE);
        if(!outer)outer=box;
        lv_obj_t *b=button(box,25,2);
        assert(app_usb_input_modal_open(box,b)==(level<8?ESP_OK:ESP_ERR_NO_MEM));
        parent=box;
    }
    advance(20);const unsigned failed_before=clicks[2];key(40,false);
    assert(clicks[2]==failed_before);
    mouse(0,0,0,1);mouse(0,0,0,0);advance(20);
    assert(clicks[2]==failed_before+1); /* Depth failure gates keypad only. */
    lv_obj_delete(outer);advance(20);lv_group_focus_obj(one);before=clicks[0];
    key(40,false);assert(clicks[0]==before+1);
    puts("PASS modal depth exhaustion fails closed for keypad while mouse remains usable; parent deletion clears tracked and overflow scopes");

    lv_group_focus_obj(text);
    pthread_t thread;assert(!pthread_create(&thread,NULL,producer,NULL));
    advance(200);assert(!pthread_join(thread,NULL));reset();
    lv_group_focus_obj(one);before=clicks[0];key(40,false);assert(clicks[0]==before+1);
    assert(lv_mem_test()==LV_RESULT_OK);
    puts("PASS concurrent bounded producer flood/overflow resets cleanly, accepts subsequent input and preserves LVGL heap");
    lv_deinit();return 0;
}
