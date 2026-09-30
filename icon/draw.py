#!/usr/bin/env python3
"""draw.py - pixel-draws amifleet68's Workbench icon artwork at final size.

Adapted from the macOS amifleet icon (amimcp/tools/amifleet/icon/makeicon.swift):
the same beige monitor showing a tiny fleet board - Workbench-blue title strip
with green/green/amber "machine" lights, two embossed machine tiles below.
The rounded macOS tile is dropped: on Workbench the monitor stands on its own
with a transparent background.

A plain downscale of icon-1024.png to 46 px is mush, so this redraws it pixel
by pixel. Colours are chosen so that the nearest-colour mapping onto the
4-colour Workbench palette (grey/black/white/blue) that mkicon.py uses for the
classic image comes out as a clean 3.1 icon: white bezel with a grey bevel,
black outline, grey screen, blue strip, black "text" rows.

Needs Pillow. Writes amifleet68.png next to this script.
    python3 draw.py
"""
import os
from PIL import Image

W, H = 46, 42
T = (0, 0, 0, 0)

outline   = (24, 24, 28)        # -> black
bezel_hi  = (244, 240, 228)     # -> white
bezel     = (230, 222, 200)     # -> white
bezel_lo  = (222, 212, 188)     # -> white (lower half of the gradient)
bevel     = (184, 172, 146)     # -> grey  (bottom/right bevel of the bezel)
standc    = (166, 154, 128)     # -> grey
screen    = (168, 168, 170)     # -> grey
tile      = (184, 184, 186)     # -> grey
tile_hi   = (250, 250, 252)     # -> white
tile_lo   = (60, 60, 66)        # -> black
textc     = (44, 44, 50)        # -> black
blue      = (16, 96, 184)       # -> blue
blue_dk   = (0, 60, 130)        # -> blue
green     = (72, 204, 104)      # -> grey (lights read as grey dots in classic)
green_dk  = (20, 80, 40)       # -> black ring
amber     = (255, 188, 56)      # -> grey
amber_dk  = (120, 70, 0)       # -> black ring
spec      = (255, 255, 255)     # -> white glint on each light

img = Image.new("RGBA", (W, H), T)
px = img.load()

def put(x, y, c):
    if 0 <= x < W and 0 <= y < H:
        px[x, y] = c + (255,) if len(c) == 3 else c

def rect(x0, y0, x1, y1, c):            # inclusive
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(x, y, c)

def rounded(x0, y0, x1, y1, fill, edge, r=2):
    """Filled rect with an outline and corners cut by r pixels."""
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            dx = min(x - x0, x1 - x)
            dy = min(y - y0, y1 - y)
            if dx + dy < r - 0:          # outside the cut corner
                if dx + dy == r - 1 and r > 0:
                    pass
                continue
            on_edge = (dx == 0 or dy == 0 or dx + dy == r)
            put(x, y, edge if on_edge else fill)

# ---- stand (drawn first, bezel overlaps its top) -------------------------
# neck
rect(19, 31, 26, 36, outline)
rect(20, 31, 25, 36, standc)
# base
rounded(12, 36, 33, 40, bezel, outline, r=2)
rect(14, 39, 31, 39, bevel)

# ---- bezel ---------------------------------------------------------------
BX0, BY0, BX1, BY1 = 1, 0, 44, 32
rounded(BX0, BY0, BX1, BY1, bezel, outline, r=3)
# vertical gradient + highlight, bevel on bottom/right
for y in range(BY0 + 1, BY1):
    for x in range(BX0 + 1, BX1):
        if px[x, y][:3] != bezel:
            continue
        c = bezel_lo if y > (BY0 + BY1) // 2 else bezel
        if y in (BY0 + 1, BY0 + 2) or x in (BX0 + 1,):
            c = bezel_hi
        if y >= BY1 - 1 or x >= BX1 - 1:
            c = bevel
        put(x, y, c)

# ---- screen --------------------------------------------------------------
SX0, SY0, SX1, SY1 = 5, 4, 40, 28
rounded(SX0, SY0, SX1, SY1, screen, outline, r=1)

# blue title strip with three machine lights
rect(SX0 + 1, SY0 + 1, SX1 - 1, SY0 + 6, blue)
rect(SX0 + 1, SY0 + 6, SX1 - 1, SY0 + 6, blue_dk)

def light(cx, cy, c, dk):
    # 4x4 disc: corners transparent to the strip
    for (dx, dy) in [(1, 0), (2, 0), (0, 1), (3, 1), (0, 2), (3, 2), (1, 3), (2, 3)]:
        put(cx + dx, cy + dy, dk)
    for (dx, dy) in [(1, 1), (2, 1), (1, 2), (2, 2)]:
        put(cx + dx, cy + dy, c)
    put(cx + 1, cy + 1, spec)

ly = SY0 + 2
light(SX0 + 4,  ly, green, green_dk)
light(SX0 + 10, ly, green, green_dk)
light(SX0 + 16, ly, amber, amber_dk)

# two embossed machine tiles
def machine_tile(x0, y0, x1, y1):
    rect(x0, y0, x1, y1, tile)
    rect(x0, y0, x1, y0, tile_hi)
    rect(x0, y0, x0, y1, tile_hi)
    rect(x0 + 1, y1, x1, y1, tile_lo)
    rect(x1, y0 + 1, x1, y1, tile_lo)
    # text rows + tiny monitor glyph
    rect(x0 + 3, y0 + 4, x1 - 4, y0 + 5, textc)
    rect(x0 + 3, y0 + 9, x0 + 5, y0 + 11, blue)
    rect(x0 + 7, y0 + 9, x1 - 4, y0 + 10, textc)

TY0, TY1 = SY0 + 9, SY1 - 2
machine_tile(SX0 + 2, TY0, SX0 + 16, TY1)
machine_tile(SX0 + 19, TY0, SX1 - 2, TY1)

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "amifleet68.png")
img.save(out)
print("wrote", out, f"{W}x{H}")
