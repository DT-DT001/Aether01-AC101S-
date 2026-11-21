#pragma once
void test_log(const char*,const char*,...);
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
const char *esp_err_to_name(int);
