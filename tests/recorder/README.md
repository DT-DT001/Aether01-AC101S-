# Recorder worker regression

Run `sh tests/recorder/run.sh` in the existing Linux/WSL environment. It compiles
the production recorder with ASan/UBSan, real pthread workers/timed mutexes,
real POSIX files and deterministic stereo input. Generated WAV fixtures are
retained in the unique temporary mount directory printed by the runner. It
does not use a microphone, physical SD card or substitute recorder algorithm.

Assertions check the RIFF/fmt/data fields, exact physical file length, even
sample alignment, recorded duration and every saved sample. Both stereo slots contain original patterns including equal full-scale extremes
and a right-only signal. Every WAV sample must equal the widened L/R average,
proving both microphone channels survive mono downmix without overflow and that
full-range peak calculation remains valid. Pause keeps
RX reads advancing with no appended data; resume, dirty-data checkpoints,
unique filenames and preservation of the preceding recording are checked.

The stop-barrier test holds capture inside an RX read and verifies that zero
and short waits retain active state/card lease. A separate stop waiter blocks
new start until the capture owner exits, file closes and lease releases.
Inactive ERROR sessions must not prevent a later media handover.

Failure injection covers session allocation, each of four PSRAM allocations,
both task creations, partial capture startup reservation, ADC read failure,
capture-stop retries and TF removal while paused. A deliberately blocked writer
fills the real 64 KiB ring: exhaustion must report an error, retain resources
while the writer is blocked, then save the valid queued PCM after unblocking.
A partial odd-byte ENOSPC write is repaired to complete samples. Independent
fflush, fsync, fclose and ftruncate failures must be visible. Signed 32-bit
file/RIFF boundary invariants and singleton/task teardown are also asserted.

These tests validate ownership, sample/file contents and error handling.
Hardware sample timing, microphone wiring, target stack watermarks, simultaneous
radio memory pressure, card latency and power-cut recovery require on-device
acceptance. No audio quality or long-duration endurance result is inferred.
