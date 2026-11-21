# USB MSC ownership regression

Run `sh tests/usb_msc/run.sh` using the existing WSL compiler. The suite compiles the production control implementation and callbacks under ASan/UBSan with a mocked USB transport and storage endpoint.

It checks lazy OFF initialization, explicit entry, full/high-speed descriptor selection, explicit GPIO51-high MSC routing only before driver installation and LOW restoration only after teardown, synchronous read/write completion,64-bit LBA/offset and error sense, cache synchronization, host prevent-removal/eject, USB-reset versus VBUS removal, startup rollback, retained ownership after uninstall failure, in-flight-write timeout/drain and100 card-removal/export/exit cycles. `tests/storage/run.sh` separately executes the production lease/raw SD handover, partial-sector read/modify/write, DMA alignment, bounds/fault cleanup, hot removal/reinsert, power-off retry and shutdown cancellation barrier.

These host checks do not establish USB enumeration, throughput, DWC DMA/cache correctness or host filesystem behavior. A separate real-board test has already established Windows USBSTOR enumeration with GPIO51 HIGH, drive E: and capacity 7,939,817,472 bytes. During that test Windows removed the drive and disk node but left the USB parent present; firmware did not observe a usable eject notification and the earlier strict exit remained blocked. The new confirmed-exit path has not yet passed on-device acceptance. Confirm the final firmware excludes the upstream default `tinyusb_msc.c.obj` and separately qualify real host read/write hashes, final safe exit, physical cable/card removal, shutdown and COM port restoration.

MAC coverage models ESP32-S31's eight-byte EUI-64 default API so ASan detects using it with a six-byte buffer. The production path uses `esp_read_mac(..., ESP_MAC_EFUSE_FACTORY)` to request exactly six bytes; both its descriptor serial and the read-failure fallback are checked. An isolated pre-fix fixture reproduced the original two-byte stack overrun under ASan. Evidence is in `tmp/usb-msc-legacy-mac-asan.log`; the complete current passing suite is recorded in `tmp/usb-msc-host.log`.

Confirmed exit is tested independently of automatic host eject: `aether_usb_msc_exit_confirmed()` may be called only following explicit user confirmation that the computer has safely removed the disk. It bypasses the ownership guard for one invocation, still drains in-flight writes and synchronizes/tears down USB, retains ownership after timeout or teardown failures, and does not grant permission to later USB sessions or fabricate `host_ejected`. Normal exit and startup rollback remain strict. PREVENT/ALLOW tests call the current TinyUSB `tud_msc_prevent_allow_medium_removal_cb()` directly, matching the library's built-in dispatch. STOP without LOEJ, ALLOW, suspend and USB resets never substitute for safe removal.

Negotiated-rate coverage invokes the production attach callback for HIGH, FULL, INVALID, AUTO and LOW stack results. It checks cached 480/12/unknown Mbps, no stack queries from get_status or detach, lock-free stack querying, and clearing on exit, retry failure and re-entry. The value is the negotiated link rate, not measured storage throughput.

The USB status snapshot also copies the raw SD actual clock and high-speed fallback flag from the storage snapshot; host coverage changes those cached values without introducing a USB-stack or SD-command query.


The current speed release uses a final16KiB MSC transfer buffer
instead of4KiB, adding12KiB of persistent internal RAM. The SD fallback bounce
buffer stays4KiB. The [MSC/link suite](../../tmp/usb-speed-link-host.log) also
checks cached raw-card actual-clock/fallback snapshots and the existing
synchronous write, safe-eject, startup rollback and100 lifecycle cases.
[Storage tests](../storage/README.md) cover16KiB direct SD transactions and
initialization-only frequency fallback; [IO tests](../../tmp/usb-speed-io-host.log)
and the [real-LVGL UI suite](../../tmp/usb-speed-ui-host.log) retain ownership
and safe-exit behavior. The user initially requested no flashing and later
authorized it; COM36 flash/boot checks pass, but the new captured tail only
shows raw USB initialization starting, not completed export. A pre-update read-only
[Windows hub query](../../tmp/usb-link-speed.json) confirms the existing
303A:40A1 connection is HS480Mb/s, but neither that observation nor these host
tests measures candidate TF throughput or sustained hardware integrity.


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
