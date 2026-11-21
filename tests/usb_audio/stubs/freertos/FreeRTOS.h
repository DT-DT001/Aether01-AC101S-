#pragma once
#include <stdint.h>
#include <stddef.h>
typedef unsigned TickType_t;typedef int BaseType_t;typedef unsigned UBaseType_t;
typedef int portMUX_TYPE;
extern unsigned critical_depth;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p),++critical_depth)
#define portEXIT_CRITICAL(p) ((void)(p),--critical_depth)
#define pdMS_TO_TICKS(v) (v)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define tskNO_AFFINITY -1
