#pragma once
void mp4_test_log(const char *level,const char *tag,const char *format,...)
    __attribute__((format(printf,3,4)));
#define ESP_LOGI(tag,...) mp4_test_log("I",tag,__VA_ARGS__)
#define ESP_LOGW(tag,...) mp4_test_log("W",tag,__VA_ARGS__)
#define ESP_LOGE(tag,...) mp4_test_log("E",tag,__VA_ARGS__)
