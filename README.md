# amifleet68

The amiagent fleet console, **native on the Amiga**, as a MUI application.
The 68k sibling of [`amimcp/tools/amifleet`](../amimcp/tools/amifleet) (the
macOS/SwiftUI app): it speaks the amimcp wire protocol
([`amimcp/PROTOCOL.md`](../amimcp/PROTOCOL.md)) straight over
`bsdsocket.library` — no MCP server, no Mac in the loop.

## What it does

- **Fleet board** — one row per machine, polled every 5 s over `INFO`:
  online/offline, round-trip time, agent version, CPU, OS release, free
  Chip/Fast RAM. A machine that drops off keeps its last good report, in
  italics. A machine running a command shows **busy** (see below).
- **Details** (double-click a row) — three pages:
  - **Info**: the full `INFO` report.
  - **Shell**: AmigaDOS commands through `EXEC`, with return code and time.
    **Break** sends Ctrl-C to a command the agent gave up waiting for.
  - **Files**: a directory browser over `LIST`; double-click to descend,
    **Parent** to go up. **Download...** saves the selected file locally
    (`GETRANGE` in 64 KB chunks, so size is not limited by memory);
    **Upload...** sends a local file into the current drawer (one `PUT`
    streamed from disk, up to the protocol's 16 MB frame). Live progress.
- **Screen** — the machine's front screen through the agent: a cheap `HASH`
  every second, a full `SHOT` only when it changed, scaled to the window.
  Clicks (as one `CLICK`, drags as a timed `SCRIPT`) and keys (Amiga rawkeys,
  forwarded 1:1) go to the machine while the pointer is over the picture.
  Works on any screen, even a Guru; a big RTG frame takes seconds.
- **VNC** — an RFB 3.3 client for [AmiVNC](http://aminet.net/comm/tcp/AmiVNC.lha).
  If nothing listens, it starts AmiVNC through the agent first (password,
  then `Run >NIL: <NIL:`), exactly as the macOS amifleet does. **Fast**
  uses AmiVNC's `-a` BGR233 mode on port 5901 (1 byte/pixel); otherwise
  full colour on 5900. Mouse (with motion) and keyboard (X keysyms via the
  local keymap) are forwarded.
- **Copy files** — two panes, each on any machine of the board (the local
  one included, through its own agent). Copy files and whole drawers either
  way, Delete, Rename, MakeDir; progress and Cancel. Same machine: one
  AmigaDOS `Copy ALL CLONE` there. Between machines: streamed agent to agent
  (`GETRANGE` → `PUT`, 256 KB pieces; files over 15 MB in 8 MB parts that
  are `Join`ed at the destination). 21 MB A4000 → PiStorm: 69.7 s, every
  file SHA-256-identical.
- **Drive popups** — every path field is a MUI Popobject listing that
  machine's volumes, assigns and devices (`Assign LIST` there, cached).
- **Scan network** — sweeps `x.y.z.1–254` for port 7846 (16 parallel
  non-blocking connects per batch, ~25 s for a /24), `PING`s every hit with
  the given token, and adds the agents it finds. The network is pre-filled
  from this Amiga's own address (`gethostid()`).
- **Add / Edit / Remove** — name, host, port, token.
- **ARexx** port **`AMIFLEET.1`** — MUI's built-ins (`QUIT`, `HIDE`, `SHOW`…)
  plus:

  | Command | |
  |---|---|
  | `MACHINES` | one line per machine: `name state ms agent cpu os` |
  | `STATE NAME/A` | `online` / `offline` / `token` / `busy` / `unknown`; RC 10 if no such machine |
  | `POLL` | poll every machine now |
  | `ADD NAME/A,HOST/A,PORT/N,TOKEN/K` | add a machine |
  | `DETAILS`, `SCREEN`, `VNC` `NAME/A` | open that window on a machine |
  | `CONNECT` / `DISCONNECT` | the VNC window's session |
  | `COPYFILES` | open the Copy files window |

  ```rexx
  /* who is up? */
  options results
  address 'AMIFLEET.1'
  'MACHINES'
  say RESULT
  ```

Machines persist in `ENV:amifleet68.prefs` and `ENVARC:amifleet68.prefs`,
one per line: `name<TAB>host<TAB>port<TAB>token`. A first run starts with
this Amiga's own agent (`127.0.0.1`).

## Requirements

- AmigaOS 2.04+ (built for a plain 68000), **MUI 3.8+** (`muimaster.library`
  v19 — `amipkg install mui38`), and a running TCP/IP stack
  (`bsdsocket.library` v4: Roadshow, AmiTCP, Miami).
- `amiagent` on each machine you want to see.

## MUI style guide

Checked against the guide in the MUI 3.8 developer kit and the Amiga UI
Style Guide:

- **Fits 640×256 topaz/8.** `amifleet68 PALTEST` opens every window on a
  real 640×256 PAL screen; board, Details, Copy, Screen and VNC all fit
  without MUI shrinking fonts (verified on the A4000). The toolbar is two
  rows because one was ~720 px wide. PALTEST keeps its own window geometry
  (a separate application base), so a test never shrinks your layout.
- **Popups for choices**, ASL for local files.
- **Keyboard**: every button has a key, labelled string gadgets have
  control chars, everything is in the Tab cycle chain, the board list is
  the default object (cursor keys work at once). No menu shortcut on
  Amiga-C/V/X — Intuition would steal them from string gadgets.
- **Menus**: Project (…, Quit last), Fleet, Windows, Settings (MUI).
- **Help** on any window opens its node of the AmigaGuide; the screen title
  shows the program; iconify uses the program's own icon.

## Things the fleet taught this app

**VNC pacing.** The first session against a real AmiVNC (A4000 → PiStorm,
2026-09-30) crashed the PiStorm after ~25 s. The client had been asking for
the next update the instant the last one arrived, and AmiVNC answers at
once, so it ran ~37 round trips a second; the agent there starved first,
then the machine went. `tests/fake_amivnc.py --eager` reproduces the loop
(~100/s from an A4000). The client now waits ≥100 ms between requests
(250 ms after an empty update): ~4–5/s against the same server, with input
still sent instantly. **Re-tested on the rebooted PiStorm the same evening:**
two minutes of VNC including the double-click, its agent answering every
5-second probe in ~0.01 s — no crash.

**AmiVNC ignores the mouse on the PiStorm.** AmiVNC 1.0.0 on the PiStorm
(Emu68, Picasso96) does not move the pointer for *any* RFB client — a
minimal Python client sending jumps, 4-pixel steps and button presses moved
it 0 pixels, while amiagent's own clicks move it fine. Picture and keys
work; use the Screen window to click there.

**MUI 3.8 `MUIM_Draw` flags.** On muimaster 19.35, `MUIM_Draw` arrives with
`flags == 0` for both `MUI_Redraw(MADF_DRAWOBJECT)` and `MADF_DRAWUPDATE`.
The view keeps its own "partial update pending" flag instead.

**The agent is single-connection** — see below.

## One thing the protocol forces on the UI

`amiagent` serves **one connection at a time**. While an `EXEC` is inside its
deadline, that machine answers nothing else — not a poll, not `BREAK`. So:

- `EXEC` runs with a **20 s deadline**. After that the agent answers *"still
  running"* and is reachable again, and that is when **Break** is enabled.
- The board does not poll a machine while its `EXEC` is in flight; the row
  says **busy** instead of falsely going offline.

Related: a machine is only shown **offline after two missed polls** — one
miss is usually just the agent serving somebody else — and it is not polled
at all while a `SHOT` or `EXEC` of ours is in flight.

## Testing without risking a machine

```sh
python3 tests/fake_amivnc.py --port 5901 --bpp 8           # like AmiVNC -a
python3 tests/fake_amivnc.py --port 5900 --bpp 16 --eager  # the 16-bit quirk, eager server
```

An RFB server that behaves like AmiVNC and **checks every byte the client
sends** (message types, lengths, coordinates, button masks, request rate),
logging violations instead of crashing. Its picture has colour swatches, a
bar that changes 5×/s, a box that toggles on each click and a crosshair
where the client says the pointer is — a screenshot proves input arrived.
Point a VNC window at the Mac's address to use it.

```sh
cc -o /tmp/des_test tests/des_test.c src/des.c && /tmp/des_test   # DES vs FIPS 46
```

## Download

Releases, with the `.lha` for the Amiga:
<https://github.com/thomas-luebker/amifleet68/releases>

The archive carries the program icon as a GlowIcon (with a clean 4-colour
image for OS 3.1 in the same file) and, in `Classic icon/`, the same icon
without its GlowIcon part - copy it over `amifleet68.info` if you prefer plain
icons.

`tools/install_fleet.py <lha> <node>...` installs a release on fleet Amigas
through their agents: unpacks it with the machine's own `lha` (default
`SYS:Programs/`, `--dest` to change), writes the fleet list if the machine
has none, and adds amifleet68 to WBDock (config backed up once, entry added
live over its ARexx port).

`./release.sh` builds `dist/amifleet68-<version>.lha` (Aminet-style drawer
with icons, both CPU builds, the AmigaGuide manual) and its `.readme`.

## Build

```sh
make                 # amifleet68      (68000, ~190 KB)
make CPU=68020       # amifleet68.020
```

bebbo's `m68k-amigaos-gcc` 6.5 at `~/opt/amiga/bin/` (see the `amiga-68k`
skill), same flags as amipkg and amiagent. Headers: `vendor/mui/include`
(MUI 3.8 developer kit, freely distributable).

## How it's built

| File | What |
|---|---|
| `src/main.c` | The MUI GUI and the ARexx commands. Never touches a socket. |
| `src/worker.c` | Network workers: five AmigaDOS processes (`poll`, `shell`, `misc`, `screen`, `vnc`), each with **its own** `bsdsocket.library` and `timer.device` base. Agent requests, SHOT→0RGB conversion, file transfer. No libc in here — newlib's malloc/stdio are not re-entrant. |
| `src/vnc.c` | The RFB client, run as one long job on the `vnc` worker. |
| `src/view.c` | `FrameView`, a MUI Area subclass: scales a 0RGB frame into its box (CyberGraphX `WritePixelArray` on RTG, a 6×6×6 pen cube + `WriteChunkyPixels` elsewhere) and reports clicks/keys in remote coordinates. |
| `src/des.c` | DES for VNC auth, FIPS-46-tested on the host. |
| `src/net.h` | Socket helpers shared by the worker files. |
| `icon/` | `amifleet68.info` (classic + GlowIcon) and the script that rebuilds it. |
| `tests/` | `fake_amivnc.py`, `des_test.c`. |
| `src/fleet.h` | The `Job` message the GUI and workers exchange. |
| `src/proto.h` | Protocol constants, copied from `amimcp/agent/proto.h`. |
| `src/muistubs.c` | Out-of-line MUI varargs stubs — **must stay its own translation unit** (GCC 6 drops variadic args of inline stubs; see its header). |

The GUI `PutMsg()`s a `Job` to a worker and gets it back on its reply port
when the conversation is over, so a switched-off machine's 2 s connect
timeout or a long `EXEC` never freezes the window. `BREAK` rides the poll
worker, because the shell worker is the one waiting on the command.

The bsdsocket and timer inlines are pointed at the worker's own bases with
`BSDSOCKET_BASE_NAME` / `TIMER_BASE_NAME`, so there is no global
`SocketBase` — a socket base belongs to the task that opened it.

## Tested

**0.2.0**, 2026-09-30, from the A4000: Screen on the PiStorm (1280×720 in
1.9 s, a double-click through the picture opened RAM Disk there), VNC to the
real AmiVNC on the PiStorm (auto-started, logged in — then the crash above),
VNC against the fake server in 8/16/32 bpp with clicks, a double-click and
typing checked byte-by-byte (0 violations), download (200,000 bytes,
SHA-256 identical) and upload (191,936 bytes, identical) against the Amigo
guest, and every ARexx command.

**0.1.0**: on the real A4000 (68060, OS 3.2.3, MUI 19.35, Roadshow 4.364), 2026-09-30,
against the A4000 itself, the PiStorm and the Amigo guest on the Mac:
board, Info, Shell, Files (descend + parent), Break of a stuck `wait 40`,
LAN scan (3 agents found), Add window, and a clean quit via ARexx with all
three worker processes gone afterwards.

Same trust model as the rest of amimcp: the token travels in cleartext. Keep
it on a LAN you trust.
