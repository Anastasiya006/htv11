"""Rasterize the Mac's own Menlo locally; never vendor the system font."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

parts = ['#include "lvgl.h"']
for size in (8, 10, 12):
    font = ImageFont.truetype(sys.argv[1], size * 4)
    ascent, descent = font.getmetrics()
    bitmap = []
    glyphs = ['{0}']
    for code in range(32, 127):
        char = chr(code)
        left, top, right, bottom = font.getbbox(char, anchor='ls')
        width, height = right-left, bottom-top
        image = Image.new('L', (max(1, width), max(1, height)))
        ImageDraw.Draw(image).text((-left, -top), char, font=font, fill=255, anchor='ls')
        glyphs.append('{.bitmap_index=%d,.adv_w=%d,.box_w=%d,.box_h=%d,.ofs_x=%d,.ofs_y=%d}' % (
            len(bitmap), round(font.getlength(char)*16), width, height, left, -bottom))
        if width and height:
            bitmap.extend(image.tobytes())
    name = f'gemmi_menlo_{size}'
    parts += [
        f'static const uint8_t {name}_bits[] = {{{",".join(map(str, bitmap))}}};',
        f'static const lv_font_fmt_txt_glyph_dsc_t {name}_glyphs[] = {{{",".join(glyphs)}}};',
        f'static const lv_font_fmt_txt_cmap_t {name}_map[] = {{'
        '{.range_start=32,.range_length=95,.glyph_id_start=1,.type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}};',
        f'static const lv_font_fmt_txt_dsc_t {name}_data = {{.glyph_bitmap={name}_bits,'
        f'.glyph_dsc={name}_glyphs,.cmaps={name}_map,.cmap_num=1,.bpp=8}};',
        f'const lv_font_t {name} = {{.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,'
        f'.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height={ascent+descent},'
        f'.base_line={descent},.static_bitmap=1,.dsc=&{name}_data,.fallback=&lv_font_montserrat_12}};'
    ]
Path(sys.argv[2]).write_text('\n'.join(parts)+'\n')
