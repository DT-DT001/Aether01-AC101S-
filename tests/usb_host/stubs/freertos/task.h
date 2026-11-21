#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
void xTaskNotifyGive(TaskHandle_t);unsigned ulTaskNotifyTake(BaseType_t,TickType_t);
TickType_t xTaskGetTickCount(void);void vTaskDelay(TickType_t);
