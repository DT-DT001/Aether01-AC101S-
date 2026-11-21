# NES host regression

Run with an existing Linux/WSL GCC: `sh components/aether_nes/test_host/run.sh`.
From the repository directory in an existing WSL environment: `wsl -d Ubuntu-22.04 -- sh ./components/aether_nes/test_host/run.sh`.

Production `nes_rom.c`/`bitmap.c` are linked with allocation-failure injection and minimal OSD/mapper stubs. Four groups check allocation rollback without save-file destruction, per-ROM filename and SRAM persistence, SRAM reload, and bitmap rollback/guard pixels. A fifth group compiles actual `gui.c`/`gui_elem.c` and checks UTF-8/control characters, bounded message formatting and bitmap clipping. All run with AddressSanitizer and UndefinedBehaviorSanitizer. Generated test images and saves stay under `build/host_media_tests`.

This does not test a complete emulation session, real mapper compatibility, ESP timer/SMP scheduling, audio or touch hardware. Legacy packed-field annotations may produce host warnings.
