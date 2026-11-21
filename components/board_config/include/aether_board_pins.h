#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

/* Three physically separate controllers. GPIO0/1 use the ESP32-S31 LP I2C. */
#define AETHER_I2C_PORT_AUDIO           I2C_NUM_0
#define AETHER_I2C_PORT_TOUCH           I2C_NUM_1
#define AETHER_I2C_PORT_SYSTEM          LP_I2C_NUM_0

/* Shared system LP I2C: TG29, QMI8658A, DPS368 and PCF85063. */
#define AETHER_GPIO_SYS_I2C_SDA        GPIO_NUM_0
#define AETHER_GPIO_SYS_I2C_SCL        GPIO_NUM_1
#define AETHER_GPIO_DPS368_INT         GPIO_NUM_2
#define AETHER_GPIO_QMI8658_INT1       GPIO_NUM_3
/* In the running state PCF85063 notifies GPIO4 and TG29 notifies GPIO5.
 * After ESP_3V3 is cut, the MCU/GPIOs are unpowered and board hardware
 * redirects PCF85063 INT to TG29 so an RTC alarm can restore DCDC2. */
#define AETHER_GPIO_RTC_INT            GPIO_NUM_4
#define AETHER_GPIO_TG29_INT           GPIO_NUM_5

/* TF card (SDMMC 4-bit). VDD_SD is supplied by TG29 BLDO1 on demand. */
#define AETHER_GPIO_TF_DET             GPIO_NUM_6
#define AETHER_GPIO_TF_D0              GPIO_NUM_20
#define AETHER_GPIO_TF_D1              GPIO_NUM_21
#define AETHER_GPIO_TF_D2              GPIO_NUM_22
#define AETHER_GPIO_TF_D3              GPIO_NUM_23
#define AETHER_GPIO_TF_CMD             GPIO_NUM_24
#define AETHER_GPIO_TF_CLK             GPIO_NUM_25

/* CO5300AF QSPI AMOLED, pin assignment revised 2026-11-02. */
#define AETHER_GPIO_LCD_TE             GPIO_NUM_8
#define AETHER_GPIO_LCD_QSPI_D3        GPIO_NUM_9
#define AETHER_GPIO_LCD_QSPI_CS        GPIO_NUM_10
#define AETHER_GPIO_LCD_QSPI_D0        GPIO_NUM_11
#define AETHER_GPIO_LCD_QSPI_CLK       GPIO_NUM_12
#define AETHER_GPIO_LCD_QSPI_D1        GPIO_NUM_13
#define AETHER_GPIO_LCD_QSPI_D2        GPIO_NUM_14
#define AETHER_GPIO_LCD_RST            GPIO_NUM_15
#define AETHER_GPIO_LCD_AVDD_EN        GPIO_NUM_17

/* CST9220 on-cell touch, separate I2C bus. */
#define AETHER_GPIO_TOUCH_SDA          GPIO_NUM_19
#define AETHER_GPIO_TOUCH_SCL          GPIO_NUM_18
#define AETHER_GPIO_TOUCH_INT          GPIO_NUM_7
#define AETHER_GPIO_TOUCH_RST          GPIO_NUM_16

/* AC101S control I2C and I2S wiring, AC101S schematic sheet 4. */
#define AETHER_GPIO_AUDIO_I2C_SDA      GPIO_NUM_43
#define AETHER_GPIO_AUDIO_I2C_SCL      GPIO_NUM_44
#define AETHER_GPIO_AC101S_MCLK         GPIO_NUM_48
#define AETHER_GPIO_AC101S_BCLK         GPIO_NUM_47
#define AETHER_GPIO_AC101S_SDOUT        GPIO_NUM_45 /* codec -> ESP32-S31 */
#define AETHER_GPIO_AC101S_LRCK         GPIO_NUM_49
#define AETHER_GPIO_AC101S_SDIN         GPIO_NUM_46 /* ESP32-S31 -> codec */
#define AETHER_GPIO_PA_ENABLE          GPIO_NUM_42

/* Board control and keys. */
#define AETHER_GPIO_USB_SELECT         GPIO_NUM_51
/* The measured reset/download route is LOW. USB MSC explicitly tests the
 * alternate board mux path; no automatic route cycling is performed. */
#define AETHER_USB_DOWNLOAD_SELECT_LEVEL 0
#define AETHER_USB_MSC_SELECT_LEVEL      1
#define AETHER_GPIO_BOOT_KEY           GPIO_NUM_61

/* User expansion header. The BSP deliberately does not configure or reserve
 * these pins; an application may assign them to SPI, UART, or plain GPIO. */
#define AETHER_GPIO_POGO_1             GPIO_NUM_40
#define AETHER_GPIO_POGO_2             GPIO_NUM_39
#define AETHER_GPIO_POGO_3             GPIO_NUM_38
#define AETHER_GPIO_POGO_4             GPIO_NUM_37

/* Native USB pins are dedicated USB_DP/USB_DM pads, not ordinary GPIOs. */
