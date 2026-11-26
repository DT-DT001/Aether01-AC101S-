#pragma once
static inline void recorder_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
#define ESP_LOGW recorder_test_log
#define ESP_LOGI recorder_test_log
