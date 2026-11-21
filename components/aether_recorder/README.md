# Microphone recorder

This component records the board's AC101S analog microphone to unique files in
`/sdcard/Recordings`. The output is ordinary RIFF/WAV, 48,000 Hz, mono PCM16.
The codec capture API configures both I2S slots as ADC; the recorder extracts
slot 0. It does not mix playback into the recording or route microphone audio
to the speaker/Bluetooth output. The fixed microphone input gain is 24 dB.

`new` creates one application-wide handle and command mutex. `get_default`
returns that handle. No recorder task, PCM buffer or open file exists until
`start`. A writer task (4 KiB stack, priority 3) and capture task (4 KiB stack,
priority 5) are created per session and exit after cleanup. PCM uses a fixed
64 KiB PSRAM ring; separate PSRAM buffers hold 4 KiB stereo RX, 4 KiB write
staging and 8 KiB stdio buffering. The ring is a single-producer/consumer queue
with acquire/release publication; copying never disables interrupts.

Only the capture task starts, reads and stops the ADC. Only the writer accesses
the FILE and filesystem. The writer holds a TF mount lease through buffered
close and final header repair. It never frees session buffers or publishes
`active=false` until the capture task confirms that it has released the codec.
A partially failed capture start is also stopped. If codec close keeps failing,
the capture task retries at 100 ms intervals and retains ownership; a timed-out
`stop_wait` leaves that session active rather than allowing another audio owner
to overlap it.

`start` is asynchronous. `set_paused` excludes subsequent captured blocks from
the file while continuing to drain RX, so resume does not append stale paused
audio. Changes apply at block boundaries. `stop` only publishes a request.
`stop_wait(0)` requests stop and polls; positive timeouts also serialize against
a new start. An already inactive recorder returns `ESP_OK` even if the previous
recording failed; the failure remains in `status.last_error/detail`. All active
states, including PAUSED and SAVING, must participate in application media
ownership and Flash-commit guards. Terminal `del` requires callers to quiesce
other references to the handle.

Files use `O_EXCL` and a boot-time/collision suffix, so previous recordings are
never overwritten. The writer checkpoints dirty data and the WAV header about
every two seconds using `fflush` and `fsync`. On normal stop it drains queued
valid PCM, closes the buffered FILE, reopens the file, obtains the actual file
length, truncates trailing/odd bytes and rewrites a matching header. Closing
before repair prevents a stale stdio buffer from later overwriting that header.
Every flush, sync, close, truncate and header-write result is checked. The
maximum data length is the largest even value below `INT32_MAX - 44`, keeping
the whole file inside signed 32-bit seek offsets as well as RIFF/FAT32 limits
(roughly 6.2 hours of unpaused audio per file).

TF removal, storage/full-card errors, ADC errors and queue exhaustion stop the
session and surface an explicit error. A slow card that fills the bounded queue
does not silently drop blocks. Valid queued audio is saved where storage still
works; after transport failure the component attempts to repair whatever bytes
actually reached the file. `path` remains available for a partially recoverable
file. Checkpoints reduce exposure to forced power removal but cannot guarantee
the last samples or FAT metadata survive power loss; clean save is the reliable
completion boundary. Persistent card failure can prevent header repair.

Native regression: `sh tests/recorder/run.sh`. Real board acceptance must still
verify the fitted microphone, input level/noise, long recording throughput,
actual TF removal and playback of the saved WAV.
