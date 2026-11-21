# MP4/M4A regression

Run with the existing WSL host compiler and Python 3; no ESP-IDF installation
or device flashing is performed:

```sh
sh tests/mp4_runtime/run.sh
sh tests/mp4_runtime/run_demux.sh
```

`generate.py` creates original 32×32 constrained-baseline H.264 I_PCM IDRs and
classic MP4 sample tables. The optional AAC-LC stereo access units are original
zero-spectrum syntax (`21 00 00 00 00 1c`), independently decoded by real MiniAAC
in the AAC fixture work. No copyrighted recording or FFmpeg installation is
needed. The generated H.264 target header is checked for reproducibility.

The worker test compiles production player, demux, AAC adapter and pull-audio
interface with pthread FreeRTOS mocks and ASan/UBSan. Only the SDK's binary H.264
and AAC engines and hardware I2S/storage endpoints are mocked. It covers native
sample timing and last-frame duration, independent paced audio, pause/resume,
PA/I2S stop, removal, blocked decode cancellation, retained-frame release,
allocation/codec failures, EOF and mount-lease cleanup. Pure M4A and audio from
an unrelated High-profile video are read in 137-frame pieces, seeked and
drained with exact PCM counts. These tests establish control flow and buffer
safety; they do not establish actual codec throughput or picture quality.
The pull test also reads an actual generated `iso5` fragmented MP4 deliberately
named `.mp3`, exercising `trex` defaults, `tfhd` overrides, version-1 `tfdt`,
per-sample `trun` entries and explicit absolute data offsets. Seek/partial reads
produce exact counts, while discontinuous decode times, oversized AAC units
and negative data offsets fail with complete cleanup. An `INT64_MAX` base plus
positive run offset, samples pointing into a box header or crossing an `mdat`
boundary, and a `UINT64_MAX` decode-time wrap are rejected. Default-valued
sample loops periodically yield and honor cancellation. fMP4 truncation and
256 deterministic metadata mutations run through the audio demux too. This proves this tested
fragment layout is parsed, rather than merely allowing its extension.

The demux test parses real boxes, AVC config and sample offsets, rebuilds Annex
B, checks explicit High-profile/fMP4 rejection, cancellation, truncated files,
oversized SPS lengths and 256 deterministic metadata mutations. This is bounded
regression coverage, not exhaustive fuzzing of every container variation.

`target/` is an optional Unity component for an existing ESP32-S31 test app with
PSRAM initialized. `[h264][mp4][s31]` calls the real official S31 C H.264 decoder
on the original SPS/PPS/IDR, checks all luma/chroma samples and repeats three
times. It is not part of normal firmware and has not yet run on the board.
`tests/aac_runtime/target` supplies the corresponding real AAC decoder test.

Production dependencies are pinned to official `esp_h264` 1.4.1 and
`esp_audio_codec` 2.6.2. Native C/optimized implementation selection follows the
target configuration; host mocks do not qualify either decoder binary. Actual S31 acceptance
must additionally check non-silent AAC, A/V synchronization, resource contention,
touch/LCD responsiveness, pause/stop/removal while I2S blocks, decoder stack high
watermarks, errors and PSRAM stability over repeated playback and long runs.
Host cadence metrics and produced frames are not LCD completion measurements.

A codec-stop fault now retains the audio child, decoder/session and media lease
until a successful close retry. The test rejects reentry while cleanup is still
pending and checks exact final release. Periodic feed timing remains cached in
status; the real-time AAC supply loop no longer prints five-second diagnostics.

Seek regression additionally delays the second real worker codec-start call by
650ms. The requested image must remain stable until non-trimmed PCM is ready,
then frames retain native cadence without IDR-drop recovery. It also covers a
paused seek followed by slow resume/STOP, replacement seek during startup,
all-trimmed audio beyond a shorter track and a failed pause-time codec close.
The private gate must not keep public preview completion pending or release
in-flight ownership early. Callback completion is waited independently from
the status flag, matching production's asynchronous frame publication.

Original GOP fixtures with64KiB filler NALs count actual fread/fortified-fread
requests: cold lookup reads only NAL headers and the four necessary decode
packets; repeated lookup uses the two-bit hint without probing again. Optional
cache OOM retains correct pictures and falls back to the header path. A corrupt
length prefix in an unplayed target sample fails before publication and drains
the complete session/cache/lease. These are
bounded IO/control tests, not physical SD latency or actual H.264 throughput.

The runner accepts `AETHER_MP4_PLAYER_SOURCE` and `AETHER_TEST_OUTPUT` for an
isolated negative control; the default always compiles production code. Do not
disable the audio clock gate in the firmware. The slow-start test must fail
when that gate is removed, demonstrating the startup-timing regression.

`run_chunk_cache.py` (also included by `run.sh`) compiles the actual production
sample-to-chunk and frame-offset functions with only a test visit counter. An
original2488-chunk/623-group/7773-AU geometry models ordinary15fps video with
three/four AAC access units per interleaved chunk.1600 sequential late-file
queries require2433 chunk visits; an isolated old-cache control requires
2368896. Both must produce the same byte offsets, sizes and timestamps through
2000 backward/random queries and final-chunk boundaries under ASan/UBSan.
These are deterministic lookup-work counts, not hardware execution-time claims.

Session-retained AAC tests count actual SDK open/reset/close calls through
paused750/0/500/0 seeks and require a single open/final close. An original
1024-sample priming edit verifies exact PCM totals when returning to zero,
including early-audio-EOF replay. A blocked old decode cannot overlap the next
reset or be deleted early; reset failure and initial task-allocation failure
still unwind the one retained instance. Multi-seek tests require exactly two
task creations (video plus AAC), so a simulated failure of any later task
allocation cannot break seek. Duplicate notifications cannot replay a completed
segment. Blocking the parked worker's final wake keeps decoder/session/lease
ownership alive even though segment EOF was acknowledged; only a distinct
final worker-exit ACK permits cleanup. These lifecycle tests do not simulate
the real target's internal/PSRAM allocation timing or replace same-device
before/after seek measurements.

Timing coverage injects17ms in the conversion-side resolution query and3ms in
the frame callback. SDK-call, conversion and callback caches remain distinct,
total decode covers both SDK and conversion, and complete/converted picture
counts match. The existing blocked800ms SDK-call case must also record an SDK
maximum, lag rebuild and nonzero skip-burst maximum without changing recovery.

`run_color.py` (also included by `run.sh`) extracts both production I420-to-RGB565
paths and compares them under ASan/UBSan across 4,255 deterministic cases:
full-range luma/chroma extremes, random even geometry/stride/crop combinations,
odd-geometry scalar fallback, 480-pixel bounds and already-cropped decoder
dimensions. Cancellation at every 16-row boundary must produce identical
partial output and leave the remainder untouched. Input immutability and
prefix/suffix canaries check buffer safety. This establishes pixel equivalence,
not conversion speed on ESP32-S31.

Decode-ahead coverage waits for a real prepared RGB picture before pausing,
resuming, stopping/releasing or replacing it with a paused seek preview. Every
P packet must decode exactly once, including the final picture retained for its
declared duration. A pause injected during publication-gate acquisition must
release that gate while holding the same picture; an AAC failure while waiting
on the common clock must terminate cleanly. An original five-second/60-picture
P-chain fixture injects 60ms SDK work plus 35ms conversion against an 83ms sample
budget. It must decode every packet with intact references, skip only expired
displays, and avoid compressed skips/rebuilds. A temporary isolated control that
disables the pre-conversion presentation-skip decision must fail this bound; do
not disable it in production. Prepared RGB is always published after its PTS
instead of being discarded when conversion/gate work crossed the sample end.
These scheduler/control checks do not substitute for native
ESP32-S31 FPS, audio continuity or physical LCD acceptance.

HP-stack lifecycle tests require every task creation to request exactly
`INTERNAL | DMA | 8BIT` with the original 24KiB/priority/core parameters. The mock
forbids ordinary/self deletion and requires genuine suspended state before
external WithCaps deletion/join. Delayed kernel suspension after AAC/video ACK
must retain ownership and honor stop/delete timeouts, then reap exactly once on
retry. Epoch-zero reservation must precede H264 open without opening AAC/I2S;
STOP and video-buffer OOM at that stage still join the reserved worker. HP stack
allocation failures must preserve NULL handles and allow retry. Concurrent stop
and replay exercise lifecycle locking, bounded mutex waiting and single-owner
reaping. A blocked detached-frame free injects a late `release_frame` request and
checks that final ACK cannot race it. Actual HP placement is validated by native
address/capability telemetry, not the host allocator mock.
