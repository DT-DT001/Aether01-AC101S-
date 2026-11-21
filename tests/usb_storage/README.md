# External USB storage regression

Run sh tests/usb_storage/run.sh with GCC, ASan and UBSan. Ten groups exercise
the production singleton/class/lease implementation against lifecycle-aware
MSC and FatFS doubles. A real temporary FILE is read/written while leases hold
the simulated mount. Assertions ensure that detach/OFF immediately close
admission and invalidate tokens, while VFS/device teardown waits for every
file owner to close and release.

Tests include cookie/generation stale copies,32-user exhaustion, unplug during
device initialization, OFF during VFS mount, install/mount errors without
autoformat,100 clean sessions using one worker, and a partial official device
teardown error that must never be retried unsafely. Class polling is cache-only.
The official API is separately compiled by the real S31 firmware build.

These tests do not simulate FAT sectors or prove physical disk compatibility.
The board acceptance matrix includes FAT32 mount/list/open, music/video/NES from
/usb, removal during playback/listing, OFF while a file is open, and reconnect
to a different FAT disk. NTFS, multiple mounted disks and default-disabled USB hubs are outside the
current scope. Shared FatFs GPT/exFAT core tests do not replace physical USB
media acceptance. Application files remain below the signed32-bit VFS limit.

Added cases verify retryable VFS-unregister failure without releasing live
handles, common TF/USB lifecycle-lock contention, retry without duplicate MSC
installation and cancellation while waiting for that lock. Actual underlying
MSC/BDL/generic-VFS error behavior is tested by tests/usb_storage_driver;
these class doubles alone would not catch an upstream unregister swallowing
errors or freeing callback contexts too early.
