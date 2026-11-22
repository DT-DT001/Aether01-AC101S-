#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#define ESP_INTR_FLAG_LOWMED 3
enum { USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_ERROR,
 USB_TRANSFER_STATUS_TIMED_OUT, USB_TRANSFER_STATUS_CANCELED,
 USB_TRANSFER_STATUS_STALL, USB_TRANSFER_STATUS_OVERFLOW,
 USB_TRANSFER_STATUS_SKIPPED, USB_TRANSFER_STATUS_NO_DEVICE };
typedef void *usb_device_handle_t;typedef void *usb_host_client_handle_t;
typedef struct {uint8_t bLength,bDescriptorType;uint16_t wTotalLength;} usb_config_desc_t;
typedef struct {uint8_t bMaxPacketSize0;uint16_t idVendor,idProduct;} usb_device_desc_t;
typedef struct usb_transfer_s {uint8_t *data_buffer;size_t data_buffer_size;usb_device_handle_t device_handle;uint8_t bEndpointAddress;
 int num_bytes,actual_num_bytes,status;unsigned timeout_ms;void (*callback)(struct usb_transfer_s*);void *context;} usb_transfer_t;
typedef struct {
    bool skip_phy_setup,root_port_unpowered;
    int intr_flags;
    unsigned peripheral_map;
    struct {unsigned rx_fifo_lines,nptx_fifo_lines,ptx_fifo_lines;} fifo_settings_custom;
} usb_host_config_t;
typedef enum {USB_HOST_CLIENT_EVENT_NEW_DEV,USB_HOST_CLIENT_EVENT_DEV_GONE} usb_host_client_event_t;
typedef struct {usb_host_client_event_t event;union {struct {uint8_t address;} new_dev;struct {usb_device_handle_t dev_hdl;} dev_gone;};} usb_host_client_event_msg_t;
typedef struct {bool is_synchronous;int max_num_event_msg;struct {void (*client_event_callback)(const usb_host_client_event_msg_t*,void*);void *callback_arg;} async;} usb_host_client_config_t;
esp_err_t usb_host_lib_handle_events(TickType_t,uint32_t*);esp_err_t usb_host_client_handle_events(usb_host_client_handle_t,TickType_t);
esp_err_t usb_host_transfer_alloc(size_t,int,usb_transfer_t**);esp_err_t usb_host_transfer_free(usb_transfer_t*);
esp_err_t usb_host_transfer_submit(usb_transfer_t*);esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t,usb_transfer_t*);
esp_err_t usb_host_device_open(usb_host_client_handle_t,uint8_t,usb_device_handle_t*);
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t,const usb_config_desc_t**);
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t,const usb_device_desc_t**);
esp_err_t usb_host_interface_claim(usb_host_client_handle_t,usb_device_handle_t,uint8_t,uint8_t);
esp_err_t usb_host_interface_release(usb_host_client_handle_t,usb_device_handle_t,uint8_t);
esp_err_t usb_host_device_close(usb_host_client_handle_t,usb_device_handle_t);
esp_err_t usb_host_endpoint_halt(usb_device_handle_t,uint8_t);esp_err_t usb_host_endpoint_flush(usb_device_handle_t,uint8_t);
esp_err_t usb_host_install(const usb_host_config_t*);esp_err_t usb_host_uninstall(void);
esp_err_t usb_host_client_register(const usb_host_client_config_t*,usb_host_client_handle_t*);
esp_err_t usb_host_client_deregister(usb_host_client_handle_t);esp_err_t usb_host_device_free_all(void);
esp_err_t usb_host_lib_set_root_port_power(bool);
