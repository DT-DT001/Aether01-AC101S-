#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S31) && defined(CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP)
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#endif

/* System-lifetime services can use the S31's non-DMA RTC heap for their
 * stacks. FreeRTOS keeps task control blocks internal. */
static inline BaseType_t app_create_service_task(void (*entry)(void *), const char *name,
                                                unsigned stack_bytes, void *arg,
                                                unsigned priority, TaskHandle_t *task)
{
#if defined(CONFIG_IDF_TARGET_ESP32S31) && defined(CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP)
    const UBaseType_t caps = MALLOC_CAP_RTCRAM | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (xTaskCreateWithCaps(entry, name, stack_bytes, arg, priority, task, caps) == pdPASS)
        return pdPASS;
#endif
    return xTaskCreate(entry, name, stack_bytes, arg, priority, task);
}
