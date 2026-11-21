# CO5300 region alignment and TE synchronization

Run in the existing Linux/WSL environment:

```sh
sh tests/display_te/run.sh
```

The first executable compiles production `co5300_te.c` with pthread-backed semaphore/GPIO mocks and ASan/UBSan. It checks rising-edge GPIO setup and shared ISR service reuse, stale-edge rejection, new-edge synchronization, timeout/cooldown fallback, recovery when edges return, cancellation followed by a fresh wake edge, concurrent wait rejection, safe active-wait deletion, an already-dispatched ISR whose semaphore publication overlaps deletion, and missing-pin/allocation/GPIO/ISR setup failures without leaked semaphores. Publication takes place outside the context spinlock; an in-flight count lets teardown drain safely.

The second executable links production `main/app_display_sync.c` with the existing real LVGL 9.5 objects in `build/host_reader_ui`. It uses RGB565 double 40-line buffers. Twelve full-screen partial flushes must wait for TE only once per refresh, after rendering the first chunk and before submitting it. No dirty region means no wait. One thousand small/odd/clipped regions, including narrow tall regions whose unrounded buffer division would produce odd row counts, must produce even starts and even width/height, packed RGB565 stride and bounded pixel lengths. Timeout and cancellation fall back without stranding flush ownership. LVGL heap integrity is checked; cached LVGL objects are not sanitizer-instrumented.

The controller contract comes from the Chipone CO5300 datasheet distributed by Espressif: [original PDF](https://dl.espressif.com/AE/esp-iot-solution/CO5300_Datasheet_V0.00.pdf), CASET/RASET restrictions on printed pages 159/161, and TE mode/commands on pages 31/33/170/180. The project module specification confirms the physical TE output on FPC pin 23 (board GPIO8). Alignment is applied at LVGL invalidation, before rendering; expanding only the flush window would mismatch its already rendered pixel buffer.

Run `python3 tests/display_te/run_staged_lvgl.py` for the independent fixed-DMA
mirror integration. It compiles production `app_display_metrics.c` and
`app_display_sync.c` against the same real LVGL objects, using a private output
directory, pthread blocking and delayed DMA completion. Actual partial-mode
20-line double buffers produce 24 full-screen transfers and one final refresh
completion. Narrow 17-by-475 and 120 changing odd/clipped regions verify packed
stride, 2-by-2 windows and byte capacity rather than a 20-row height limit.
The fake original port swaps the mirror exactly once; source bytes must remain
unchanged. A simulated transfer checks every mirror byte throughout three
milliseconds of ownership. Delayed old flush-ready/completion publication and
old semaphore give also run through the real LVGL pipeline. Sequence, frame,
copy-byte and dropped-region diagnostics must agree. The native render buffers
represent PSRAM ownership but are ordinary host memory; only the mirror's
requested INTERNAL/DMA/8BIT capabilities and alignment are intercepted. The test
and production wrappers use ASan/UBSan, while cached LVGL objects remain
uninstrumented and are checked with `lv_mem_test()`. This establishes software
ordering and pixel-window correctness, not target memcpy cost, radio memory
headroom, panel output or hardware DMA timing.

The hardware wait test replaces GPIO/RTOS, and the real LVGL test replaces only the TE wait/metrics API. Neither establishes physical GPIO edges, panel scanout phase, QSPI signal quality or absence of visible tearing. Internal diagnostic metrics distinguish edges, synchronization and fallback, while normal hardware logging now reports only measured TE frequency in Hz at the user's request. No cumulative TE counts are printed. Successful setup alone is not successful synchronization, and TE frequency is not application FPS.

Normal frequency and fallback-error messages are reported from the existing
priority-3 telemetry task, outside the LVGL flush callback. The real LVGL tests
assert that neither successful nor failed TE waits print these messages in GUI
context. The background reporter retains the 10-second frequency and 2-second
error limits, ignores a zero sleep-period measurement, permits logging without
holding the diagnostic cache lock, and discards pending errors when a display
is deleted or reattached. The separate power telemetry regression verifies the
reporter runs on that worker and stops after shutdown quiescence acknowledgement.
These tests establish scheduling ownership, not on-target console latency or
the cause of any remaining media stutter.

An additional diagnostic build runs the same real LVGL refreshes with simulated
execution cores and a changed task affinity. It verifies that sampling happens
after rendering on the GUI task, while only background polling prints the
placement; deleting and reattaching a display discards the previous samples.
The reported core mask describes cores observed at render completion, and
does not measure CPU utilisation or count all context switches or migrations.

The real LVGL 40-line and 20-line cases also inject known elapsed costs into
the render, first TE wait, flush callback and delayed DMA wait. They verify the
`display_perf` batch row's exact frame/chunk/pixel totals and phase arithmetic,
including a synthetic nested DMA wait inside a flush callback, which must be
subtracted only once. Idle refreshes emit no row and gaps over 500ms start a
fresh measurement window. Continuous drawing logs once per 1.5s window in GUI
context. `start_fps_x100` measures render-start cadence, not LCD completion;
`render_us` ends when the last chunk is submitted, so its final DMA can finish
later. `rest_wall_us` is render wall time minus disjoint TE/flush/DMA waits; it
includes scheduling and other work and is not CPU time or utilization. A wait
outside the render interval is reported separately and never subtracted from
that render duration. The profiler does not change TE or 2-by-2 alignment.

At the current48MHz four-line QSPI, 480×480×2 bytes alone take at least19.20ms, before commands, rendering and scheduling (previously23.04ms at40MHz). A frame-start TE wait cannot guarantee a full frame fits within a nominal60Hz scan. No undocumented panel frame-rate registers are written. Verify TE edge period, first transfer alignment and visible results with a logic analyzer and scrolling patterns before claiming tear-free operation.
