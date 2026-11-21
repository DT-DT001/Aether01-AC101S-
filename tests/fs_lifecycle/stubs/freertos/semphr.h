#pragma once
#include "FreeRTOS.h"
typedef struct test_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
