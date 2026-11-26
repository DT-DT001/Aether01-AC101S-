# AC101S 固件升级 / Firmware upgrade

适用于搭载 ESP32-S31、AC101S 音频芯片和 64 MiB Flash 的以太01。工具和镜像请使用同一个 Release 的附件，并先核对 `SHA256SUMS.txt`。

For the ESP32-S31 AC101S edition with 64 MiB flash, use files from one release and verify them against `SHA256SUMS.txt`. The package includes the application image, partition table, full image, and a read-only layout checker.

## 降噪固件与模型升级 / Noise-reduction firmware and model updates

检查 Release 的 `SHA256SUMS.txt`，确认应用、分区表、模型和检查器都来自同一版本。当前 AI 分区表保留 `0x10000` 的 8 MiB 应用区，并从内部 `storage` 尾部划出模型区。它保留 NVS、阅读记录、扩展拼音词库和已有壁纸的地址。

Verify the release's `SHA256SUMS.txt` and use the application, partition table, model and checker from that release. The AI layout keeps the 8 MiB application partition at `0x10000` and allocates model space at the end of internal `storage`. NVS, reader data, the extended Pinyin dictionary and existing wallpaper remain at their original addresses.

| 分区或镜像 / Partition or image | 起始地址 / Start | 大小或内容 / Size or contents |
| --- | --- | --- |
| `factory` | `0x10000` | 8 MiB，应用镜像 / 8 MiB, application image |
| `model` | `0x3E00000` | 512 KiB，ESP-SR NSNet3（`srmodels/srmodels.bin`） |
| `wallpaper` | `0x3F00000` | 1 MiB，壁纸 / 1 MiB, wallpaper |

对于已有降噪模型的设备，可以只更新 `0x10000` 的应用。需要写入降噪模型时，以实机分区表中的 `model` 地址为准。已部署的 `0x3E20000` 变体采用 384 KiB 的模型区。

Devices with the denoise model already installed can be updated by writing only the application at `0x10000`. Use the installed table's `model` address when writing model data. The deployed `0x3E20000` variant uses a 384 KiB model partition.

1. 先确认板型和串口。读取并保存原分区表，再备份整片 64 MiB Flash。整片备份包含现有应用、NVS、阅读记录、内部存储和壁纸。升级前确认备份可读并记录其 SHA-256。以下命令用于读取和检查。

   Confirm the board and serial port, then save the installed partition table and back up the entire 64 MiB flash. This preserves the current application, NVS, reader data, internal storage and wallpaper. Check that the backup is readable and record its SHA-256 before updating. The commands below read and check data.

   ```sh
   python -m esptool --chip esp32s31 --port COMx read-flash 0x8000 0xc00 installed-table.bin
   python -m esptool --chip esp32s31 --port COMx read-flash --flash-size 64MB 0x0 0x4000000 installed-full.bin
   python check-ac101s-layout.py installed-table.bin
   ```

2. 显示 `AI_READY` 时，设备已使用 AI 分区表。显示 `WALLPAPER_READY` 或 `LEGACY` 时，用下方对应的命令读取拟分配的完整区域，以及内部 `storage` 的首个 4 KiB 扇区。检查器核对原分区表的 SHA-256，两个区域的长度和擦除状态均通过后才输出 `AI_MIGRATION_READY`。

   `AI_READY` means the AI table is already installed. For `WALLPAPER_READY` or `LEGACY`, use the matching read command below and read the first 4 KiB sector of internal `storage`. The checker matches the installed table's SHA-256 and reports `AI_MIGRATION_READY` only when both reads have the expected length and contain erased bytes throughout.

   ```sh
   # WALLPAPER_READY: read 1 MiB before the existing wallpaper partition
   python -m esptool --chip esp32s31 --port COMx read-flash --flash-size 64MB 0x3E00000 0x100000 installed-ai-region.bin

   # LEGACY: read 2 MiB for the model and wallpaper partitions
   python -m esptool --chip esp32s31 --port COMx read-flash --flash-size 64MB 0x3E00000 0x200000 installed-ai-region.bin

   # Run after the matching region read above
   python -m esptool --chip esp32s31 --port COMx read-flash --flash-size 64MB 0x850000 0x1000 installed-storage-head.bin
   python check-ac101s-layout.py installed-table.bin --ai-region installed-ai-region.bin --storage-head installed-storage-head.bin
   ```

3. `AI_READY` 使用对应发布中的应用与模型镜像。只有 `AI_MIGRATION_READY` 才适用保留原数据地址的分区升级。分区表地址为 `0x8000`，应用和模型按上表定位。无法识别原表、检测到内部 FAT 数据或预留区已有数据时，应保留备份，单独迁移这些数据后再复查。检查器只读，任何刷写均需单独执行。

   For `AI_READY`, use the matching application and model images. `AI_MIGRATION_READY` is the result required for an in-place partition-table migration. The table starts at `0x8000`; the application and model offsets are listed above. With an unknown table, internal FAT data or other occupied regions, keep the backup and migrate the data separately before rechecking. The checker is read-only, and flashing is a separate operation.

## 壁纸版固件升级并保留设置 / In-place upgrade for wallpaper-only firmware

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
