#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(value) (value)
#define portMAX_DELAY UINT32_MAX
