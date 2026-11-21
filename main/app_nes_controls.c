#include "app_nes_controls.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define SAVE_QUIET_MS 250U
#define SAVE_RETRY_MS 1000U
static const uint8_t defaults[APP_NES_CONTROL_COUNT][APP_NES_CONTROL_SLOTS] = {
    {0x52,0x1a}, {0x51,0x16}, {0x50,0x04}, {0x4f,0x07},
    {0x1d,0}, {0x1b,0}, {0xe5,0xe1}, {0x28,0}, {0x13,0}, {0x15,0}, {0x29,0},
};
static const char *const actions[APP_NES_CONTROL_COUNT] = {
    "上", "下", "左", "右", "A", "B", "选择", "开始", "暂停", "复位", "退出",
};
typedef struct { const char *name; uint8_t usage; } key_choice_t;
static const key_choice_t choices[] = {
    {"None",0x00},
    {"A",0x04},
    {"B",0x05},
    {"C",0x06},
    {"D",0x07},
    {"E",0x08},
    {"F",0x09},
    {"G",0x0a},
    {"H",0x0b},
    {"I",0x0c},
    {"J",0x0d},
    {"K",0x0e},
    {"L",0x0f},
    {"M",0x10},
    {"N",0x11},
    {"O",0x12},
    {"P",0x13},
    {"Q",0x14},
    {"R",0x15},
    {"S",0x16},
    {"T",0x17},
    {"U",0x18},
    {"V",0x19},
    {"W",0x1a},
    {"X",0x1b},
    {"Y",0x1c},
    {"Z",0x1d},
    {"1",0x1e},
    {"2",0x1f},
    {"3",0x20},
    {"4",0x21},
    {"5",0x22},
    {"6",0x23},
    {"7",0x24},
    {"8",0x25},
    {"9",0x26},
    {"0",0x27},
    {"Up",0x52},
    {"Down",0x51},
    {"Left",0x50},
    {"Right",0x4f},
    {"Enter",0x28},
    {"Esc",0x29},
    {"Space",0x2c},
    {"Tab",0x2b},
    {"Backspace",0x2a},
    {"Delete",0x4c},
    {"Home",0x4a},
    {"End",0x4d},
    {"Page Up",0x4b},
    {"Page Down",0x4e},
    {"Left Shift",0xe1},
    {"Right Shift",0xe5},
    {"Left Ctrl",0xe0},
    {"Right Ctrl",0xe4},
    {"Left Alt",0xe2},
    {"Right Alt",0xe6},
    {"F1",0x3a},
    {"F2",0x3b},
    {"F3",0x3c},
    {"F4",0x3d},
    {"F5",0x3e},
    {"F6",0x3f},
    {"F7",0x40},
    {"F8",0x41},
    {"F9",0x42},
    {"F10",0x43},
    {"F11",0x44},
    {"F12",0x45},
    {"Minus",0x2d},
    {"Equals",0x2e},
    {"Left Bracket",0x2f},
    {"Right Bracket",0x30},
    {"Backslash",0x31},
    {"Semicolon",0x33},
    {"Quote",0x34},
    {"Grave",0x35},
    {"Comma",0x36},
    {"Period",0x37},
    {"Slash",0x38},
    {"Caps Lock",0x39},
    {"Left GUI",0xe3},
    {"Right GUI",0xe7},
    {"Num Enter",0x58},
    {"Num Divide",0x54},
    {"Num Multiply",0x55},
    {"Num Minus",0x56},
    {"Num Plus",0x57},
    {"Num 1",0x59},
    {"Num 2",0x5a},
    {"Num 3",0x5b},
    {"Num 4",0x5c},
    {"Num 5",0x5d},
    {"Num 6",0x5e},
    {"Num 7",0x5f},
    {"Num 8",0x60},
    {"Num 9",0x61},
    {"Num 0",0x62},
    {"Num Decimal",0x63},
};
typedef struct { uint32_t version; uint8_t keys[APP_NES_CONTROL_COUNT][APP_NES_CONTROL_SLOTS]; uint8_t reserved[2]; } settings_t;
static struct {
    portMUX_TYPE lock;
    TaskHandle_t task;
    app_nes_controls_snapshot_t view;
    TickType_t changed;
    bool (*busy)(void *);
    void *busy_context;
} s = {.lock=portMUX_INITIALIZER_UNLOCKED};

unsigned app_nes_controls_key_count(void) { return sizeof(choices)/sizeof(*choices); }
const char *app_nes_controls_key_name(unsigned index) { return index<app_nes_controls_key_count()?choices[index].name:"None"; }
uint8_t app_nes_controls_key_usage(unsigned index) { return index<app_nes_controls_key_count()?choices[index].usage:0; }
unsigned app_nes_controls_key_index(uint8_t usage)
{ for(unsigned i=0;i<app_nes_controls_key_count();++i)if(choices[i].usage==usage)return i;return 0; }
const char *app_nes_controls_action_name(unsigned action) { return action<APP_NES_CONTROL_COUNT?actions[action]:"?"; }
static bool supported(uint8_t usage) { return !usage || app_nes_controls_key_index(usage)!=0; }
static bool valid(const uint8_t keys[APP_NES_CONTROL_COUNT][APP_NES_CONTROL_SLOTS])
{
    uint8_t used[32]={0};
    for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a)for(unsigned k=0;k<APP_NES_CONTROL_SLOTS;++k){
        const uint8_t u=keys[a][k];if(!supported(u))return false;if(!u)continue;
        if(used[u/8]&(1U<<(u%8)))return false;
        used[u/8]|=(uint8_t)(1U<<(u%8));
    }
    return true;
}
static TickType_t ticks_ms(uint32_t ms) { TickType_t t=pdMS_TO_TICKS(ms);return t?t:1; }
static TickType_t service_once(void)
{
    app_nes_controls_snapshot_t view;TickType_t changed;bool (*busy)(void*);void *context;
    portENTER_CRITICAL(&s.lock);view=s.view;changed=s.changed;busy=s.busy;context=s.busy_context;portEXIT_CRITICAL(&s.lock);
    if(!view.ready||!view.pending)return portMAX_DELAY;
    if(busy&&busy(context)){
        portENTER_CRITICAL(&s.lock);s.view.deferred=true;portEXIT_CRITICAL(&s.lock);
        return ticks_ms(SAVE_QUIET_MS);
    }
    const TickType_t elapsed=xTaskGetTickCount()-changed,quiet=ticks_ms(SAVE_QUIET_MS);
    if(elapsed<quiet)return quiet-elapsed;
    settings_t record={.version=1};memcpy(record.keys,view.keys,sizeof(record.keys));
    nvs_handle_t nvs;esp_err_t error=nvs_open("nes_keys",NVS_READWRITE,&nvs);
    if(error==ESP_OK){error=nvs_set_blob(nvs,"mapping",&record,sizeof(record));if(error==ESP_OK)error=nvs_commit(nvs);nvs_close(nvs);}
    portENTER_CRITICAL(&s.lock);
    s.view.error=error;s.view.deferred=false;
    if(error==ESP_OK&&s.view.revision==view.revision)s.view.pending=false;
    const bool pending=s.view.pending;
    portEXIT_CRITICAL(&s.lock);
    return pending?ticks_ms(error==ESP_OK?SAVE_QUIET_MS:SAVE_RETRY_MS):portMAX_DELAY;
}
static void service(void *unused)
{
    (void)unused;
    for(;;)ulTaskNotifyTake(pdTRUE,service_once());
}
esp_err_t app_nes_controls_init(void)
{
    if(s.view.ready)return ESP_OK;
    app_nes_controls_snapshot_t view={.revision=1};memcpy(view.keys,defaults,sizeof(defaults));
    nvs_handle_t nvs;esp_err_t error=nvs_open("nes_keys",NVS_READONLY,&nvs);
    if(error==ESP_OK){
        settings_t record;size_t size=sizeof(record);error=nvs_get_blob(nvs,"mapping",&record,&size);nvs_close(nvs);
        if(error==ESP_OK){
            if(size==sizeof(record)&&record.version==1&&valid(record.keys))memcpy(view.keys,record.keys,sizeof(view.keys));
            else error=ESP_ERR_INVALID_RESPONSE;
        }
    }
    view.error=error==ESP_ERR_NVS_NOT_FOUND?ESP_OK:error;
    portENTER_CRITICAL(&s.lock);s.view=view;portEXIT_CRITICAL(&s.lock);
    if(xTaskCreate(service,"nes_keys",3072,NULL,2,&s.task)!=pdPASS)return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s.lock);s.view.ready=true;portEXIT_CRITICAL(&s.lock);
    return ESP_OK;
}
void app_nes_controls_set_persistence_guard(bool (*busy)(void*),void *context)
{ portENTER_CRITICAL(&s.lock);s.busy=busy;s.busy_context=context;portEXIT_CRITICAL(&s.lock); }
void app_nes_controls_get_snapshot(app_nes_controls_snapshot_t *view)
{ if(view){portENTER_CRITICAL(&s.lock);*view=s.view;portEXIT_CRITICAL(&s.lock);} }
uint16_t app_nes_controls_map(void *unused,const uint8_t keys[32])
{
    (void)unused;if(!keys)return 0;
    uint8_t bindings[APP_NES_CONTROL_COUNT][APP_NES_CONTROL_SLOTS];
    portENTER_CRITICAL(&s.lock);memcpy(bindings,s.view.ready?s.view.keys:defaults,sizeof(bindings));portEXIT_CRITICAL(&s.lock);
    uint16_t result=0;
    for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a)for(unsigned k=0;k<APP_NES_CONTROL_SLOTS;++k){
        const uint8_t u=bindings[a][k];if(u&&(keys[u/8]&(1U<<(u%8))))result|=(uint16_t)(1U<<a);
    }
    return result;
}
esp_err_t app_nes_controls_set(app_nes_control_t action,unsigned slot,uint8_t usage,unsigned *conflict_action)
{
    if(conflict_action)*conflict_action=APP_NES_CONTROL_COUNT;
    if((unsigned)action>=APP_NES_CONTROL_COUNT||slot>=APP_NES_CONTROL_SLOTS||!supported(usage))return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s.lock);
    if(!s.view.ready){portEXIT_CRITICAL(&s.lock);return ESP_ERR_INVALID_STATE;}
    if(s.view.keys[action][slot]==usage){portEXIT_CRITICAL(&s.lock);return ESP_OK;}
    if(usage)for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a)for(unsigned k=0;k<APP_NES_CONTROL_SLOTS;++k){
        if(s.view.keys[a][k]==usage){if(conflict_action)*conflict_action=a;portEXIT_CRITICAL(&s.lock);return ESP_ERR_INVALID_ARG;}
    }
    s.view.keys[action][slot]=usage;s.view.revision++;s.view.pending=true;s.view.deferred=false;
    s.changed=xTaskGetTickCount();portEXIT_CRITICAL(&s.lock);xTaskNotifyGive(s.task);return ESP_OK;
}
esp_err_t app_nes_controls_reset(void)
{
    portENTER_CRITICAL(&s.lock);
    if(!s.view.ready){portEXIT_CRITICAL(&s.lock);return ESP_ERR_INVALID_STATE;}
    memcpy(s.view.keys,defaults,sizeof(defaults));s.view.revision++;s.view.pending=true;s.view.deferred=false;
    s.changed=xTaskGetTickCount();portEXIT_CRITICAL(&s.lock);xTaskNotifyGive(s.task);return ESP_OK;
}
