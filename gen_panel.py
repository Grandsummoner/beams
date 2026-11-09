import fontTools.ttLib
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.ttLib import TTFont

FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def text_to_path(text, x, y, size_mm, font_path=FONT_BOLD, tracking=1.08, anchor="start"):
    font = TTFont(font_path)
    glyf = font['glyf']
    cmap = font.getBestCmap()
    units_per_em = font['head'].unitsPerEm
    scale = size_mm / units_per_em

    hmtx = font['hmtx']
    total_width = 0
    glyph_data = []
    for ch in text:
        if ch == ' ':
            try:
                adv = hmtx['space'][0]
            except Exception:
                adv = units_per_em * 0.3
            glyph_data.append((None, adv))
            total_width += adv * tracking
            continue
        gid = cmap.get(ord(ch))
        if gid is None:
            continue
        adv = hmtx[gid][0]
        glyph_data.append((gid, adv))
        total_width += adv * tracking
    total_width_mm = total_width * scale

    start_x = x
    if anchor == "middle":
        start_x = x - total_width_mm / 2
    elif anchor == "end":
        start_x = x - total_width_mm

    cursor = 0.0
    paths = []
    for gid, adv in glyph_data:
        if gid is not None:
            pen = SVGPathPen(glyf)
            glyf[gid].draw(pen, glyf)
            d = pen.getCommands()
            if d:
                px = start_x + cursor * scale
                paths.append(f'<path d="{d}" transform="translate({px:.3f},{y:.3f}) scale({scale:.6f},-{scale:.6f})"/>')
        cursor += adv * tracking
    return "\n".join(paths), total_width_mm
