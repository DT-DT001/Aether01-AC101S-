# Music player host regression

`sh tests/player/run.sh` executes the production player and WAV stream code
under ASan/UBSan with pthread implementations of task/mutex APIs. It verifies
RIFF bounds, odd chunks, unsupported formats, mono/stereo conversion, 44.1 kHz
duration, full-scale interpolation, silence, seek, pause/resume, rapid next/stop,
playlist EOF selection, removal during pause and NVS preference persistence.
It also decodes real MPEG-1 Layer III mono/stereo streams and an original
440/880Hz two-tone MP3. Nonzero PCM, distinct channels, duration and indexed
seek against a complete sequential decode are checked. No external codec or
encoder is required to run these fixtures.

The stop-barrier regression holds a real worker inside the fake codec write,
starts `stop_wait` on another thread, and submits select, next, previous and
play commands. Each command must be rejected while cleanup owns the command
mutex. After releasing the write, the barrier must complete with no new stream,
codec owner or storage lease. A terminated player's next command is also
rejected. These checks prevent a selection from silently restarting playback
while another media entry waits for music to stop.

The actual ESP32-S31 GCC 15.2 / project `-O2` compile with `-fstack-usage`
reports 16656 bytes in `mp3dec_decode_frame`; the worker reserves 24KiB for
that scratch and its call chain. Device stack watermark remains an acceptance
check. Playlist paths and decoder buffers/index use PSRAM. Preferences debounce
for 800ms and are committed while idle/paused or at teardown, avoiding NVS
commits on every slider movement or during a running audio stream.

Codec output is captured by a paced fake. Real I2S DMA latency, sound quality,
PA electrical behavior and TF timing still require board testing.

The music worker uses priority 7, above GUI priority 6, with the same 24KiB
stack and PSRAM allocations. The player regression asserts that scheduling
configuration. Timing is cached per block; the supplying loop does not print
periodic console logs or scan PCM twice for log-only peak measurements. The
idle housekeeping caller may print one cached profile after pause/stop. A
logging hook verifies that profile output occurs outside the state lock,
ignores active supply and preserves the recorded values.

`sh tests/player/run_refill.sh` runs the real decoder with both the upstream
128KiB and production 32KiB streaming buffers under ASan/UBSan. An original
512-frame MP3 decodes to the same 641986 stereo output frames in both builds;
seeking to 1000ms leaves the same 593986 frames through EOF. The fread hook
wraps both plain and glibc-fortified entry points. A deterministic 2MB/s card
model measures the whole synchronous refill, including all 16KiB fread calls:
128KiB costs 65536 modeled microseconds versus 16384 for 32KiB. This demonstrates
the smaller blocking burst without introducing false short-read EOF. These
numbers are a host latency model, not a TF-card throughput benchmark or an
I2S underrun measurement; real playback still requires board acceptance.

The owning music page now calls `aether_audio_player_release_session`: this is
an asynchronous close, distinct from an ordinary Stop button. Tests retain a
failed codec-close owner and card lease until a checked retry succeeds, reject
restart/playlist controls meanwhile, then verify the playlist/path are discarded.
The persistent shared manager waits for notifications when no deferred settings
need saving; it holds no codec, file, decoder or playlist for a closed page.
