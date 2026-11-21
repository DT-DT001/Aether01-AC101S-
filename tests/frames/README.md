# Media frame ownership regression

Run `sh tests/frames/run.sh` with an existing Linux/WSL C compiler. On the current Windows machine the verified invocation is `wsl -d Ubuntu-22.04 -- sh /mnt/c/UserFiles/Workspace/Code/Aether01/tests/frames/run.sh`. No tools are installed by these tests. Build products stay under `build/host_frames_tests`.

An optional WebAssembly runner is also provided: `powershell -ExecutionPolicy Bypass -File tests/frames/run.ps1 -Clang <full-LLVM-clang>`. It requires a Clang build containing the wasm32 backend, lld and Node; paths can be overridden using script parameters. The bundled ESP Clang only includes RISC-V/Xtensa backends and cannot run this optional route.

The harness compiles the actual production `main/app_frames.c` as native host code (or optionally WebAssembly). Small stubs replace FreeRTOS mutexes, LVGL object/cache calls, timing and heap allocation. It checks copied frame ownership, latest-frame selection, protection of the currently displayed frame, inactive/pending cancellation, nonblocking contention, invalid dimensions, the maximum 480x480 frame, mutable-image cache invalidation, queue/replacement metrics and 10,000 handover cycles, plus rollback at every startup allocation failure. Allocation tracking also checks leaks and double frees.

These deterministic tests exercise production buffering logic and failure branches. `presented_frames` counts images handed to LVGL, not LCD transport completion; `replaced_frames` counts accepted pending frames superseded by a newer frame, not every possible physical display drop. They do not model SMP scheduling, PSRAM throughput, DMA/cache coherency, real LVGL rendering or on-device frame timing; those still require hardware testing.

Optional viewport fitting also checks320x180→480x270,256x224→480x420,
portrait/extreme aspect ratios and disabling fitting. Repeated same-size frames
must not resize/recenter the image again. The real-LVGL media harness separately
checks the resulting black bars and actual dirty/flush bounds; native stubs alone
cannot validate transformed-image invalidation.
