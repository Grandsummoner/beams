"""
Beams panel layout -- DRAFT v1, for visual discussion only. Not backed by
any DSP/widget code yet. Reuses Stellar's actual shared constants/helpers
verbatim (palette, section() with rotated side-title convention, txt()/
micro() glyph-path text) rather than reinventing them -- see Intel's own
build history for why that matters.

Layout, top to bottom:
  CLOCK row (I/O-style, 4 slots): CLOCK IN, TEMPO, MASTER L, MASTER R.
  TRACKS section: one bordered box, 4 columns (one per drum track), each
    a compact vertical "channel strip":
      - TRIG input (overrides the track's own internal Euclidean gen
        when patched -- same normalled-input idiom as Stellar's velocity
        bypass)
      - SAMPLE slot: LOAD button + a small readout/dot-row standing in
        for "which round-robin sample is loaded/up next" -- placeholder
        content only, real widget would drive this live
      - RR (round-robin mode: SEQ/RND) toggle
      - LEVEL / TUNE / TONE / CHOKE -- 2x2 knob grid, small radius
      - STEP LEN / STEPS / ROTATION -- row of 3, same small radius
      - FILL / INV -- two tiny toggle buttons

All open questions (master level? per-track pan? exact sample-display
behavior?) are deliberately left OUT of this draft rather than guessed --
see the accompanying discussion.
"""
from gen_panel import text_to_path
import json

HP = 5.08
PANEL_H = 128.5

# --- Palette/metrics copied verbatim from Stellar's own layout script ---
BG, BG2 = "#F5F1E9", "#EDE7DC"
BORDER = "#8A7F6A"
TEXT_DIM = "#4A4438"
TEXT_BRIGHT = "#2A2620"
MICRO = "#2E281F"
SLOT = "#DDD6C6"
BRASS = "#8A6423"
MAROON = "#8A2A2A"
NAVY = "#2E4A6E"

LABEL_GAP = 1.0
LABEL_H = 1.6
GAP = 5.0
SIDE_TITLE_W = 5.0
MIN_MARGIN = 1.4
OUTER_MARGIN = 4.5

R = {
    "knob":  2.8,               # CLOCK row's TEMPO knob -- normal family size
    "port":  26 / 2 / (75/25.4),
    "light": 1.0,
}

# Per-track controls are dense (13 elements x4), so they use a smaller
# knob/button scale than the rest of the family -- closer to Command's
# KEY-row Trimpot scale than a standard knob. New to this module, kept
# local rather than added to the shared convention until proven out.
R_SMALL = 3.0     # (v3) per-track knob radius -- now the SAME 6.0mm knob as
                  # Intel's FX section (IntelKnob, FX_KNOB_DIAM = 6.0), laid
                  # out 2 columns x 3 rows instead of the old 3-across rows
BTN_SMALL = 2.3   # per-track FILL/INV button half-height (was 1.8)

# (v3) type sizes (em, mm) -- everything per-track went up
TRIG_LABEL_SZ = 1.9    # "TRIG n"
KNOB_LABEL_SZ = 1.7    # LEVEL / TUNE / DECAY / LEN / STEPS / ROT
RR_LABEL_SZ = 1.6      # "ROUND ROBIN"
BTN_TEXT_SZ = 1.8      # SMPL / FILL / INV (bold, drawn by the widget too)
LOAD_TEXT_SZ = 2.6     # LOAD (bold)
HEADER_SZ = 1.7        # "SELECT UP TO 4 SAMPLES"
CAP = 0.73             # DejaVu Bold cap-height / em
HDR_GAP = 1.6          # clear space above/below the header text (mirrored)
SAMPLE_ROW_H = 5.4     # SMPL button + dots readout row
LOAD_H = 5.4           # LOAD button height (was 4.2)
KNOB_ROW_GAP = 1.2     # between one knob's label and the next row's knob

def row_height(r, extra=0.0):
    block_h = extra + 2*r
    return block_h + LABEL_GAP + LABEL_H + 2*MIN_MARGIN

def centered_y(box_y, box_h, r, extra=0.0):
    block_h = extra + 2*r
    block_top = box_y + (box_h - block_h - LABEL_GAP - LABEL_H) / 2
    return block_top + extra + r

port_pad = R["port"] + 0.3 + 1.2
knob_half = R["knob"] + 0.8

# --- Width budget ---
# CLOCK row: 4 jacks/knob-in-a-jack-slot, same io_w_needed formula as
# the rest of the family.
io_w_needed = 4 * 10.5 + 2*port_pad

# TRACKS: 4 columns, each needs to fit a 2-wide knob grid comfortably.
col_content_w = 17.0   # fits 2x(R_SMALL*2) + gap + side pad
col_gap = 1.2
tracks_w_needed = 4*col_content_w + 3*col_gap + 2*2.0  # + side pad inside the section

content_w_needed = max(io_w_needed, tracks_w_needed)
x0 = OUTER_MARGIN
PANEL_W = content_w_needed + 2*x0 + SIDE_TITLE_W
WIDTH_HP = int(-(-PANEL_W // HP))
PANEL_W = WIDTH_HP * HP
full_w = PANEL_W - 2*x0

svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PANEL_W} {PANEL_H}" width="{PANEL_W}mm" height="{PANEL_H}mm">']
svg.append(f'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="{BG}"/><stop offset="100%" stop-color="{BG2}"/></linearGradient></defs>')
svg.append(f'<rect x="0" y="0" width="{PANEL_W}" height="{PANEL_H}" fill="url(#bg)"/>')
svg.append(f'<rect x="0.5" y="0.5" width="{PANEL_W-1}" height="{PANEL_H-1}" fill="none" stroke="{BORDER}" stroke-width="0.4" opacity="0.6"/>')

def txt(text, x, y, size, color, anchor="start", tracking=1.05):
    p, w = text_to_path(text, x, y, size, tracking=tracking, anchor=anchor)
    return f'<g fill="{color}">{p}</g>', w

def add(s): svg.append(s)

title_x = x0 + (R["light"]+0.6)*2 + 1.5
p, _ = txt("BEAMS", title_x, 7.2, 4.4, TEXT_BRIGHT)
add(p)
p, _ = txt("quad drum sampler", title_x, 10.4, 1.9, TEXT_DIM)
add(p)
TITLE_H = 12.5

link_r_cx, link_r_cy = PANEL_W - OUTER_MARGIN - R["light"], OUTER_MARGIN + R["light"]
add(f'<circle cx="{link_r_cx}" cy="{link_r_cy}" r="{R["light"]}" fill="{NAVY}" opacity="0.85"/>')
add(f'<circle cx="{link_r_cx}" cy="{link_r_cy}" r="{R["light"]+0.6}" fill="none" stroke="{BORDER}" stroke-width="0.3" opacity="0.4"/>')
link_l_cx, link_l_cy = OUTER_MARGIN + R["light"], OUTER_MARGIN + R["light"]
add(f'<circle cx="{link_l_cx}" cy="{link_l_cy}" r="{R["light"]}" fill="{NAVY}" opacity="0.85"/>')
add(f'<circle cx="{link_l_cx}" cy="{link_l_cy}" r="{R["light"]+0.6}" fill="none" stroke="{BORDER}" stroke-width="0.3" opacity="0.4"/>')

layout = {"io": [], "tracks": []}
sections = {}
def section(name, x, y, w, h, title, color=TEXT_DIM, border_color=None):
    bc = border_color or BORDER
    sections[name] = (x + SIDE_TITLE_W, y, w - SIDE_TITLE_W, h, title)
    add(f'<rect x="{x+SIDE_TITLE_W}" y="{y}" width="{w-SIDE_TITLE_W}" height="{h}" rx="1.0" fill="{SLOT}" opacity="0.9" stroke="{bc}" stroke-width="0.5" stroke-opacity="0.8"/>')
    cx, cy = x + SIDE_TITLE_W/2 + 0.6, y + h/2
    max_size = 1.7
    available = h - 2.0
    _, measured_w = text_to_path(title, 0, 0, max_size, tracking=1.1, anchor="middle")
    size = max_size if measured_w <= available else max(1.0, max_size * available / measured_w)
    p, w = text_to_path(title, 0, 0, size, tracking=1.1, anchor="middle")
    p2, _ = text_to_path(title, 0.04, 0, size, tracking=1.1, anchor="middle")
    add(f'<g fill="{color}" transform="translate({cx},{cy}) rotate(-90)">{p}{p2}</g>')

def micro(text, x, y, color=MICRO, size=1.2):
    p, _ = txt(text, x, y, size, color, anchor="middle")
    add(p)
    p2, _ = txt(text, x + 0.04, y, size, color, anchor="middle")
    add(p2)

# ---------------------------------------------------------------------
# CLOCK row
# ---------------------------------------------------------------------
io_h = row_height(R["port"])
y = TITLE_H
# Renamed from "CLOCK" -- the row is CLOCK in, TEMPO (fallback knob),
# and the master outs, not all clock jacks. "I/O" matches how Intel/
# Stellar name their own mixed in/out rows.
section("io", x0, y, full_w, io_h, "I/O")
cy, ty, cw, ch, _ = sections["io"]
io_names = ["CLOCK", "TEMPO", "OUT L", "OUT R"]
# TEMPO renders as a knob (radius R["knob"]=2.8), visibly smaller than
# the 3 jack ports (radius R["port"]=4.4) -- equal CENTER pitch (the
# previous version) makes the gap around the smaller knob look wider
# than the gaps between the full-size ports, even though the centers
# are technically even. Fixed properly: equal EDGE-TO-EDGE gaps, with
# the outer margins on both sides matching that same gap too, so it's
# edge-spacing-even end to end, not just center-pitch-even.
io_radii = [R["port"], knob_half, R["port"], R["port"]]  # knob_half = TEMPO's
                                                          # outer guide-ring
                                                          # radius, its real
                                                          # visible edge
jack_y = centered_y(ty, ch, R["port"])
label_y = jack_y + R["port"] + LABEL_GAP + LABEL_H
n = len(io_names)
io_gap = (cw - 2*sum(io_radii)) / (n + 1)
cx_positions = []
cursor = cy + io_gap
for r in io_radii:
    cursor += r
    cx_positions.append(cursor)
    cursor += r + io_gap
for i, nm in enumerate(io_names):
    cx_i = cx_positions[i]
    if nm == "TEMPO":
        add(f'<circle cx="{cx_i:.3f}" cy="{jack_y:.3f}" r="{knob_half:.3f}" fill="none" stroke="{BORDER}" stroke-width="0.5" opacity="0.55"/>')
        add(f'<circle cx="{cx_i:.3f}" cy="{jack_y:.3f}" r="{R["knob"]:.3f}" fill="{BG2}" stroke="{TEXT_DIM}" stroke-width="0.35"/>')
    else:
        # Family nomenclature (same rule as Intel/Stellar): input rings
        # NAVY, output rings BRASS, drawn as a ring only -- the real jack
        # widget sits inside it.
        rc = NAVY if nm == "CLOCK" else BRASS
        add(f'<circle cx="{cx_i:.3f}" cy="{jack_y:.3f}" r="{R["port"]+0.3:.3f}" fill="none" stroke="{rc}" stroke-width="1.6" opacity="0.55"/>')
    micro(nm, cx_i, label_y, size=1.5)
    layout["io"].append({"name": nm, "x": round(cx_i, 2), "y": round(jack_y, 2), "r": (R["knob"] if nm == "TEMPO" else R["port"])})
y += io_h + GAP

# ---------------------------------------------------------------------
# TRACKS section -- 4 per-track columns (v3)
# ---------------------------------------------------------------------
# Per column, top to bottom:
#   TRIG input jack + "TRIG n"
#   SMPL toggle + 4 round-robin dots, "ROUND ROBIN" caption
#   [shared full-width header: SELECT UP TO 4 SAMPLES]
#   LOAD (bold, big)
#   knobs, 2 columns x 3 rows, 6.0mm (Intel FX size):
#       left  = sound  : LEVEL / TUNE / DECAY
#       right = rhythm : LEN   / STEPS / ROT
#   FILL / INV
hdr_cap = CAP * HEADER_SZ
HDR_BLOCK_H = HDR_GAP + hdr_cap + HDR_GAP            # divider -> text -> next control
trig_lbl_cap = CAP * TRIG_LABEL_SZ
knob_lbl_cap = CAP * KNOB_LABEL_SZ
rr_lbl_cap = CAP * RR_LABEL_SZ

trig_block_h = 2*R["port"] + LABEL_GAP + trig_lbl_cap
sample_block_h = SAMPLE_ROW_H + LABEL_GAP + rr_lbl_cap
load_block_h = HDR_BLOCK_H + LOAD_H
knob_rows_h = 3 * (2*R_SMALL + LABEL_GAP + knob_lbl_cap) + 2 * KNOB_ROW_GAP
fill_block_h = 2*BTN_SMALL

# Zero-slack solver (same convention as Intel/Stellar): the vertical budget
# left after the fixed-size blocks is spread evenly over the 4 gaps between
# the 5 blocks and the 2 section margins, so TRACKS ends exactly at the
# bottom outer margin. Control sizes never change -- only the spacing.
blocks_h = trig_block_h + sample_block_h + load_block_h + knob_rows_h + fill_block_h
tracks_h = PANEL_H - OUTER_MARGIN - y
spacing_unit = (tracks_h - blocks_h) / 6.0
assert spacing_unit >= 2.0, f"not enough vertical room ({spacing_unit:.2f}mm per gap)"
SP = spacing_unit

section("tracks", x0, y, full_w, tracks_h, "TRACKS", color=BRASS, border_color=BRASS)
tx, ty, tw, th, _ = sections["tracks"]
col_w = tw / 4
col_pad = 1.2

trig_y = ty + SP + R["port"]
trig_label_y = trig_y + R["port"] + LABEL_GAP + trig_lbl_cap
sample_y = trig_label_y + SP + SAMPLE_ROW_H / 2
sample_label_y = sample_y + SAMPLE_ROW_H / 2 + LABEL_GAP + rr_lbl_cap
hdr_div_y = sample_label_y + SP
hdr_text_y = hdr_div_y + HDR_GAP + hdr_cap
load_top = hdr_text_y + HDR_GAP
load_y = load_top + LOAD_H / 2
knob_ys, knob_label_ys = [], []
cursor = load_y + LOAD_H / 2 + SP
for r_i in range(3):
    ky = cursor + R_SMALL
    kl = ky + R_SMALL + LABEL_GAP + knob_lbl_cap
    knob_ys.append(ky); knob_label_ys.append(kl)
    cursor = kl + KNOB_ROW_GAP
fill_y = knob_label_ys[-1] + SP + BTN_SMALL

# Shared header band across the full TRACKS width (like Intel's "FOR:"
# headers): dashed divider above, text, then LOAD row.
add(f'<line x1="{tx+col_pad:.3f}" y1="{hdr_div_y:.3f}" x2="{tx+tw-col_pad:.3f}" y2="{hdr_div_y:.3f}" stroke="{BORDER}" stroke-width="0.25" stroke-dasharray="0.6,0.6" opacity="0.6"/>')
micro("SELECT UP TO 4 SAMPLES", tx + tw/2, hdr_text_y, color=TEXT_DIM, size=HEADER_SZ)

SOUND = [("LEVEL", TEXT_DIM), ("TUNE", TEXT_DIM), ("DECAY", MAROON)]
RHYTHM = [("LEN", BRASS), ("STEPS", BRASS), ("ROT", BRASS)]

for i in range(4):
    cx = tx + col_w*(i+0.5)
    ix0 = cx - col_w/2 + col_pad
    ix1 = cx + col_w/2 - col_pad
    inner_w = ix1 - ix0
    if i > 0:
        dx = tx + col_w*i
        # vertical dividers skip the header band so they never cut the text
        for (y1, y2) in [(ty+0.8, hdr_div_y-0.3), (load_top-0.4, ty+th-0.8)]:
            add(f'<line x1="{dx:.3f}" y1="{y1:.3f}" x2="{dx:.3f}" y2="{y2:.3f}" stroke="{BORDER}" stroke-width="0.3" opacity="0.5"/>')

    # TRIG input -- input ring NAVY, labelled "TRIG 1".."TRIG 4"
    add(f'<circle cx="{cx:.3f}" cy="{trig_y:.3f}" r="{R["port"]+0.3:.3f}" fill="none" stroke="{NAVY}" stroke-width="1.6" opacity="0.55"/>')
    micro(f"TRIG {i+1}", cx, trig_label_y, color=TEXT_BRIGHT, size=TRIG_LABEL_SZ)
    track = {"index": i, "trig": {"x": round(cx, 2), "y": round(trig_y, 2)}}

    # SMPL toggle (left) + the 4 round-robin dots (right).
    toggle_w = inner_w * 0.42
    toggle_h = 4.4
    add(f'<rect x="{ix0:.3f}" y="{sample_y-toggle_h/2:.3f}" width="{toggle_w:.3f}" height="{toggle_h:.3f}" rx="0.5" fill="{BG2}" stroke="{TEXT_DIM}" stroke-width="0.4"/>')
    micro("SMPL", ix0+toggle_w/2, sample_y + CAP*BTN_TEXT_SZ/2, size=BTN_TEXT_SZ, color=TEXT_DIM)
    disp_x0 = ix0 + toggle_w + 1.2
    disp_w = ix1 - disp_x0
    add(f'<rect x="{disp_x0:.3f}" y="{sample_y-SAMPLE_ROW_H/2:.3f}" width="{disp_w:.3f}" height="{SAMPLE_ROW_H:.3f}" rx="0.7" fill="#FFFFFF" opacity="0.5" stroke="{BORDER}" stroke-width="0.3"/>')
    dot_colors = [BRASS, MAROON, NAVY, "#6B4C8A"]  # same quartet as Intel's FX buttons
    ndots, dot_r, dot_pad = 4, 0.65, 1.2
    dot_pitch = (disp_w - 2*dot_pad) / (ndots-1)
    for d in range(ndots):
        ddx = disp_x0 + dot_pad + d*dot_pitch
        add(f'<circle cx="{ddx:.3f}" cy="{sample_y:.3f}" r="{dot_r:.3f}" fill="none" stroke="{dot_colors[d]}" stroke-width="0.35"/>')
    micro("ROUND ROBIN", cx, sample_label_y, size=RR_LABEL_SZ)
    track["rr_toggle"] = {"x": round(ix0+toggle_w/2, 2), "y": round(sample_y, 2), "w": round(toggle_w, 2), "h": toggle_h}
    track["rr_dots"] = {"box_x": round(disp_x0, 2), "box_y": round(sample_y-SAMPLE_ROW_H/2, 2), "box_w": round(disp_w, 2), "box_h": SAMPLE_ROW_H,
                        "pad": dot_pad, "pitch": round(dot_pitch, 3), "dot_r": dot_r, "count": ndots}

    # LOAD -- bigger, bold
    add(f'<rect x="{ix0:.3f}" y="{load_top:.3f}" width="{inner_w:.3f}" height="{LOAD_H:.3f}" rx="0.6" fill="{BG2}" stroke="{TEXT_DIM}" stroke-width="0.4"/>')
    micro("LOAD", cx, load_y + CAP*LOAD_TEXT_SZ/2, size=LOAD_TEXT_SZ, color=TEXT_DIM)
    track["load_btn"] = {"x": round(cx, 2), "y": round(load_y, 2), "w": round(inner_w, 2), "h": LOAD_H}

    # Knobs: 2 columns x 3 rows, left = sound, right = rhythm.
    track["knobs"] = {}
    kx_left = ix0 + inner_w * 0.25
    kx_right = ix0 + inner_w * 0.75
    for row in range(3):
        for kx, (nm, col) in ((kx_left, SOUND[row]), (kx_right, RHYTHM[row])):
            add(f'<circle cx="{kx:.3f}" cy="{knob_ys[row]:.3f}" r="{R_SMALL:.3f}" fill="{BG2}" stroke="{col}" stroke-width="0.4"/>')
            micro(nm, kx, knob_label_ys[row], size=KNOB_LABEL_SZ)
            track["knobs"][nm.lower()] = {"x": round(kx, 2), "y": round(knob_ys[row], 2), "r": R_SMALL}

    # FILL / INV -- two equal-width buttons splitting the column span
    half_w = inner_w/2 - 0.6
    for bx, nm in ((ix0 + half_w/2, "FILL"), (ix1 - half_w/2, "INV")):
        add(f'<rect x="{bx-half_w/2:.3f}" y="{fill_y-BTN_SMALL:.3f}" width="{half_w:.3f}" height="{BTN_SMALL*2:.3f}" rx="0.4" fill="{BG2}" stroke="{TEXT_DIM}" stroke-width="0.35"/>')
        micro(nm, bx, fill_y + CAP*BTN_TEXT_SZ/2, size=BTN_TEXT_SZ)
    track["fill_btn"] = {"x": round(ix0+half_w/2, 2), "y": round(fill_y, 2), "w": round(half_w, 2), "h": round(BTN_SMALL*2, 2)}
    track["inv_btn"] = {"x": round(ix1-half_w/2, 2), "y": round(fill_y, 2), "w": round(half_w, 2), "h": round(BTN_SMALL*2, 2)}

    layout["tracks"].append(track)

y += tracks_h

margin = PANEL_H - y - OUTER_MARGIN
print(f"PANEL {PANEL_W:.1f}mm ({WIDTH_HP}HP) x {PANEL_H}mm -- content ends {y:.1f}mm, bottom slack {margin:.2f}mm")
assert y + OUTER_MARGIN <= PANEL_H, f"OVERFLOW by {y+OUTER_MARGIN-PANEL_H:.1f}mm"

layout["link"] = {
    "left": {"x": round(link_l_cx, 2), "y": round(link_l_cy, 2)},
    "right": {"x": round(link_r_cx, 2), "y": round(link_r_cy, 2)},
}
layout["panel"] = {"hp": WIDTH_HP, "w_mm": round(PANEL_W, 2), "h_mm": PANEL_H}

svg.append('</svg>')
open("res/Beams.svg", "w").write("\n".join(svg))
json.dump(layout, open("beams_layout.json", "w"), indent=2)
print("wrote res/Beams.svg and beams_layout.json")

# ---------------------------------------------------------------------
# Emit src/BeamsLayout.hpp -- the widget code includes this instead of
# carrying hand-copied coordinates, so panel art and widgets cannot drift
# apart (the recurring "coordinate-desync" bug class in this family).
# ---------------------------------------------------------------------
def _arr(name, vals):
    return f"static const float {name}[4] = {{" + ", ".join(f"{v:.2f}f" for v in vals) + "};"

T = layout["tracks"]
io = {o["name"]: o for o in layout["io"]}
k0 = T[0]["knobs"]
hpp = [
    "// AUTO-GENERATED by beams_layout.py -- do not edit by hand.",
    "#pragma once",
    "namespace BeamsLayout {",
    f"static const float LINK_L_X = {layout['link']['left']['x']:.2f}f, LINK_L_Y = {layout['link']['left']['y']:.2f}f;",
    f"static const float LINK_R_X = {layout['link']['right']['x']:.2f}f, LINK_R_Y = {layout['link']['right']['y']:.2f}f;",
    f"static const float IO_Y = {io['CLOCK']['y']:.2f}f;",
    f"static const float CLOCK_X = {io['CLOCK']['x']:.2f}f, TEMPO_X = {io['TEMPO']['x']:.2f}f, OUTL_X = {io['OUT L']['x']:.2f}f, OUTR_X = {io['OUT R']['x']:.2f}f;",
    f"static const float TEMPO_SIZE = {2*R['knob']:.2f}f;",
    _arr("TRIG_X", [t["trig"]["x"] for t in T]),
    f"static const float TRIG_Y = {T[0]['trig']['y']:.2f}f;",
    _arr("RR_X", [t["rr_toggle"]["x"] for t in T]),
    f"static const float RR_Y = {T[0]['rr_toggle']['y']:.2f}f, RR_W = {T[0]['rr_toggle']['w']:.2f}f, RR_H = {T[0]['rr_toggle']['h']:.2f}f;",
    _arr("DOTS_BOX_X", [t["rr_dots"]["box_x"] for t in T]),
    f"static const float DOTS_BOX_Y = {T[0]['rr_dots']['box_y']:.2f}f, DOTS_BOX_W = {T[0]['rr_dots']['box_w']:.2f}f, DOTS_BOX_H = {T[0]['rr_dots']['box_h']:.2f}f;",
    f"static const float DOTS_PAD = {T[0]['rr_dots']['pad']:.2f}f, DOTS_PITCH = {T[0]['rr_dots']['pitch']:.3f}f, DOTS_R = {T[0]['rr_dots']['dot_r']:.2f}f;",
    _arr("LOAD_X", [t["load_btn"]["x"] for t in T]),
    f"static const float LOAD_Y = {T[0]['load_btn']['y']:.2f}f, LOAD_W = {T[0]['load_btn']['w']:.2f}f, LOAD_H = {T[0]['load_btn']['h']:.2f}f;",
    _arr("KNOB_XL", [t["knobs"]["level"]["x"] for t in T]),
    _arr("KNOB_XR", [t["knobs"]["len"]["x"] for t in T]),
    f"static const float KNOB_Y[3] = {{{k0['level']['y']:.2f}f, {k0['tune']['y']:.2f}f, {k0['decay']['y']:.2f}f}};",
    f"static const float KNOB_SIZE = {2*R_SMALL:.2f}f;",
    _arr("FILL_X", [t["fill_btn"]["x"] for t in T]),
    _arr("INV_X", [t["inv_btn"]["x"] for t in T]),
    f"static const float FILL_Y = {T[0]['fill_btn']['y']:.2f}f, FILL_W = {T[0]['fill_btn']['w']:.2f}f, FILL_H = {T[0]['fill_btn']['h']:.2f}f;",
    "}",
]
open("src/BeamsLayout.hpp", "w").write("\n".join(hpp) + "\n")
print("wrote src/BeamsLayout.hpp")
