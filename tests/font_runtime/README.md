# Native font runtime regression

Run with the existing Linux/WSL GCC and Python environment:

```sh
sh tests/font_runtime/run.sh
```

The runner reuses current LVGL and production font objects from
`build/host_reader_ui/`. Missing or stale objects are compiled into its separate
`build/host_font_runtime/objects/` cache; it never changes the reader-UI objects
or launches reader tests. It uses `tests/reader_ui/lv_conf.h`, including the
large-font layout and disabled compression. The output is
`build/host_font_runtime/font_sweep`. No dependencies are installed.

The test calls real LVGL glyph lookup and bitmap expansion for all 8,642 mapped
codepoints in each supplied 16/24 px font. Every nonempty 4bpp glyph is converted
to A8 and compared pixel by pixel with the production bitmap. Descriptors,
offsets, advance widths, ASCII, Montserrat symbol fallback and the production
font-copy/fallback pattern are checked. TAB follows LVGL's two-space semantics
and rounding in 8.4 units. After 17,284 codepoints, the LVGL allocation baseline
must be unchanged.

This suite uses the existing native LVGL objects, **without ASan/UBSan
instrumentation**. It proves lookup/bitmap consistency in that native build;
it does not reproduce target Flash, PSRAM/cache, DMA, display-window alignment
or physical panel transfer behavior.

During the 2026-11-03 read-only target audit, the built ELF32 contained both
complete bitmaps, byte-for-byte equal to the project font sources: 1,010,314
bytes for 16 px and 2,222,887 bytes for 24 px. All 78 cmaps, 8,642 mappings and
16-byte large glyph descriptors were valid. Actual target preprocessing of the
font, LVGL fmt-txt and UI translation units agreed on 32-bit pointers,
little-endian order, `LV_FONT_FMT_TXT_LARGE=1`, UTF-8 and disabled compression;
the fonts themselves use 4bpp/plain format. Their ELF font data was below the
16 MiB physical Flash boundary. These facts describe the audited ELF and
compile commands, not every future build or the bytes read by the board.

For a new target investigation, repeat the effective-macro checks using the
matching `build/compile_commands.json` and inspect the matching ELF32 font
descriptors, cmap pointers and bitmap contents. Native success alone does not
establish a target ABI or firmware-image match, nor rule out electrical or
display-transfer faults.
