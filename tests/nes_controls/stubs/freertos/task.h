#pragma once
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*)(void *), const char *, uint32_t, void *, int, TaskHandle_t *);
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
