# ESP32-S31 MP4/M4A

This component uses the existing IDF 6.1 environment, official ESP32-S31
H.264/AAC decoders and a pinned CC0 minimp4 demux. Native optimized decoder
selection follows the target configuration. It does not assume
ESP32-S31 has a hardware H.264 decoder. See `vendor/SOURCE.md` and the retained
AAC license for source provenance and local hardening.

Video currently accepts classic, non-fragmented MP4 with constrained-baseline
H.264 (`avc1`), progressive I420, CAVLC, one slice group, P/I pictures, coded
dimensions at most 480×480, native average rate at most 30fps, and ordinary
single normal-speed edit lists. Timing follows the actual sample table. Source
rate limits describe accepted input, not guaranteed decoding speed. Main/High,
B/reordered pictures, HEVC, fragmented MP4, encryption and complex edit timelines
are unsupported and return an explicit error. Active-session seeking rebuilds
H.264 references from the preceding actual IDR, decodes through the requested
picture and repositions AAC with overlap preroll and exact output trimming.
Paused seeking publishes one preview and remains paused. Speed override is not
implemented. Seek commands are nonblocking; a busy publication gate asks the GUI
to retain the final target and retry. Accepted commands suppress old pictures.

Audio is AAC-LC, standard 8–48kHz mono/stereo with 1024-frame access units. PCM
is resampled with persistent phase/history to 48kHz stereo, passed to AC101S,
and scaled by software volume. HE-AAC/SBR/PS, PCE and multichannel/ER/ELD are
rejected rather than silently discarded. Single AAC priming edits are trimmed.
The video-session AAC task and H.264 task each reserve a 24KiB stack with
`INTERNAL | DMA | 8BIT` capabilities, excluding S31's slow LP memory from their
stacks. Actual stack address/capability are cached at entry; a single final-exit
watermark scan records the minimum free bytes without scanning during playback.
The project keeps `ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP` enabled for ordinary
system tasks. S31 PIE vector loads cannot read LP SRAM, so a project-owned shim
adds DMA capability to this decoder's INTERNAL allocations, preserving each
original HP/PSRAM preference and the SDK alignment/cache/size-reporting paths.
At build time existing objcopy generates a same-name S31 archive under `build/`
with only three undefined allocator references redirected; codec instructions
and the managed input remain unchanged. The source is hash-locked to official
ESP-H2641.4.1; generation checks its symbols and records provenance. The public
H264 front-end calloc API uses a fixed-signature wrapper. System malloc and
heap functions are not globally wrapped. This avoids the wireless-memory
regression observed when the RTC heap was disabled globally.

CMake rejects S31 assembly with RTC heap unless this scoped allocator is on,
or with dual-task H264/unicore. The source and build hooks are in
`h264_hp_alloc.c`, `cmake/s31_h264_memory.cmake` and
`tools/prepare_s31_h264.py`; the generated archive is an ignored build artifact.

`aether_mp4_audio.h` also provides a pull interface for the music player:
audio-only M4A, or the AAC track of an MP4 regardless of its unrelated video
profile. `FILE` is borrowed, and all calls belong to the music-owner task. The
interface buffers output across partial reads and seeks with one AAC access
unit of overlap preroll. It exposes source codec object/ASC metadata so file
magic can be diagnosed independently of a misleading filename extension.
This audio-only path also indexes real fragmented MP4 (`iso5` is a possible
brand): `moov`/`trex` initialization plus `moof`/`traf`/`tfhd`/`tfdt`/`trun`
samples, with ordinary explicit or default-moof data offsets. It accepts
continuous decode timelines and unencrypted AAC, at most 100,000 access units
(about 35 minutes at 48kHz). Discontinuous timelines, complex implicit offsets,
encryption and larger indexes fail explicitly. The brand alone is never treated
as proof of fragmentation or AAC encoding. Fragmented **video** remains outside
the video backend's supported format.

Video and audio readers use independent `FILE` objects while retaining a single
storage lease. Stop/removal cancels both workers before releasing metadata or
the mount lease. Pausing stops the audio hardware and freezes the shared media
clock; resume reconstructs AAC overlap and trims to that clock. At severe
decode lag, video skips to an IDR and recreates decoder state; it never drops
reference packets and continues decoding invalid predictions. Read/decode/
callback time, generated FPS, late/drop counters and format errors are
available in status. These are producer metrics, not physical LCD transport
completion or a guarantee of tear-free output.

Cached timing also separates the SDK H264-call wall time and I420-to-RGB565
conversion wall time, aggregated per feed with session maxima and output versus
converted-picture counts. Read/decode/callback maxima and lag-rebuild/maximum
consecutive compressed-picture-skip counts identify recovery bursts. These
elapsed measurements include scheduler preemption; video read time also
includes Annex-B work and any lag-reset priming, so it is not pure SD latency.
Metrics do not change pacing, resolution, picture quality or recovery policy.
Session-wide SDK and conversion time sums include priming/preroll; they permit
whole-run averages without treating sparse last-value samples as an average.

Normal playback decodes/prepares one RGB picture ahead and waits until its PTS
before publication. A prepared picture remains owned during pause or a busy
publication gate; resume never re-feeds its compressed packet. STOP, removal or
a newer seek invalidates it before publication. If an ordinary picture has
already passed its sample end, its H.264 packet still decodes to preserve all P
references, while RGB conversion/display can be skipped. Once RGB is prepared,
it is the newest available picture and is published after its PTS even if
conversion or gate waiting crossed its sample end. `presentation_drops`
counts only those expired displays, separately from compressed IDR-recovery
skips. The first displayed picture and seek previews remain forced. The common
audio clock gate and 500ms severe-lag recovery policy remain in effect. This
reduces avoidable display work; a source whose decoder alone exceeds its frame
budget can still require severe-lag recovery.

I420 conversion reuses the signed 32-bit U/V color terms across each 2×2 luma
block while preserving the scalar converter's clipping, rounding and RGB565
packing. Odd crop coordinates or dimensions use the original scalar path.
Both paths retain cancellation checks every 16 rows and accept either coded
or already-cropped decoder dimensions. The native color regression compares
every output pixel and partial cancellation output; device measurements, not
the host comparison, establish any frame-time improvement.

The first valid post-preroll PCM block commits the common presentation clock.
Decoder/codec startup after seek or resume is excluded from media time, so a
slow audio reopen does not leave permanent A/V startup debt or trigger video
IDR recovery while sound is still being prepared. A target preview completes
independently of this private clock gate. Short/fully-trimmed audio EOF releases
the gate, errors stop the session and STOP/new seek cancels an old commit.
Failed pause-time codec close retains the same checked ownership as final stop.

One AAC worker, its original HP task stack and decoder/private work area belong
to the entire video session. Its stack is reserved after demux validation and
before H264 buffers/DPB; epoch zero parks without codec, file or I2S ownership.
The first requested segment creates the decoder; later
seek segments reset that same instance after the previous segment completes.
Between segments/early audio EOF the worker blocks on task notifications;
requested/completed epochs reject stale or duplicate wakes. Seeking back to zero also
resets the codec and restores encoder priming from exact edit units. The first
cold start retains its original no-extra-reset path. Each segment still owns
and releases its FILE/packet/PCM-output buffers and audio-hardware session.
The video parent requests final worker exit and waits for both a separate exit
ACK and real kernel suspension before externally deleting the WithCaps worker,
deleting AAC exactly once and releasing its shared session/lease,
including close failures and partial-startup errors. This avoids reallocating
the task stack or small hot codec arrays after H264 preroll changed the heap layout; actual
memory placement and sustained device throughput are qualified separately.

The video worker also finishes all logging, buffer release and handle access
before publishing its final ACK and permanently suspending. A separate lifecycle
mutex serializes external task creation/reaping across replay and stop/wait;
timeouts preserve the task handle/stack for a later retry. Natural EOF can retain
the suspended video stack until replay, stop/wait or handle deletion reaps it.
WithCaps workers never self-delete or allocate a temporary deletion helper.
Late `release_frame` requests are drained before the final ACK. Deleting the
player handle requires exclusive caller ownership, with no active/queued API
calls; the lifecycle mutex does not make a freed handle usable concurrently.

IDR lookup probes only sample length prefixes and NAL headers instead of reading
and copying every intervening compressed frame twice. A video-owner-only
two-bit/sample PSRAM hint uses at most75000 bytes at the300000-sample input
limit; OOM falls back to header probes. Metadata sync flags are not trusted. The
selected IDR and dependent samples still receive complete Annex-B validation
before decoding. Scanning yields and honors cancellation. Status caches seek
scan/preroll and audio-clock-wait durations; intentional pauses can also be
included in clock-wait time, so that metric is not an underrun counter.

Start device qualification with 320×240 at 15fps, then measure before increasing
resolution/rate. `tools/convert-video.ps1` creates baseline H.264 + AAC-LC MP4
with an existing PC FFmpeg executable; the script installs nothing. For example:

```powershell
./tools/convert-video.ps1 -InputFile C:/Media/input.mp4 -OutputFile C:/Media/s31.mp4
```

No board FPS, universal MP4 compatibility or stutter-free claim is made before
actual decoding and LCD/audio acceptance. Independent native regression and
optional real-library device test instructions are in `tests/mp4_runtime` and
`tests/aac_runtime`.
