#pragma once
#include <pthread.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
#define portENTER_CRITICAL_ISR(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL_ISR(p) pthread_mutex_unlock(p)
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portYIELD_FROM_ISR() ((void)0)
