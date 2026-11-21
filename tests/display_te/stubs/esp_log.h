#pragma once
void test_display_log(const char *tag, const char *format, ...);
#define ESP_LOGW(tag,...) test_display_log(tag,__VA_ARGS__)
#define ESP_LOGI(tag,...) test_display_log(tag,__VA_ARGS__)
