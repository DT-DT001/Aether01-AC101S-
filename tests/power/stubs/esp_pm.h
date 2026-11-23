#pragma once
#include "power_runtime.h"

typedef void *esp_pm_lock_handle_t;
typedef struct {
    int max_freq_mhz;
    int min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;

#define ESP_PM_CPU_FREQ_MAX 0
esp_err_t esp_pm_lock_create(int type, int argument, const char *name, esp_pm_lock_handle_t *lock);
esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t lock);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock);
esp_err_t esp_pm_configure(const esp_pm_config_t *config);
