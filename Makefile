## -----------------------------------------------------------------------
##  $Id$
##
##   Copyright 1998-2001 H. Peter Anvin - All Rights Reserved
##
##   This program is free software; you can redistribute it and/or modify
##   it under the terms of the GNU General Public License as published by
##   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
##   USA; either version 2 of the License, or (at your option) any later
##   version; incorporated herein by reference.
##
## -----------------------------------------------------------------------

#
# Main Makefile for SYSLINUX
#

CC	 = gcc
INCLUDE  =
CFLAGS	 = -Wall -O2 -fomit-frame-pointer
LDFLAGS	 = -O2 -s

NASM	 = nasm -O99
NINCLUDE = 
BINDIR   = /usr/bin
LIBDIR   = /usr/lib/syslinux

PERL     = perl

VERSION  = $(shell cat version)

.c.o:
	$(CC) $(INCLUDE) $(CFLAGS) -c $<

#
# The BTARGET refers to objects that are derived from ldlinux.asm; we
# like to keep those uniform for debugging reasons; however, distributors 
# want to recompile the installers (ITARGET).
#
CSRC    = syslinux.c gethostip.c
NASMSRC  = ldlinux.asm syslinux.asm copybs.asm \
	  pxelinux.asm mbr.asm isolinux.asm isolinux-debug.asm
SOURCES = $(CSRC) $(NASMSRC) *.inc
BTARGET = kwdhash.gen version.gen ldlinux.bss ldlinux.sys ldlinux.bin \
	  pxelinux.0 mbr.bin isolinux.bin isolinux-debug.bin
ITARGET = syslinux.com syslinux copybs.com gethostip mkdiskimage
DOCS    = COPYING NEWS README TODO *.doc sample com32/include
OTHER   = Makefile bin2c.pl now.pl genhash.pl keywords findpatch.pl \
	  keytab-lilo.pl version version.pl sys2ansi.pl \
	  ppmtolss16 lss16toppm memdisk bin2hex.pl mkdiskimage.in
OBSOLETE = pxelinux.bin

# Things to install in /usr/bin
INSTALL_BIN   =	syslinux gethostip ppmtolss16 lss16toppm
# Things to install in /usr/lib/syslinux
INSTALL_LIB   =	pxelinux.0 isolinux.bin isolinux-debug.bin \
		syslinux.com copybs.com memdisk/memdisk

# The DATE is set on the make command line when building binaries for
# official release.  Otherwise, substitute a hex string that is pretty much
# guaranteed to be unique to be unique from build to build.
ifndef HEXDATE
HEXDATE := $(shell $(PERL) now.pl ldlinux.asm pxelinux.asm isolinux.asm)
endif
ifndef DATE
DATE    := $(HEXDATE)
endif

all:	$(BTARGET) $(ITARGET) samples memdisk
	ls -l $(BTARGET) $(ITARGET) memdisk/memdisk

installer: $(ITARGET) samples
	ls -l $(BTARGET) $(ITARGET)

.PHONY: samples
samples:
	$(MAKE) -C sample all

.PHONY: memdisk
memdisk:
	$(MAKE) -C memdisk all

version.gen: version version.pl
	$(PERL) version.pl version

kwdhash.gen: keywords genhash.pl
	$(PERL) genhash.pl < keywords > kwdhash.gen

ldlinux.bin: ldlinux.asm kwdhash.gen version.gen
	$(NASM) -f bin -DDATE_STR="'$(DATE)'" -DHEXDATE="$(HEXDATE)" \
		-l ldlinux.lst -o ldlinux.bin ldlinux.asm

pxelinux.bin: pxelinux.asm kwdhash.gen version.gen
	$(NASM) -f bin -DDATE_STR="'$(DATE)'" -DHEXDATE="$(HEXDATE)" \
		-l pxelinux.lst -o pxelinux.bin pxelinux.asm

isolinux.bin: isolinux.asm kwdhash.gen version.gen
	$(NASM) -f bin -DDATE_STR="'$(DATE)'" -DHEXDATE="$(HEXDATE)" \
		-l isolinux.lst -o isolinux.bin isolinux.asm

pxelinux.0: pxelinux.bin
	cp pxelinux.bin pxelinux.0

# Special verbose version of isolinux.bin
isolinux-debug.bin: isolinux-debug.asm kwdhash.gen
	$(NASM) -f bin -DDATE_STR="'$(DATE)'" -DHEXDATE="$(HEXDATE)" \
		-l isolinux-debug.lst -o isolinux-debug.bin isolinux-debug.asm

ldlinux.bss: ldlinux.bin
	dd if=ldlinux.bin of=ldlinux.bss bs=512 count=1

ldlinux.sys: ldlinux.bin
	dd if=ldlinux.bin of=ldlinux.sys  bs=512 skip=1

patch.offset: ldlinux.sys findpatch.pl
	$(PERL) findpatch.pl > patch.offset

mbr.bin: mbr.asm
	$(NASM) -f bin -l mbr.lst -o mbr.bin mbr.asm

syslinux.com: syslinux.asm ldlinux.bss ldlinux.sys patch.offset
	$(NASM) -f bin -DPATCH_OFFSET=`cat patch.offset` \
		-l syslinux.lst -o syslinux.com syslinux.asm

copybs.com: copybs.asm
	$(NASM) -f bin -l copybs.lst -o copybs.com copybs.asm

bootsect_bin.c: ldlinux.bss bin2c.pl
	$(PERL) bin2c.pl bootsect < ldlinux.bss > bootsect_bin.c

ldlinux_bin.c: ldlinux.sys bin2c.pl
	$(PERL) bin2c.pl ldlinux < ldlinux.sys > ldlinux_bin.c

syslinux: syslinux.o bootsect_bin.o ldlinux_bin.o
	$(CC) $(LDFLAGS) -o syslinux \
		syslinux.o bootsect_bin.o ldlinux_bin.o

syslinux.o: syslinux.c patch.offset
	$(CC) $(INCLUDE) $(CFLAGS) -DPATCH_OFFSET=`cat patch.offset` \
		-c -o $@ $<

gethostip.o: gethostip.c

gethostip: gethostip.o

mkdiskimage: mkdiskimage.in mbr.bin bin2hex.pl
	$(PERL) bin2hex.pl < mbr.bin | cat mkdiskimage.in - > $@
	chmod a+x $@

install: installer
	mkdir -m 755 -p $(INSTALLROOT)$(BINDIR) $(INSTALLROOT)$(LIBDIR)
	install -m 755 -c $(INSTALL_BIN) $(INSTALLROOT)$(BINDIR)
	install -m 644 -c $(INSTALL_LIB) $(INSTALLROOT)$(LIBDIR)

local-tidy:
	rm -f *.o *_bin.c stupid.* patch.offset
	rm -f syslinux.lst copybs.lst pxelinux.lst isolinux*.lst
	rm -f $(OBSOLETE)

tidy: local-tidy
	$(MAKE) -C memdisk tidy

local-clean:
	rm -f $(ITARGET)

clean: local-tidy local-clean
	$(MAKE) -C sample clean
	$(MAKE) -C memdisk clean

dist: tidy
	for dir in . sample memdisk ; do \
		( cd $$dir && rm -f *~ \#* core ) ; \
	done

local-spotless:
	rm -f $(BTARGET) .depend

spotless: local-clean dist local-spotless
	$(MAKE) -C sample spotless
	$(MAKE) -C memdisk spotless

.depend:
	rm -f .depend
	for csrc in $(CSRC) ; do $(CC) $(INCLUDE) -M $$csrc >> .depend ; done
	for nsrc in $(NASMSRC) ; do $(NASM) -DDEPEND $(NINCLUDE) -o `echo $$nsrc | sed -e 's/\.asm/\.bin/'` -M $$nsrc >> .depend ; done

depend:
	rm -f .depend
	$(MAKE) .depend

# Hook to add private Makefile targets for the maintainer.
-include Makefile.private

# Include dependencies file
-include .depend
