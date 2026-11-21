# Native battery display verification

Run `python3 tests/battery_ui/build_run.py` after the existing native reader cache
is available. The runner compiles the production battery UI and the common
`app_ui_begin_page` header verbatim, with real LVGL, the full Chinese fonts, and
the production `app_tools_snapshot_t` / `tg29_battery_state_t` declarations.
Hardware handles and the background snapshot getter are mocked. The shared
reader cache is read only; output goes to `build/host_battery_ui`.

Checks cover valid 0–100%, the 15% low-battery boundary, charging/completion,
USB without a battery, discharge overriding a retained charge-complete phase,
invalid presence/SOC/ADC, failed/stale samples and millisecond-clock wrap.
They verify text fits the 116px badge alongside the longest current title,
unchanged values cause no repaint, 24 page replacements keep the timer count
constant, and object deletion releases the badge timer/context. The new code
runs with ASan/UBSan; cached LVGL objects are not instrumented and are checked
with LVGL heap integrity tests.

Rendered pages are saved as `battery-unknown.png`, `battery-low.png`,
`battery-charging.png`, `battery-usb-no-battery.png`, `battery-stale.png` and
`battery-details.png`. These are software/geometry regressions; they do not
validate the PMIC's battery model or physical charging accuracy.
