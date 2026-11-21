#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
enum {UART_NUM_1=1, UART_DATA_8_BITS=8, UART_PARITY_DISABLE=0, UART_STOP_BITS_1=1,
      UART_HW_FLOWCTRL_DISABLE=0, UART_SCLK_DEFAULT=0, UART_PIN_NO_CHANGE=-1};
typedef struct {int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;} uart_config_t;
esp_err_t uart_driver_install(int port, int rx_buffer, int tx_buffer, int queue_size, void *, int flags);
esp_err_t uart_param_config(int port, const uart_config_t *config);
esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts);
esp_err_t uart_flush_input(int port);
esp_err_t uart_driver_delete(int port);
int uart_read_bytes(int port, void *bytes, uint32_t size, TickType_t timeout);
