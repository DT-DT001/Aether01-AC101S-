#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#define TAG "test"
typedef void *aether_bluetooth_handle_t;
static void *s_storage=(void*)1,*s_ui=(void*)2,*s_wifi=(void*)3;
static esp_err_t storage_error,resume_error;
static unsigned storage_calls,resume_calls,unblock_calls,audio_calls,wifi_calls,bt_calls,hid_calls,clock_calls;
static bool ui_blocked;
int64_t esp_timer_get_time(void){return 0;}
static uint32_t shutdown_remaining(int64_t deadline){return (uint32_t)(deadline/1000);}
static aether_bluetooth_handle_t app_audio_system_bluetooth(void){return (void*)4;}
static esp_err_t aether_storage_cancel_shutdown(void *p,uint32_t timeout){assert(p==s_storage&&timeout<=100);++storage_calls;return storage_error;}
static esp_err_t app_ui_quiesce_storage_resume(void *p){assert(p==s_ui);++resume_calls;return resume_error;}
static bool app_ui_input_blocked(void *p){assert(p==s_ui);return ui_blocked;}
static esp_err_t aether_storage_gate_new_users(void *p,bool blocked){assert(p==s_storage&&!blocked);++unblock_calls;return ESP_OK;}
static void app_audio_system_quiesce_controls(bool blocked){assert(!blocked);++audio_calls;}
static void app_tools_set_clock_quiesced(bool blocked){assert(!blocked);++clock_calls;}
static esp_err_t aether_bluetooth_set_enabled(void *p,bool enabled){assert(p&&enabled);++bt_calls;return ESP_OK;}
static esp_err_t aether_bluetooth_hid_set_enabled(void *p, bool enabled)
{
    assert(p && enabled);
    ++hid_calls;
    return ESP_OK;
}
static esp_err_t aether_wifi_set_enabled(void *p,bool enabled){assert(p==s_wifi&&enabled);++wifi_calls;return ESP_OK;}

static aether_board_t s_board;
static lv_display_t display;
static struct {bool render_paused;lv_display_t *display;} s_power={.display=&display};
static bool gui_busy, gui_locked, touch_enabled, touch_asleep, panel_asleep;
static unsigned gui_locks, invalidation_pauses, invalidation_resumes, park_calls, repaint_calls;
static esp_err_t touch_error, park_error;
bool lvgl_port_lock(uint32_t timeout)
{
    assert(timeout == 30 && !gui_locked);
    ++gui_locks;
    return (gui_locked = !gui_busy);
}
void lvgl_port_unlock(void){assert(gui_locked);gui_locked=false;}
lv_timer_t *lv_display_get_refr_timer(lv_display_t *target){assert(gui_locked&&target==&display);return &display.refresh;}
void lv_timer_pause(lv_timer_t *timer){assert(gui_locked&&timer==&display.refresh);}
void lv_timer_resume(lv_timer_t *timer){assert(gui_locked&&timer==&display.refresh);}
void lv_timer_ready(lv_timer_t *timer){assert(gui_locked&&timer==&display.refresh);}
void lv_display_enable_invalidation(lv_display_t *target,bool enabled)
{
    assert(gui_locked&&target==&display);
    if(enabled) ++invalidation_resumes;
    else ++invalidation_pauses;
}
lv_obj_t *lv_display_get_screen_active(lv_display_t *target){assert(gui_locked&&target==&display);return &display.screen;}
void lv_obj_invalidate(lv_obj_t *screen){assert(gui_locked&&screen==&display.screen);++repaint_calls;}
void lv_display_trigger_activity(lv_display_t *target){assert(gui_locked&&target==&display);}
static bool app_touch_set_shutdown_paused(bool paused)
{
    assert(gui_locked);
    const bool old=touch_enabled;
    touch_enabled=!paused;
    return old;
}
static esp_err_t aether_board_park_peripherals(aether_board_t *board)
{
    assert(gui_locked&&board==&s_board&&!touch_enabled);
    ++park_calls;
    panel_asleep=touch_asleep=true;
    return park_error;
}
static esp_err_t aether_board_set_touch_sleep(aether_board_t *board,bool asleep)
{
    assert(gui_locked&&board==&s_board&&!asleep);
    if(touch_error==ESP_OK) touch_asleep=false;
    return touch_error;
}
esp_err_t aether_board_set_display_sleep(aether_board_t *board,bool asleep)
{
    assert(gui_locked&&board==&s_board&&!asleep);
    panel_asleep=false;
    return ESP_OK;
}
#include "shutdown_cancel.inc"

static void test_peripheral_park_and_cancel(void)
{
    shutdown_context_t state={.active=true};
    s_board.display_ready=s_board.touch_ready=touch_enabled=true;
    park_shutdown_peripherals(&state,279);
    assert(!gui_locks&&!park_calls&&!state.peripherals_parked);
    gui_busy=true;
    park_shutdown_peripherals(&state,300);
    assert(gui_locks==1&&!park_calls&&!state.peripherals_parked&&touch_enabled);
    gui_busy=false;
    park_error=ESP_FAIL;
    park_shutdown_peripherals(&state,300);
    assert(park_calls==1&&state.peripherals_parked&&panel_asleep&&touch_asleep&&!touch_enabled);
    assert(invalidation_pauses==1&&!invalidation_resumes&&!gui_locked);

    const unsigned resumes=resume_calls;
    touch_error=ESP_ERR_TIMEOUT;
    assert(actual_cancel(&state,100)==ESP_ERR_TIMEOUT);
    assert(state.peripherals_parked&&!panel_asleep&&touch_asleep&&!touch_enabled);
    assert(!invalidation_resumes&&resume_calls==resumes);
    touch_error=ESP_OK;
    assert(actual_cancel(&state,100)==ESP_OK);
    assert(!state.peripherals_parked&&!panel_asleep&&!touch_asleep&&touch_enabled);
    assert(invalidation_resumes==1&&resume_calls==resumes+1&&repaint_calls==1);

    state=(shutdown_context_t){.active=true};
    s_power.render_paused=true;
    s_board.touch_sleeping=true;
    touch_enabled=false;
    park_error=ESP_OK;
    park_shutdown_peripherals(&state,300);
    assert(park_calls==2&&state.peripherals_parked&&!state.restore_render);
    assert(actual_cancel(&state,100)==ESP_OK);
    assert(panel_asleep&&touch_asleep&&!touch_enabled&&invalidation_pauses==1&&invalidation_resumes==1);
    puts("PASS shutdown peripheral park and rollback balance LVGL gating, preserve prior sleep and retry on touch failure");
}

int main(void)
{
    shutdown_context_t state={.active=true,.storage_gate=true,.lease_gate=true,.wifi_enabled=true,
        .bluetooth_audio_requested=true,.bluetooth_hid_requested=true};
    storage_error=ESP_ERR_TIMEOUT;resume_error=ESP_ERR_INVALID_STATE;ui_blocked=true;
    assert(actual_cancel(&state,100)==ESP_ERR_TIMEOUT&&state.active&&state.storage_gate&&state.lease_gate&&!state.ui_resume_requested);
    assert(resume_calls==1&&!unblock_calls&&!audio_calls&&!clock_calls&&!wifi_calls&&!bt_calls&&!hid_calls);
    storage_error=resume_error=ESP_OK;
    assert(actual_cancel(&state,100)==ESP_ERR_NOT_FINISHED&&state.active&&!state.storage_gate&&state.lease_gate&&state.ui_resume_requested);
    assert(storage_calls==2&&resume_calls==2&&!audio_calls&&!clock_calls&&!unblock_calls);
    assert(actual_cancel(&state,100)==ESP_ERR_NOT_FINISHED&&storage_calls==2&&resume_calls==2);
    ui_blocked=false;
    assert(actual_cancel(&state,100)==ESP_OK&&!state.active&&!state.lease_gate&&unblock_calls==1&&audio_calls==1&&clock_calls==1&&wifi_calls==1&&bt_calls==1&&hid_calls==1);
    assert(actual_cancel(&state,100)==ESP_OK&&unblock_calls==1&&audio_calls==1&&clock_calls==1&&resume_calls==2&&hid_calls==1);
    puts("PASS actual main CANCEL survives resume-mutex contention, retains storage/UI gates until worker acknowledgment, and resumes controls/radios exactly once without replaying media");
    test_peripheral_park_and_cancel();
    return 0;
}
