# LVGL custom memory regression

Run `sh tests/lvgl_memory/run.sh` using the existing host compiler. The test
links the production `main/app_lvgl_memory.c`, the installed LVGL 9.5 public
`lv_mem.c` wrapper, its CLIB string implementation and its actual headers.
Only IDF heap endpoints are replaced with checked host allocation records.
ASan/UBSan check leaks, access bounds and allocation lifetimes.

Coverage includes 1000 small UI allocations requesting only PSRAM/8BIT caps,
four-byte alignment, grow/shrink content preservation, realloc OOM and huge
requests retaining the original block, public reallocf fault cleanup, all
zero/sentinel/calloc/zalloc paths, free(NULL), repeated initialization,
unsupported pools, zeroed unsupported monitoring, integrity failures and the
real wrapper's zero-sentinel protection. Arbitrary invalid or freed pointers
remain caller errors, as with the normal allocator; no range check can prove
that an address is an owned live allocation.

The backend has no HP fallback and does not wrap system malloc or change
display DMA buffers or task stacks. Init/deinit and pool/monitor semantics
match LVGL's CLIB backend. The optional explicit `lv_mem_test()` checks the
entire shared PSRAM heap, not UI-specific usage; allocation hot paths do not
perform an integrity scan. Monitor data stays unsupported/zero rather than
presenting global PSRAM totals as LVGL-exclusive usage.

Host caps modeling does not establish actual ESP32-S31 placement, GUI latency,
radio DMA budget or screen throughput. Those require native acceptance with
the existing IDF and board. This suite installs no dependency and changes no
managed LVGL source.
