#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(x) pthread_mutex_lock(x)
#define portEXIT_CRITICAL(x) pthread_mutex_unlock(x)
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define configASSERT(x) assert(x)
