#pragma once

#define pdPASS 1
int xTaskCreate(void (*function)(void *), const char *name, unsigned stack_size,
                void *argument, unsigned priority, void *task);
void vTaskDelete(void *task);
