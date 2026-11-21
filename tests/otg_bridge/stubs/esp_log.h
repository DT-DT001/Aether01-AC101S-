#pragma once

#if defined(TEST_OTG_CAPTURE_LOGS)
void test_otg_log(const char *level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#define ESP_LOGI(tag, ...) test_otg_log("I", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_otg_log("W", tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) test_otg_log("E", tag, __VA_ARGS__)
#else
#include "../../tg29/stubs/esp_log.h"
#endif
