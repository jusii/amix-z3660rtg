# The driver as an Amix kernel module (amix-dlm C3-6 section 3), built with the ABI kit of
# one line at a time (GNU make):
#
#	make KIT=../../amix/kernel/dlm/build/kit/k30
#	make KIT=../../amix/kernel/dlm/build/kit/k46
#
# gives build/<line>/mod.d/z3660rtg, checked by the kit's modcheck.py.  The source is the
# static driver's, compiled with -DZZ9K_MODULE; the static build (overlay/) is unchanged.
# The defaults assume amix (github.com/amigaux/amix) checked out beside this repository.
#   KIT       a kit built by `make kit` in amix's kernel/dlm
#   MONSWMOD  the monsw module this one names in depend= (its globals monsw_*), by default
#             amix's kernel/modules/monsw build for the kit's line (`make -C monsw/module`
#             there); the checker refuses monsw_* without it.  <sys/monsw.h> comes from
#             the kit (MONSW_REV in kit.conf)
#   LOCALIZE_OBJCOPY  an objcopy newer than binutils 2.8.1 (gcc14-cross-amix's)
# Master is generated from z3660rtg.keys; it has no $cpu, so the kit gives each line its
# own (68030, 68040).

ifndef KIT
$(error set KIT to a built ABI kit, e.g. ../../amix/kernel/dlm/build/kit/k46)
endif

SRC      = ../overlay/usr/sys/amiga/driver/zz9000
MONSWMOD ?= ../../amix/kernel/modules/monsw/build/$(LINE)/mod.d/monsw
MODULE   = z3660rtg
OBJS     = zz9000.o
DEPS     = $(MONSWMOD)
EXTRA    = $(KIT)/stock/usr/sys/amiga/console/sunfont.o
LOCALIZE = builtinfont
CFLAGS   = -DZZ9K_MODULE -DZZ9K_TARGET_Z3660 -DZZ9K_MONSW $(ZZ9K_OPTS)

include $(KIT)/mk/module.mk

# the fragment compiles $(MODOUT)/obj/zz9000.o from the source in the static driver's directory
vpath %.c $(SRC)
$(MODOUT)/obj/zz9000.o: $(SRC)/zz9000.h

Master: z3660rtg.keys
	{ echo '# generated from z3660rtg.keys by module/Makefile'; \
	  echo '$$modtype drv'; \
	  sed -n 's/^\([a-z]*\)=\(.*\)$$/$$\1 \2/p' z3660rtg.keys | grep -v '^\$$name '; \
	  echo '$(MODULE) zz9000'; } > Master
