# amifleet68 - AmigaOS build (bebbo m68k-amigaos-gcc, ~/opt/amiga/bin)
#
#   make            -> amifleet68       (68000 baseline; runs on every Amiga)
#   make CPU=68020  -> 68020+ build
#
# Same toolchain and flag set as amipkg and amiagent.

CC      = $(HOME)/opt/amiga/bin/m68k-amigaos-gcc
CPU     = 68000
CFLAGS  = -Os -fomit-frame-pointer -m$(CPU) -Wall -Wextra -D__amigaos__ -Ivendor/mui/include -Ivendor/cgx
# -lamiga for DoMethod/HookEntry; -lgcc after the objects (newlib's printf
# pulls in libgcc's 64-bit helpers, which sit after libc in the link order).
LDFLAGS = -s -lamiga -lgcc

SRC  = src/main.c src/worker.c src/vnc.c src/view.c src/des.c src/muistubs.c
HDRS = src/fleet.h src/proto.h src/net.h src/view.h src/des.h

ifeq ($(CPU),68000)
OUT = amifleet68
else
OUT = amifleet68.$(subst 68,,$(CPU))
endif

all: $(OUT)

$(OUT): $(SRC) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f amifleet68 amifleet68.020

.PHONY: all clean
