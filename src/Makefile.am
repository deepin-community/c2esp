#C2esp makefile source for src directory

# what flags you want to pass to the C compiler & linker
AM_CFLAGS = --pedantic -Wall -std=c99 -O2
AM_LDFLAGS =
EXTRA_DIST = KodakESP_16.drv KodakESP_C_07.drv

# install directory for cups filters
bindir = $(cupsfilterdir)
	@echo "bindir is $(bindir)";

# this lists the binaries to produce, the (non-PHONY, binary) targets in
# the previous manual Makefile
bin_PROGRAMS = c2esp c2espC command2esp
c2esp_SOURCES = c2esp.c c2espcommon.c c2espcommon.h
c2espC_SOURCES = c2espC.c c2espcommon.c c2espcommon.h
command2esp_SOURCES = command2esp.c c2espcommon.c c2espcommon.h

# install directory for cups drv files
drvdir = $(cupsdatadir)/drv/@PACKAGE@
	@echo "drvdir is $(drvdir)";
#	for drvfile in *.drv; do \
#		ppdc $(drvfile); \
#	done;


# data to install
drv_DATA = KodakESP_16.drv KodakESP_C_07.drv

