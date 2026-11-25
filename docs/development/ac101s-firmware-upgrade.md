# AC101S 固件升级 / Firmware upgrade

适用于搭载 ESP32-S31、AC101S 音频芯片和 64 MiB Flash 的以太01。工具和镜像请使用同一个 Release 的附件，并先核对 `SHA256SUMS.txt`。

For the ESP32-S31 AC101S edition with 64 MiB flash, use files from one release and verify them against `SHA256SUMS.txt`. The package includes the application image, partition table, full image, and a read-only layout checker.

## 已有固件并保留设置 / In-place upgrade

1. 确认设备型号与串口。用 esptool 读取并保存现有分区表。

   Confirm the board and serial port, then save its installed partition table.

   ```sh
   python -m esptool --chip esp32s31 --port COMx read-flash 0x8000 0xc00 installed-table.bin
   python check-ac101s-layout.py installed-table.bin
   ```

2. 检查器显示 `WALLPAPER_READY` 时，将本版的 `aether01_ac101s.bin` 写入 `0x10000`。显示 `LEGACY` 时，先备份并检查 Flash 最后的 1 MiB。检查器只在该范围全部为空时输出 `WALLPAPER_MIGRATION_READY`。其他布局或已有数据应保留并停止分区升级。

   On `WALLPAPER_READY`, write the application at `0x10000`. On `LEGACY`, save and check the entire final 1 MiB first. Leave unfamiliar layouts and existing tail data intact.

   ```sh
   python -m esptool --chip esp32s31 --port COMx read-flash --flash-size 64MB 0x3f00000 0x100000 installed-tail.bin
   python check-ac101s-layout.py installed-table.bin installed-tail.bin
   ```

3. 按检查结果选择一条烧录命令。分区升级保持 NVS、阅读记录和扩展词库的原地址。烧录期间保持供电稳定。

   Choose the corresponding write command. The layout change preserves the original addresses of NVS, reader data, and the optional vocabulary extension. Maintain stable power throughout the write.

   ```sh
   # WALLPAPER_READY
   python -m esptool --chip esp32s31 --port COMx write-flash --flash-size 64MB 0x10000 aether01_ac101s.bin

   # WALLPAPER_MIGRATION_READY
   python -m esptool --chip esp32s31 --port COMx write-flash --flash-size 64MB 0x8000 partition_table/partition-table.bin 0x10000 aether01_ac101s.bin
   ```

保留旧分区表也可只升级应用，主题配色仍可用。TF 壁纸导入需要专用分区。

An application-only upgrade can keep the legacy table. The colour palettes remain available, and TF wallpaper import requires the dedicated partition.

## 空白板和完整重装 / Clean install

完整镜像 `Aether01-AC101S-full.bin` 从 `0x0` 烧录，会覆盖原系统设置区。已有设备在重装前应先备份设置数据。

The full image is written at `0x0` and overwrites the original system settings area. Back up existing device settings before a clean install.
