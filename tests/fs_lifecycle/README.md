# Shared filesystem registration lifecycle

Run `sh tests/fs_lifecycle/run.sh`. It compiles the production FatFs lifecycle
guard with pthread-backed FreeRTOS mutex doubles, ASan/UBSan and strict warnings.
Two threads race the initial mutex allocation, then execute400 deliberately
interleaved drive/context allocation transactions under the shared guard.
Allocation failure, losing-candidate cleanup, immediate/20ms timeout admission
and recovery are checked. Ordinary per-volume I/O does not use this guard.

`tests/storage` and `tests/usb_storage` separately verify that their real worker
adapters hold this guard across whole SDK mount/unmount calls, retain owners on
failure and defer contention without classifying it as bad media.
