# amifleet68 — Agent Guide

## Long-term memory: the Obsidian vault

Durable state lives in the **Loki** vault:

```
~/Library/Mobile Documents/iCloud~md~obsidian/Documents/Loki/20 - Private/Retro Computing/Projects/amifleet68.md
```

Read its `## Status` and `## Next Actions` at the start of a session; update
it when the state changes. Directory-wide rules: `~/Development/CLAUDE.md`.

## What this is

A MUI 3.8 application for AmigaOS (68000+) that watches and drives every
machine running `amiagent`. Sibling of the macOS `amimcp/tools/amifleet`.
See README.md for the architecture.

## Rules

- **Test on the fleet, not by reasoning** — use the `amiga-fleet` skill;
  claim the machine (`fleet.py A4000 --claim --as <name>`) before driving its
  GUI. Quit a running copy with ARexx: `address 'AMIFLEET68.1'; 'quit'`.
- `src/worker.c` and `src/vnc.c` run on separate processes: **no malloc, no
  stdio** there. Exec/dos/bsdsocket only.
- **Test VNC against `tests/fake_amivnc.py` first**, not a real AmiVNC: the
  first real session crashed the PiStorm (2026-09-30). The fake server logs
  every protocol violation and the request rate.
- Don't trust `MUIM_Draw`'s `flags` on MUI 19.35 — they arrive as 0.
- `src/muistubs.c` must stay a separate translation unit.
- `src/proto.h` is a copy of `amimcp/agent/proto.h`; re-copy, don't fork.
- No tokens or private hosts in the repo — prefs live on the Amiga in
  `ENV:`/`ENVARC:amifleet68.prefs`.
