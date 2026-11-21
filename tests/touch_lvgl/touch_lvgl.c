#include "app_touch.h"
#include "esp_lvgl_port.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdio.h>

static int64_t now_us;
static esp_err_t read_error;
static uint8_t held;
static uint16_t point_x, point_y;
static uint32_t source_sequence;
static unsigned reads, pressed, pressing, clicked, released, value_changes, scroll_events;
static uint16_t draw_buffer[480 * 40];

int64_t esp_timer_get_time(void) { return now_us; }
uint32_t cst9220_report_sequence(esp_lcd_touch_handle_t touch)
{ assert(touch); return source_sequence; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock transfer error"; }
bool lvgl_port_lock(uint32_t timeout) { (void)timeout; return true; }
void lvgl_port_unlock(void) {}
void lvgl_port_task_wake(int event,void *context) { (void)event; (void)context; }
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,uint32_t stack,void *ctx,unsigned prio,TaskHandle_t *task)
{ (void)fn; (void)name; (void)stack; (void)ctx; (void)prio; (void)task; return pdFALSE; }
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t wait) { (void)clear; (void)wait; return 0; }
void vTaskNotifyGiveFromISR(TaskHandle_t task,BaseType_t *yield) { (void)task; *yield=pdFALSE; }
bool app_power_is_sleeping(const app_power_t *power) { return power && power->sleeping; }
esp_err_t app_power_wake(app_power_t *power) { power->sleeping=false; return ESP_OK; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch)
{ assert(touch); ++reads; return read_error; }
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t touch,esp_lcd_touch_point_data_t *points,uint8_t *count,uint8_t max)
{
    assert(touch && max); *count=held;
    if(held) points[0]=(esp_lcd_touch_point_data_t){.x=point_x,.y=point_y};
    return ESP_OK; /* Persistent last physical report, even without a new IRQ. */
}
esp_err_t esp_lcd_touch_register_interrupt_callback_with_data(esp_lcd_touch_handle_t touch,
    void (*callback)(esp_lcd_touch_handle_t),void *ctx)
{ (void)touch; (void)callback; (void)ctx; return ESP_OK; }

static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void advance(unsigned ms)
{
    for(unsigned i=0;i<ms;++i) { lv_tick_inc(1); now_us+=1000; lv_timer_handler(); }
}
static void event(lv_event_t *e)
{
    switch(lv_event_get_code(e)) {
        case LV_EVENT_PRESSED: ++pressed; break;
        case LV_EVENT_PRESSING: ++pressing; break;
        case LV_EVENT_RELEASED: ++released; break;
        case LV_EVENT_CLICKED: ++clicked; break;
        case LV_EVENT_VALUE_CHANGED: ++value_changes; break;
        case LV_EVENT_SCROLL: ++scroll_events; break;
        default: break;
    }
}
static lv_obj_t *screen(void)
{
    held=0; ++source_sequence; read_error=ESP_OK; advance(300);
    lv_obj_t *old=lv_screen_active(),*next=lv_obj_create(NULL);
    lv_obj_set_style_pad_all(next,0,0); lv_obj_remove_flag(next,LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(next); if(old) lv_obj_delete(old);
    pressed=pressing=clicked=released=value_changes=scroll_events=0;
    return next;
}
static void point(uint16_t x,uint16_t y,unsigned ms)
{ point_x=x; point_y=y; held=1; ++source_sequence; advance(ms); }
static void up(unsigned ms) { held=0; ++source_sequence; advance(ms); }

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    lv_init(); lv_display_t *display=lv_display_create(480,480); assert(display);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    struct esp_lcd_touch touch={.config={.int_gpio_num=GPIO_NUM_NC}};
    lv_indev_t *indev=app_touch_add(display,&touch,NULL); assert(indev);

    lv_obj_t *page=screen(),*button=lv_button_create(page);
    lv_obj_set_pos(button,40,40); lv_obj_set_size(button,200,80);
    lv_obj_add_event_cb(button,event,LV_EVENT_ALL,NULL); lv_obj_update_layout(page);
    unsigned before=reads; point(100,80,100);
    assert(pressed==1 && pressing>=4 && !released && !clicked && reads>=before+5);
    up(50); assert(clicked==1 && released==1); advance(100); assert(clicked==1);
    puts("PASS actual LVGL pointer: 100ms held touch is not clicked/released; physical release clicks exactly once");

    page=screen(); lv_obj_t *slider=lv_slider_create(page);
    lv_obj_set_pos(slider,40,100); lv_obj_set_size(slider,340,40); lv_slider_set_range(slider,0,100);
    lv_obj_add_event_cb(slider,event,LV_EVENT_ALL,NULL); lv_obj_update_layout(page);
    point(60,120,40); int start=lv_slider_get_value(slider);
    for(unsigned x=80;x<=350;x+=15) point((uint16_t)x,120,20);
    int end=lv_slider_get_value(slider); assert(end>start+60 && end>80 && value_changes>=8 && !released);
    up(50); assert(released==1);
    puts("PASS actual LVGL slider: continuous held reports drag through the range and emit value-change events");

    page=screen(); lv_obj_t *list=lv_obj_create(page);
    lv_obj_set_pos(list,30,200); lv_obj_set_size(list,360,220); lv_obj_set_scroll_dir(list,LV_DIR_VER);
    lv_obj_set_style_pad_all(list,0,0); lv_obj_remove_flag(list,LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(list,event,LV_EVENT_SCROLL,NULL);
    for(unsigned i=0;i<12;++i) {
        lv_obj_t *row=lv_button_create(list); lv_obj_set_pos(row,0,(int32_t)i*55);
        lv_obj_set_size(row,330,50); lv_obj_add_event_cb(row,event,LV_EVENT_CLICKED,NULL);
    }
    lv_obj_update_layout(page); point(180,370,40);
    for(int y=355;y>=235;y-=10) point(180,(uint16_t)y,20);
    up(80); assert(lv_obj_get_scroll_y(list)>60 && scroll_events>=4 && !clicked);
    puts("PASS actual LVGL pointer scrolling: held drag scrolls the list and does not click its rows");

    page=screen(); button=lv_button_create(page);
    lv_obj_set_pos(button,40,40); lv_obj_set_size(button,200,80); lv_obj_add_event_cb(button,event,LV_EVENT_ALL,NULL);
    lv_obj_update_layout(page); point(100,80,100); assert(pressed==1 && !clicked);
    read_error=ESP_ERR_TIMEOUT; before=reads; advance(20);
    assert(!clicked && reads==before+1);
    read_error=ESP_OK; before=reads; advance(230); assert(!clicked && reads==before);
    advance(50); /* Canceled gesture stays consumed until physical release. */
    up(50);
    if(clicked || pressed!=1) fprintf(stderr,"canceled gesture resumed: pressed=%u clicked=%u released=%u\n",pressed,clicked,released);
    assert(!clicked && pressed==1); advance(100); assert(!clicked);
    point(100,80,100); assert(!clicked); up(50); assert(clicked==1);
    puts("PASS actual LVGL error cancellation: recovery consumes the interrupted gesture; the next physical gesture clicks once");

    page=screen(); button=lv_button_create(page);
    lv_obj_set_pos(button,40,40); lv_obj_set_size(button,200,80); lv_obj_add_event_cb(button,event,LV_EVENT_ALL,NULL);
    lv_obj_update_layout(page); point(100,80,1000);
    assert(pressed==1 && lv_obj_has_state(button,LV_STATE_PRESSED) && !released && !clicked);
    advance(1000); /* The release report is lost; the driver returns the same held point. */
    assert(!lv_obj_has_state(button,LV_STATE_PRESSED) && pressed==1 && !clicked);
    advance(100); assert(pressed==1 && !clicked);
    point(110,85,40); up(50);
    assert(pressed==2 && clicked==1 && !lv_obj_has_state(button,LV_STATE_PRESSED));
    puts("PASS actual LVGL lost release: discard stale pressed state without click, then accept the next physical report");

    page=screen(); button=lv_button_create(page);
    lv_obj_set_pos(button,40,40); lv_obj_set_size(button,200,80); lv_obj_add_event_cb(button,event,LV_EVENT_ALL,NULL);
    lv_obj_update_layout(page);
    for(unsigned i=0;i<100;++i) { point(100,80,32); assert(clicked==i); up(32); assert(clicked==i+1); }
    assert(lv_mem_test()==LV_RESULT_OK); lv_indev_delete(indev); lv_display_delete(display); lv_deinit();
    puts("PASS 100 actual pointer press/release cycles and real LVGL allocator integrity");
    return 0;
}
