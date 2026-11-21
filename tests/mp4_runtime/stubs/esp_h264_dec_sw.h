#pragma once
#include <stdint.h>
typedef int esp_h264_err_t;
#define ESP_H264_ERR_OK 0
#define ESP_H264_ERR_FAIL -1
#define ESP_H264_ERR_MEM -3
#define ESP_H264_ERR_UNSUPPORTED -5
#define ESP_H264_RAW_FMT_I420 0x32315559
typedef struct test_h264 *esp_h264_dec_handle_t;
typedef esp_h264_dec_handle_t esp_h264_dec_param_sw_handle_t;
typedef struct { unsigned pic_type; } esp_h264_dec_cfg_sw_t;
typedef struct { uint16_t width,height; } esp_h264_resolution_t;
typedef struct { uint8_t *buffer; uint32_t len; } esp_h264_pkt_t;
typedef struct { esp_h264_pkt_t raw_data; uint32_t consume,dts,pts; } esp_h264_dec_in_frame_t;
typedef struct { int frame_type; uint8_t *outbuf; uint32_t out_size,dts,pts; } esp_h264_dec_out_frame_t;
esp_h264_err_t esp_h264_dec_sw_new(const esp_h264_dec_cfg_sw_t *,esp_h264_dec_handle_t *);
esp_h264_err_t esp_h264_dec_open(esp_h264_dec_handle_t);
esp_h264_err_t esp_h264_dec_process(esp_h264_dec_handle_t,esp_h264_dec_in_frame_t *,esp_h264_dec_out_frame_t *);
esp_h264_err_t esp_h264_dec_close(esp_h264_dec_handle_t);
esp_h264_err_t esp_h264_dec_del(esp_h264_dec_handle_t);
esp_h264_err_t esp_h264_dec_sw_get_param_hd(esp_h264_dec_handle_t,esp_h264_dec_param_sw_handle_t *);
esp_h264_err_t esp_h264_dec_get_resolution(esp_h264_dec_param_sw_handle_t,esp_h264_resolution_t *);
