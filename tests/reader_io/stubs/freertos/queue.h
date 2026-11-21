#pragma once
#include "FreeRTOS.h"
typedef struct fake_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueOverwrite(QueueHandle_t, const void *);
void vQueueDelete(QueueHandle_t);
