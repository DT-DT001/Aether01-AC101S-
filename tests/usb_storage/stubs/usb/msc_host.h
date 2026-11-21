#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef void *msc_host_device_handle_t;
typedef struct {enum {MSC_DEVICE_CONNECTED,MSC_DEVICE_DISCONNECTED} event;
 union {uint8_t address;msc_host_device_handle_t handle;} device;} msc_host_event_t;
typedef struct {bool create_backround_task;size_t task_priority,stack_size;BaseType_t core_id;
 void (*callback)(const msc_host_event_t*,void*);void *callback_arg;} msc_host_driver_config_t;
typedef struct {uint32_t sector_count,sector_size;} msc_host_device_info_t;
esp_err_t msc_host_install(const msc_host_driver_config_t*);
esp_err_t msc_host_uninstall(void);
esp_err_t msc_host_install_device(uint8_t,msc_host_device_handle_t*);
esp_err_t msc_host_uninstall_device(msc_host_device_handle_t);
esp_err_t msc_host_get_device_info(msc_host_device_handle_t,msc_host_device_info_t*);
