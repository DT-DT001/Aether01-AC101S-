# USB audio host regression

Run `sh tests/usb_audio/run.sh` using the existing Linux/WSL C compiler.
The test includes actual `aether_usb_audio.c` and links actual `pcm_convert.c`.
Only RTOS scheduling, allocators and the public official UAC class API are mocked.
No USB-core/PHY/power symbols exist in the harness, so accidental ownership of
those functions fails to link. UAC calls assert they occur outside cache locks.

The deterministic worker tests exercise48k stereo playback/capture,44.1k mono
negotiation/resampling, continuous-rate selection, unsupported PCM formats,
whole-block FIFO backpressure, timeout/Host-OFF start cancellation, disconnect,
explicit capture overflow, failed stream stop/device close/client uninstall,
retryable cleanup and idempotent stop after Host OFF. Converter tests compare
chunked and whole-input output and cover PCM16 extrema and mono/stereo mapping.
ASan/UBSan and `-Wall -Wextra -Werror` are enabled.

This is software evidence, not a physical isochronous/headset or clock-drift test.

The runner also extracts the actual vendored `uac_host_device_open` and
`_uac_host_device_delete` function bodies, plus the exact interface-add failure
tail, and compiles them against fault-injecting resource stubs. Regressions cover
failures before interface allocation, ring-buffer allocation failure, existing
physical-device ownership, partial interface cleanup, and empty/nonmember/member
device-list removal. The extraction checks the recorded vendor source hash.
These tests exercise cleanup code, not descriptor enumeration or UAC2 support.


Scheduling regressions cover notification-only idle/HID-only sleep, unchanged
2ms active playback/capture, immediate OFF/command/disconnect wake, exponential
10..200ms cleanup retry, class-uninstall backoff, healthy playback during failed
capture cleanup, FIFO-only cleanup, ordinary STOP recovery without auto-restart,
and tick rollover. Backoff never releases DMA/client ownership or hides the
recorded failure, and no pending object is freed on a deadline.
