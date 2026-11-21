#pragma once
#include "FreeRTOS.h"
typedef struct fake_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);BaseType_t xSemaphoreTake(SemaphoreHandle_t,TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);void vSemaphoreDelete(SemaphoreHandle_t);
