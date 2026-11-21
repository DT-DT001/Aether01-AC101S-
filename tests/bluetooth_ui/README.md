# Bluetooth UI pointer regression

Run `python3 tests/bluetooth_ui/build_run.py` after the reader UI suite has
created the real LVGL and project font object cache. The complete production
Bluetooth page is compiled with mocked radio/audio interfaces; input goes
through LVGL's real pointer processing.

The production scanner may replace a non-audio entry when its 16 slots are full.
Before the fix, pressing address `..01`, replacing that slot with `..63` while
held through two 300ms UI polls, then releasing connected `..63`. The regression
now checks that both the displayed row and captured address remain stable until
release, after which discovery updates resume. A scrolling gesture cancels the
selection and must never connect.

Additional checks cover visible ACL error `0x09`, message/list separation,
disabled scan/selection/output during disconnecting or stopping, a late
`connected=true` while disconnecting, and allocator integrity. Radio pairing,
HCI resources and actual headphone audio need separate device verification.
