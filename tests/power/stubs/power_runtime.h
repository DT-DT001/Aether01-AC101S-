#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FINISHED 0x10C
#define portTICK_PERIOD_MS 1
#define ESP_RETURN_ON_FALSE(c, e, ...) do { if (!(c)) return (e); } while (0)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE 1
#define pdPASS 1
#define CONFIG_AETHER_SCREEN_TIMEOUT_SEC 30
#define CONFIG_AETHER_SCREEN_BRIGHTNESS 80
#define AETHER_GPIO_BOOT_KEY 61
#define AETHER_GPIO_TOUCH_INT 7
#define AETHER_GPIO_TG29_INT 5
#define TG29_KEY_EVENT_RELEASE (1U << 0)
#define TG29_KEY_EVENT_PRESS (1U << 1)
#define TG29_KEY_EVENT_LONG_PRESS (1U << 2)
#define TG29_KEY_EVENT_SHORT_PRESS (1U << 3)
typedef void *tg29_handle_t;
typedef void *qmi8658a_handle_t;
typedef void *dps368_handle_t;
typedef struct { float accel_mps2[3], gyro_dps[3]; } qmi8658a_sample_t;
typedef struct { float pressure_pa, temperature_c; } dps368_sample_t;
typedef struct { unsigned value; } pcf85063_time_t;
typedef struct { bool display_sleeping; bool touch_ready; tg29_handle_t pmic;
    qmi8658a_handle_t imu; dps368_handle_t barometer; bool imu_ready, barometer_ready;
} aether_board_t;
typedef struct { bool paused; unsigned ready_calls; } lv_timer_t;
typedef struct { unsigned invalidations; } lv_obj_t;
typedef struct { bool invalidation; int invalidation_count; uint32_t last_activity; lv_obj_t screen; lv_timer_t refresh; } lv_display_t;
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
esp_err_t aether_board_set_display_sleep(aether_board_t *, bool);
esp_err_t aether_board_set_display_brightness(aether_board_t *, uint8_t);
void lv_display_enable_invalidation(lv_display_t *, bool);
lv_timer_t *lv_display_get_refr_timer(lv_display_t *);
void lv_timer_pause(lv_timer_t *);
void lv_timer_resume(lv_timer_t *);
void lv_timer_ready(lv_timer_t *);
esp_err_t tg29_configure_key_wakeup(tg29_handle_t);
esp_err_t tg29_get_key_events(tg29_handle_t, uint8_t *);
esp_err_t tg29_request_power_off(tg29_handle_t);
esp_err_t qmi8658a_set_sleep(qmi8658a_handle_t, bool);
esp_err_t dps368_set_sleep(dps368_handle_t, bool);
void lv_obj_invalidate(lv_obj_t *);
lv_obj_t *lv_display_get_screen_active(lv_display_t *);
void lv_display_trigger_activity(lv_display_t *);
uint32_t lv_display_get_inactive_time(lv_display_t *);
uint32_t lv_tick_get(void);
bool lvgl_port_lock(uint32_t);
void lvgl_port_unlock(void);
int gpio_get_level(int);
TickType_t xTaskGetTickCount(void);
unsigned ulTaskNotifyTake(BaseType_t, TickType_t);
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void xTaskNotifyGive(TaskHandle_t);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);

esp_err_t tg29_get_cc_events(tg29_handle_t, uint8_t *);

#define pdFALSE 0
#define GPIO_INTR_NEGEDGE 2
#define portENTER_CRITICAL_ISR(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_ISR(m) portEXIT_CRITICAL(m)
#define portYIELD_FROM_ISR() ((void)0)
TickType_t xTaskGetTickCountFromISR(void);
void vTaskNotifyGiveFromISR(TaskHandle_t,BaseType_t *);
esp_err_t gpio_isr_handler_add(int,void (*)(void *),void *);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_set_intr_type(int,int);
esp_err_t gpio_intr_enable(int);
int64_t esp_timer_get_time(void);
