#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#define TAG "test"
typedef void *aether_bluetooth_handle_t;
static void *s_storage=(void*)1,*s_ui=(void*)2,*s_wifi=(void*)3;
static esp_err_t storage_error,resume_error;
static unsigned storage_calls,resume_calls,unblock_calls,audio_calls,wifi_calls,bt_calls;
static bool ui_blocked;
int64_t esp_timer_get_time(void){return 0;}
static uint32_t shutdown_remaining(int64_t deadline){return (uint32_t)(deadline/1000);}
static aether_bluetooth_handle_t app_audio_system_bluetooth(void){return (void*)4;}
static esp_err_t aether_storage_cancel_shutdown(void *p,uint32_t timeout){assert(p==s_storage&&timeout<=100);++storage_calls;return storage_error;}
static esp_err_t app_ui_quiesce_storage_resume(void *p){assert(p==s_ui);++resume_calls;return resume_error;}
static bool app_ui_input_blocked(void *p){assert(p==s_ui);return ui_blocked;}
static esp_err_t aether_storage_gate_new_users(void *p,bool blocked){assert(p==s_storage&&!blocked);++unblock_calls;return ESP_OK;}
static void app_audio_system_quiesce_controls(bool blocked){assert(!blocked);++audio_calls;}
static esp_err_t aether_bluetooth_set_enabled(void *p,bool enabled){assert(p&&enabled);++bt_calls;return ESP_OK;}
static esp_err_t aether_wifi_set_enabled(void *p,bool enabled){assert(p==s_wifi&&enabled);++wifi_calls;return ESP_OK;}
#include "shutdown_cancel.inc"
int main(void)
{
    shutdown_context_t state={.active=true,.storage_gate=true,.lease_gate=true,.wifi_enabled=true,.bluetooth_audio_requested=true};
    storage_error=ESP_ERR_TIMEOUT;resume_error=ESP_ERR_INVALID_STATE;ui_blocked=true;
    assert(actual_cancel(&state,100)==ESP_ERR_TIMEOUT&&state.active&&state.storage_gate&&state.lease_gate&&!state.ui_resume_requested);
    assert(resume_calls==1&&!unblock_calls&&!audio_calls&&!wifi_calls&&!bt_calls);
    storage_error=resume_error=ESP_OK;
    assert(actual_cancel(&state,100)==ESP_ERR_NOT_FINISHED&&state.active&&!state.storage_gate&&state.lease_gate&&state.ui_resume_requested);
    assert(storage_calls==2&&resume_calls==2&&!audio_calls&&!unblock_calls);
    assert(actual_cancel(&state,100)==ESP_ERR_NOT_FINISHED&&storage_calls==2&&resume_calls==2);
    ui_blocked=false;
    assert(actual_cancel(&state,100)==ESP_OK&&!state.active&&!state.lease_gate&&unblock_calls==1&&audio_calls==1&&wifi_calls==1&&bt_calls==1);
    assert(actual_cancel(&state,100)==ESP_OK&&unblock_calls==1&&audio_calls==1&&resume_calls==2);
    puts("PASS actual main CANCEL survives resume-mutex contention, retains storage/UI gates until worker acknowledgment, and resumes controls/radios exactly once without replaying media");
    return 0;
}
