#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#define TUSB_DESC_DEVICE 1
#define TUSB_DESC_DEVICE_QUALIFIER 6
#define CFG_TUD_ENDPOINT0_SIZE 64
#define TUD_CONFIG_DESC_LEN 9
#define TUD_MSC_DESC_LEN 23
#define TUD_CONFIG_DESCRIPTOR(a,b,c,d,e,f) 9,2,(d),0,b,a,c,0x80,((f)/2)
#define TUD_MSC_DESCRIPTOR(a,b,c,d,e) 9,4,a,0,2,8,6,80,b,7,5,c,2,((e)&255),((e)>>8),0,7,5,d,2,((e)&255),((e)>>8),0
#define SCSI_SENSE_ILLEGAL_REQUEST 5
#define SCSI_SENSE_NOT_READY 2
#define SCSI_SENSE_MEDIUM_ERROR 3
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1e
typedef struct {uint8_t bLength,bDescriptorType;uint16_t bcdUSB;uint8_t bDeviceClass,bDeviceSubClass,bDeviceProtocol,bMaxPacketSize0;uint16_t idVendor,idProduct,bcdDevice;uint8_t iManufacturer,iProduct,iSerialNumber,bNumConfigurations;} tusb_desc_device_t;
typedef struct {uint8_t bLength,bDescriptorType;uint16_t bcdUSB;uint8_t bDeviceClass,bDeviceSubClass,bDeviceProtocol,bMaxPacketSize0,bNumConfigurations,bReserved;} tusb_desc_device_qualifier_t;
typedef enum { TUSB_SPEED_FULL=0, TUSB_SPEED_LOW, TUSB_SPEED_HIGH, TUSB_SPEED_AUTO=0xaa, TUSB_SPEED_INVALID=0xff } tusb_speed_t;
tusb_speed_t tud_speed_get(void);
typedef enum {TINYUSB_EVENT_ATTACHED,TINYUSB_EVENT_DETACHED} tinyusb_event_id_t;
typedef struct {tinyusb_event_id_t id;} tinyusb_event_t;
typedef struct {struct {const tusb_desc_device_t *device;const uint8_t *full_speed_config,*high_speed_config;const tusb_desc_device_qualifier_t *qualifier;const char **string;int string_count;} descriptor;void (*event_cb)(tinyusb_event_t*,void*);void *event_arg;} tinyusb_config_t;
esp_err_t tinyusb_driver_install(const tinyusb_config_t*);
esp_err_t tinyusb_driver_uninstall(void);
bool tud_disconnect(void);
bool tud_msc_set_sense(uint8_t,uint8_t,uint8_t,uint8_t);
