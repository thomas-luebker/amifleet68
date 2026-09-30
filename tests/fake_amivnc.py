#!/usr/bin/env python3
"""fake_amivnc.py - an RFB 3.3 server that behaves like AmiVNC, for testing
amifleet68's VNC client without risking a real Amiga.

Why: the first real session (A4000 -> PiStorm AmiVNC, 2026-09-30) ended with
the PiStorm crashed. This server speaks what AmiVNC speaks and CHECKS every
byte the client sends - message types, lengths, coordinates, button masks,
request rate - so a malformed or over-eager client shows up as a logged
violation instead of a Guru.

AmiVNC traits reproduced (see amimcp tools/amifleet RFBClient.swift):
  --bpp 8   BGR233 true colour, what `AmiVNC -a` serves ("Fast")
  --bpp 16  RGB565 flagged LITTLE-endian but sent BIG-endian (the quirk)
  --bpp 32  flagged little-endian and really little-endian
  Raw and CopyRect only; RFB 3.3 with security type None (--auth none) or
  VNC auth (--auth vnc: the response is length-checked, not verified).

The picture: a grey desktop, a clock bar that changes 5x a second, a box
that toggles colour on every click, and a crosshair wherever the client
says the pointer is - so a screenshot of the viewer proves input arrived.

Standard library only. Run:  python3 tests/fake_amivnc.py --port 5901 --bpp 8
"""

import argparse
import socket
import struct
import threading
import time

W, H = 800, 480


class Frame:
    def __init__(self):
        self.lock = threading.Lock()
        self.px = [[(170, 170, 170)] * W for _ in range(H)]
        self.dirty = None               # (x0, y0, x1, y1) or None
        self.box_on = False
        self.cursor = None
        self.version = 0

    def fill(self, x0, y0, x1, y1, rgb):
        for y in range(max(0, y0), min(H, y1)):
            row = self.px[y]
            for x in range(max(0, x0), min(W, x1)):
                row[x] = rgb
        self.damage(x0, y0, x1, y1)

    def damage(self, x0, y0, x1, y1):
        x0, y0, x1, y1 = max(0, x0), max(0, y0), min(W, x1), min(H, y1)
        if x1 <= x0 or y1 <= y0:
            return
        if self.dirty is None:
            self.dirty = (x0, y0, x1, y1)
        else:
            a = self.dirty
            self.dirty = (min(a[0], x0), min(a[1], y0), max(a[2], x1), max(a[3], y1))
        self.version += 1

    def draw_static(self):
        self.fill(0, 0, W, 14, (255, 255, 255))                 # a "title bar"
        self.fill(0, 14, W, H, (170, 170, 170))
        for i, rgb in enumerate([(255, 0, 0), (0, 255, 0), (0, 0, 255),
                                 (255, 255, 0), (0, 255, 255), (255, 0, 255)]):
            self.fill(20 + i * 60, 40, 70 + i * 60, 90, rgb)    # colour swatches
        self.draw_box()

    def draw_box(self):
        self.fill(W - 220, 40, W - 20, 140, (0, 160, 0) if self.box_on else (160, 0, 0))

    def tick(self, t):
        # a bar whose length is the tenths of a second: changes every 200 ms
        n = int(t * 5) % 50
        self.fill(20, H - 40, 20 + 700, H - 20, (100, 100, 100))
        self.fill(20, H - 40, 20 + n * 14, H - 20, (255, 170, 0))

    def pointer(self, x, y):
        if self.cursor:
            cx, cy = self.cursor
            self.fill(cx - 8, cy, cx + 9, cy + 1, (170, 170, 170))
            self.fill(cx, cy - 8, cx + 1, cy + 9, (170, 170, 170))
        self.cursor = (x, y)
        self.fill(x - 8, y, x + 9, y + 1, (0, 0, 0))
        self.fill(x, y - 8, x + 1, y + 9, (0, 0, 0))


def encode(rgb, bpp):
    r, g, b = rgb
    if bpp == 8:        # BGR233: r 3 bits @0, g 3 bits @3, b 2 bits @6
        return bytes([(r >> 5) | ((g >> 5) << 3) | ((b >> 6) << 6)])
    if bpp == 16:       # RGB565, sent big-endian despite the LE flag
        return struct.pack(">H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return struct.pack("<I", (r << 16) | (g << 8) | b)


def pixel_format(bpp):
    if bpp == 8:
        return struct.pack(">BBBBHHHBBB3x", 8, 8, 0, 1, 7, 7, 3, 0, 3, 6)
    if bpp == 16:
        return struct.pack(">BBBBHHHBBB3x", 16, 16, 0, 1, 31, 63, 31, 11, 5, 0)
    return struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)


class Session:
    def __init__(self, conn, addr, args):
        self.c, self.addr, self.args = conn, addr, args
        self.f = Frame()
        self.f.draw_static()
        self.f.dirty = None
        self.want = None                # pending FramebufferUpdateRequest
        self.full = True
        self.cond = threading.Condition()
        self.alive = True
        self.stats = dict(requests=0, incremental=0, pointer=0, keys=0, updates=0,
                          bytes_out=0, violations=0)
        self.last_mask = 0

    def log(self, *a):
        print(f"[{time.strftime('%H:%M:%S')}] {self.addr[0]}:", *a, flush=True)

    def violation(self, what):
        self.stats["violations"] += 1
        self.log("VIOLATION:", what)

    def recv(self, n):
        b = b""
        while len(b) < n:
            chunk = self.c.recv(n - len(b))
            if not chunk:
                raise EOFError
            b += chunk
        return b

    def handshake(self):
        self.c.sendall(b"RFB 003.003\n")
        v = self.recv(12)
        self.log("client version", v)
        if v != b"RFB 003.003\n":
            self.violation(f"client asked for {v!r}, AmiVNC only does 3.3")
        if self.args.auth == "vnc":
            self.c.sendall(struct.pack(">I", 2) + bytes(range(16)))
            resp = self.recv(16)
            self.log("auth response", resp.hex())
            self.c.sendall(struct.pack(">I", 0))
        else:
            self.c.sendall(struct.pack(">I", 1))
        shared = self.recv(1)
        self.log("ClientInit shared =", shared[0])
        name = b"fake AmiVNC (amifleet68 test server)"
        self.c.sendall(struct.pack(">HH", W, H) + pixel_format(self.args.bpp)
                       + struct.pack(">I", len(name)) + name)

    def reader(self):
        try:
            while self.alive:
                t = self.recv(1)[0]
                if t == 0:
                    self.recv(3)
                    self.recv(16)
                    self.violation("SetPixelFormat sent - AmiVNC's clients should not")
                elif t == 2:
                    _, n = struct.unpack(">BH", self.recv(3))
                    encs = struct.unpack(f">{n}i", self.recv(4 * n))
                    self.log("SetEncodings", encs)
                    bad = [e for e in encs if e not in (0, 1)]
                    if bad:
                        self.violation(f"asked for encodings AmiVNC lacks: {bad}")
                elif t == 3:
                    inc, x, y, w, h = struct.unpack(">BHHHH", self.recv(9))
                    self.stats["requests"] += 1
                    self.stats["incremental"] += inc
                    if x + w > W or y + h > H or w == 0 or h == 0:
                        self.violation(f"update request out of bounds {x},{y} {w}x{h}")
                    with self.cond:
                        self.want = (inc, x, y, w, h)
                        if not inc:
                            self.full = True
                        self.cond.notify()
                elif t == 4:
                    down, key = struct.unpack(">B2xI", self.recv(7))
                    self.stats["keys"] += 1
                    if down > 1:
                        self.violation(f"key down flag {down}")
                    ch = f" {chr(key)!r}" if 32 <= key < 256 else ""
                    self.log(f"key {'down' if down else 'up  '} keysym 0x{key:04x}{ch}")
                elif t == 5:
                    mask, x, y = struct.unpack(">BHH", self.recv(5))
                    self.stats["pointer"] += 1
                    if mask > 7:
                        self.violation(f"button mask {mask}")
                    if x >= W or y >= H:
                        self.violation(f"pointer outside the screen: {x},{y}")
                    if mask != self.last_mask:
                        self.log(f"pointer {x},{y} buttons {self.last_mask:03b} -> {mask:03b}")
                        if (mask & 1) and not (self.last_mask & 1):
                            with self.cond:
                                self.f.box_on = not self.f.box_on
                                self.f.draw_box()
                                self.cond.notify()
                    self.last_mask = mask
                    with self.cond:
                        self.f.pointer(min(x, W - 1), min(y, H - 1))
                        self.cond.notify()
                elif t == 6:
                    _, n = struct.unpack(">3sI", self.recv(7))
                    self.recv(n)
                    self.log("ClientCutText", n, "bytes")
                else:
                    self.violation(f"unknown client message type {t} - stream is now garbage")
                    break
        except (EOFError, ConnectionError, OSError):
            pass
        self.alive = False
        with self.cond:
            self.cond.notify()

    def rect_raw(self, x0, y0, x1, y1):
        bpp = self.args.bpp
        out = [struct.pack(">HHHHi", x0, y0, x1 - x0, y1 - y0, 0)]
        for y in range(y0, y1):
            row = self.f.px[y]
            out.append(b"".join(encode(row[x], bpp) for x in range(x0, x1)))
        return b"".join(out)

    def writer(self):
        start = time.time()
        next_tick = 0
        copy_every = 25
        while self.alive:
            with self.cond:
                now = time.time()
                if now >= next_tick:
                    self.f.tick(now - start)
                    next_tick = now + 0.2
                # --eager: answer every request at once, even with nothing
                # changed (0 rects) - what the real AmiVNC appears to do.
                ready = self.want is not None and (self.full or self.f.dirty or self.args.eager)
                if not ready:
                    self.cond.wait(max(0.01, next_tick - time.time()))
                    continue
                if self.full:
                    rect = (0, 0, W, H)
                else:
                    rect = self.f.dirty
                self.full = False
                self.f.dirty = None
                self.want = None
                rects = [self.rect_raw(*rect)] if rect else []
                # now and then, a CopyRect: shift the swatch row right by 2 px
                self.stats["updates"] += 1
                if rects and self.stats["updates"] % copy_every == 0:
                    rects.append(struct.pack(">HHHHiHH", 22, 40, 358, 50, 1, 20, 40))
                    for y in range(40, 90):
                        row = self.f.px[y]
                        row[22:380] = row[20:378]
                msg = struct.pack(">BxH", 0, len(rects)) + b"".join(rects)
            try:
                self.c.sendall(msg)
                self.stats["bytes_out"] += len(msg)
            except OSError:
                break
        self.alive = False

    def run(self):
        try:
            self.handshake()
        except (EOFError, OSError) as e:
            self.log("handshake failed:", e)
            return
        rd = threading.Thread(target=self.reader, daemon=True)
        wr = threading.Thread(target=self.writer, daemon=True)
        rd.start(); wr.start()
        t0 = time.time()
        last = dict(self.stats)
        while self.alive:
            time.sleep(5)
            s = dict(self.stats)
            dt = 5.0
            self.log(f"{(s['requests'] - last['requests']) / dt:.1f} req/s, "
                     f"{(s['updates'] - last['updates']) / dt:.1f} upd/s, "
                     f"{(s['bytes_out'] - last['bytes_out']) / dt / 1024:.0f} KB/s, "
                     f"pointer {s['pointer']}, keys {s['keys']}, violations {s['violations']}")
            last = s
        self.log(f"closed after {time.time() - t0:.0f} s: {self.stats}")
        self.c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5901)
    ap.add_argument("--bpp", type=int, choices=(8, 16, 32), default=8)
    ap.add_argument("--auth", choices=("none", "vnc"), default="vnc")
    ap.add_argument("--eager", action="store_true",
                    help="answer every update request immediately, even when nothing changed")
    args = ap.parse_args()
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", args.port))
    s.listen(1)
    print(f"fake AmiVNC on :{args.port}, {W}x{H} @ {args.bpp} bpp, auth {args.auth}", flush=True)
    while True:
        c, a = s.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        Session(c, a, args).run()


if __name__ == "__main__":
    main()
