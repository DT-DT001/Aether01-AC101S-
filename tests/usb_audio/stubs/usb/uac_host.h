#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#define UAC_FREQ_NUM_MAX 32
typedef struct fake_uac *uac_host_device_handle_t;
typedef enum{UAC_HOST_DRIVER_EVENT_RX_CONNECTED,UAC_HOST_DRIVER_EVENT_TX_CONNECTED}uac_host_driver_event_t;
typedef enum{UAC_HOST_DEVICE_EVENT_RX_DONE,UAC_HOST_DEVICE_EVENT_TX_DONE,UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR,UAC_HOST_DRIVER_EVENT_DISCONNECTED}uac_host_device_event_t;
typedef void(*uac_host_driver_event_cb_t)(uint8_t,uint8_t,uac_host_driver_event_t,void*);
typedef void(*uac_host_device_event_cb_t)(uac_host_device_handle_t,uac_host_device_event_t,void*);
typedef struct{bool create_background_task;size_t task_priority,stack_size;BaseType_t core_id;uac_host_driver_event_cb_t callback;void*callback_arg;}uac_host_driver_config_t;
typedef struct{uint8_t addr,iface_num;uint32_t buffer_size,buffer_threshold;uac_host_device_event_cb_t callback;void*callback_arg;}uac_host_device_config_t;
typedef enum{UAC_STREAM_TX,UAC_STREAM_RX}uac_host_stream_t;
typedef struct{uac_host_stream_t type;uint8_t iface_num,iface_alt_num,addr;uint16_t VID,PID;}uac_host_dev_info_t;
typedef struct{uint8_t format,channels,subframe_size,bit_resolution,sample_freq_type;union{uint32_t sample_freq[UAC_FREQ_NUM_MAX];struct{uint32_t sample_freq_lower,sample_freq_upper;};};}uac_host_dev_alt_param_t;
typedef struct{uint8_t channels,bit_resolution;uint32_t sample_freq;uint16_t flags;}uac_host_stream_config_t;
esp_err_t uac_host_install(const uac_host_driver_config_t*);esp_err_t uac_host_uninstall(void);
esp_err_t uac_host_device_open(const uac_host_device_config_t*,uac_host_device_handle_t*);
esp_err_t uac_host_device_close(uac_host_device_handle_t);
esp_err_t uac_host_get_device_info(uac_host_device_handle_t,uac_host_dev_info_t*);
esp_err_t uac_host_get_device_alt_param(uac_host_device_handle_t,uint8_t,uac_host_dev_alt_param_t*);
esp_err_t uac_host_device_start(uac_host_device_handle_t,const uac_host_stream_config_t*);
esp_err_t uac_host_device_stop(uac_host_device_handle_t);
esp_err_t uac_host_device_set_mute(uac_host_device_handle_t,bool);
esp_err_t uac_host_device_set_volume_db(uac_host_device_handle_t,float);
esp_err_t uac_host_device_write(uac_host_device_handle_t,uint8_t*,uint32_t,uint32_t);
esp_err_t uac_host_device_read(uac_host_device_handle_t,uint8_t*,uint32_t,uint32_t*,uint32_t);
