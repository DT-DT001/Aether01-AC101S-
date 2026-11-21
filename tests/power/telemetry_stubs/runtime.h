#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "esp_err.h"
#define ESP_ERR_NOT_FINISHED 0x10C
#define ESP_ERR_NOT_FOUND 0x105
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
#define pdMS_TO_TICKS(t) (t)
#define pdPASS 1
typedef struct {
    float accel_mps2[3], gyro_dps[3], gyro_raw_dps[3], gyro_bias_dps[3];
    bool gyro_bias_valid, gyro_stationary;
} qmi8658a_sample_t;
typedef struct { float pressure_pa, temperature_c; } dps368_sample_t;
typedef struct { unsigned value; } pcf85063_time_t;
typedef struct { void *imu, *barometer, *rtc, *display_io; bool imu_ready, barometer_ready, rtc_ready; } aether_board_t;
esp_err_t qmi8658a_read(void *, qmi8658a_sample_t *);
esp_err_t qmi8658a_read_fresh(void *, qmi8658a_sample_t *);
esp_err_t dps368_read(void *, dps368_sample_t *);
esp_err_t pcf85063_get_time(void *, pcf85063_time_t *);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
unsigned uxTaskGetStackHighWaterMark(void *);
int64_t esp_timer_get_time(void);
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
#define MALLOC_CAP_SPIRAM 8
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_minimum_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
