# Native LVGL reader and music smoke

Uses the **project's actual LVGL source, font_16/font_24 assets, reader/music
pages, text layout, and file-reading implementation**. It builds with an
existing Linux GCC/Python installation; no SDL/window server or new ESP-IDF is
needed. The mock display is a 480x480 RGB565 framebuffer.

From the existing WSL environment:

```sh
python3 tests/reader_ui/build_run.py
```

Outputs are in `build/host_reader_ui/`: the native executable, a generated
UTF-8, GBK and BOM-marked UTF-16LE/BE books, and PPM screenshots in one folder
per encoding. Convert to PNG for inspection using the
existing bundled Python/Pillow environment if desired.

The smoke actually renders the book shelf, 16/24-pixel day/night reader,
restored bookmark, hot-unplug/reinsert states, and music player/library. Each
encoding exercises all four turn modes with real LVGL pointer input. The
toolbar stays hidden on entry, opens on long press, and hides at three seconds.
The selected turn mode persists across reopening. The suite drives 100 more
page turns and 200 reader/music page rebuilds, checks bitmap rendering and
allocator integrity with LVGL, and exercises pause/resume, previous/
next track, repeat mode, volume, seek, and library track selection. Each valid
Chinese fixture must show zero missing glyphs and zero malformed codes. Manual
encoding switching is saved and restored on reopen. Seeking to 100%, reopening,
turning to the previous page and advancing again verify the exact source-byte
boundary after a reverse scan without page history. Busy/OOM
playlist refresh preserves usable old row indices, with automatic busy retry.
File-browser direct playback also checks that a timeout, busy, missing-storage
or allocation error cannot start a new playlist and bypass the media-owner stop
barrier. Only a successful scan (including an empty catalog) may service the
retained direct-play request; a subsequent successful refresh exercises recovery.

The board, storage mount state, audio worker state, and persistence queue are
mocked. Native snapshots demonstrate real fonts, layout, and event wiring;
they do not prove panel timing, touch coordinate mapping, I2S sound quality,
actual TF removal behavior, NVS flash power-loss recovery, or on-device FPS.
The independent production IO/NVS-queue tests live in `tests/reader_io/` and
are included in `tests/reader/run.sh`.
