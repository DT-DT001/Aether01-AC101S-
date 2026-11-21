#pragma once
#include "FreeRTOS.h"
typedef struct test_sem *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t,TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t,BaseType_t *);
void vSemaphoreDelete(SemaphoreHandle_t);
