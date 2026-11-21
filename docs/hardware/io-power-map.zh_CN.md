# Aether01 IO 与电源域确认表

当前硬件已移除MMC5603；驱动源码保留但排除编译，不再探测地址0x30或显示指南针。下方历史断电兼容性记录仅作为原板资料。

本表根据 Aether01 原理图、TG29 数据手册及 KD021WVOSN009-C003B 屏幕规格整理，屏幕与触摸引脚以用户 2026-11-02 提供的修订图为准。方向以 ESP32-S31 为中心，`输入` 表示信号进入 MCU。

## GPIO 对应关系

| GPIO | 信号/外设 | MCU 方向 | 说明 |
|---:|---|---|---|
| 0 | SYS_LPI2C_SDA | 双向 | ESP32-S31 LP I2C；TG29、QMI8658A、DPS368、PCF85063 共用 |
| 1 | SYS_LPI2C_SCL | 输出 | ESP32-S31 LP I2C 系统总线时钟 |
| 2 | DPS368_INT | 输入 | 气压计中断 |
| 3 | QMI8658A_INT1 | 输入 | IMU INT1 |
| 4 | PCF85063_INT | 输入 | 正常运行时 PCF85063 单独通知 MCU；ESP_3V3 断电后 GPIO4 无效，硬件把该中断切换到 TG29 唤醒路径 |
| 5 | TG29_INT | 输入 | 正常运行时 TG29 单独通知 MCU；ESP_3V3 断电后 MCU/GPIO5 无效 |
| 6 | TF_DET | 输入 | microSD 插卡检测 |
| 7 | TP_INT | 输入 | CST9220 触摸中断，当前配置为低有效 |
| 8 | LCD_TE | 输入 | CO5300AF垂直消隐TE上升沿；每轮LVGL首个flush等待新边沿，超时降级，不逐40行块重复等待 |
| 9 | LCD_QSPI_D3 | 双向 | 屏幕 QSPI 数据 3 |
| 10 | LCD_QSPI_CS | 输出 | 屏幕片选 |
| 11 | LCD_QSPI_D0 | 双向 | 屏幕 QSPI 数据 0 |
| 12 | LCD_QSPI_CLK | 输出 | 屏幕 QSPI 时钟，默认 40 MHz |
| 13 | LCD_QSPI_D1 | 双向 | 屏幕 QSPI 数据 1 |
| 14 | LCD_QSPI_D2 | 双向 | 屏幕 QSPI 数据 2 |
| 15 | LCD_RST | 输出 | 屏幕复位，低有效 |
| 16 | TP_RST | 输出 | CST9220 复位，低有效 |
| 17 | AVDD_EN | 输出 | AMOLED 模组模拟电源使能；启动时在 CLDO3 稳定后拉高 |
| 18 | TP_I2C_SCL | 输出 | CST9220 独立 I2C 时钟 |
| 19 | TP_I2C_SDA | 双向 | CST9220 独立 I2C 数据 |
| 20 | TF_D0 | 双向 | SDMMC D0 |
| 21 | TF_D1 | 双向 | SDMMC D1 |
| 22 | TF_D2 | 双向 | SDMMC D2 |
| 23 | TF_D3 | 双向 | SDMMC D3 |
| 24 | TF_CMD | 双向 | SDMMC CMD |
| 25 | TF_CLK | 输出 | SDMMC CLK |
| 37 | POGO_4 | 用户定义 | 通用扩展 IO；板级程序不配置、不占用 |
| 38 | POGO_3 | 用户定义 | 通用扩展 IO；板级程序不配置、不占用 |
| 39 | POGO_2 | 用户定义 | 通用扩展 IO；板级程序不配置、不占用 |
| 40 | POGO_1 | 用户定义 | 通用扩展 IO；板级程序不配置、不占用 |
| 42 | PA_CTRL | 输出 | 功放使能；上电默认低，只有音频启动后才拉高 |
| 43 | AUDIO_I2C_SDA | 双向 | AC101S 控制 I2C 数据 |
| 44 | AUDIO_I2C_SCL | 输出 | AC101S 控制 I2C 时钟 |
| 45 | AC101S_SDOUT | 输入 | AC101S → ESP32-S31；接 MCU 的 I2S DIN/RX |
| 46 | AC101S_SDIN | 输出 | ESP32-S31 → AC101S；接 MCU 的 I2S DOUT/TX |
| 47 | AC101S_BCLK | 输出 | ESP32-S31 → AC101S；I2S 位时钟 |
| 48 | AC101S_MCLK | 输出 | ESP32-S31 → AC101S；12.288 MHz 主时钟 |
| 49 | AC101S_LRCK | 输出 | ESP32-S31 → AC101S；48 kHz 左右声道时钟 |
| 51 | USB_SEL | 输出 | USB 路径选择，安全初值为低；高/低对应路径需上板确认 |
| 61 | BOOT_KEY | 输入 | 启动/用户按键 |

原理图中的 USB_DP/USB_DM 是 ESP32-S31 专用 USB 引脚，不作为普通 GPIO 分配。

### POGO 扩展接口约束

GPIO37–40 没有固定功能和固定方向，由最终用户的应用自行开发。常见用途是四线 SPI，或 UART TX/RX 并将剩余引脚用于 RTS/CTS、片选或普通 GPIO。BSP 只提供引脚编号，不会在 `aether_board_init()` 中配置方向、上下拉或外设复用，也不会创建默认 SPI/UART 驱动实例。

### 显示窗口与TE策略

CO5300的CASET/RASET要求起点和宽高均按2像素对齐。固件已在LVGL渲染前对脏区向外取整，分块行数也经同一rounder约束；此前任意局部窗口违反该限制，是点击后文字变斜/显示损坏的已确认软件缺陷，修复效果仍需实机核对。

FPC23的TE接GPIO8，`35h=00`、`44h=0000`选择垂直消隐高电平信号。GPIO上升沿ISR仅记录/唤醒，LVGL任务在每轮首块发送前同步；缺信号35ms等待后降级并冷却重试，息屏/唤醒取消旧等待。状态页及日志显示实际TE频率、边沿、同步、超时和旁路，安装ISR成功不等于收到TE。

QSPI保持40MHz。480×480 RGB565仅像素理论传输已需23.04ms，不能据首块TE同步保证60Hz扫描下全屏无撕裂。未启用未经验证的80MHz或面板30Hz设置。时序、统计定义与实机检查见[显示运行说明](../development/display-runtime.zh_CN.md)。

## 电源域策略

| TG29/电源网 | 电压 | 负载 | 启动策略 |
|---|---:|---|---|
| DCDC2 / ESP_3V3 | 3.3 V | ESP32-S31 主电源 | PMIC 默认开启；驱动校验电压和使能，明确禁止关闭 |
| ALDO2 | 3.3 V | QMI8658A、DPS368 | 系统运行时保持开启，初始化/错误清理不关闭；禁用传感器采样也不切断共享 I2C 器件电源 |
| ALDO4 / CODEC_3V3 | 3.3 V | AC101S | 音频功能启用时开启 |
| CLDO1 / TP_VCC | 3.3 V | CST9220 | 触摸功能启用时开启；模组绝对最大值 3.6 V |
| CLDO3 / LCD_VCC | 3.3 V | CO5300AF 逻辑电源 | 显示功能启用时开启；模组允许 2.5–3.6 V |
| BLDO1 / VDD_SD | 3.3 V | microSD | 默认关闭，仅由 `aether_board_set_sd_power()` 按需开启 |
| VSYS / LCD_VBAT | 电池/系统输入 | AMOLED 模组 VBAT | 不受 LDO 开关控制；模组规格 3.45–5.0 V，典型 3.7 V |
| VSYS / PA_PVDD | 电池/系统输入 | 音频功放 | 供电直连；GPIO42 保持功放关断直到音频启动 |
| VCC_RTC | 原理图独立网 | PCF85063 | 不由当前 TG29 输出策略改写 |
| DCDC1、DCDC3 | — | 未使用 | 关闭 |
| ALDO1、ALDO3 | — | 未使用 | 关闭 |
| BLDO2、BLDO3、BLDO4 | — | 未使用 | 关闭 |
| CLDO2、CLDO4、CPUSLDO | — | 未使用 | 关闭 |

TG29 驱动会以完整使能掩码写入寄存器并回读，因此上表中的“关闭”是显式策略，不依赖芯片复位默认值。

## RTC 定时唤醒路径

| 系统状态 | DCDC2 / ESP_3V3 | MCU 状态 | PCF85063 中断去向 | TG29 中断去向 |
|---|---|---|---|---|
| 正常运行 | 开启，3.3 V | 上电运行 | 通知 MCU GPIO4 | 通知 MCU GPIO5 |
| 超低功耗关机 | 关闭 | MCU 完全断电，GPIO4/5 均无效 | 由硬件切换至 TG29 的唤醒/中断通路 | MCU 侧输出无效；TG29 内部接收 RTC 唤醒事件 |
| RTC 闹钟触发后 | TG29 恢复 DCDC2 | MCU 重新上电启动 | 启动后由固件读取并清除 RTC 闹钟状态 | GPIO5 恢复为 TG29 对 MCU 的独立中断 |

因此这里不是 ESP32-S31 的深睡眠 GPIO 唤醒：超低功耗状态下 MCU 已断电，定时开机链路是 `PCF85063 → 板级硬件切换 → TG29 → DCDC2/ESP_3V3 → ESP32-S31 重新启动`。运行态 PCF85063 与 TG29 分别使用 GPIO4、GPIO5，不需要在同一 GPIO 上区分来源。

## I2C 地址

三路总线使用三个独立硬件控制器，不采用自动端口分配：

| 用途 | ESP32-S31 控制器 | 引脚 | 设备 |
|---|---|---|---|
| 系统/低功耗总线 | `LP_I2C_NUM_0` | SDA GPIO0、SCL GPIO1 | TG29、QMI8658A、DPS368、PCF85063 |
| 音频控制总线 | `I2C_NUM_0`（HP） | SDA GPIO43、SCL GPIO44 | AC101S |
| 触摸控制总线 | `I2C_NUM_1`（HP） | SDA GPIO19、SCL GPIO18 | CST9220 |

这里的 LP I2C 是 ESP32-S31 的专用低功耗 I2C 控制器，不是第三路 HP I2C。BSP 使用 `LP_I2C_SCLK_DEFAULT` 初始化系统总线；三个总线均使用原理图上的外部上拉，不启用内部上拉。

| 总线 | 器件 | 7-bit 地址 |
|---|---|---:|
| SYS_LPI2C (GPIO0/1) | TG29 | `0x34` |
| SYS_LPI2C (GPIO0/1) | QMI8658A | `0x6B` |
| SYS_LPI2C (GPIO0/1) | DPS368 | `0x77` |
| SYS_LPI2C (GPIO0/1) | PCF85063 | `0x51` |
| AUDIO_I2C (GPIO43/44) | AC101S | `0x33`（DEVID 接地，原理图标注的 8 位写地址为 `0x66`） |
| TP_I2C (SDA GPIO19 / SCL GPIO18) | CST9220 | `0x5A` |

## 共享系统 I2C 的断电兼容性核查（2026-11-03）

这里的断电指电源实际为 0 V，不等于器件保持供电时的软件休眠。没有掉电 Ioff/高阻保证，不能直接认定器件一定钳位，也不能按掉电安全器件设计。

| 器件 | 本板电源 | 厂家资料结论 |
|---|---|---|
| DPS368 | ALDO2 / SS_VDD，VDD 与 VDDIO 同域 | 英飞凌官方社区确认 VDD=VDDIO=0 时可能影响共享 I2C；同类案例也是总线上拉常供 3.3 V、传感器单独断电。未给出本板 1.4 V 的成因证明。 |
| QMI8658A | ALDO2 / SS_VDD | 官方 Rev.A 的正常工作输入上限为 VDDIO+0.3 V，SDA/SCL 有默认 200 kΩ 内部上拉；未找到 VDDIO=0 的 Ioff/高阻保证。200 kΩ 本身与外部 4.7 kΩ 分压不足以解释 1.4 V。 |
| MMC5603NJ | ALDO2 / SS_VDD | 官方 Rev.B 的 VIO 工作范围上限为 VDD，漏电指标不是 VDD=0 条件；未保证掉电高阻。只有 VDD/GND/SDA/SCL，不能单独维持其 VDDIO。 |
| PCF85063A | VCC_RTC | 未找到 VDD=0 的掉电高阻保证。手册另说明慢速/非单调上电可能导致 POR 失败及 SDA 卡低，正常供电不排除先前上电问题；此前 RTC 电源约 3 V，关闭 ALDO2 不会使它断电。 |
| TG29 | VBAT/VBUS，内部供电 | 厂家手册第16页说明仅电池且未开机时串行通信不可用，未给出全断电的 Ioff/高阻保证。本板 MCU 已由 DCDC2 运行，不应把关闭 ALDO2 等同于 TG29 断电。 |

来源：[英飞凌 DPS368 官方答复](https://community.infineon.com/t5/Pressure-sensors/DPS368-I2C-bus-communication-issue/td-p/795383)、[QST QMI8658A Rev.A](https://www.qstcorp.com/upload/pdf/202301/13-52-25%20QMI8658A%20Datasheet%20Rev%20A.pdf)、[MEMSIC MMC5603NJ Rev.B](https://www.memsic.com/Public/Uploads/uploadfile/files/20220119/MMC5603NJDatasheetRev.B.pdf)、[NXP PCF85063A](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf)，以及项目中的 TG29 厂家手册。

运行策略：ALDO2 保持 3.3 V，传感器使用软件休眠而不是切断共享 I2C 的 I/O 电源。初始化先设置/核验 ALDO2 再开启，通用关闭和错误清理均保留其已有使能状态。若出厂默认 ALDO2 关闭就已钳位总线，程序无法通过这条失效总线打开它，需要确认默认上电配置或隔离传感器支路。

硬件验证：测 ALDO2/SS_VDD、两只上拉电阻电源端及 SDA/SCL 的实际电压；断电后逐个隔离传感器的 SDA/SCL 支路，再上电观察总线及 TG29 应答。优先检查具有同类厂家案例的 DPS368。单纯断开传感器电源不能排除掉电钳位，必须隔离其信号支路才能锁定来源。RTC 的9时钟恢复只适用于 SCL 可正常拉高的情况，不能替代两线电平检查。

## 请优先确认的硬件项

1. GPIO51 `USB_SEL` 的有效电平和默认目标路径。
2. 480×480 AMOLED的安装方向、RGB/BGR顺序，以及GPIO8实际TE电平/周期与首个QSPI传输的相位；TE已接入软件同步，仍需实测其有效性及可见撕裂。
3. 触摸 CLDO1、音频 ALDO4 是否希望常开；当前策略随对应 Kconfig 功能开关启用。传感器 ALDO2 保持 3.3 V，防止共享系统 I2C 的掉电器件钳位；出厂默认供电状态仍需硬件确认。

