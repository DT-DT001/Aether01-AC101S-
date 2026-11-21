#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef unsigned TickType_t;typedef int BaseType_t;typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(v) (v)
void test_enter(int *);void test_exit(int *);
#define portENTER_CRITICAL(p) test_enter(p)
#define portEXIT_CRITICAL(p) test_exit(p)

#define portMAX_DELAY UINT32_MAX
