Runs the current production functions extracted from `components/fatfs/vfs/vfs_fat.c`
against the real FatFs header and fault-observable FatFs calls. The host uses a
signed 32-bit `off_t`/`ssize_t` and stat size, matching the ESP32-S31 toolchain.
This does not claim the host libc's 64-bit offsets validate the target ABI.

`sh tests/fatfs_vfs_bounds/run.sh` checks oversized opens and descriptor cleanup,
SEEK_SET/CUR/END overflow and negative positions (including INT32_MIN), read and
write return-size bounds, append and positional writes before any mutating call,
EOF positional reads, zero writes, direct and cached stat failures, safe small
file transfers, copy refusal before creating a truncated destination, and
truncate guards. ASan/UBSan and lock-balance checks are enabled.
