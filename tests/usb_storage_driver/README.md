# Real MSC / BDL / VFS teardown faults

Run `sh tests/usb_storage_driver/run.sh` with the existing WSL/compiler.
The extraction step reads the current production functions directly from the
owned MSC1.3.0 and FatFs VFS sources. The test chains the actual generic VFS
unregister, BDL unmount and MSC register/unregister bodies; only hardware and
OS registration primitives are replaced. ASan/UBSan detect stale embedded-FATFS
access, double free, leaked wrappers and file caches.

The fixture injects repeated `esp_vfs_unregister` errors, FatFs detach failure,
wrapper/path OOM and mount failure. A failed unregister must preserve the live
callback context, BDL/diskio registration, MSC wrapper and device registration
guard; retry succeeds once and reclaims every allocation. It also runs100
complete registration/teardown cycles. Read/write operations and actual USB
signaling are not simulated, and no physical/user disk is accessed.

The SD harness also extracts the actual SD unmount/context and disk-binding
functions. It verifies retries after VFS/FatFs errors, stale card addresses
whose old drive number has been reused by USB, multi-partition path lookup,
actual failed-mount rollback binding cleanup, cleanup-owner allocation before
mount publication, and a failed native host
deinit. That last error is latched: a partially destroyed controller is never
blindly deinitialized twice, and its card/drive/context remain reserved until
reset. The host harness explicitly simulates reset only to reclaim its test
allocations. These eleven groups do not qualify the inherited compound failure
case of a failed mount whose own VFS rollback also fails.

The SD harness also extracts the actual SD unmount/context and disk-binding
functions. It verifies retries after VFS/FatFs errors, stale card addresses
whose old drive number has been reused by USB, multi-partition path lookup,
actual failed-mount rollback binding cleanup, cleanup-owner allocation before
mount publication, and a failed native host
deinit. That last error is latched: a partially destroyed controller is never
blindly deinitialized twice, and its card/drive/context remain reserved until
reset. The host harness explicitly simulates reset only to reclaim its test
allocations. These eleven groups do not qualify the inherited compound failure
case of a failed mount whose own VFS rollback also fails.
