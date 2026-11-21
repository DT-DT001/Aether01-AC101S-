#include "esp_vfs_fat_lifecycle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_mutex;

esp_err_t esp_vfs_fat_lifecycle_acquire(uint32_t timeout_ms)
{
    portENTER_CRITICAL(&s_guard);
    SemaphoreHandle_t mutex = s_mutex;
    portEXIT_CRITICAL(&s_guard);
    if (!mutex) {
        /* Allocate outside the critical section; discard a losing first-use
         * candidate after publication. The shared mutex has firmware lifetime. */
        SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
        if (!candidate) return ESP_ERR_NO_MEM;
        portENTER_CRITICAL(&s_guard);
        if (!s_mutex) { s_mutex = candidate; candidate = NULL; }
        mutex = s_mutex;
        portEXIT_CRITICAL(&s_guard);
        if (candidate) vSemaphoreDelete(candidate);
    }
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms && !ticks) ticks = 1;
    return xSemaphoreTake(mutex, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void esp_vfs_fat_lifecycle_release(void)
{
    configASSERT(s_mutex);
    const BaseType_t released = xSemaphoreGive(s_mutex);
    configASSERT(released == pdTRUE);
    (void)released;
}
