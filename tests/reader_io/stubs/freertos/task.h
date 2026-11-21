#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack, void *ctx, UBaseType_t priority, void *handle);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
