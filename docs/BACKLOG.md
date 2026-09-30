# amifleet68 backlog

The fine-grained working list. The durable summary and the *why* live in the
Loki vault note `20 - Private/Retro Computing/Projects/amifleet68.md`.

## Bugs and risks — including in sibling repos

- [x] **Mac amifleet: unpaced VNC loop** (`amimcp/tools/amifleet`, `RFBView.swift`).
      It requests the next update the moment one arrives — the same loop that
      crashed the PiStorm from amifleet68 (~37 round trips/s against AmiVNC).
      Port amifleet68's pacing (≥100 ms, 250 ms after an empty update).
- [x] **amiagent: `LIST` reports soft links as directories** (`amimcp/agent`).
      Seen on the Amigo's `RAM:Disk.info`; the Files tab and the copier show it
      as "(dir)", and a recursive copy would try to descend into it.
- [ ] **AmiVNC ignores all pointer input on the PiStorm** (Emu68, P96), from any
      client. AmiVNC ships its C source — find out why (mouse insertion vs.
      Emu68/P96) and whether a patched build is worth it.

## Testing gaps

- [x] **Planar drawing path** (Screen/VNC on a non-RTG screen: 6×6×6 pen cube +
      `WriteChunkyPixels`) has never run. `amifleet68 PALTEST` on the A4000
      opens a native 3-plane PAL screen — use it.
- [ ] Classic 4-colour icons on a real OS 3.1 `icon.library` (only previewed).
- [ ] The 68000 build on a real 68000 (only the 68060/68040 fleet so far).

## Features

- [x] **Upload over 16 MB** in the Files tab (one PUT is capped): reuse the
      copier's parts + `Join`.
- [x] **Move** in the copier (copy, then delete the source), and an
      overwrite question per existing file instead of one up-front warning.
- [x] Remember each copier pane's machine and path between runs.
- [ ] Per-machine VNC password and Fast setting, saved in the prefs.
- [x] Shell tab: command history (a popup of recent commands, per the style guide).
- [ ] ARexx `EXEC NAME/A,CMD/F` returning the output synchronously.
- [ ] Redraw `drawer.png` / `guide.png` in the new shaded style (the tool
      icon was redone in 0.4.1; its siblings still carry the flat monitor).
- [ ] Locale catalogs (German first — the fleet's own keymap).

## Distribution

- [ ] amipkg / amiga-pkg catalog entry (deps: mui38, a TCP stack).
- [ ] Aminet `comm/net` — `.readme` in `docs/`; the upload form was down, FTP
      means a removal mail per later version (see the `amiga-release` skill).
- [ ] Install on the iPad node once it is up:
      `tools/install_fleet.py dist/amifleet68-<ver>.lha iPad`.
- [ ] Post the current release link in the imp3 chat (0.4.0's is there).

## Done

- [x] 0.5.0 — Move, overwrite question, big uploads, shell history, remembered
      panes, AmigaOS UI Style Guide pass (NTSCTEST 640x200 verified) (2026-10-01)
- [x] amimcp fixes committed locally, NOT pushed/released: Mac amifleet VNC
      pacing (3a7007f; ~24,000 → ~6 req/s against an eager server) and
      amiagent LIST soft links as `L` (58e7252; agent 0.13.1 unreleased)

- [x] 0.4.1 — new shaded GlowIcon + classic-only icon; fleet installer; on
      A4000, PiStorm, Amigo and in their WBDocks (2026-09-30)
- [x] 0.4.0 — file copy, drive popups, MUI style guide pass (2026-09-30)
- [x] 0.3.0 — first public release: Screen, VNC, transfers, ARexx (2026-09-30)
