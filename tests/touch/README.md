# Touch adapter regression

Run `sh tests/touch/run.sh` using an existing Linux/WSL C compiler. The harness includes actual `main/app_touch.c`; only LVGL, touch hardware, timers and power requests are stubbed. No software is installed.

The current policy is button-only panel wake. Production touch reads and
GPIO interrupts never call `app_power_wake`; the former dedicated touch-wake
task is removed. An IRQ may still notify LVGL to service a report, and8ms
polling still recovers lost release IRQs. Neither action lights a sleeping
panel or reverses a pending sleep request.

The suite covers read/get-data failures and250ms backoff, valid bounds/origin,
IRQ/poll fallback, sleep-request and asleep touch consumption, a finger held
across a later explicit-key wake, real-release recovery, NES raw-input
exclusivity and return-to-GUI gesture isolation. `app_touch_set_enabled()` is
the owner's handoff API. `app_touch_consume_wake_gesture()` retains its name
but only marks an existing contact for consumption after explicit wake; it
does not request panel wake. `ESP_ERROR_CHECK` is deliberately absent from
the stubs, so a fatal check fails compilation. Evidence:
[button-only touch regression](../../tmp/button-only-touch-host.log).

These deterministic tests do not model real CST9220 packets, interrupt polarity, bus timing or SMP scheduling.

The production CST9220 driver has separate packet/cache regressions at `components/cst9220/test_host/run.sh`: markerless polls preserve physical points and the report sequence, while valid touch snapshots advance the sequence. The adapter keeps brief cached contacts, cancels a stale press after 1.8 s or after 64 ms of polling without a valid report after a new IRQ, and waits for a fresh report before accepting input again. The test covers identical fresh samples during a stationary hold and recovery when the release report is missed. [Real LVGL pointer tests](../touch_lvgl/README.md) also cover the visible button state and click count.


The [final actual-LVGL gesture run](../../tmp/otg-final-touch-lvgl-host.log)
also passes. That harness uses power=NULL and qualifies pointer/gesture
behavior, not panel wake; the separate native adapter/power suites above
qualify the button-only wake policy. The [final candidate manifest](../../build/nes-hid-otg-manifest.json)
records a [passing final IDF6.1 build](../../tmp/nes-hid-otg-final-build.log)
but no flash or device acceptance.
