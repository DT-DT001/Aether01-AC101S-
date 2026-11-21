# Project-owned IDF 6.1 FatFs compatibility override

This is the FatFs component from the existing ESP-IDF 6.1 installation at
commit `fff9895c82d744c7237be8847347bdd1b07c6643`. It keeps the same component name
so IDF resolves all VFS, TF, raw-Flash, wear-leveling, BDL and USB MSC consumers
against one local `ff.h`/`ffconf.h` ABI. No SDK installation is modified.
`PROVENANCE.json` records the upstream and local hashes; original copyright and
license notices are retained. No NTFS implementation or GPL dependency is added.

The user's 30436 MiB TF card has a GPT partition table. The default SDK
`FF_LBA64=0` omits the official GPT scan and reports `FR_NO_FILESYSTEM`, even
when the partition itself is FAT32. Enabling that scanner also requires the
official exFAT path, long filenames and a complete component rebuild. The core
FAT parser is unchanged. FAT32/exFAT use the same read/write VFS.

Local changes:

- Enable official GPT/LBA64 and exFAT support; normalize the disabled label
  Kconfig option to a valid C value.
- Keep the existing 32-bit SD/USB disk callback ABI. The central bridge checks
  entire sector ranges, widens `GET_SECTOR_COUNT` safely and converts validated
  `CTRL_TRIM` pairs. This does not enable physical disks beyond those drivers'
  32-bit sector/count limits.
- Use project-owned filesystem image helpers, keeping clean-checkout builds
  independent of this workstation's SDK source paths.
- Enforce the platform's signed VFS file-offset limit before FatFs operations.
  ESP32-S31 currently has 32-bit `off_t`/`stat.st_size`: application files must
  fit within 2147483647 bytes. Unrepresentable files, transfers and seeks fail
  with `EOVERFLOW`; exFAT does not imply unlimited application file sizes.
- Serialize TF and USB whole mount/unmount transactions with a bounded shared
  lifecycle mutex. Normal reads/writes retain their independent volume locks.
- Retain VFS/BDL/SD ownership on failed unregister; clear released SD drive
  bindings so a reinserted card cannot alias a USB drive. Failed nontransactional
  host/device teardown retains ownership and requires reset; it is not repeated.

TF and external-USB mounting explicitly disable automatic formatting. Failed
mount diagnostics are read-only; the application bounds retries and offers a
manual retry. Do not resolve an unsupported layout by formatting user media.
The official GPT scanner expects a valid protective MBR/header and a supported
Microsoft Basic Data partition; arbitrary partition types are not promised.

Validation uses the production core and central bridge in
`tests/fatfs_layout/run.sh`: original sparse 32 GiB GPT/MBR FAT32 fixtures,
Chinese filenames, corruption rejection, checked disk ABI and RAM-only
FAT32/exFAT create/write/append/remount/read/delete cycles. VFS file-size and
offset guards are checked separately by `tests/fatfs_vfs_bounds/run.sh`.
`tests/fs_lifecycle/run.sh` covers raced initialization, concurrent registration
and bounded admission. `tests/usb_storage_driver/run.sh` exercises the actual
MSC/BDL/VFS/SD cleanup functions with injected teardown and allocation failures.
Physical-card integrity, transfer performance and long-duration playback still
require device acceptance; host fixtures never access the user's card.

TF and external USB lifecycle operations share a project-owned registration
mutex (separate from ordinary per-volume file I/O locks). Checked unregister
failures retain callback contexts and diskio/card ownership for retry. SD drive
bindings are cleared when released so a later card allocation cannot alias an
old drive now occupied by USB. A nontransactional native host-deinit failure
is latched without retrying a partially destroyed controller; upper layers
retain ownership until reset or final bounded system shutdown. The inherited
compound case of a failed mount whose own VFS rollback also fails still needs
separate fault-policy work and is not claimed fully transactional.
