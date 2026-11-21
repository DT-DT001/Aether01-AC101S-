# Reader layout regression

Run `sh tests/reader/run.sh` under the existing WSL Ubuntu/GCC environment.
The production pagination helper is compiled with ASan/UBSan and checked for
measured CJK/ASCII widths, line-height limits, CRLF byte offsets, blank lines,
UTF-8 boundaries, supplementary Han, bounded output, and malformed inputs.
The actual file/chunk implementation is also exercised with real temporary
files: byte-exact UTF-8 reconstruction for chunk sizes 1..24, BOM handling,
seeks inside multibyte characters, EOF, damaged final bytes and empty files.
GBK conversion uses the real CP936 tables from the already-installed IDF's
`ffunicode.c`; the runner locates them using `IDF_PATH` or the existing
`build/project_description.json`. Nothing is installed or copied from `Files/`.
Tests cover UTF-8, GBK, BOM-marked UTF-16LE/BE and manual overrides (including
ambiguous UTF-8/GBK bytes and BOM-less UTF-16), source-byte maps, surrogate
boundaries, malformed-code diagnostics, NUL/binary rejection and both text/map
allocation-failure cleanup. A 1000-paragraph GBK book paginates at 16/24px with
exact text reconstruction and original-byte bookmark resume. UTF-8 seeks after
an ASCII-only preface cannot mistake leading continuation bytes for GBK.

Normal forward/history/reflow requests carry `source_boundary=true`, so they
do not scan backward for GBK alignment. Arbitrary seeks align from a guaranteed
source boundary; a very long unbroken GBK paragraph can require more scanning.
The production IO caller supplies generation/card cancellation checked every
256 bytes of reverse scanning and every 128 units of forward scanning/decoding.
Tests cancel a 90KB reverse seek and conversion after both buffers are allocated,
checking that file/text/map ownership is released.
The same runner invokes `font_assets_test.py`, which validates both generated
font assets (8642 matching mapped codepoints each, including 196 supplementary
glyphs), representative CJK metrics, and every glyph's bitmap bounds. The
provided assets do not cover every character in the full Unicode standard.
The reader IO/NVS queue suite is also invoked; see `tests/reader_io/README.md`.

The test verifies byte/layout logic. It does not replace visual LVGL checks on
the actual 480x480 panel, real glyph measurements, TF removal tests, or NVS
power-loss acceptance on the board.
For actual font/page rasterization and repeated UI navigation, see
`tests/reader_ui/README.md`.
