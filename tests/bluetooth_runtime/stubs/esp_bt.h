#pragma once
#include "esp_err.h"
typedef enum {ESP_BT_MODE_IDLE,ESP_BT_MODE_BLE,ESP_BT_MODE_CLASSIC_BT,ESP_BT_MODE_BTDM} esp_bt_mode_t;
typedef struct {int mode;} esp_bt_controller_config_t;
#define BTDM_CONTROLLER_MODE_EFF ESP_BT_MODE_BTDM
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() {.mode=ESP_BT_MODE_BTDM}
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *);
esp_err_t esp_bt_controller_enable(esp_bt_mode_t);
esp_err_t esp_bt_controller_disable(void);
esp_err_t esp_bt_controller_deinit(void);
