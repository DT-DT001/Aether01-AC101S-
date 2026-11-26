# Touch adapter regression

Run `sh tests/touch/run.sh` with a Linux or WSL C compiler. The harness compiles the production touch adapter and simulates the controller, timers, IRQs and the power state.

The test covers three standby cycles. Swipes while the panel is asleep cause no controller reads, GUI IRQ dispatch or display wake. A finger held across a power-key wake is consumed until release.

Other checks cover read errors and backoff, coordinate limits, IRQ/polling fallback, NES raw input ownership, missed releases, and shutdown rollback. [The LVGL suite](../touch_lvgl/README.md) verifies actual pointer events and click counts.
