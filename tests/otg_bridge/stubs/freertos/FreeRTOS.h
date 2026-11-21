#pragma once
#include <stdint.h>
#include <pthread.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
void test_enter(portMUX_TYPE *lock);
void test_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) test_enter(lock)
#define portEXIT_CRITICAL(lock) test_exit(lock)
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(value) (value)
