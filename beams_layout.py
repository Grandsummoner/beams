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
R_SMALL = 2.3     # small per-track knob radius -- bumped up twice now
                  # (1.5 -> 1.8 -> 2.3) once the real vertical budget
                  # kept showing slack to spend on legibility instead
BTN_SMALL = 1.8   # small per-track button half-size

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
    micro(nm, cx_i, label_y, size=1.3)
    layout["io"].append({"name": nm, "x": round(cx_i, 2), "y": round(jack_y, 2), "r": (R["knob"] if nm == "TEMPO" else R["port"])})
y += io_h + GAP

# ---------------------------------------------------------------------
# TRACKS section -- 4 dense per-track columns
# ---------------------------------------------------------------------
# Vertical cursor budget within each column (top-to-bottom):
#   TRIG jack + label
#   SAMPLE row: LOAD button + dot-row display, side by side
#   RR toggle + label
#   2x2 knob grid: LEVEL/TUNE  //  TONE/CHOKE
#   3-knob row: LEN / STEPS / ROT
#   FILL / INV row: 2 tiny buttons
TRACK_MARGIN = 4.0
TRACK_GAP = 4.0
sample_row_h = 5.4
rr_row_h = 4.4
fill_row_h = 5.0
SUBLABEL_H = 1.1   # individual per-knob/button label height, small tier

trig_block_h = 2*R["port"] + LABEL_GAP + 1.3
sample_block_h = sample_row_h + LABEL_GAP + SUBLABEL_H
rr_block_h = rr_row_h + LABEL_GAP + SUBLABEL_H
# 2x2 knob grid: each of the 2 rows gets its OWN label line underneath
# it (LEVEL/TUNE under row 1, TONE/CHOKE under row 2) -- matches the
# rest of the family labeling every single knob individually, rather
# than one shared caption for a whole block.
knobrow_block_h = 2*R_SMALL + LABEL_GAP + SUBLABEL_H
knob3_block_h = 2*R_SMALL + LABEL_GAP + SUBLABEL_H
fill_block_h = fill_row_h + LABEL_GAP + SUBLABEL_H

tracks_h = (TRACK_MARGIN
            + trig_block_h + TRACK_GAP
            + sample_block_h + TRACK_GAP
            + rr_block_h + TRACK_GAP
            + knobrow_block_h + TRACK_GAP
            + knob3_block_h + TRACK_GAP
            + fill_block_h
            + TRACK_MARGIN)

section("tracks", x0, y, full_w, tracks_h, "TRACKS", color=BRASS, border_color=BRASS)
tx, ty, tw, th, _ = sections["tracks"]
col_w = tw / 4
col_pad = 1.2

trig_y = ty + TRACK_MARGIN + R["port"]
trig_label_y = trig_y + R["port"] + LABEL_GAP + 1.3
sample_y = trig_label_y + TRACK_GAP + sample_row_h/2
sample_label_y = sample_y + sample_row_h/2 + LABEL_GAP + SUBLABEL_H
rr_y = sample_label_y + TRACK_GAP + rr_row_h/2
rr_label_y = rr_y + rr_row_h/2 + LABEL_GAP + SUBLABEL_H
knob_row1_y = rr_label_y + TRACK_GAP + R_SMALL
knob_row1_label_y = knob_row1_y + R_SMALL + LABEL_GAP + SUBLABEL_H
knob3_y = knob_row1_label_y + TRACK_GAP + R_SMALL
knob3_label_y = knob3_y + R_SMALL + LABEL_GAP + SUBLABEL_H
fill_y = knob3_label_y + TRACK_GAP + fill_row_h/2
fill_label_y = fill_y + fill_row_h/2 + LABEL_GAP + SUBLABEL_H

for i in range(4):
    cx = tx + col_w*(i+0.5)
    # Common left/right bounds every row in this column aligns to --
    # the fix for the "evenly fill across" ask: every row below now
    # spans exactly ix0-to-ix1, instead of each row inventing its own
    # margin (the old SAMPLE row was left-packed while the knob rows
    # were center-symmetric -- a real inconsistency, not a style choice).
    ix0 = cx - col_w/2 + col_pad
    ix1 = cx + col_w/2 - col_pad
    inner_w = ix1 - ix0
    if i > 0:
        dx = tx + col_w*i
        add(f'<line x1="{dx:.3f}" y1="{ty+0.8:.3f}" x2="{dx:.3f}" y2="{ty+th-0.8:.3f}" stroke="{BORDER}" stroke-width="0.3" opacity="0.5"/>')

    # TRIG input
    add(f'<circle cx="{cx:.3f}" cy="{trig_y:.3f}" r="{R["port"]+0.3:.3f}" fill="none" stroke="{NAVY}" stroke-width="1.6" opacity="0.55"/>')  # input -> NAVY ring
    micro("TRIG", cx, trig_label_y, size=1.3)
    track = {"index": i, "trig": {"x": round(cx, 2), "y": round(trig_y, 2)}}

    # SAMPLE row: SEQ/RND toggle (left) + the 4 round-robin dots (right)
    # -- grouped together since the toggle directly controls how the
    # dots cycle, swapped out of LOAD's old spot per direct feedback.
    # Dots are capped at exactly 4 now (matches the fixed 4-file load
    # limit below), each with its own fixed identity color reusing the
    # same brass/maroon/navy/plum quartet as Intel's FX buttons and
    # Command's depth gauges -- same family thread a third time.
    toggle_w = inner_w * 0.34
    add(f'<rect x="{ix0:.3f}" y="{sample_y-1.9:.3f}" width="{toggle_w:.3f}" height="3.8" rx="0.5" fill="{BG2}" stroke="{TEXT_DIM}" stroke-width="0.4"/>')
    micro("SEQ", ix0+toggle_w/2, sample_y+0.5, size=1.2, color=TEXT_DIM)
    disp_x0 = ix0 + toggle_w + 1.4
    disp_w = ix1 - disp_x0
    add(f'<rect x="{disp_x0:.3f}" y="{sample_y-2.6:.3f}" width="{disp_w:.3f}" height="5.2" rx="0.7" fill="#FFFFFF" opacity="0.5" stroke="{BORDER}" stroke-width="0.3"/>')
    dot_colors = [BRASS, MAROON, NAVY, "#6B4C8A"]  # PLUM, matches Intel's FX quartet
    ndots = 4
    dot_r = 0.6
    dot_gap = (disp_w - 2*1.4) / max(1, ndots-1)
    for d in range(ndots):
        ddx = disp_x0 + 1.4 + d*dot_gap
        col = dot_colors[d]
        add(f'<circle cx="{ddx:.3f}" cy="{sample_y:.3f}" r="{dot_r:.3f}" fill="none" stroke="{col}" stroke-width="0.35"/>')
    micro("ROUND ROBIN", cx, sample_label_y, size=1.1)
    track["rr_toggle"] = {"x": round(ix0+toggle_w/2, 2), "y": round(sample_y, 2), "w": round(toggle_w, 2), "h": 3.8}
    track["rr_dots"] = {"x": round(disp_x0+1.4, 2), "y": round(sample_y, 2), "pitch": round(dot_gap, 2), "count": ndots, "box_x": round(disp_x0, 2), "box_w": round(disp_w, 2), "box_h": 5.2, "dot_r": dot_r, "pad": 1.4}

    # LOAD -- now its own full-width row (swapped position with the
    # toggle above), explicit about the 4-file cap.
    add(f'<rect x="{ix0:.3f}" y="{rr_y-2.1:.3f}" width="{inner_w:.3f}" height="4.2" rx="0.6" fill="{BG2}" stroke="{TEXT_DIM}" stroke-width="0.4"/>')
    micro("LOAD", cx, rr_y+0.5, size=1.4, color=TEXT_DIM)
    micro("UP TO 4 SAMPLES", cx, rr_label_y, size=1.1)
    track["load_btn"] = {"x": round(cx, 2), "y": round(rr_y, 2), "w": round(inner_w, 2), "h": 4.2}

    # LEVEL / TUNE / DECAY -- one row of 3, evenly spread across the
    # same ix0-ix1 bounds as every other row (dropped a 4th knob, TONE,
    # per direct feedback -- it was the one unjustified control, added
    # only to round out a 2x2 grid). DECAY was CHOKE -- same knob, same
    # slot, upgraded from a hard truncate-point to a real exponential
    # decay envelope (shapes punch/character, not just length, and
    # tapers to silence on its own with no separate declick fade
    # needed). Stays MAROON, not arbitrary: it's the knob Intel's TONE
    # knob offsets over the link, and TONE lives on Intel's REVERB page,
    # whose own family color is already MAROON -- the ring inherits
    # meaning from where the modulation actually comes from, same as
    # Command's gauge colors inheriting from Intel's FX quartet.
    track["knobs"] = {}
    for j, (nm, col) in enumerate([("LEVEL", TEXT_DIM), ("TUNE", TEXT_DIM), ("DECAY", MAROON)]):
        kx = ix0 + R_SMALL + j * ((inner_w - 2*R_SMALL) / 2)
        add(f'<circle cx="{kx:.3f}" cy="{knob_row1_y:.3f}" r="{R_SMALL:.3f}" fill="{BG2}" stroke="{col}" stroke-width="0.4"/>')
        micro(nm, kx, knob_row1_label_y, size=1.0)
        track["knobs"][nm.lower()] = {"x": round(kx, 2), "y": round(knob_row1_y, 2), "r": R_SMALL}

    # 3-knob row: LEN / STEPS / ROT -- evenly spread across the same
    # ix0-ix1 bounds (first/last at the edges, middle centered) instead
    # of a fixed, narrower pitch that left extra margin unused.
    for j, nm in enumerate(["LEN", "STEPS", "ROT"]):
        kx = ix0 + R_SMALL + j * ((inner_w - 2*R_SMALL) / 2)
        add(f'<circle cx="{kx:.3f}" cy="{knob3_y:.3f}" r="{R_SMALL:.3f}" fill="{BG2}" stroke="{BRASS}" stroke-width="0.4"/>')
        micro(nm, kx, knob3_label_y, size=1.0)
        track["knobs"][nm.lower()] = {"x": round(kx, 2), "y": round(knob3_y, 2), "r": R_SMALL}

    # FILL / INV -- two equal-width buttons splitting the same ix0-ix1
    # span down the middle, rather than fixed-width buttons floating in
    # the center with unused space on both sides.
    half_w = inner_w/2 - 0.6
    add(f'<rect x="{ix0:.3f}" y="{fill_y-BTN_SMALL:.3f}" width="{half_w:.3f}" height="{BTN_SMALL*2:.3f}" rx="0.4" fill="{BG2}" stroke="{TEXT_DIM}" stroke-width="0.35"/>')
    add(f'<rect x="{ix1-half_w:.3f}" y="{fill_y-BTN_SMALL:.3f}" width="{half_w:.3f}" height="{BTN_SMALL*2:.3f}" rx="0.4" fill="{BG2}" stroke="{TEXT_DIM}" stroke-width="0.35"/>')
    micro("FILL", ix0+half_w/2, fill_y+0.4, size=1.1)
    micro("INV", ix1-half_w/2, fill_y+0.4, size=1.1)
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
