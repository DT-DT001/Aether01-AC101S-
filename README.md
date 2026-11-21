# 以太01（Aether01）AC101S 固件

以太01是一款基于 ESP32-S31 的便携触屏设备。这个仓库是 AC101S 音频硬件版的独立固件工程，使用 ESP-IDF 6.1 和 LVGL 9.5。当前固件为 **Demo，可能存在 Bug**。AC101S 版本已通过编译并在实板启动，完整音频体验仍需实板验证。

[硬件设计文件](https://github.com/DT-DT001/Aether01-Hardware-AC101S) · [引脚与电源域](docs/hardware/io-power-map.zh_CN.md) · [English](README.en.md)

## 硬件与功能

| 模块 | 配置 |
| --- | --- |
| 主控与存储 | ESP32-S31、64 MiB Flash、32 MiB PSRAM、TF 卡 |
| 显示与触摸 | 480 × 480 AMOLED、CO5300AF、CST9220 |
| 音频 | AC101S、板载麦克风与扬声器、48 kHz / 16 位 I²S |
| 电源与传感器 | TG29-ESP、QMI8658A、DPS368、PCF85063，按实板装配使用 |

- 音乐播放器支持曲库、切歌、进度控制、五段实时 EQ、原声对比和音乐增益。录音机将 WAV 保存到 TF 卡，视频播放器支持受限格式的全屏播放。[视频格式说明](docs/development/video-formats.zh_CN.md)
- TXT 阅读器支持中文分页、编码选择和书签。NES 模拟器支持 USB 键盘、通用 HID 手柄、键位设置和电池 SRAM 存档。
- USB 支持 TF 卡虚拟 U 盘、USB OTG、标准 HID 以及相应的 USB 存储和音频设备。TF 卡支持 FAT32 和 exFAT。
- 无线功能包括 Wi-Fi、蓝牙耳机、蓝牙键盘、空中飞鼠、OBS 场景快捷键和 ESP-NOW 按键对讲。
- 设备工具包括电量、时钟、气压高度、实时姿态、短程位移估算及反应力游戏。

## 构建

激活支持 `esp32s31` 目标的 ESP-IDF 6.1 环境后，在本仓库目录运行以下命令。组件管理器按 `dependencies.lock` 恢复依赖。

```sh
idf.py --preview set-target esp32s31
idf.py --preview build
```

固件应用镜像为 `build/aether01_ac101s.bin`，完整烧录参数见 `build/flash_args`。应用源码位于 `main/`，板载音频驱动位于 `components/ac101s_audio/`。[主机回归测试说明](tests/README.md)

## 许可证

DarkThunder 的原创固件代码采用 [Apache-2.0](LICENSE)。第三方库和字体适用各自的许可与署名。[NOTICE](NOTICE) · [第三方资料说明](THIRD_PARTY_NOTICES.md)
