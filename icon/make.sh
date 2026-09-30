#!/bin/sh
# make.sh - rebuild the release icons (classic 4-colour planar + GlowIcon)
# from the checked-in PNGs:
#
#   amifleet68.info  TOOL     amifleet68.png  (draw.py)
#   drawer.info      DRAWER   drawer.png      (draw_extra.py)  the archive's
#                                             top-level amifleet68 drawer
#   guide.info       PROJECT  guide.png       (draw_extra.py)  amifleet68.guide,
#                                             opened by SYS:Utilities/MultiView
#
#   ./make.sh            # PNGs -> .info
#   ./make.sh --redraw   # draw.py + draw_extra.py -> PNGs -> .info (needs Pillow)
#
# mkicon.py lives in AllAmigaTooling (skills/amiga-icon); override with MKICON=.
set -e
cd "$(dirname "$0")"
P="${CLAUDE_PLUGIN_ROOT:-$HOME/Development/AllAmigaTooling}"
MKICON="${MKICON:-$P/skills/amiga-icon/scripts/mkicon.py}"
[ -f "$MKICON" ] || { echo "mkicon.py not found at $MKICON (set MKICON=)" >&2; exit 1; }

if [ "$1" = "--redraw" ]; then
    python3 draw.py
    python3 draw_extra.py
fi

# No --selected: mkicon synthesises the classic selected image (plane
# complement, as GADGHCOMP would look on 3.1) and a glow halo for 3.5+.
python3 "$MKICON" amifleet68.info --normal amifleet68.png \
    --type tool --stack 16384

# Drawer: mkicon writes the DrawerData block Workbench needs to open it, with
# a small default window (60,50 320x130) that fits a 640x256 PAL screen, and
# no icon position (NO_ICON_POSITION) so Workbench places the icon itself.
# Rename to amifleet68.info next to the drawer when packaging the LHA.
python3 "$MKICON" drawer.info --normal drawer.png --type drawer

# The manual: a project icon whose DefaultTool is MultiView.
# Rename to amifleet68.guide.info when packaging the LHA.
python3 "$MKICON" guide.info --normal guide.png \
    --type project --default-tool SYS:Utilities/MultiView
