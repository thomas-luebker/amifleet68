#!/usr/bin/env python3
"""install_fleet.py - install an amifleet68 release on fleet Amigas, through
their amiagent, and put it in their WBDock.

    python3 tools/install_fleet.py dist/amifleet68-0.4.1.lha PiStorm Amigo
    python3 tools/install_fleet.py dist/amifleet68-0.4.1.lha A4000 --dest Porgrams:

Per machine:
  1. quits a running amifleet68 (ARexx AMIFLEET68.1, or AMIFLEET.1 before
     0.5.0), so the files can be replaced;
  2. unpacks the archive with the machine's own C:lha into DEST (default
     SYS:Programs/), which yields DEST/amifleet68/ + its drawer icon;
  3. writes ENVARC:+ENV:amifleet68.prefs if the machine has none: the whole
     fleet from the Mac amifleet's preferences, this machine as 127.0.0.1;
  4. WBDock: finds its config beside the WBDock program, backs it up once
     (<config>.pre-amifleet68), appends the entry, and ADDs it live through
     the WBDOCK ARexx port so it shows without a restart.

Idempotent: re-running updates the files and adds nothing twice.
Hosts and tokens come from the amiga-fleet skill (amifleet's preferences),
never from this repo. Python standard library only.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.expanduser("~/Development/AllAmigaTooling/skills/amiga-fleet/scripts"))
from fleet import connect, nodes as load_nodes  # noqa: E402
import socket  # noqa: E402

DOCK_DIRS = ["SYS:WBStartup", "SYS:Programs/WBDock", "SYS:Utilities/WBDock", "SYS:Tools/WBDock"]


def rexx(a, port, cmd):
    _, out = a.arexx("/**/\noptions results\naddress '%s'\n'%s'\nreturn RC'|'RESULT\n" % (port, cmd))
    code, _, res = out.partition("|")
    return int(code or 0), ("" if res == "RESULT" else res)


def exists(a, path):
    return a.exec_command('List "%s" NOHEAD' % path, timeout=20)[0] == 0


def find_dock_config(a):
    for d in DOCK_DIRS:
        rc, out = a.exec_command('List "%s" PAT=#?.config LFORMAT="%%N"' % d, timeout=20)
        names = [n for n in out.split() if n.lower() == "wbdock.config"]
        if rc == 0 and names and exists(a, d + "/WBDock"):
            return d + "/" + names[0]
    return None


def mac_lan_ip():
    """This Mac's LAN address: a node at 127.x (the Amigo guest) is reached
    from the Amigas through the Mac's port forward."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))     # no packet is sent for UDP connect
        return s.getsockname()[0]
    finally:
        s.close()


def fleet_prefs(me):
    lines = ["# amifleet68 machines: name<TAB>host<TAB>port<TAB>token"]
    for n in load_nodes():
        host = n["host"]
        if n["name"] == me:
            host = "127.0.0.1"
        elif host.startswith("127."):
            host = mac_lan_ip()  # a Mac-local node, forwarded on the Mac's LAN address
        lines.append("\t".join([n["name"], host, str(n.get("port", 7846)), n.get("token", "")]))
    return ("\n".join(lines) + "\n").encode("latin-1")


def install(node, lha, dest):
    a = connect(node, timeout=120)
    name = os.path.basename(lha)
    target = dest.rstrip("/") + ("" if dest.endswith(":") else "/")
    prog = target + "amifleet68/amifleet68"
    print(f"== {node}: {name} -> {target}amifleet68/")

    # The ARexx port was AMIFLEET.1 up to 0.4.1 and AMIFLEET68.1 since 0.5.0.
    ports = [p for p in ("AMIFLEET68.1", "AMIFLEET.1") if p in a.rexx_ports()]
    running = bool(ports)
    for p in ports:
        rexx(a, p, "QUIT")
    if running:
        time.sleep(3)

    a.write_file("RAM:" + name, open(lha, "rb").read())
    # Replace the program's files; keep nothing stale (prefs live in ENVARC:).
    a.exec_command('Delete "%samifleet68" ALL QUIET FORCE' % target, timeout=60)
    rc, out = a.exec_command('lha -q x "RAM:%s" "%s"' % (name, target), timeout=120)
    a.exec_command('Delete "RAM:%s" QUIET' % name)
    if rc != 0 or not exists(a, prog):
        print("   unpack FAILED:", rc, out[-200:])
        return False
    print("   unpacked:", a.exec_command('Version "%s"' % prog)[1].strip())

    if not exists(a, "ENVARC:amifleet68.prefs"):
        p = fleet_prefs(node)
        a.write_file("ENVARC:amifleet68.prefs", p)
        a.write_file("ENV:amifleet68.prefs", p)
        print("   wrote the fleet list (%d machines)" % (p.count(b"\n") - 1))
    else:
        print("   kept its existing fleet list")

    cfg = find_dock_config(a)
    if not cfg:
        print("   no WBDock config found - dock not touched")
    else:
        data = a.read_file(cfg)
        entry = prog.encode("latin-1")
        if entry in data.split(b"\n"):
            print("   dock: already in", cfg)
            if "WBDOCK" in a.rexx_ports():
                rexx(a, "WBDOCK", "REFRESH")     # pick up a new icon
        else:
            if not exists(a, cfg + ".pre-amifleet68"):
                a.write_file(cfg + ".pre-amifleet68", data)
            a.write_file(cfg, (data if data.endswith(b"\n") or not data else data + b"\n") + entry + b"\n")
            if "WBDOCK" in a.rexx_ports():
                print("   dock: ADD ->", rexx(a, "WBDOCK", "ADD " + prog))
            print("   dock: entry appended to", cfg)

    if running:     # it was running before: bring it back
        a.exec_command('Run >NIL: <NIL: "%s"' % prog)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lha")
    ap.add_argument("nodes", nargs="+")
    ap.add_argument("--dest", default="SYS:Programs/")
    args = ap.parse_args()
    ok = all([install(n, args.lha, args.dest) for n in args.nodes])
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
