#!/usr/bin/env python3
"""draw.py - draws amifleet68's Workbench TOOL icon artwork at final size.

The motif is the macOS amifleet icon (amimcp/tools/amifleet/icon): a beige
monitor showing a small fleet board. This version is shaded to sit in a
WBDock next to modern GlowIcons: a bevelled, gradient-lit case, a recessed
lit blue screen with a glossy reflection, and on it three little Amiga
"machine" tiles, each with a status light (green / green / amber), linked
underneath by a glowing network bus. A soft opaque drop shadow falls to the
lower right (GlowIcons only have 1-bit transparency, so the shadow is solid
pixels, not alpha).

46x46, transparent background, every pixel placed by code so it stays crisp
at 1:1 - icons are never scaled, and a downscaled drawing turns to mush.

The classic 4-colour planar image (OS 3.1) is not drawn separately: mkicon.py
maps every pixel to the nearest Workbench colour (grey/black/white/blue).
The colour ramps here are kept inside those buckets so the classic image
comes out clean: case -> white with a grey bevel, outline -> black, screen ->
blue, tiles and bus -> white, lights -> grey, the drop shadow -> grey (i.e.
it vanishes into the Workbench background, as a classic icon should).

Needs Pillow. Writes amifleet68.png next to this script.
    python3 draw.py
"""
import os
from PIL import Image

W, H = 46, 46
T = (0, 0, 0, 0)

img = Image.new("RGBA", (W, H), T)
px = img.load()


def put(x, y, c):
    if 0 <= x < W and 0 <= y < H:
        px[x, y] = tuple(int(round(v)) for v in c[:3]) + (255,)


def get(x, y):
    return px[x, y]


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def inside_round(x, y, x0, y0, x1, y1, r):
    """Is (x, y) inside the inclusive rect with corners rounded by r?"""
    if not (x0 <= x <= x1 and y0 <= y <= y1):
        return False
    cx = x0 + r if x < x0 + r else (x1 - r if x > x1 - r else x)
    cy = y0 + r if y < y0 + r else (y1 - r if y > y1 - r else y)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r + 0.5


def is_edge(x, y, x0, y0, x1, y1, r):
    if not inside_round(x, y, x0, y0, x1, y1, r):
        return False
    return any(not inside_round(x + dx, y + dy, x0, y0, x1, y1, r)
               for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))


# ---- palette --------------------------------------------------------------
outline    = (40, 34, 30)       # warm near-black           -> black
shadow_in  = (114, 114, 118)    # drop shadow core          -> grey
shadow_out = (140, 140, 144)    # drop shadow fringe        -> grey
case_top   = (255, 252, 242)    # case gradient, top        -> white
case_bot   = (234, 226, 204)    # case gradient, bottom     -> white
case_hi    = (255, 255, 255)    # top/left rim highlight    -> white
case_bev   = (196, 184, 156)    # bottom/right bevel        -> grey
case_bev2  = (170, 158, 130)    # outermost bevel row       -> grey
recess_dk  = (120, 108, 90)     # screen recess, top/left   -> grey
recess_lt  = (255, 255, 250)    # screen recess, bottom/right -> white
scr_top    = (72, 132, 214)     # lit screen, top           -> blue
scr_bot    = (40, 86, 164)      # lit screen, bottom        -> blue
scr_gloss  = (90, 148, 226)     # glossy reflection band    -> blue
tile_ol    = (14, 26, 58)       # tile outline              -> black
tile_top   = (252, 253, 255)    # tile body gradient        -> white
tile_bot   = (206, 214, 228)    # tile body gradient        -> white
tile_scr   = (80, 140, 224)     # tile's little lit screen  -> blue
tile_scr_b = (40, 88, 172)      # ...its lower row          -> blue
tile_scr_hi = (255, 255, 255)   # glint on that screen      -> white
bus        = (150, 236, 255)    # network bus               -> white
bus_node   = (255, 255, 255)    # bus junctions             -> white
green      = (60, 220, 96)      # status light              -> grey
green_dk   = (8, 78, 30)  
amber      = (255, 184, 40)     # status light              -> grey/white
amber_dk   = (104, 52, 0)
stand_l    = (240, 232, 212)    # stand neck, lit side
stand_r    = (178, 166, 138)    # stand neck, shaded side
power      = (70, 230, 110)     # power LED on the chin

# ---- geometry -------------------------------------------------------------
CX0, CY0, CX1, CY1 = 2, 2, 42, 33       # case
SX0, SY0, SX1, SY1 = 6, 6, 38, 27       # screen recess (outer edge)
NX0, NX1, NY0, NY1 = 18, 26, 34, 37     # stand neck
BX0, BY0, BX1, BY1 = 11, 37, 33, 41     # stand base

# ---- 1. drop shadow (lower right, two tones) -------------------------------
def shadow_shape(x, y):
    return (inside_round(x, y, CX0, CY0, CX1, CY1, 4)
            or (NX0 <= x <= NX1 and NY0 <= y <= NY1)
            or inside_round(x, y, BX0, BY0, BX1, BY1, 2))

for y in range(H):
    for x in range(W):
        if shadow_shape(x - 2, y - 2):
            put(x, y, shadow_in)
        elif shadow_shape(x - 3, y - 3) or shadow_shape(x - 1, y - 3):
            put(x, y, shadow_out)

# ---- 2. stand --------------------------------------------------------------
for y in range(NY0, NY1 + 1):
    for x in range(NX0, NX1 + 1):
        if x in (NX0, NX1):
            put(x, y, outline)
        else:
            t = (x - NX0 - 1) / (NX1 - NX0 - 2)
            c = mix(stand_l, stand_r, t)
            if y == NY0:
                c = outline                    # contact shadow under the case
            put(x, y, c)

for y in range(BY0, BY1 + 1):
    for x in range(BX0, BX1 + 1):
        if not inside_round(x, y, BX0, BY0, BX1, BY1, 2):
            continue
        if is_edge(x, y, BX0, BY0, BX1, BY1, 2):
            put(x, y, outline)
            continue
        t = (y - BY0 - 1) / (BY1 - BY0 - 2)
        c = mix(case_top, case_bev, t * 0.9)
        if y == BY0 + 1:
            c = case_hi
        if y == BY1 - 1:
            c = case_bev2
        put(x, y, c)
# the neck casts a short shadow onto the base
for x in range(NX0 + 1, NX1 + 1):
    put(x, BY0 + 1, mix(case_bev, outline, 0.2))
    put(x, BY0, outline)
for x in (NX0, NX1):
    put(x, BY0, outline)

# ---- 3. case ---------------------------------------------------------------
R = 4
for y in range(CY0, CY1 + 1):
    for x in range(CX0, CX1 + 1):
        if not inside_round(x, y, CX0, CY0, CX1, CY1, R):
            continue
        if is_edge(x, y, CX0, CY0, CX1, CY1, R):
            put(x, y, outline)
            continue
        t = (y - CY0) / (CY1 - CY0)
        c = mix(case_top, case_bot, t)
        # rim light on the top and left, bevel on the bottom and right
        if not inside_round(x, y - 1, CX0, CY0, CX1, CY1, R) or \
           not inside_round(x, y - 2, CX0, CY0, CX1, CY1, R) or \
           not inside_round(x - 2, y, CX0, CY0, CX1, CY1, R):
            c = case_hi
        if not inside_round(x + 2, y, CX0, CY0, CX1, CY1, R) or \
           not inside_round(x, y + 2, CX0, CY0, CX1, CY1, R):
            c = case_bev
        if not inside_round(x + 3, y + 3, CX0, CY0, CX1, CY1, R) and \
           (not inside_round(x, y + 2, CX0, CY0, CX1, CY1, R)
            or not inside_round(x + 2, y, CX0, CY0, CX1, CY1, R)):
            c = case_bev2 if (x >= CX1 - 1 or y >= CY1 - 1) else case_bev
        put(x, y, c)

# power LED on the chin, bottom right, with a little glow
put(35, 30, power)
put(36, 30, power)
put(35, 31, mix(power, case_bot, 0.5))
put(36, 31, mix(power, case_bot, 0.5))

# ---- 4. screen recess + lit screen ----------------------------------------
for y in range(SY0, SY1 + 1):
    for x in range(SX0, SX1 + 1):
        if not inside_round(x, y, SX0, SY0, SX1, SY1, 2):
            continue
        # recess bevel: dark top/left (in shadow), light bottom/right (lit lip)
        if is_edge(x, y, SX0, SY0, SX1, SY1, 2):
            if y == SY1 or x == SX1 or (x - SX0) + (SY1 - y) > (SX1 - x) + (y - SY0) + 20:
                put(x, y, recess_lt)
            else:
                put(x, y, recess_dk)
            continue
        if is_edge(x, y, SX0 + 1, SY0 + 1, SX1 - 1, SY1 - 1, 1):
            put(x, y, outline)
            continue
        t = (y - SY0) / (SY1 - SY0)
        c = mix(scr_top, scr_bot, t)
        # glossy diagonal reflection across the upper left
        d = (x - SX0) + (y - SY0) * 1.6
        if 6 <= d <= 15:
            c = mix(c, scr_gloss, 0.55 if 8 <= d <= 13 else 0.3)
        put(x, y, c)

# ---- 5. fleet board: three linked machine tiles ----------------------------
TW, TGAP = 7, 3
TX = [9, 9 + TW + TGAP, 9 + 2 * (TW + TGAP)]   # 9, 19, 29
TY0, TY1 = 10, 19
lights = [(green, green_dk), (green, green_dk), (amber, amber_dk)]

for tx, (lc, ldk) in zip(TX, lights):
    x0, x1 = tx, tx + TW - 1
    for y in range(TY0, TY1 + 1):
        for x in range(x0, x1 + 1):
            if not inside_round(x, y, x0, TY0, x1, TY1, 1):
                continue
            if is_edge(x, y, x0, TY0, x1, TY1, 1):
                put(x, y, tile_ol)
                continue
            t = (y - TY0) / (TY1 - TY0)
            c = mix(tile_top, tile_bot, t)
            if x == x1 - 1 or y == TY1 - 1:
                c = mix(c, (150, 160, 182), 0.6)      # tile's own bevel
            put(x, y, c)
    # tiny screen on each machine, with a glint
    for y in range(TY0 + 2, TY0 + 5):
        for x in range(x0 + 2, x1 - 1):
            put(x, y, tile_scr if y < TY0 + 4 else tile_scr_b)
    put(x0 + 2, TY0 + 2, tile_scr_hi)
    # status light: 3x2 with a hot centre and a darker rim
    ly = TY0 + 6
    put(x0 + 2, ly, ldk); put(x0 + 3, ly, lc); put(x0 + 4, ly, ldk)
    put(x0 + 2, ly + 1, ldk); put(x0 + 3, ly + 1, ldk); put(x0 + 4, ly + 1, ldk)
    put(x0 + 3, ly, mix(lc, (255, 255, 255), 0.45))

# network bus: a stub down from each tile to a glowing line
BUSY = 23
for tx in TX:
    cx = tx + 3
    for y in range(TY1 + 1, BUSY):
        put(cx, y, bus)
    put(cx, BUSY, bus_node)
for x in range(TX[0] + 1, TX[2] + TW - 1):
    if get(x, BUSY)[:3] != bus_node:
        put(x, BUSY, bus)
# soft glow row under the bus
for x in range(TX[0] + 1, TX[2] + TW - 1):
    c = get(x, BUSY + 1)[:3]
    put(x, BUSY + 1, mix(c, bus, 0.35))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "amifleet68.png")
img.save(out)
print("wrote", out, f"{W}x{H}")
