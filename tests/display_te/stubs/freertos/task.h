#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t);
#ifdef AETHER_VIDEO_DIAGNOSTIC
#define tskNO_AFFINITY ((BaseType_t)0x7fffffff)
BaseType_t xPortGetCoreID(void);
BaseType_t xTaskGetCoreID(void *task);
#endif
