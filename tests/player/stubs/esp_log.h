#pragma once
#define ESP_LOGW(tag, ...) ((void)(tag))
#ifdef PLAYER_LOG_TEST
void test_player_log_info(const char *, const char *, ...);
#define ESP_LOGI(tag, ...) test_player_log_info(tag, __VA_ARGS__)
#else
#define ESP_LOGI(tag, ...) ((void)(tag))
#endif
