#pragma once
#include "FreeRTOS.h"
typedef struct test_task *TaskHandle_t;
typedef enum { eRunning,eReady,eBlocked,eSuspended,eDeleted,eInvalid } eTaskState;
BaseType_t xTaskCreate(void (*)(void *), const char *, uint32_t, void *, int, TaskHandle_t *);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, int, TaskHandle_t *, BaseType_t);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*)(void *), const char *, uint32_t, void *, int, TaskHandle_t *, BaseType_t, unsigned);
void vTaskDeleteWithCaps(TaskHandle_t);
void vTaskSuspend(TaskHandle_t);
eTaskState eTaskGetState(TaskHandle_t);
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
TickType_t xTaskGetTickCount(void);
void test_yield(void);
#define taskYIELD() test_yield()
