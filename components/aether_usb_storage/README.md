# External USB FAT storage

This Host class uses the project-owned pinned espressif/usb_host_msc1.3.0 on the existing
ESP-IDF6.1. Its class hooks join aether_usb_host's one USB library, PHY, root port
and event pump; it owns only the MSC client/driver and its filesystem worker.
It does not alter the Type-C policy, PMIC power controls or USB routing.

The first selected disk mounts at /usb through the official MSC FatFS VFS.
The shared owned FatFs configuration supports FAT12/16/32, exFAT and supported
GPT Basic Data partitions. Applications still use signed32-bit VFS offsets;
files must be smaller than2GiB. NTFS is intentionally not integrated. Core
filesystem fixtures establish this support; physical exFAT USB media still
requires acceptance. No failed mount is automatically formatted. Additional disks are
counted as ignored, and an already ignored disk must be reconnected after the
selected disk is removed. Hubs are disabled by default. Standard composite
devices do not require a hub. The native TF-card virtual U-disk feature remains
a separate USB Device mode and cannot share the Host reservation.

Acquire a zero-initialized lease before any fopen/opendir or filesystem call.
Hold it until all owned files/directories are closed and operations finish.
The bounded32-entry lease table uses generation and cookie checks; copied stale
tokens cannot release another user's slot. Detach and OFF revoke admission in
the callback immediately. is_available becomes false so callers can stop work,
close files and release promptly. VFS/device teardown waits for all leases.
A physical removal can still cause an in-flight I/O error; preserving a live
mount until its users close prevents use-after-free, not lost-media I/O.

Class request_stop is nonblocking and idempotent. poll_stopped reads cached
state only. A persistent worker performs potentially blocking filesystem/device
cleanup and the MSC driver's task join; Host does not uninstall its library
until this completes. The worker is reused across sessions. Mount/unmount use the same bounded
registration mutex as TF lifecycle operations; regular file I/O does not take
that global lock. Mount lock contention retries the existing installed device
rather than installing it twice. Unmount lock contention or a checked VFS
unregister failure retains ownership and retries with20–200ms backoff. While
leases drain, the worker sleeps until a release or bus event notifies it. The default
singleton performs no USB, task, PHY or power operations until class_start.

The owned MSC VFS adapter allocates its wrapper/path before mount publication
and propagates unregister errors while retaining its wrapper and the device
registration guard. The owned FatFs BDL/VFS paths retain callbacks/diskio/contexts
until unregister succeeds; detached FatFs state can be retried idempotently.
Provenance and the upstream Apache license are preserved under vendor/.

The official MSC1.3.0 device teardown is not transactional on every failure:
it can remove/free part of a device before a later interface-release error.
This wrapper records that failure and retains class/core ownership rather than
retry a partly destroyed object or free a pending transfer on a timer. VBUS-off
is still attempted by the outer Host controller. A device reset is required
after this exceptional unrecoverable cleanup failure.

tests/usb_storage runs production code with API-shaped USB/FatFS doubles under
ASan/UBSan. Ten groups cover admission, actual FILE read/write while leased,
immediate detach invalidation, delayed unmount, generation/cookie protection,
slot exhaustion, removal during initialization, mount failure without format,
100 sessions and nontransactional teardown failure. Real external FAT-disk
mount/read/write, unplug during media playback and OFF acceptance still require
board tests; the doubles do not establish USB electrical compatibility.

The four-group tests/usb_storage_driver suite separately executes the actual
owned generic-VFS/BDL/MSC VFS functions together, injecting detach/unregister
errors and allocation failures, and checking100 complete register/unregister
cycles under ASan/UBSan. It does not access a real USB device or disk.
