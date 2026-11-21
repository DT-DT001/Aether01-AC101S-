# MJPEG stream host regression

Run with an existing Linux/WSL GCC: `sh components/aether_mjpeg_player/test_host/run.sh`.
On this machine: `wsl -d Ubuntu-22.04 -- sh /mnt/c/UserFiles/Workspace/Code/Aether01/components/aether_mjpeg_player/test_host/run.sh`.

The script compiles the production `mjpeg_stream.c` with AddressSanitizer and UndefinedBehaviorSanitizer; only ESP error constants are stubbed. Eight groups exercise JPEG segment payload markers, consecutive frames, read-block boundaries, malformed/truncated/oversized frames, cancellation, cooperative yielding, classic RIFF/LIST AVI traversal, odd padding, nested `rec ` lists, video-stream selection, invalid chunk bounds and rejection of progressive JPEG. JPEG-like bytes in header thumbnails, audio payloads and indexes must never become video frames. Artifacts are confined to `build/host_media_tests`.

These are parser safety tests, not hardware JPEG decoding or a full AVI compatibility test. The parser handles classic single-RIFF MJPEG AVI and raw MJPEG; audio playback, seeking/index use, OpenDML/AVIX continuation and non-baseline JPEG are outside its scope. AVI frame timing is read from `avih`; raw streams have no timing metadata. The production worker and lifecycle are exercised separately by `sh tests/mjpeg_runtime/run.sh`.
