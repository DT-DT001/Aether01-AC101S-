#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*task)(void *), const char *, uint32_t, void *, unsigned, TaskHandle_t *);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
void vTaskDelay(TickType_t ticks);
