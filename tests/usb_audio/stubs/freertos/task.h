#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void(*fn)(void*),const char*,uint32_t,void*,UBaseType_t,TaskHandle_t*);
void xTaskNotifyGive(TaskHandle_t);uint32_t ulTaskNotifyTake(BaseType_t,TickType_t);
TickType_t xTaskGetTickCount(void);void vTaskDelay(TickType_t);
