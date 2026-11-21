#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef int portMUX_TYPE;
#define pdPASS 1
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(value) (value)
#define portENTER_CRITICAL(lock) do { (void)(lock); } while (0)
#define portEXIT_CRITICAL(lock) do { (void)(lock); } while (0)

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
