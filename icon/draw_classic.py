#!/usr/bin/env python3
"""draw_classic.py - the hand-tuned 4-colour art for amifleet68-classic.info,
the plain (non-GlowIcon) TOOL icon for OS 3.1-style Workbenches and for
people who switch GlowIcons off.

That file carries ONLY classic planar images, so they are the whole icon and
are worth tuning by hand rather than trusting mkicon's nearest-colour
quantisation of the GlowIcon art. This script starts from exactly that
quantisation of amifleet68.png (so the shape always matches the GlowIcon) and
then fixes, pixel by pixel, what the 4-colour mapping cannot know:

  * a clean screen recess: grey bevel on the top and left edge,
  * a white glint in the screen's top-left corner (the GlowIcon's gloss),
  * the status lights as clean 3-pixel lamps: lit blue on the two "OK"
    machines, dark on the amber one, so the board still says OK/OK/warn,
  * the chin's power light as a blue lamp with a white lip.

The selected image is hand-made too. Complementing the planes (what 3.1 does
with GADGHCOMP) turns the transparent background into a solid blue box; here
the background stays put and the monitor itself "switches on": the case goes
grey (pressed), the screen and the tiles invert to a lit white board with
blue machines.

Only the four Workbench colours are written, so mkicon.py maps them 1:1.
Needs Pillow and amifleet68.png (run draw.py first). Writes
amifleet68-classic.png and amifleet68-classic-sel.png.
    python3 draw_classic.py
"""
import os
import sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

# The Workbench 3.x colours, in pen order, as mkicon.py uses them.
WB4 = [(0x95, 0x95, 0x95), (0x00, 0x00, 0x00),
       (0xFF, 0xFF, 0xFF), (0x3B, 0x67, 0xA2)]
G, K, Wh, B = 0, 1, 2, 3


def nearest(rgb):
    # the same weighting as mkicon.py's nearest()
    r, g, b = rgb
    return min(range(4), key=lambda i: 2 * (r - WB4[i][0]) ** 2
               + 4 * (g - WB4[i][1]) ** 2 + 3 * (b - WB4[i][2]) ** 2)


src = Image.open(os.path.join(HERE, "amifleet68.png")).convert("RGBA")
w, h = src.size
grid = [[G if src.getpixel((x, y))[3] < 128 else nearest(src.getpixel((x, y))[:3])
         for x in range(w)] for y in range(h)]


def put(x, y, c):
    grid[y][x] = c


# geometry shared with draw.py
SX0, SY0, SX1, SY1 = 6, 6, 38, 27       # screen recess
TX = [9, 19, 29]                        # machine tiles, 7 wide
TY0 = 10

# -- screen recess: grey bevel along the whole top and left edge
for x in range(SX0 + 1, SX1):
    put(x, SY0, G)
for y in range(SY0 + 1, SY1):
    put(SX0, y, G)

# -- glint in the screen's top-left corner
for x, y in [(9, 8), (10, 8), (11, 8)]:
    put(x, y, Wh)

# -- status lights: a 3-pixel lamp under each machine's screen, lit blue on
#    the two OK machines and dark on the amber one, on a clean white tile
for i, tx in enumerate(TX):
    for dx in (2, 3, 4):
        put(tx + dx, TY0 + 6, B if i < 2 else K)
        put(tx + dx, TY0 + 7, Wh)

# -- power lamp on the chin
put(35, 30, B); put(36, 30, B)
put(35, 31, Wh); put(36, 31, Wh)

# -- selected: the monitor switches on
sel = [row[:] for row in grid]
for y in range(h):
    for x in range(w):
        c = grid[y][x]
        on_screen = SX0 + 2 <= x <= SX1 - 2 and SY0 + 2 <= y <= SY1 - 2
        if on_screen:
            # blue board -> white, white tiles/bus -> blue, black stays black
            sel[y][x] = {B: Wh, Wh: B, G: B, K: K}[c]
        elif c == Wh:
            sel[y][x] = G        # the case / stand face, pressed
# the case bevel was grey; with a grey face it becomes the black edge
for y in range(h):
    for x in range(w):
        if grid[y][x] == G and sel[y][x] == G and src.getpixel((x, y))[3] >= 128:
            r, g, b = src.getpixel((x, y))[:3]
            # only the bevel/recess greys of the case, not the drop shadow
            if not (r == g and abs(g - b) <= 6):
                sel[y][x] = K


# the recess bevel disappears into the grey face; the screen's own black
# outline is edge enough, and the black bottom/right reads as "pressed"
for x in range(SX0, SX1 + 1):
    sel[SY0][x] = G
for y in range(SY0, SY1):
    sel[y][SX0] = G


def save(g, name):
    img = Image.new("RGBA", (w, h))
    for y in range(h):
        for x in range(w):
            img.putpixel((x, y), WB4[g[y][x]] + (255,))
    img.save(os.path.join(HERE, name))
    print("wrote", os.path.join(HERE, name), f"{w}x{h}")


save(grid, "amifleet68-classic.png")
save(sel, "amifleet68-classic-sel.png")
