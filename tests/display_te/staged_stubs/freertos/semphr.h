#pragma once
#include "freertos/FreeRTOS.h"
typedef struct stage_semaphore *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t *);
void vSemaphoreDelete(SemaphoreHandle_t);
