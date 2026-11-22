# File browser UI verification

Run `python3 tests/files_ui/build_run.py`. The production file page, font assets
and existing icons run through real LVGL input/rendering; filesystem and app
launches are mocked. Existing `host_reader_ui` objects are read only. All new
objects and snapshots stay in `build/host_files_ui`.

Actual pointer tests cover opening M4A through the music page, bidirectional
scrolling without launching a file, busy controls, refresh recovery and volume
removal/reinsertion. A 4,100-entry directory fixture verifies the 4,096-entry
index cap, a successful tap on the last indexed file, and the fixed pool of
12 rows throughout scrolling. Allocator integrity is checked at teardown.

The saved baseline was rendered before the production change with identical
file data. Compare `files-before.png` and `files-after.png`; error and scrolling
states are `files-after-error.png` and `files-after-scrolled.png`. The `before`
runner argument is historical and expects the previous implementation.
