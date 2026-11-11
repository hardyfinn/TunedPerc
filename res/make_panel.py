#!/usr/bin/env python3
# Draws res/TunedPerc.svg. VCV Rack can't draw SVG <text>, so every label is
# turned into outline paths with fontTools. Positions are in mm and must match
# src/TunedPerc.cpp.
#   pip install fonttools
#   python3 res/make_panel.py
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")

W, H = 60.96, 128.5  # 12 HP

BG = "#1F2B2A"
BG_DARK = "#182221"
INK = "#EFE8DA"
SOFT = "#A79FB2"
HOT = "#E9B44C"
TEAL = "#7FC8B4"
OUTBOX = "#111817"

fonts = {}


def text(s, x, y, size, font=FONT_MED, fill=INK, anchor="middle", spacing=0.04):
    """Return an SVG path for string s; (x, y) is the baseline point in mm."""
    if font not in fonts:
        f = TTFont(font)
        fonts[font] = (f, f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    f, gs, cmap, upm = fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    if anchor == "middle":
        x0 = x - width * scale / 2
    elif anchor == "end":
        x0 = x - width * scale
    else:
        x0 = x
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        tp = TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y))
        gs[n].draw(tp)
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


parts = []
add = parts.append


def die(x, y, s, pips, fill=INK, pip=BG):
    """A small die face, top-left at (x, y), size s, showing 1-6 pips."""
    add(f'<rect x="{x}" y="{y}" width="{s}" height="{s}" rx="{s * 0.22:.2f}" fill="{fill}"/>')
    q = s / 4
    spots = {
        1: [(2, 2)], 2: [(1, 1), (3, 3)], 3: [(1, 1), (2, 2), (3, 3)],
        4: [(1, 1), (3, 1), (1, 3), (3, 3)], 5: [(1, 1), (3, 1), (2, 2), (1, 3), (3, 3)],
        6: [(1, 1), (3, 1), (1, 2), (3, 2), (1, 3), (3, 3)],
    }[pips]
    for (a, b) in spots:
        add(f'<circle cx="{x + a * q:.2f}" cy="{y + b * q:.2f}" r="{s * 0.09:.2f}" fill="{pip}"/>')



add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')
# a tuning-fork / staff texture: five faint lines across the middle
for i in range(5):
    y = 58.2 + i * 1.2
    add(f'<line x1="3" y1="{y}" x2="{W-3}" y2="{y}" stroke="{BG_DARK}" stroke-width="0.35"/>')
add(text("TUNED PERC", 5.5, 10.4, 3.9, FONT_BOLD, INK, anchor="start", spacing=0.12))
add(f'<circle cx="{W-8.5}" cy="8.9" r="2.2" fill="{HOT}"/>')
add(text("C", W-8.5, 9.75, 2.4, FONT_BOLD, BG))
add(f'<rect x="3.6" y="12.6" width="{W-7.2}" height="26.8" rx="1.4" fill="#0B100F"/>')

# knobs
for (x, y, lab, lo, hi) in [(15.5, 49.5, "OCTAVE", "-3", "+3"), (45.5, 49.5, "FINE", "-50c", "+50c"),
                             (15.5, 70.5, "DECAY", "short", "full"), (45.5, 70.5, "LEVEL", "", "")]:
    add(text(lab, x, y - 7.0, 3.2, FONT_BOLD, INK, spacing=0.05))
    if lo: add(text(lo, x - 7.6, y + 7.4, 2.6, FONT_MED, SOFT))
    if hi: add(text(hi, x + 7.6, y + 7.4, 2.6, FONT_MED, SOFT))

# switches and button
SW_Y = 87.0
add(text("TUNE", 11.0, SW_Y - 4.9, 2.9, FONT_BOLD, HOT))
add(text("to C", 14.4, SW_Y - 0.6, 2.4, FONT_MED, SOFT, anchor="start"))
add(text("as is", 14.4, SW_Y + 3.0, 2.4, FONT_MED, SOFT, anchor="start"))
add(text("PITCH", 30.5, SW_Y - 4.9, 2.9, FONT_BOLD, INK))
add(text("track", 33.9, SW_Y - 0.6, 2.4, FONT_MED, SOFT, anchor="start"))
add(text("hold", 33.9, SW_Y + 3.0, 2.4, FONT_MED, SOFT, anchor="start"))
add(text("DETECT", 51.0, SW_Y - 4.9, 2.9, FONT_BOLD, INK))
for y in (79.0, 92.6):
    add(f'<line x1="4" y1="{y}" x2="{W-4}" y2="{y}" stroke="{BG_DARK}" stroke-width="0.5"/>')

# jacks
IN_Y, OUT_Y = 100.8, 117.4
for x, lab in zip([10.5, 30.5, 50.5], ["TRIG", "V/OCT", "VEL"]):
    add(text(lab, x, IN_Y - 5.6, 2.9, FONT_BOLD, HOT if lab == "V/OCT" else INK))
add(f'<rect x="3.6" y="108.6" width="{W-7.2}" height="14.6" rx="1.2" fill="{OUTBOX}"/>')
for x, lab in zip([10.5, 30.5, 50.5], ["OUT", "ENV", "NOTE"]):
    add(text(lab, x, OUT_Y - 5.6, 2.9, FONT_BOLD, INK))

svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">\n' + "\n".join(parts) + "\n</svg>\n")
open(os.path.join(HERE, "TunedPerc.svg"), "w").write(svg)
print("ok")
