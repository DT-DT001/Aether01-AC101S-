# Production touch adapter with real LVGL pointer events

```sh
python3 tests/touch_lvgl/build_run.py
```

Uses the existing native LVGL objects from `build/host_reader_ui` and compiles production `main/app_touch.c`. The test feeds persistent physical point state through the adapter's actual registered read callback, driven by real LVGL input timers, hit testing, pointer processing and widget events. It never sends synthetic `LV_EVENT_CLICKED` events directly.

Checks 100ms press retention, exactly one click after physical release, continuous slider dragging, scrolling without clicking a child row, I²C error recovery without a false click, cancellation of a stale contact after a missed release, and 100 press/release cycles with LVGL heap integrity.

Only the touch-controller transport/state, time, RTOS/IRQ and power/port interfaces are mocked. The mock obeys the controller contract of retaining the last point until a real release report; this separates adapter/LVGL behavior from CST9220 report parsing. Driver-specific tests must separately check that empty polling does not erase a held point. Cached LVGL objects are not ASan-instrumented; `lv_mem_test()` checks its allocator. No installation, flashing, physical timing or touch-coordinate accuracy is claimed.
