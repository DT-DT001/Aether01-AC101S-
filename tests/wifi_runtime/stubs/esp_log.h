#pragma once
void test_log(const char *,const char *,...) __attribute__((format(printf,2,3)));
#define ESP_LOGI(tag,...) test_log(tag,__VA_ARGS__)
