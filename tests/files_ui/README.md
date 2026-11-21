# File browser UI verification

Run `python3 tests/files_ui/build_run.py`. The production file page, font assets
and existing icons run through real LVGL input/rendering; filesystem and app
launches are mocked. Existing `host_reader_ui` objects are read only. All new
objects and snapshots stay in `build/host_files_ui`.

Actual pointer tests cover opening M4A through the music page, upward/downward
list scrolling without launching a file, busy controls, restoring Refresh after
a timeout, page boundaries, and removal/reinsertion. Allocator integrity is
checked at teardown. The list now uses 32 entries per page to keep the maximum
widget count comparable to the previous 64 plain rows, while showing native
48px existing icons, fixed filenames and size/type metadata without circular
scroll animations.

The saved baseline was rendered before the production change with identical
file data. Compare `files-before.png` and `files-after.png`; error and scrolling
states are `files-after-error.png` and `files-after-scrolled.png`. The `before`
runner argument is historical and expects the previous implementation.
