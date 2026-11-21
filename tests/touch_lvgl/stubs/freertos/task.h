#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*)(void *),const char *,uint32_t,void *,unsigned,TaskHandle_t *);
uint32_t ulTaskNotifyTake(BaseType_t,TickType_t);
void vTaskNotifyGiveFromISR(TaskHandle_t,BaseType_t *);
