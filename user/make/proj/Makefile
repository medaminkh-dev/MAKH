# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# A real multi-file build driven by GNU make running ON MAKH (F21-c): make
# compiles two translation units with tcc and links them, plus musl's crt and
# libc, into one static non-PIE executable the higher-half kernel loads.
#
# TCC and SYSROOT default to MAKH's own paths but are overridable, so the exact
# same Makefile also builds on a host (the prebuilt tcc is an ordinary
# x86_64-linux-musl binary): make TCC=.../user/tcc/tcc SYSROOT=.../user/sysroot/usr
TCC     ?= /bin/tcc
SYSROOT ?= /usr

CFLAGS  = -nostdinc -I$(SYSROOT)/include
LDFLAGS = -nostdlib -static -L$(SYSROOT)/lib
CRT     = $(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o
CRTN    = $(SYSROOT)/lib/crtn.o
LIBS    = -lc $(SYSROOT)/lib/libtcc1.a

app: main.o greet.o
	$(TCC) $(LDFLAGS) $(CRT) main.o greet.o $(CRTN) $(LIBS) -o app

main.o: main.c greet.h
	$(TCC) $(CFLAGS) -c main.c -o main.o

greet.o: greet.c greet.h
	$(TCC) $(CFLAGS) -c greet.c -o greet.o

clean:
	/bin/busybox rm -f app main.o greet.o
