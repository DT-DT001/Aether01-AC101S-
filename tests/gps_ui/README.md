# Native GNSS page regression

Run `python3 tests/home_ui/build_run.py` once, then
`python3 tests/gps_ui/build_run.py` using an existing Linux/WSL C compiler.
The latter runs the production GNSS page against real LVGL and project fonts,
with mocked GPS/power snapshots. It covers pointer-driven start/stop, unavailable
and error states, external USB input versus battery OTG source indications,
valid/faded fixes, UTC and supply data, north/east satellite
geometry, invalid geometry, signal bars and 100 page replacement cleanups.

ASan/UBSan instrument the production page and test (cached LVGL/font objects are
not instrumented). Frames are exported to `build/host_gps_ui/gps-fixed.png`
and `gps-signals.png`; `gps-external.png` shows the external-input state.
This is not a GPS, USB power or physical display test.
