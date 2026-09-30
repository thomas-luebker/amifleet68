#!/usr/bin/env python3
"""draw_extra.py - pixel-draws the two companion icons for the release archive,
in the same family as draw.py's monitor (same palette, same beige bezel,
Workbench-blue strip, grey screen with embossed machine tiles):

  drawer.png  a beige folder (back panel + tab, front panel) carrying a small
              copy of the amifleet68 monitor - the "amifleet68" drawer.
  guide.png   a blue hard-cover book with white page edges and the monitor on
              its cover - the AmigaGuide manual.

Colours are the draw.py ones, so the nearest-colour mapping to the 4-colour
Workbench palette in mkicon.py gives the same clean classic look:
beige -> white, bevel/screen -> grey, outline/text -> black, blue -> blue.

Needs Pillow. Writes drawer.png and guide.png next to this script.
    python3 draw_extra.py
"""
import os
from PIL import Image

T = (0, 0, 0, 0)
outline   = (24, 24, 28)
bezel_hi  = (244, 240, 228)
bezel     = (230, 222, 200)
bezel_lo  = (222, 212, 188)
bevel     = (184, 172, 146)
screen    = (168, 168, 170)
tile      = (184, 184, 186)
textc     = (44, 44, 50)
blue      = (16, 96, 184)
blue_dk   = (0, 60, 130)
blue_hi   = (60, 130, 210)
paper     = (250, 250, 252)
paper_ln  = (170, 170, 176)

HERE = os.path.dirname(os.path.abspath(__file__))


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.img = Image.new("RGBA", (w, h), T)
        self.px = self.img.load()

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[x, y] = c + (255,)

    def rect(self, x0, y0, x1, y1, c):          # inclusive
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.put(x, y, c)

    def box(self, x0, y0, x1, y1, fill, edge, r=0):
        """Filled rect, 1-px outline, corners cut by r pixels."""
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx, dy = min(x - x0, x1 - x), min(y - y0, y1 - y)
                if dx + dy < r:
                    continue
                on_edge = dx == 0 or dy == 0 or dx + dy == r
                self.put(x, y, edge if on_edge else fill)

    def mini_monitor(self, x0, y0):
        """The amifleet68 monitor at 18x17: bezel, blue strip with three
        lights, two machine tiles, neck and foot. (x0, y0) = top-left."""
        self.box(x0, y0, x0 + 17, y0 + 12, bezel, outline, r=1)
        self.rect(x0 + 1, y0 + 11, x0 + 16, y0 + 11, bevel)     # bottom bevel
        self.rect(x0 + 16, y0 + 1, x0 + 16, y0 + 11, bevel)     # right bevel
        sx0, sy0, sx1, sy1 = x0 + 2, y0 + 2, x0 + 15, y0 + 10
        self.box(sx0, sy0, sx1, sy1, screen, outline)
        self.rect(sx0 + 1, sy0 + 1, sx1 - 1, sy0 + 2, blue)
        for i, c in enumerate(((72, 204, 104), (72, 204, 104), (255, 188, 56))):
            self.put(sx0 + 2 + 2 * i, sy0 + 1, c)
        self.rect(sx0 + 1, sy0 + 4, sx0 + 5, sy1 - 1, tile)     # two tiles
        self.rect(sx0 + 7, sy0 + 4, sx1 - 1, sy1 - 1, tile)
        self.rect(sx0 + 2, sy0 + 5, sx0 + 4, sy0 + 5, textc)
        self.rect(sx0 + 8, sy0 + 5, sx1 - 2, sy0 + 5, textc)
        # neck and foot
        self.box(x0 + 7, y0 + 12, x0 + 10, y0 + 14, bevel, outline)
        self.box(x0 + 4, y0 + 14, x0 + 13, y0 + 16, bezel, outline, r=1)


def drawer():
    c = Canvas(46, 36)
    # back panel with tab (drawn first, front overlaps it)
    c.box(2, 0, 17, 5, bevel, outline, r=1)
    c.box(1, 3, 44, 33, bevel, outline, r=1)
    c.rect(3, 1, 16, 1, bezel_lo)                  # tab highlight
    # front panel
    c.box(0, 9, 45, 35, bezel, outline, r=1)
    for y in range(10, 35):
        for x in range(1, 45):
            if c.px[x, y][:3] != bezel:
                continue
            col = bezel_lo if y > 22 else bezel
            if y in (10, 11) or x == 1:
                col = bezel_hi
            if y >= 34 or x >= 44:
                col = bevel
            c.put(x, y, col)
    c.mini_monitor(14, 13)
    return c.img


def guide():
    c = Canvas(40, 44)
    # page block (visible at right and bottom), then the cover over it
    c.box(6, 3, 38, 43, paper, outline, r=1)
    for y in (39, 41):
        c.rect(8, y, 37, y, paper_ln)
    for x in (35, 37):
        c.rect(x, 5, x, 38, paper_ln)
    c.box(1, 0, 34, 39, blue, outline, r=1)
    c.rect(2, 1, 6, 38, blue_dk)                   # spine
    c.rect(7, 1, 7, 38, outline)                   # hinge crease
    c.rect(8, 1, 33, 1, blue_hi)                   # top highlight
    c.rect(8, 1, 8, 38, blue_hi)
    # title label plate with the monitor on it
    c.box(10, 5, 31, 25, bezel, outline, r=1)
    c.rect(11, 24, 30, 24, bevel)
    c.rect(30, 6, 30, 24, bevel)
    c.mini_monitor(12, 7)
    # title "text" under the plate
    c.rect(11, 29, 30, 30, bezel_hi)
    c.rect(14, 33, 27, 33, bezel_hi)
    return c.img


if __name__ == "__main__":
    for name, fn in (("drawer.png", drawer), ("guide.png", guide)):
        img = fn()
        out = os.path.join(HERE, name)
        img.save(out)
        print("wrote", out, f"{img.width}x{img.height}")
