Run `sh tests/nes_runtime/run.sh` with GCC/Clang and AddressSanitizer/UBSan.

The production `aether_nes_save.c` runs against temporary host files with an
emulated FAT no-overwrite rename contract. Five groups inject missing media,
open/short-write/flush/fsync/fclose errors, backup/install/rollback rename errors,
disconnect after write or backup, stale backup cleanup, bad stat/path/directory
cases. Assertions retain a complete old canonical or recoverable backup, forbid
new opens after known removal, never promote an uncommitted temporary file and
balance all opened streams. The worker test additionally verifies save errors
survive requested stop while codec/lease/global ownership release normally and
the next session starts cleanly. The companion component ROM/bitmap suite checks
short SRAM reads cannot later overwrite the original partial save. Only the host
fixture is written; these checks do not establish physical power-loss atomicity.

These tests compile the production `nes.c` loop and `aether_nes_port.c`; they
replace CPU/PPU execution, RTOS timing, touch and codec hardware with deterministic
fakes. They check timer pacing, frameskip/audio stepping, pause/resume, tick wrap,
bounded catch-up, measured rates, silent continuation after an audio failure,
retained ownership and session cancellation when audio-write and codec-stop fail
together,
touch failure/exit-hold cancellation, HID sleep-gesture consumption, reset/exit and
immediate TF-card removal handling. The production ROM loader is also exercised
with real files for NTSC/PAL/NES2 headers, exact DiskDude signature versus near-matches,
unsupported mapper, user-readable failure detail, trainer length, corruption, allocation
failure, stop and TF removal. HID tests combine a diagonal direction with an
action button; touchscreen contacts no longer emulate controller buttons.

Remote pause/resume uses the real public `aether_nes_set_paused` API and owner
adapter. Tests check deferred application, repeated-state idempotence, a queued
STARTING request, response during touch-I2C error backoff, and stop/unplug taking
priority over a pending resume. The caller never executes core events or codec
operations itself.

The frame-debt regression executes the real production loop for 3,600 frames
with an I2S-like sink advancing exactly one 60Hz tick per 800-sample frame.
After two initial skips it must display all 3,598 remaining frames and consume
3,603 total ticks; discarding ticks after audio output would incorrectly add
idle periods and fail this assertion. Additional traces cover timer rollover,
sustained two-tick overloaded frames, two seconds of paused wall time, and
stop/input on an undisplayed frame. No trace may skip more than two consecutive
frames. This addresses permanent video starvation when audio backpressure keeps
the old catch-up debt constant above one; it does not lower the emulation timer
or suppress APU advancement on skipped frames.

The adapter's phase-cost test injects known wall times into a 50FPS window and
checks CPU/PPU core, APU generation, audio output, frame callback, I2C input and
idle-yield averages independently. RGB conversion has zero simulated elapsed
time in that deterministic test. These fields include blocking/preemption and
are deliberately named wall microseconds per emulated frame, not CPU load.

The real configuration parser is tested with UTF-8 values, a final line without
a newline, repeated launches, embedded NUL, overlong lines, excessive entries and
allocation failures. Its limits are 1,024 bytes per line, 64 KiB total input and
128 distinct variables. Invalid inputs fail without overwriting the source file.

The full-core test generates its own original iNES NROM program in memory. The
real 6502 CPU polls PPU vblank, changes a backdrop, increments RAM and programs
the APU square wave. It verifies changing output frames/audio, pause/reset, 101
fresh sessions with a deliberately held controller button, unsupported-mapper
cleanup and successful restart afterwards. The OSD and its timer are the only
fakes in this test; CPU, PPU, APU, mapper, event, video, ROM and config sources
are production code. This fixture covers mapper 0 only and is not evidence of
other mapper/game compatibility. CPU helper tests also cover unaligned word fetches and
separately allocated bank boundaries. No third-party game ROM is included.

Every original NROM session also exercises the real PPU controller ports:
holding strobe high must repeatedly read the live A button on both ports;
the falling edge restarts the eight serial bits; a further 1,000 reads must
remain high without an oversized shift. This covers the previous undefined
shift after 32 reads, without claiming support for unexposed accessories.

The same original NROM also runs the real CPU/PPU/APU/mapper with an exact
historical `DiskDude!` header. The production outer loader accepts only this
exact signature for the core's established high-mapper repair; standard PAL,
NES2 and a one-byte near-match remain rejected. This does not add PAL emulation
or validate a different mapper.

They do not establish target CPU performance, I2S/LCD throughput, ROM mapper
compatibility or the board's actual frame rate; those need on-device acceptance.

The platform now expands the256x224 palette image once into480x420 RGB565 on
its owner task. Tests verify every output pixel against the exact two-dimensional
nearest mapping, including duplicated rows. The real LVGL UI consumes an unscaled
480x420 image; its actual board throughput still needs measurement. The retained
producer frame grows by288512 PSRAM bytes (281.75KiB), and the480-byte x map is
static. Audio generation remains800 samples per NTSC frame, delivered in two
400-sample writes instead of seven128-sample writes. The12KiB task stack is kept;
the two temporary PCM arrays use2400 bytes rather than768. Device stack watermark
and target scheduling acceptance remain necessary.

External-controller snapshots use the real public setter and atomic state.
Directions, simultaneous A/B and diagonals, complete releases, press-edge pause/
reset/exit, sleeping-state consumption until full release, and the invisible
upper-left72x72 corner hold (1.2s) are covered. Raw touch only polls at8Hz; it
cannot act as an invisible joypad. Timer stop/delete injection proves that failed
cleanup retains its timer handle and clears the tick callback before returning.
One hundred subsequent install/stop/delete cycles leave no live timer.

The production task/control test completes101 clean launches, then injects audio
stop and timer cleanup failures. Failed ownership cleanup blocks a new NES session
and its media stop barrier until a successful retry; an in-progress barrier also
rejects reentry. Zero-budget calls only poll, and inline audio recovery is skipped
when fewer than1000ms remain, matching the codec stop's existing mutex budget.
Normal media preparation has a larger budget. It asserts priority7, external-state
reset, storage-lease release and frame deallocation. These fault models do not
establish physical USB-host compatibility or long-term device stability.
