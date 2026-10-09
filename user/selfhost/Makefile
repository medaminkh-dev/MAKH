# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# F21-d (the self-hosting summit): rebuild a program MAKH actually ships —
# /bin/muslhello — from its own source, with the on-MAKH toolchain (tcc + the
# musl sysroot), driven by make, ON MAKH. muslhello.c here is MAKH's own
# user/musl/hello.c (copied into the initrd verbatim, one source of truth); the
# shipped /bin/muslhello was cross-built on a host with zig cc. Both exit 42, so
# a matching run proves MAKH can regenerate its userland on itself.
#
# TCC and SYSROOT default to MAKH's paths but are overridable, so the same build
# runs on a host: make TCC=.../user/tcc/tcc SYSROOT=.../user/sysroot/usr
TCC     ?= /bin/tcc
SYSROOT ?= /usr

CFLAGS  = -nostdinc -I$(SYSROOT)/include
LDFLAGS = -nostdlib -static -L$(SYSROOT)/lib
CRT     = $(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o
CRTN    = $(SYSROOT)/lib/crtn.o
LIBS    = -lc $(SYSROOT)/lib/libtcc1.a

muslhello: muslhello.o
	$(TCC) $(LDFLAGS) $(CRT) muslhello.o $(CRTN) $(LIBS) -o muslhello

muslhello.o: muslhello.c
	$(TCC) $(CFLAGS) -c muslhello.c -o muslhello.o
