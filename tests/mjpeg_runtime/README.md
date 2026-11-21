# Production MJPEG worker regression

Run with the existing Linux/WSL GCC:

```sh
sh tests/mjpeg_runtime/run.sh
```

The harness compiles the real `aether_mjpeg_player.c` and `mjpeg_stream.c` under AddressSanitizer and UndefinedBehaviorSanitizer with pthread-backed FreeRTOS task/lock mocks. It feeds an original, valid 16x16 RGB baseline JPEG (`frame.jpg`) both as a raw stream and as standard classic MJPEG AVI files constructed at runtime. Hardware JPEG calls are mocked; parsing, scheduling, status, cancellation, buffer ownership and cleanup execute production code.

Nine groups cover:

- Consecutive real JPEG frames, callbacks, frame metrics, pacing, EOF and cleanup.
- Native AVI 25/29.97/30 fps pacing, odd chunk padding and exclusion of JPEG header thumbnails.
- Automatic raw-MJPEG fallback to 24 fps.
- Empty files, unsupported automatic rates and unaligned dimensions rejected explicitly.
- Pause/resume, fixed/native rate changes, active-time frame metrics, duplicate play and invalid parameters.
- Immediate pause during a 1 fps wait, followed by card removal and mount-lease cleanup.
- Stop and release while decode is blocked, with no callback after the stop request.
- Injected input/output allocation, decoder creation and decode failures.
- Thirty retained-frame release/restart cycles without JPEG buffer, decoder or storage-lease leaks.

`actual_fps_x100` measures worker frame production, not display scanout or user-perceived smoothness. Host tests do not establish S31 hardware decode output, panel colors, SD-card throughput, task stack margin or sustained device frame rate; those need the board and representative files. AVI audio, seeking/indexes, OpenDML/AVIX continuation and compressed codecs other than baseline MJPEG remain unsupported. Decoded dimensions must be multiples of 16, up to 480x480; input frame bytes are capped at 512 KiB.

Artifacts remain in `build/host_mjpeg_runtime`; generated streams use one temporary file which is removed after a successful run. No IDF installation, firmware changes or device flashing is performed by this script.
