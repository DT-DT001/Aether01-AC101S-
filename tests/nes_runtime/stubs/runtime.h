#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NO_MEM 0x105
#define ESP_ERR_NOT_FOUND 0x106
#define ESP_ERR_INVALID_RESPONSE 0x107
#define ESP_ERR_NOT_SUPPORTED 0x108
#define ESP_ERR_TIMEOUT 0x109
typedef void *aether_storage_handle_t;
typedef void *ac101s_audio_handle_t;
typedef void *esp_lcd_touch_handle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdPASS 1
typedef uint32_t TickType_t;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(uint32_t ticks);
void vTaskDelete(void *task);
TickType_t xTaskGetTickCount(void);
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *, unsigned, void *, unsigned, void *, int);
typedef void *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *);
    const char *name;
} esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t, uint64_t);
esp_err_t esp_timer_stop(esp_timer_handle_t);
esp_err_t esp_timer_delete(esp_timer_handle_t);
typedef struct { uint16_t x, y, strength; } esp_lcd_touch_point_data_t;
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t,
    esp_lcd_touch_point_data_t *, uint8_t *, uint8_t);
bool aether_storage_card_inserted(aether_storage_handle_t);
esp_err_t aether_storage_acquire(aether_storage_handle_t, uint32_t);
void aether_storage_release(aether_storage_handle_t);
esp_err_t ac101s_audio_write(ac101s_audio_handle_t, void *, size_t);
esp_err_t ac101s_audio_start(ac101s_audio_handle_t, int, float);
esp_err_t ac101s_audio_stop(ac101s_audio_handle_t);
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_FALSE(condition, error, ...) do { if (!(condition)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(expression, ...) do { esp_err_t result_ = (expression); if (result_ != ESP_OK) return result_; } while (0)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_malloc(size_t, unsigned);
const char *esp_err_to_name(esp_err_t);
size_t strlcpy(char *, const char *, size_t);
