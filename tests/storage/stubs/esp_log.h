#pragma once
static inline const char *esp_err_to_name(int error) { (void)error; return "test"; }
#define ESP_LOGI(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
