"""Validate generated font asset data, independently of a display or Files/."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def inspect_font(px: int) -> set[int]:
    source = (ROOT / f"main/assets/font_{px}.c").read_text(encoding="utf-8")
    assert '#include "lvgl.h"' in source and '../../lvgl.h' not in source
    lists = {
        name: [int(value.strip(), 0) for value in values.split(",") if value.strip()]
        for name, values in re.findall(
            r"static const uint(?:8|16)_t\s+(\w+)\[\]\s*=\s*\{([^}]*)\}", source, re.S
        )
    }
    glyphs_text = source.split("glyph_dsc[] =", 1)[1].split("};", 1)[0]
    glyphs = [dict((name, int(value, 0)) for name, value in
                  re.findall(r"\.(\w+)\s*=\s*([\-\w]+)", row))
              for row in re.findall(r"\{([^{}]+)\}", glyphs_text)]
    bitmap_text = source.split("glyph_bitmap[] =", 1)[1].split("};", 1)[0]
    bitmap_length = len(re.findall(r"0x[0-9a-fA-F]+", bitmap_text))
    cmap_text = source.split("static const lv_font_fmt_txt_cmap_t cmaps[]", 1)[1].split("};", 1)[0]
    mapping = {}
    maps = re.findall(r"\{([^{}]+)\}", cmap_text, re.S)
    assert len(maps) == 78
    for row in maps:
        values = dict(re.findall(r"\.(\w+)\s*=\s*([^,\n]+)", row))
        start, length, first = (int(values[key]) for key in
                                ("range_start", "range_length", "glyph_id_start"))
        kind = values["type"].strip()
        offsets = values["glyph_id_ofs_list"].strip()
        if "SPARSE" in kind:
            unicodes = lists[values["unicode_list"].strip()]
            assert len(unicodes) == int(values["list_length"])
            assert unicodes == sorted(set(unicodes))
            entries = ((value, first + (index if "TINY" in kind else lists[offsets][index]))
                       for index, value in enumerate(unicodes))
        elif "TINY" in kind:
            entries = ((value, first + value) for value in range(length))
        else:
            entries = ((value, first + offset) for value, offset in enumerate(lists[offsets])
                       if offset or value == 0)
        for value, glyph_id in entries:
            assert 0 <= value < length and 0 < glyph_id < len(glyphs)
            glyph = glyphs[glyph_id]
            bitmap_end = glyph["bitmap_index"] + (glyph["box_w"] * glyph["box_h"] * 4 + 7) // 8
            assert bitmap_end <= bitmap_length
            mapping[start + value] = glyph
    assert len(mapping) == 8642
    samples = "天地玄黄宇宙洪荒小说阅读器书架上一页下一页切换字体音乐暂停继续"
    for char in samples:
        glyph = mapping[ord(char)]
        assert glyph["adv_w"] > 0 and glyph["box_w"] > 0 and glyph["box_h"] > 0
    supplementary = [glyph for codepoint, glyph in mapping.items() if codepoint > 0xFFFF]
    assert len(supplementary) == 196 and all(glyph["box_w"] for glyph in supplementary)
    print(f"PASS font_{px}: 8642 mapped codepoints, 196 supplementary glyphs, CJK metrics/bitmap bounds")
    return set(mapping)


assert inspect_font(16) == inspect_font(24)
