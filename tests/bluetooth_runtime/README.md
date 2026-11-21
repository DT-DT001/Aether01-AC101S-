# Bluetooth production regression

With the existing compiler and ESP-IDF 6.1 source tree:

```sh
sh tests/bluetooth_runtime/run.sh
```

The Windows/WSL default locates the already installed tree at `/mnt/c/APP/ESP/.espressif/v6.1/esp-idf`. Set `IDF_PATH` when it is elsewhere. Nothing is installed or flashed. Build products default to `build/host_bluetooth_runtime`; set `AETHER_TEST_OUTPUT` to an absolute private directory when validating concurrently.

Each audio cycle additionally blocks a new sink start behind the previous producer, sends STOP after the start has captured its generation, then releases the producer. The production sink must reject that cancelled generation without reopening its media request or FIFO. START activation and STOP cancellation publish the request flag under the same lock, preventing a late `media_requested=true` after STOP.

Cold lifecycle tests also execute 100 create/off/delete cycles without creating a manager task or controller, plus task-allocation failure, deletion after failed creation and successful retry. Only the first explicit radio-on allocates the 6KiB internal task stack. Existing controller/host/Flash-sensitive code is not moved onto an external stack.

First controller initialization checks internal DMA-capable 8-bit heap free size
and largest block. The diagnostic starting budget is 64KiB total / 24KiB largest;
these are conservative admission thresholds, not measured SDK minimums or a
guarantee against every OOM. Tests reject low-total and fragmented heaps without
calling the SDK or busy retrying, recover on explicit retry at the exact boundary,
and allow retained-controller re-enable without repeating first-init admission.
Production logs record heap values before and after actual SDK initialization.

Connection regressions hold the SDK's connection request pending and deliver
actual GAP/A2DP callback parameter types independently of API return. They cover
HCI ACL status 0x09, authentication rejection, SDK failed-OPEN reported as NORMAL,
selected/unselected PIN/SSP responses, raw ACL disconnect reasons, fast timeout
completion, rejected disconnect and retry, and an accepted cancellation with no
completion that closes profiles/host before explicit re-enable. The tests assert
non-connectable/non-discoverable source mode and no automatic connection retry.
Cancellation ownership also covers a rejected pending cancel followed by late
CONNECTED/configuration callbacks: new connect, scan and audio/media start remain
blocked. Direct OFF while a connect is pending covers both rejected disconnect
and accepted-without-completion, retaining the FIFO/host until profile deinit
completes and proving cancellation waits are not restarted on cleanup retries.

Both suites use ASan/UBSan. The PCM suite checks 200,000 source frames, irregular/un-aligned caller buffers, exact 48k→44.1k rational phase across chunk boundaries, stereo sample values and callback errors. The production-manager suite includes the actual component source and uses the **real public GAP/A2DP/AVRCP headers** from the existing IDF tree. Pthread tasks and deterministic SDK radio functions replace actual hardware. It exercises 20 radio/profile/host on/off cycles with one retained controller initialization, inquiry result parsing/deduplication, connection and negotiated 44.1/48k mono/stereo, production FIFO conversion/downmix/zero-fill, bounded full-buffer writes, interruption by stop, AVRCP key release filtering/queue overflow, independent peer volume, all 16 startup API failure positions, FIFO OOM, delayed profile teardown, late PCM callbacks during shutdown, pending-connect cancellation and radio-off failure followed by immediate re-enable. Terminal controller deletion prevents unsupported S31 reinitialization.

These are production control/PCM tests with mocked RF and stack scheduling, not real headphone pairing, actual SBC encoding or on-device playback. Real S31 controller memory behavior, SMP interleavings, Bluedroid callback timing, Wi-Fi coexistence, latency/underruns and long-run headset compatibility still need device tests. No ESP32-S3 performance figure is used as an S31 result.
