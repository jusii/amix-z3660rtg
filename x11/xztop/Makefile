# xztop, cross-built with gcc14-cross-amix against the x11r6.3 package's headers and
# static libraries (Xaw, Xmu, Xt, SM, ICE, Xext, X11), which a box with that package has
# under /opt/amix/X11R6:
#
#	make XROOT=<the package's dest>/opt/amix/X11R6
#
# m68k-elf-gcc (gcc14-cross-amix's build/prefix/bin) must be on PATH.
#
# Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT

XROOT  = /opt/amix/X11R6
CC     = m68k-elf-gcc
CFLAGS = -O1 -Wall
ZZ9K   = ../../overlay/usr/sys/amiga/driver/zz9000
LIBS   = -L$(XROOT)/lib -lXaw -lXmu -lXt -lSM -lICE -lXext -lX11 -lsocket -lnsl

xztop: xztop.c $(ZZ9K)/zz9000.h
	$(CC) $(CFLAGS) -I$(XROOT)/include -I$(ZZ9K) -o xztop xztop.c $(LIBS)

clean:
	-rm -f xztop
