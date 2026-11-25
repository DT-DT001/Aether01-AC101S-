# Aether01 IO and power-domain map

The current board has no MMC5603. Its driver sources are retained but excluded from compilation; address0x30 is no longer probed and the compass page is removed.

This map is based on the Aether01 schematic, TG29 datasheet, and KD021WVOSN009-C003B module specification. Display and touch pins follow the user's revised schematic supplied on 2026-11-02. Directions are relative to the ESP32-S31.

## GPIO map

| GPIO | Signal / peripheral | Direction | Notes |
|---:|---|---|---|
| 0 | SYS_LPI2C_SDA | bidirectional | ESP32-S31 LP I2C shared by TG29, QMI8658A, DPS368, and PCF85063 |
| 1 | SYS_LPI2C_SCL | output | ESP32-S31 LP I2C system-bus clock |
| 2 | DPS368_INT | input | Barometer interrupt |
| 3 | QMI8658A_INT1 | input | IMU INT1 |
| 4 | PCF85063_INT | input | PCF85063 independently notifies the MCU while running. With ESP_3V3 off, GPIO4 is unavailable and hardware routes this interrupt to the TG29 wake path |
| 5 | TG29_INT | input | TG29 independently notifies the MCU while running. With ESP_3V3 off, the MCU and GPIO5 are unpowered |
| 6 | TF_DET | input | microSD card detect |
| 7 | TP_INT | input | CST9220 interrupt, configured active-low |
| 8 | LCD_TE | input | CO5300AF vertical-blank rising edge; synchronize the first flush of each LVGL refresh, with bounded fallback, rather than waiting for every 40-line chunk |
| 9 | LCD_QSPI_D3 | bidirectional | Display QSPI data 3 |
| 10 | LCD_QSPI_CS | output | Display chip select |
| 11 | LCD_QSPI_D0 | bidirectional | Display QSPI data 0 |
| 12 | LCD_QSPI_CLK | output | Display QSPI clock, 40 MHz by default |
| 13 | LCD_QSPI_D1 | bidirectional | Display QSPI data 1 |
| 14 | LCD_QSPI_D2 | bidirectional | Display QSPI data 2 |
| 15 | LCD_RST | output | Active-low display reset |
| 16 | TP_RST | output | Active-low CST9220 reset |
| 17 | AVDD_EN | output | AMOLED analog power enable; asserted after CLDO3 stabilizes |
| 18 | TP_I2C_SCL | output | Dedicated CST9220 I2C clock |
| 19 | TP_I2C_SDA | bidirectional | Dedicated CST9220 I2C data |
| 20–23 | TF_D0–D3 | bidirectional | SDMMC 4-bit data |
| 24 | TF_CMD | bidirectional | SDMMC command |
| 25 | TF_CLK | output | SDMMC clock |
| 37–40 | POGO_4–1 | user-defined | General expansion IO; not configured or reserved by the BSP |
| 42 | PA_CTRL | output | Amplifier enable; held low until audio starts |
| 43 | AUDIO_I2C_SDA | bidirectional | AC101S control data |
| 44 | AUDIO_I2C_SCL | output | AC101S control clock |
| 45 | AC101S_SDOUT | input | AC101S to ESP32-S31, MCU I2S DIN/RX |
| 46 | AC101S_SDIN | output | ESP32-S31 to AC101S, MCU I2S DOUT/TX |
| 47 | AC101S_BCLK | output | ESP32-S31 to AC101S, I2S bit clock |
| 48 | AC101S_MCLK | output | ESP32-S31 to AC101S, 12.288 MHz master clock |
| 49 | AC101S_LRCK | output | ESP32-S31 to AC101S, 48 kHz word clock |
| 51 | USB_SEL | output | Safe default is low; path polarity needs device confirmation |
| 61 | BOOT_KEY | input | Boot/user key |

The schematic USB_DP/USB_DM signals use dedicated ESP32-S31 USB pads rather than ordinary GPIO assignments.

### POGO expansion policy

GPIO37–40 have no fixed function or direction and are owned by the end-user application. Typical assignments are a four-wire SPI bus, or UART TX/RX with the remaining pins used as RTS/CTS, chip-select, or plain GPIO. The BSP only publishes the pin numbers: `aether_board_init()` does not set their direction, pulls, peripheral matrix, or create a default SPI/UART instance.

### Display windows and TE policy

CO5300 CASET/RASET require even starts and even width/height. LVGL invalidation now expands regions before rendering; its chunk-row calculation uses the same rounder. Previously unrestricted partial windows violated this requirement, a confirmed software defect consistent with text becoming slanted/corrupt after interaction. Device retesting must establish whether this explains every observed artifact.

Module FPC23 feeds TE to GPIO8. `35h=00` and `44h=0000` select a high vertical-blank signal. The rising-edge ISR only records/notifies; the LVGL task waits before its first transfer per refresh. A missing edge times out after the 35ms budget, then uses cooldown fallback; sleep/wake cancels the old wait. Status/logs report observed TE frequency, edges, successful synchronizations, timeouts and bypasses. Installing an ISR alone does not prove synchronization.

QSPI remains 40MHz. A 480×480 RGB565 pixel payload alone requires at least 23.04ms, so first-transfer TE alignment cannot guarantee tear-free full-screen updates during a 60Hz scan. Neither unverified 80MHz timing nor undocumented 30Hz panel settings are enabled. See [display runtime and device checks](../development/display-runtime.zh_CN.md) for metric definitions and acceptance limits.

## Power policy

| Rail / net | Voltage | Load | Boot policy |
|---|---:|---|---|
| DCDC2 / ESP_3V3 | 3.3 V | ESP32-S31 | Default-on; verified by the driver and cannot be disabled through its API |
| ALDO2 | 3.3 V | QMI8658A, DPS368 | Kept on while running, including initialization/error cleanup and disabled sensor sampling, to maintain the shared I2C bus |
| ALDO4 / CODEC_3V3 | 3.3 V | AC101S | On when audio is enabled |
| CLDO1 / TP_VCC | 3.3 V | CST9220 | On with touch; module absolute maximum is 3.6 V |
| CLDO3 / LCD_VCC | 3.3 V | CO5300AF logic | On with display; module range is 2.5–3.6 V |
| BLDO1 / VDD_SD | 3.3 V | microSD | Off by default and controlled on demand |
| VSYS / LCD_VBAT | battery/system input | AMOLED VBAT | Not LDO-switched; module range is 3.45–5.0 V |
| VSYS / PA_PVDD | battery/system input | Audio amplifier | Directly supplied; GPIO42 keeps the amplifier shut down until audio starts |
| VCC_RTC | separate schematic net | PCF85063 | Not modified by the current TG29 policy |
| DCDC1, DCDC3 | — | unused | Off |
| ALDO1, ALDO3 | — | unused | Off |
| BLDO2–4 | — | unused | Off |
| CLDO2, CLDO4, CPUSLDO | — | unused | Off |

## RTC timed power-on path

| System state | DCDC2 / ESP_3V3 | MCU state | PCF85063 interrupt destination | TG29 interrupt destination |
|---|---|---|---|---|
| Running | On, 3.3 V | Powered | MCU GPIO4 | MCU GPIO5 |
| Ultra-low-power shutdown | Off | MCU and GPIO4/5 unpowered | Hardware-routed to the TG29 wake/interrupt path | MCU-side output is unavailable; TG29 internally receives the RTC wake event |
| After RTC alarm | TG29 restores DCDC2 | MCU cold-boots | Firmware reads and clears the RTC alarm state after boot | GPIO5 resumes as the independent TG29-to-MCU interrupt |

This is not an ESP32-S31 deep-sleep GPIO wake. The power-on chain is `PCF85063 → board routing → TG29 → DCDC2/ESP_3V3 → ESP32-S31 cold boot`. While running, PCF85063 and TG29 use separate GPIO4 and GPIO5 inputs, so their sources are not multiplexed on one MCU pin.

Software shutdown verifies sensor sleep and disables USB output. It then attempts to drive PA_CTRL low, park touch, and put the display to sleep before asserting reset and disabling AVDD_EN. The display VBAT and amplifier PVDD are both tied to VSYS, which USB can continue supplying. TG29 REG27[0] shuts down the regulated outputs, retaining the RTCLDO used by the RTC. ALDO2 stays powered until that command so the sensors cannot clamp the shared system I²C bus. A peripheral preparation error still permits TG29 shutdown. If the TG29 command fails, firmware restores the display and touch.

Verify battery-only and USB-connected shutdown separately. Measure battery current and the DCDC2, ALDO2, ALDO4, CLDO1, CLDO3 and VSYS voltages after shutdown, then confirm cold boots via the power key and RTC alarm. Firmware acknowledgments and register readbacks do not establish physical rail voltages.

## I2C addresses

The three buses use three explicitly selected hardware controllers; no controller is auto-assigned:

| Purpose | ESP32-S31 controller | Pins | Devices |
|---|---|---|---|
| System / low-power bus | `LP_I2C_NUM_0` | SDA GPIO0, SCL GPIO1 | TG29, QMI8658A, DPS368, PCF85063 |
| Audio control bus | `I2C_NUM_0` (HP) | SDA GPIO43, SCL GPIO44 | AC101S |
| Touch control bus | `I2C_NUM_1` (HP) | SDA GPIO19, SCL GPIO18 | CST9220 |

The system bus uses the ESP32-S31 dedicated LP I2C controller, not a third HP I2C controller. The BSP selects `LP_I2C_SCLK_DEFAULT`; all three buses rely on their schematic external pull-ups and leave internal pull-ups disabled.

| Bus | Device | 7-bit address |
|---|---|---:|
| SYS_LPI2C (GPIO0/1) | TG29 | `0x34` |
| SYS_LPI2C (GPIO0/1) | QMI8658A | `0x6B` |
| SYS_LPI2C (GPIO0/1) | DPS368 | `0x77` |
| SYS_LPI2C (GPIO0/1) | PCF85063 | `0x51` |
| AUDIO_I2C (GPIO43/44) | AC101S | `0x33` (DEVID grounded, schematic write byte `0x66`) |
| TP_I2C (SDA GPIO19 / SCL GPIO18) | CST9220 | `0x5A` |

Confirm USB_SEL polarity, AMOLED orientation and color order, and whether touch/audio domains should remain continuously enabled. ALDO2 is retained at 3.3 V for shared-bus sensor I/O; its factory-default power state still needs hardware confirmation.

