# Host regressions

Run the host suites with an existing Linux/WSL C compiler:

```sh
sh tests/run_host.sh
```

On the development Windows machine:

```powershell
wsl -d Ubuntu-22.04 -- sh /mnt/c/UserFiles/Workspace/Code/Aether01/tests/run_host.sh
```

The actual LVGL pointer suite runs separately using the native objects produced by `python3 tests/reader_ui/build_run.py`:

```sh
python3 tests/touch_lvgl/build_run.py
python3 tests/nes_controls_ui/build_run.py
```

CO5300 alignment/TE tests also have an independent entry; the second command runs only their real-LVGL portion using the same cached objects:

```sh
sh tests/display_te/run.sh
python3 tests/display_te/run_lvgl.py
```

The default runner also covers the project LVGL9 PSRAM allocator through the
real public memory wrappers (`lvgl_memory`), including realloc failure and the
zero sentinel. `h264_memory` tests the decoder-scoped capability policy,
hash-locked archive generation and a negative-control late-archive link case.
Neither suite replaces native FPS, radio memory admission or physical UI tests.

No dependencies are installed. Build products, generated media and test saves use `build/host_*` or automatically cleaned temporary directories. Tests compile production source; stubs replace the SDK, hardware and OS interfaces needed for deterministic fault injection.

| Suite | Coverage |
|---|---|
| frames | Copy ownership, latest frame, front protection, contention, five allocation rollback points, mutable-image cache invalidation and 10,000 handover/metric cycles |
| display_metrics | Production completion counting and sequence-based DMA ownership; pthread late ISR/stale semaphore races, timeout retention, fixed aligned internal mirror allocation/error/geometry/source checks; no physical scanout measurement |
| [display_te](display_te/README.md) | Six ASan/UBSan production TE groups: fresh/stale edges, timeout/cooldown/recovery, sleep cancellation, concurrent wait and in-flight ISR deletion, setup failures; real-LVGL groups run with both 40-line/12-chunk and 20-line/24-chunk buffers: one TE wait per full refresh, 1,000 odd/clipped/narrow regions preserve 2×2 alignment, timeout/cancel fallback preserves flush ownership |
| audio | 100 restart cycles, PA/mute rollback, master/external output PCM ownership, disconnect retention, and actual async system-volume/NVS retry/media deferral |
| recorder | Production dual-task microphone/WAV writer, PSRAM ring, pause/resume/stop, unique files and RIFF repair, storage leases/removal, slow/full media and injected allocation/read/write/sync/close failures |
| [recorder_ui](recorder_ui/README.md) | Real LVGL pointer recording controls, time/level, IO preparation/stale callbacks, save/replay/folder navigation, missing-card/backend errors and cleanup; launcher has14 entries with a vector microphone |
| [volume_ui](volume_ui/README.md) | Six real-LVGL pointer groups: center and ±20px slider dragging, held timers, exactly one persistent request on release, one master shared between settings/music and unchanged 100% music stream gain |
| [media_control](media_control/README.md) | Actual central dispatcher: STARTING/running/paused ownership, unavailable snapshots defer Flash, video/NES keys cannot start old music, NES pause/resume and ambiguous-owner rejection |
| [ui_media_control](ui_media_control/README.md) | Actual UI request/completion core and 16-slot queue: pending media handovers, callback launch, delayed producers/epoch invalidation, overflow, recorder/USB ownership gates and no worker-thread LVGL calls; included in the default host runner |
| [files_ui](files_ui/README.md) | Actual LVGL pointer M4A dispatch, bidirectional scroll without launch, busy/boundary/error controls and TF removal/reinsert; before/after rendering with project icons and fonts |
| storage | Concurrent leases, hotplug/removal, power retry/rollover, persistent aligned 4KiB DMA ownership/OOM cleanup; installed IDF cached SD path executes 4160 unaligned reads while DMA allocations are forced to fail |
| fatfs_layout | Actual project-local FatFs/diskio: original sparse 32GB GPT/MBR FAT32 directories/content/seek, GPT validation, 64-to-32-bit device range checks and TRIM conversion; RAM-only FAT32/exFAT Chinese-name mkdir/write/sync/close/append/remount/read/seek/delete roundtrips. No real card or user files are touched |
| [fatfs_vfs_bounds](fatfs_vfs_bounds/README.md) | Extracted production VFS functions with signed 32-bit file positions/stat sizes: oversized open cleanup, SEEK_SET/CUR/END and INT32_MIN bounds, read/write/pread/pwrite and append overflow guards, EOF/zero transfers, direct/cached stat errors, copy refusal before destination creation and truncate limits; mutations are observed by fault doubles, not real media |
| power | Sleep/wake/commit failures, media-time Flash commit deferral, 1,000 balanced invalidation cycles, refresh pause/resume, TG29 key requests before the LVGL lock, disabled screen-off touch wake, retries and NES ownership; BOOT/user-key debounce/one-shot dispatch and sleep-wake gesture consumption |
| user_key_ui | Actual UI user-key event publication/navigation: one-shot exit while loading/running, page-generation and timestamp rejection of stale events, shutdown/input gates and cancellation; no LVGL dependency |
| board_display | 100 retained-supply sleep/wake cycles, QSPI framing, 120ms settles, command failures, cold recovery, rollback and retries |
| touch | Nonfatal I2C faults, retry backoff, cancellation/consumption until real release after recovery, gesture isolation after key wake, disabled screen-off touch wake, held NES-exit gesture consumption, bounds and IRQ fallback |
| CST9220 protocol | Project-ID-selected D109/no-ACK or D101/strict-ACK, two-point bursts, status 6/7, four-bit IDs, physical cache retained through 128 no-fresh polls, non-consuming getters, ID0/partial release, read/ACK error continuity, corrupt-active-coordinate errors and wake mode restoration |
| [touch_lvgl](touch_lvgl/README.md) | Five actual LVGL pointer groups: held touch without early click, release click exactly once, slider drag, list scroll without row click, interrupted-gesture cancellation through error recovery and 100 press/release cycles; production adapter with mocked physical state, no synthetic CLICKED events |
| text | UTF-8 boundaries/BOM, invalid bytes, 20,000 bounded malformed input cases |
| tg29 | Rail voltage/readback, startup load isolation, injected I2C failures and shutdown cleanup; no charger/protection/DCDC2 voltage writes |
| [battery_ui](battery_ui/README.md) | Production battery badge/common-page header and details: native0/100/invalid SOC, charging/done/USB/discharge, no battery, read errors/staleness/tick wrap, no idle repaint, screen replacement and timer cleanup |
| reader | CJK/ASCII pagination; UTF-8, real-IDF CP936 GBK, UTF-16LE/BE conversion/source maps; manual override, surrogates/seek/EOF/malformed bytes, 1,000-paragraph 16/24px reconstruction, allocation failures and cancellation during long GBK scans/conversion |
| [font_runtime](font_runtime/README.md) | Actual LVGL lookup and 4bpp-to-A8 pixel comparison for both production fonts, 8,642 codepoints each, production-style copies/fallback, ASCII/symbols and unchanged allocation baseline; native objects are not ASan/UBSan-instrumented |
| [reader_io](reader_io/README.md) | Production bookmark save/load ordering, queue saturation/coalescing, sequence wrap, stale-result cancellation, retained writes with backoff after persistence errors, media-time commit deferral and pending-cache growth |
| tools | Leap/invalid dates, weekday and timestamp boundaries, pressure altitude, and actual worker media-time persistence deferral/coalescing/failure retry |
| player | Production WAV/player worker with pthread tasks, resampling, pause/seek/playlist/removal/preferences, pending-selection/playlist-refresh ordering, queued-next/other-media Flash deferral, real mono/stereo/two-tone MP3 decode and indexed seek; asynchronous page-session release discards decoder/file/catalog, retains a failed codec/lease owner for retry and rejects restart meanwhile |
| [aac_runtime](aac_runtime/README.md) | Strict AAC-LC ASC validation, cross-AU exact resampling, seek/reset/cancel and SDK fault handling; host substitutes the AAC primitive |
| [mp4_runtime](mp4_runtime/README.md) | Real demux/AAC pull/worker control with decoder doubles; classic/fMP4 audio indexes, misleading .mp3 names, checked offsets/mdat/timing, stop/removal and cancellation |
| video_facade | Backend selection/native MP4 timing, four concurrent play/stop windows, shared timeout budget, constructor and partial deletion failures |
| [bluetooth_runtime](bluetooth_runtime/README.md) | Cold/off task-free lifecycle, task OOM retry, 20 radio cycles, startup failures, GAP/A2DP failure diagnostics, pending cancellation and late callbacks, bounded profile teardown, negotiated-rate PCM FIFO/backpressure and AVRCP queues; RF/peer hardware mocked |
| [bluetooth_ui](bluetooth_ui/README.md) | Real LVGL pointer selection while discovery replaces a row, canceled scrolling, connection-stage/error feedback and controls disabled during cancellation; hardware services mocked |
| [wifi_runtime](wifi_runtime/README.md) | Cold/off task-free lifecycle, scan/connect/IP/timeout, five bounded reconnects, deferred credentials/forget, OOM retry and teardown; RF/NVS mocked |
| MJPEG parser | Eight groups: segment/stuffing/restart markers, consecutive frames, block boundaries, capacity, cancellation, classic RIFF/LIST bounds, odd padding, nested records and exclusion of thumbnail/audio/index payloads; rejects non-baseline JPEG |
| [mjpeg_runtime](mjpeg_runtime/README.md) | Nine groups execute the production worker: real baseline JPEG and classic AVI, native 25/29.97/30 and raw24 cadence, metrics/rate changes, pause/removal, blocked-decode stop/release, allocation/engine/decode faults and 30 replay/release cycles |
| NES | SRAM path/persistence, failed-load save preservation, allocation rollback, bitmap guards, UTF-8/long overlay clipping |
| [nes_controls](nes_controls/README.md) | Eleven actions/two key slots, defaults/aliases, unbinding/conflict rejection, immediate HID mapping, media-deferred NVS save/reload/default restoration, corrupt-record fallback, commit retries and concurrent changes; included in the default host runner |
| [nes_controls_ui](nes_controls_ui/README.md) | Actual LVGL mapping page: all 22 selectors, real dropdown changes, conflict rollback with a popup open, custom Exit, persistence feedback, defaults/reload and 100 page cleanup cycles; run separately with cached reader_ui objects |
| [nes_runtime](nes_runtime/README.md) | Six groups: deterministic production loop/platform/ROM/config faults; real CPU word alignment and bank boundaries; original generated NROM running the complete production CPU/PPU/APU/mapper-0/event/video core with changing picture/audio, pause/reset, 101 fresh sessions, held-input cleanup and unsupported-mapper failure/recovery |
| [fs_lifecycle](fs_lifecycle/README.md) | Actual shared mutex first-use/OOM race,400 concurrent TF/USB registration transactions, immediate/bounded busy admission and recovery |
| [usb_storage_driver](usb_storage_driver/README.md) | Actual MSC/BDL/VFS/SD cleanup functions: failed unregister ownership, SD driver/card binding reuse, checked host teardown and pre-publication allocation faults |
| [usb_storage](usb_storage/README.md) | External-disk lease generations, mount/detach/OFF races, timed shared-lifecycle contention, retained cleanup ownership and repeated sessions |
| [usb_audio](usb_audio/README.md) | Shared Host UAC1 class lifecycle, PCM format conversion, endpoint cleanup, disconnect and retained failed-stop ownership |

Text, TG29, reader, tools, player, MJPEG and NES safety/runtime tests use AddressSanitizer and UndefinedBehaviorSanitizer. The MJPEG fixture is an original valid JPEG; actual hardware decode is mocked. The NES full-core fixture executes real CPU/PPU/APU and mapper 0 with virtual OSD timing; it does not establish compatibility with other mappers or representative commercial games. These host tests do not establish real PSRAM/DMA performance, I2C/QSPI electrical timing, complete SMP interleavings, ROM/mapper compatibility, touch latency or long-run hardware stability. Firmware remains built separately using ESP-IDF 6.1 for `esp32s31`; follow [device acceptance](../docs/development/acceptance.zh_CN.md) and [media metric/soak definitions](../docs/development/media-runtime.zh_CN.md) for physical checks. Producer/generated FPS and LCD transport-completion FPS are distinct metrics, neither a guarantee of unique visible frames nor panel scanout FPS.

The current storage scope is FAT32/exFAT read/write, including GPT scanning. NTFS is not integrated. The reported30436MiB GPT card mounted successfully on COM36 in the previous release; host fixtures still do not qualify all physical TF/USB media. Application files at or above2GiB remain unsupported despite64-bit filesystem metadata. Persistent managers and notification-waiting workers are intentional; page-exit checks require session files, decoders, codec ownership, storage leases, catalog and frame references to be released after successful cleanup, rather than requiring every shared task to be deleted.
