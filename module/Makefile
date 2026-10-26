# The driver as an Amix kernel module (amix-dlm C3-6 section 3), built with the ABI kit of
# one line at a time (GNU make):
#
#	make KIT=<amix-dlm>/build/kit/k30 MONSWMOD=<the monsw module for k30>
#	make KIT=<amix-dlm>/build/kit/k46 MONSWMOD=<the monsw module for k46>
#
# gives build/<line>/mod.d/z3660rtg, checked by the kit's modcheck.py.  The source is the
# static driver's, compiled with -DZZ9K_MODULE; the static build (overlay/) is unchanged.
#   MONSWMOD  the monsw module this one names in depend= (its globals monsw_*); the
#             checker refuses monsw_* without it
#   MONSWINC  where <sys/monsw.h> is: the 68040/68060 port's copy until monsw's owner is
#             settled (C3-6 section 6)
#   LOCALIZE_OBJCOPY  an objcopy newer than binutils 2.8.1 (gcc14-cross-amix's)
# Master is generated per line from z3660rtg.keys, since $cpu differs (68030, 68040):
# build the two lines one after the other, not in parallel.

ifndef KIT
$(error set KIT to a built ABI kit, e.g. <amix-dlm>/build/kit/k46)
endif

SRC      = ../overlay/usr/sys/amiga/driver/zz9000
MONSWINC ?= ../../amix-040-060-port/src
MODULE   = z3660rtg
MODOUT   = build/$(LINE)
OBJS     = $(MODOUT)/zz9000.o
DEPS     = $(MONSWMOD)
EXTRA    = $(KIT)/stock/usr/sys/amiga/console/sunfont.o
LOCALIZE = builtinfont
CFLAGS   = -DZZ9K_MODULE -DZZ9K_TARGET_Z3660 -DZZ9K_MONSW -I$(MONSWINC) $(ZZ9K_OPTS)

include $(KIT)/mk/module.mk

MODCPU_k30 = 68030
MODCPU_k46 = 68040

$(MODOUT)/zz9000.o: $(SRC)/zz9000.c $(SRC)/zz9000.h
	mkdir -p $(MODOUT)
	$(KIT_CC) $(KIT_CFLAGS) $(CFLAGS) -c $(SRC)/zz9000.c -o $@

Master: z3660rtg.keys FORCE
	{ echo '# generated from z3660rtg.keys for $(LINE) by module/Makefile'; \
	  echo '$$modtype drv'; echo '$$cpu $(MODCPU_$(LINE))'; \
	  sed -n 's/^\([a-z]*\)=\(.*\)$$/$$\1 \2/p' z3660rtg.keys | grep -v '^\$$name '; \
	  echo '$(MODULE) zz9000'; } > Master

FORCE:
.PHONY: FORCE
