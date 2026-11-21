#pragma once
#include <stdbool.h>
#include <stdint.h>
#define LVGL_PORT_EVENT_TOUCH 2
bool lvgl_port_lock(uint32_t);
void lvgl_port_unlock(void);
void lvgl_port_task_wake(int,void *);
