# Aether01 NES component

The bundled Nofrendo core runs NTSC iNES1.0 images with its included mappers.
The API still rejects unsupported mappers, PAL-only and NES2.0 images with an
actual error. The original regression fixture exercises mapper0; broad game
compatibility and real-device FPS are not established by host tests.

The owner task runs at priority7 with its existing12KiB stack. It advances APU
time for every emulated frame, including undisplayed frames, and writes800 mono
samples as two400-sample stereo fragments. No periodic console log is emitted
from the supply loop. The producer uses a 3:2 nearest-neighbor expansion from
256×224 to 384×336 RGB565. The resulting 258,048-byte image fits the centered
viewport without further LVGL scaling. At 48 MHz QSPI, pixel transfer alone
takes 10.75 ms within the panel's approximately 16.7 ms scan period. LVGL
adopts the latest complete frame at refresh start and keeps its source stable
through all DMA strips, with the existing TE alignment before the first strip.
Releasing the page requests safe deferred deallocation.

`aether_nes_set_external_input(handle,buttons)` publishes a complete snapshot
without calling the core, audio, filesystem or LVGL. Bits0..7 are UP,DOWN,LEFT,
RIGHT,A,B,SELECT,START; bits8/9/10 are edge-triggered PAUSE,RESET,EXIT. Send zero
on controller disconnect or host shutdown. Nonzero input is rejected while no
session runs or a stop barrier owns cleanup; zero always releases stale state.
Screen-off and in-game pause are independent. A screen wake clears only the
screen-off pause. Both pause states stop the emulation tick timer and audio.
Screen-off suspends raw touch reads and consumes held inputs through release.
Holding the upper-left72x72 screen corner for1.2s exits without a HID
controller; screen contacts do not supply joypad input. The raw touch poll rate
is8Hz instead of one I2C read per emulated frame.

A stop barrier waits for the task to finish frame callbacks, touch access, ROM/
configuration IO and codec closure. Audio/timer cleanup failure is propagated
and keeps ownership until retry; new sessions cannot start over a retained
owner. Failed timer deletion keeps its handle with a disabled callback rather
than dropping an active timer. Zero timeout is a poll. Inline codec recovery
requires at least1000ms remaining because the codec stop API has that existing
mutex budget; shorter power-cleanup calls can time out and retain the owner.
If an audio write fails and codec stop also fails, the owner task ends the
session and retries cleanup rather than continuing DMA writes or losing ownership.

See `tests/nes_runtime/` and `tests/media_ui/` for fault and real-LVGL coverage.
Physical CPU/PSRAM/LCD/USB timing, stack watermark, exact controllers and long
runs still require device acceptance.

SRAM and emulator-configuration writes occur only on exit. The removable-media
lease is checked before opening a save and before committing metadata. A new
`.s31tmp` must finish writing, flushing, syncing and closing before the old
canonical file is renamed to `.s31bak`; the checked temporary file is then
installed. A failed install attempts rollback, otherwise retains the old backup.
Loading restores a backup when the canonical file is absent; it never promotes
an uncommitted temporary file. A failed/short SRAM read disables later overwrite
for that ROM session. Errors are retained in NES state/detail while normal
codec/file/task cleanup still completes and permits a fresh launch.

This is a recoverable backup protocol, not filesystem journaling or guaranteed
power-loss atomicity. FAT metadata and physical media can still fail. Known
disconnect prevents new save opens/rename stages; closing an already open
stream remains necessary and can itself attempt a bounded backend flush.
Normal frame pacing, the priority7 task and its idle yield are unchanged.
The legacy `ROMs/nofrendo.cfg` path is used when its directory exists; otherwise
configuration falls back to the same volume's root, allowing ROMs opened from
other browser directories without requiring a special folder.
