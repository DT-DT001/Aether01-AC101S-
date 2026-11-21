#pragma once
#include "FreeRTOS.h"
typedef struct test_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
