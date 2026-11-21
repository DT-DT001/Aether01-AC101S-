#pragma once
static inline void media_test_log(const char *format, ...) { (void)format; }
#define ESP_LOGI(tag, ...) ((void)(tag), media_test_log(__VA_ARGS__))
