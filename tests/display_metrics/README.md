# Display completion and blocking DMA wait regression

```sh
sh tests/display_metrics/run.sh
```

Set `AETHER_TEST_OUTPUT` to an absolute private directory for concurrent runs.
The runner compiles the production `main/app_display_metrics.c` under
ASan/UBSan. `metrics_test.c` supplies deterministic timestamps;
`wait_test.c` uses real pthread mutexes, condition variables and explicit
callback scheduling gates. ESP-IDF/LVGL endpoints are mocked; these tests do
not measure S31 latency, physical panel FPS or scanout tearing.

Coverage:

- Binary-semaphore OOM and panel callback registration failure leave the
  original LVGL flush/wait hooks unchanged and release temporary resources.
- The original flush callback still receives each submitted area and buffer;
  the original port flush-ready runs exactly once per physical completion.
- Completion before wait needs no token. An old binary token cannot authorize
  buffer reuse for the following transfer.
- An old callback can pause after flush-ready but before completion publication,
  while another core submits the next chunk. Its later sequence/token cannot
  complete that next chunk.
- A callback can instead pause immediately before its semaphore give, after
  completion publication; that delayed give remains only a wake hint when the
  following chunk is already waiting.
- A missing completion for over 100ms increments diagnostic timeout/stall
  counters without returning the wait callback or releasing the DMA buffer.
  Real completion recovers the wait. ISR wakeups explicitly request a yield.
- Twenty-four transfers make one last-chunk refresh. Media-active resets do
  not reset the lifetime submitted/completed sequences. Existing synthetic
  cadence, stale-window and 10,000-refresh counter checks remain covered.

The adapter is coupled to the project's pinned LVGL 9.5 private display
structure only to read its original flush callback: that release has no public
getter. Hooks are installed with public setters and require no new LVGL event
allocation. The caller must hold the recursive LVGL lock across display
creation and attachment, before the first render and with no color transfer
in flight. One display and its semaphore live for the application's lifetime.

The staged RGB565 API additionally reserves one 64-byte-aligned
`INTERNAL|DMA|8BIT` mirror. Both render buffers may remain in PSRAM; this does
not use direct PSRAM DMA or per-transfer bounce allocations. The wrapper waits
for the preceding submitted sequence, checks packed partial RGB565 metadata,
2x2-aligned panel bounds and source/capacity limits, then copies to the mirror.
The original port performs its single byte swap on that copy, leaving render
source bytes intact. Capacity is a byte limit: 240x40 and 2x478 chunks fit the
19,200-byte mirror even though their height exceeds 20 rows.

`staged_test.c` checks copy/swap ownership, mirror immutability across delayed
completion/give, exact allocation caps/alignment, all allocation/registration
rollback paths, invalid bounds/format/stride/source/oversized chunks and a full
24-chunk refresh after recovery. Invalid chunks first wait for any older DMA,
then increment a drop/error diagnostic and release only their LVGL flag. They
do not call the panel, increment submitted sequence or fabricate an LCD
completion. A staged attach failure must be fatal to an application configured
with PSRAM draw buffers; continuing via the original port would reintroduce
the per-transfer allocation path. Real LVGL coverage is separately available
with `python3 tests/display_te/run_staged_lvgl.py`.

The local ESP-IDF SPI panel IO discards the color callback's bool return, so
the adapter explicitly yields after `xSemaphoreGiveFromISR` when required.
Completion is published after port flush-ready; publishing it earlier could
let a returning waiter submit a new chunk whose LVGL flag the previous ISR
would then incorrectly clear.

Device validation should compare task CPU time, touch/scroll latency, DMA wait
maxima, TE wait and completed-refresh cadence under the same screen content.
Blocking yields CPU to other tasks; it does not raise QSPI bandwidth or make
the LVGL owner task process input while it is inside a refresh. A missing real
DMA completion remains fail-closed: never call flush-ready or return early as
a UI recovery shortcut while hardware may still reference that buffer.
