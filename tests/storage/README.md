# Storage lifecycle regression

Run `sh tests/storage/run.sh` with an existing Linux/WSL C compiler. The test includes production `aether_storage.c` and injects mount, DET and BLDO1 failures. It checks concurrent lease drain, rejection of new leases during removal, rollback if removal/stop occurs during a blocking mount, and failed power-off retry across 32-bit tick rollover.

The scheduler and physical SDMMC/FatFS hardware are stubbed. Actual card-bounce timing, SMP scheduling, card-controller timeouts and FAT corruption recovery still require device testing.


USB/shutdown extensions run under ASan/UBSan too: local leases drain before a fresh raw SD host is created; aligned4KiB transfers and partial-sector read/modify/write preserve surrounding bytes; bounds/read/write faults propagate; hot removal/reinsert stays gated until explicit exit. A cancelled handover cannot power off a still-mounted filesystem with live users. The power-off barrier waits actual unmount and BLDO1-off retry and restores local access on explicit cancellation.


USB throughput candidate (not device-qualified): normal FatFS mounts retain
20MHz. Raw USB export attempts SD-card initialization at40/32/25/20MHz with
3.3V signalling, fully destroying the old host and powering off for20ms before each
lower-clock attempt. Only card initialization failure advances the ladder;
power/host/slot failures and cleanup failures keep ownership gated. Export
reports the SDK's real configured clock, not the requested maximum. A successful
initialization does not prove long-term signal integrity at that frequency.

DMA-capable, host-aligned USB buffers transfer directly in up to16KiB/32-sector
commands, without enlarging the persistent4KiB internal buffer. Other buffers
and partial-sector accesses retain the old bounce/read-modify-write path.
Tests check8/16KiB single-command and32KiB two-command data integrity, alignment
and capability rejection, all clock fallbacks, teardown/power-off faults,
unknown/rounded real-clock reporting, and no retry/reclock on failed writes or
physical removal. The installed IDF's actual read and write wrappers are also
compiled under ASan/UBSan:4160 unaligned reads and4160 unaligned writes, plus
aligned8/16KiB direct requests and injected faults, need no temporary DMA heap
allocations. These tests establish code paths and transaction counts, not USB
or TF throughput; hardware CRC, speed and sustained integrity remain untested.


The final TinyUSB MSC buffer is16KiB (12KiB more persistent internal RAM than
the previous4KiB); this is separate from the unchanged4KiB SD bounce buffer.
The user initially requested no flashing, then authorized the 16KiB update.
COM36 flash hashes and bounded boot checks pass; this does not qualify new
USB export or transfer speed. Passing host evidence is recorded in the
[storage-speed log](../../tmp/usb-storage-speed-host.log); the read-only
[Windows hub record](../../tmp/usb-link-speed.json) establishes an actual
HS480Mb/s link only, not a new TF throughput result. The user-reported
approximately1.2MB/s writes have not yet been re-benchmarked with this candidate.


The final16KiB candidate [IDF6.1 build passes](../../tmp/usb-speed16-final-build.log).
The fixed [manifest](../../build/usb-speed16-candidate-manifest.json) records
`build/releases/usb-speed16-candidate/`, generated-config/ELF confirmation of
the16,384-byte MSC buffer and a net12,288-byte internal-RAM increase with IRAM
unchanged. The user initially requested no flashing and later authorized it;
COM36 flash/boot checks now pass, as recorded in the local
[deployment record](../../build/usb-speed16-com36-manifest.json). These checks
do not establish completed USB export, throughput, sustained integrity or
wireless memory headroom. `build/` and `tmp/` evidence remains local and is
excluded from the source repository.


A debounced physical removal now invalidates the current local mount until a
fresh mount succeeds. Reinsert during USB/shutdown lease drain cannot revive
old FILE users when the transition is cancelled: admission and availability
stay closed, old users close, and normal unmount/power-off/remount must complete.
Two deterministic regressions cover USB and shutdown cancellation after card
replacement; cancellation without removal still preserves a valid live mount.

aether_storage_generation is cache-only: zero before first mount, nonzero after
mount, changed once on physical invalidation and on every successful new mount.
Repeated absent samples/reinsert/cancellation cannot restore the old epoch.
Rollover skips zero. UI caches can reject entries from a replaced TF card even
if removal and remount both occur between two UI timer ticks.


32GB/FAT32 recognition triage (2026-11-06): the observed SDK mount result13 is
FR_NO_FILESYSTEM, after card initialization succeeded. The firmware has no
32GB capacity ceiling; byte capacity is computed in uint64_t. FAT32 BPB,
partition layout and transport faults must be distinguished before attributing
the problem to capacity. The original installed IDF6.1 FatFs has FF_FS_EXFAT
and FF_LBA64 set to0: a FAT32 volume inside GPT can therefore fail even when a
desktop reports "FAT32". The project-local `components/fatfs` override enables
the official GPT/exFAT paths with a checked32-bit driver bridge; its actual
FatFs layout/read/write tests are documented in `tests/fatfs_layout/README.md`.
No automatic card format is enabled.

After a generic mount failure, the worker performs one bounded read-only probe
per insertion or explicit retry, using a fresh20MHz raw descriptor and the
reserved DMA buffer. It reads only LBA0 and up to four MBR partition boot sectors;
metadata includes physical capacity, layout, FAT type, logical sector/cluster
sizes, reserved/FAT sectors and cluster count. GPT protective layouts,
unsupported filesystems and invalid BPB geometry stop automatic reinitialization.
The probe never writes sectors, reads file content or borrows a freed FAT card.
Failed raw cleanup retains its descriptor and Host until checked worker teardown.

Other repeated mount failures retry after2 and4 seconds, then stop after the
third failure on that insertion. This removes the old endless2-second SDMMC
power/mount/log loop that can contend with media and LVGL. The nonblocking
request_retry API and debounced removal/reinsert reset that budget; a valid live
mount is not interrupted. Cached diagnostics are safe to read from the GUI.
An actual USB export exit resets the old format diagnosis before a local retry,
allowing a card deliberately repaired on the host to be recognized again.

Restoring local ownership after USB/shutdown does not require a healthy volume:
a failed local mount remains ERROR with its original error, but once every
lease/card/host/power cleanup is complete it cannot permanently trap USB exit or
aborted shutdown. Files remain unavailable until a future mount actually succeeds.

The runner now covers32 PASS groups: the earlier lifecycle and installed-IDF
DMA paths, actual storage worker retry/suspension and reset behavior,32GiB
FAT32 superfloppy/MBR metadata, legacy FAT12/16 recognition, GPT/exFAT/NTFS,
invalid sector/cluster/FAT/root/version geometry, out-of-range partitions,
read faults, read-only cleanup retention and failed filesystem restoration.
Two additional fault regressions cover a probe's retained raw host while
shutdown is requested: the raw mutex is honored, deinit is retried without
parking pins/cutting BLDO1 or falsely completing shutdown, and a recovered
deinit permits normal checked power-off. Cancelling shutdown preserves that
ownership and lets the existing worker cleanup path recover before local
access is restored. The caller's existing timeout remains the shutdown budget.
All fixtures are host-memory data and never access the physical card. The
device's read-only boot log separately identified the actual30436MiB card as
GPT, consistent with the original FatFs configuration's mount error13.
