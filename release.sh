#!/bin/sh
# release.sh - build dist/amifleet68-<version>.lha and its .readme.
#
# Clean build of both CPUs, the version checked INSIDE each binary, the
# drawer staged Aminet-style, packed with AmigaDiskKit's LHA writer (macOS has
# no LHA writer; Homebrew's lha is extract-only), then unpacked again and
# compared file by file. A release nobody can unpack is worse than none.
set -eu
cd "$(dirname "$0")"

VER=$(sed -n 's/^#define AMIFLEET_VERSION "\(.*\)"/\1/p' src/fleet.h)
LHACLI=${LHACLI:-$HOME/Development/AmigaDiskKit/.build/arm64-apple-macosx/release/AmigaDiskCLI}
[ -x "$LHACLI" ] || { echo "AmigaDiskCLI missing: (cd ~/Development/AmigaDiskKit && swift build -c release)"; exit 1; }
for f in icon/amifleet68.info icon/amifleet68-classic.info icon/drawer.info icon/guide.info docs/amifleet68.guide docs/amifleet68.readme LICENSE; do
    [ -f "$f" ] || { echo "missing $f"; exit 1; }
done

make clean >/dev/null
make >/dev/null
make CPU=68020 >/dev/null
for b in amifleet68 amifleet68.020; do
    strings "$b" | grep -q "\$VER: amifleet68 $VER " || { echo "$b does not say $VER"; exit 1; }
done
grep -q "amifleet68.guide $VER " docs/amifleet68.guide || { echo "guide \$VER is not $VER"; exit 1; }
grep -q "^Version: *$VER\$" docs/amifleet68.readme || { echo "readme Version is not $VER"; exit 1; }

STAGE=dist/stage
rm -rf "$STAGE"
mkdir -p "$STAGE/amifleet68"
D="$STAGE/amifleet68"
cp amifleet68 amifleet68.020 "$D/"
cp icon/amifleet68.info "$D/amifleet68.info"
cp icon/amifleet68.info "$D/amifleet68.020.info"
cp docs/amifleet68.guide "$D/amifleet68.guide"
cp icon/guide.info "$D/amifleet68.guide.info"
cp docs/amifleet68.readme "$D/README"
cp LICENSE "$D/LICENSE"
cp icon/drawer.info "$STAGE/amifleet68.info"
# The same icon without its GlowIcon part, for OS 3.1 or plain-icon fans:
# copy "Classic icon/amifleet68.info" over the program's own to use it.
mkdir -p "$D/Classic icon"
cp icon/amifleet68-classic.info "$D/Classic icon/amifleet68.info"
cp icon/drawer.info "$D/Classic icon.info"
# The classic file must really carry no GlowIcon (IFF FORM ICON) part.
if grep -q "FORM" "$D/Classic icon/amifleet68.info"; then echo "classic icon contains a GlowIcon"; exit 1; fi

OUT="dist/amifleet68-$VER.lha"
rm -f "$OUT"
"$LHACLI" lha create "$OUT" "$STAGE" >/dev/null
cp docs/amifleet68.readme "dist/amifleet68-$VER.readme"

# Unpack and compare every file - with AmigaDiskKit's extractor, NOT
# Homebrew's lha (Lhasa): the writer stores subdirectories as level-0 names
# with 0xFF separators, which Lhasa fails on ("Failure") while the Amiga's
# own C:lha extracts them cleanly (checked on the A4000, 2026-09-30).
CHK=dist/check
rm -rf "$CHK"; mkdir -p "$CHK"
"$LHACLI" lha extract "$OUT" "$CHK" >/dev/null
( cd "$STAGE" && find . -type f ) | while read -r f; do
    cmp -s "$STAGE/$f" "$CHK/$f" || { echo "MISMATCH in archive: $f"; exit 1; }
done
strings "$CHK/amifleet68/amifleet68" | grep -q "\$VER: amifleet68 $VER " || { echo "archived binary is not $VER"; exit 1; }
rm -rf "$CHK"

echo "$OUT  ($(wc -c < "$OUT" | tr -d ' ') bytes, $VER)"
"$LHACLI" lha extract "$OUT" /dev/null 2>/dev/null | head -1 || true
shasum -a 256 "$OUT"
