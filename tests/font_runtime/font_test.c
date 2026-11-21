/* Real LVGL glyph lookup/bitmap expansion against the production font data. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
extern const lv_font_t font_16, font_24;

static void check_glyph(const lv_font_t *font, uint32_t cp, uint32_t gid)
{
    const lv_font_fmt_txt_dsc_t *fd = font->dsc;
    const lv_font_fmt_txt_glyph_dsc_t *gd = &fd->glyph_dsc[gid];
    lv_font_glyph_dsc_t actual;
    assert(lv_font_get_glyph_dsc(font, &actual, cp, 0));
    if (cp == '\t') {
        lv_font_glyph_dsc_t space;
        assert(lv_font_get_glyph_dsc(font, &space, ' ', 0));
        assert(actual.resolved_font == font && actual.gid.index == space.gid.index);
        assert(actual.adv_w == (fd->glyph_dsc[space.gid.index].adv_w * 2U + 8U) / 16U);
        return;
    }
    if (actual.resolved_font != font || actual.is_placeholder || actual.gid.index != gid) {
        fprintf(stderr, "cp U+%06lx expected gid %lu actual %lu resolved=%p primary=%p fallback=%p placeholder=%u\n",
            (unsigned long)cp, (unsigned long)gid, (unsigned long)actual.gid.index,
            (void *)actual.resolved_font, (void *)font, (void *)font->fallback, actual.is_placeholder);
        assert(0);
    }
    assert(actual.box_w == gd->box_w && actual.box_h == gd->box_h);
    assert(actual.ofs_x == gd->ofs_x && actual.ofs_y == gd->ofs_y);
    assert(actual.adv_w == (gd->adv_w + 8U) / 16U);
    if (!gd->box_w || !gd->box_h) return;
    lv_draw_buf_t *buffer = lv_draw_buf_create(gd->box_w, gd->box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    assert(buffer);
    memset(buffer->data, 0xa5, buffer->data_size);
    const lv_draw_buf_t *decoded = lv_font_get_glyph_bitmap(&actual, buffer);
    assert(decoded == buffer);
    const uint8_t *raw = fd->glyph_bitmap + gd->bitmap_index;
    for (unsigned y = 0; y < gd->box_h; ++y) {
        for (unsigned x = 0; x < gd->box_w; ++x) {
            unsigned index = y * gd->box_w + x;
            unsigned nibble = (raw[index / 2] >> (index % 2 ? 0 : 4)) & 15U;
            assert(decoded->data[y * decoded->header.stride + x] == nibble * 17U);
        }
    }
    lv_draw_buf_destroy(buffer);
}

static void sweep(lv_font_t *font)
{
    const lv_font_fmt_txt_dsc_t *fd = font->dsc;
    unsigned checked = 0;
    assert(fd->bpp == 4 && fd->bitmap_format == 0 && !fd->stride);
    for (unsigned map = 0; map < fd->cmap_num; ++map) {
        const lv_font_fmt_txt_cmap_t *cm = &fd->cmaps[map];
        unsigned length = cm->type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL ||
                          cm->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY ? cm->list_length : cm->range_length;
        for (unsigned j = 0; j < length; ++j) {
            uint32_t cp = cm->range_start, gid = cm->glyph_id_start;
            switch (cm->type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY: cp += j; gid += j; break;
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL: {
                const uint8_t *offsets = cm->glyph_id_ofs_list;
                if (j && !offsets[j]) continue;
                cp += j; gid += offsets[j]; break;
            }
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY: cp += cm->unicode_list[j]; gid += j; break;
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL:
                cp += cm->unicode_list[j]; gid += ((const uint16_t *)cm->glyph_id_ofs_list)[j]; break;
            default: assert(0);
            }
            check_glyph(font, cp, gid);
            ++checked;
        }
    }
    assert(checked == 8642);
    for (unsigned ch = 32; ch < 127; ++ch) {
        lv_font_glyph_dsc_t glyph;
        assert(lv_font_get_glyph_dsc(font, &glyph, ch, 0) && !glyph.is_placeholder);
    }
    lv_font_glyph_dsc_t icon;
    assert(lv_font_get_glyph_dsc(font, &icon, 0xf001, 0));
    assert(icon.resolved_font == font->fallback && !icon.is_placeholder);
    printf("PASS font line-height %ld: all %u real glyph descriptors and 4bpp-to-A8 pixels; ASCII and Montserrat audio-icon fallback\n",
           (long)font->line_height, checked);
}

int main(void)
{
    lv_init();
    /* Match production's font copies and fallback assignment exactly. */
    lv_font_t small = font_16, large = font_24;
    small.fallback = &lv_font_montserrat_16;
    large.fallback = &lv_font_montserrat_24;
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    sweep(&small);
    sweep(&large);
    lv_mem_monitor(&after);
    assert(after.used_cnt == before.used_cnt && after.free_size == before.free_size);
    lv_deinit();
    puts("PASS unchanged LVGL allocation baseline after 17,284 font glyphs");
    return 0;
}
