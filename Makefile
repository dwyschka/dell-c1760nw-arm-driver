# rastertoxrhbpl - CUPS driver for the Xerox Phaser 6000/6010 (HBPL)
#
#   make                      build the filter (needs libcups2-dev)
#   make check                run the self tests
#   sudo make install XEROX_PAYLOAD=/path/to/Payload
#                             install filter, DLUT and generated PPDs
#
# Cross compiling, e.g. for a Raspberry Pi:
#   make CC=aarch64-linux-gnu-gcc     (needs libcups2-dev:arm64)

CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -Wall -Wextra -Wno-deprecated-declarations -ffp-contract=off -D_DEFAULT_SOURCE
LDLIBS   += -lcups -lm

PREFIX        ?= /usr
DATADIR       ?= $(PREFIX)/share/xerox-phaser6000
PPDDIR        ?= $(PREFIX)/share/ppd/xerox-phaser6000
CUPS_SERVERBIN ?= $(shell cups-config --serverbin 2>/dev/null || echo $(PREFIX)/lib/cups)
DLUT_NAME      = Xerox Phaser 6010.dlut

SRC = src/rastertoxrhbpl.c src/xrsettings.c src/xrpage.c src/xrcolor.c src/alla5.c src/hbpl.c
HDR = $(wildcard src/*.h)

all: rastertoxrhbpl

rastertoxrhbpl: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) '-DDEFAULT_DLUT="$(DATADIR)/$(DLUT_NAME)"' -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

# stand-alone stage drivers used by the regression tests
test/al5filter: test/al5filter.c src/alla5.c src/alla5.h
	$(CC) $(CFLAGS) -o $@ test/al5filter.c src/alla5.c

test/ccfilter: test/ccfilter.c src/xrcolor.c src/xrcolor.h
	$(CC) $(CFLAGS) -o $@ test/ccfilter.c src/xrcolor.c -lm

check: rastertoxrhbpl test/al5filter test/ccfilter
	sh test/run-tests.sh

install: rastertoxrhbpl
	@test -n "$(XEROX_PAYLOAD)" || { echo "set XEROX_PAYLOAD to the Payload directory of the Xerox macOS driver package"; exit 1; }
	install -d $(DESTDIR)$(CUPS_SERVERBIN)/filter $(DESTDIR)$(DATADIR) $(DESTDIR)$(PPDDIR)
	install -m 755 rastertoxrhbpl $(DESTDIR)$(CUPS_SERVERBIN)/filter/
	install -m 644 "$(XEROX_PAYLOAD)/Library/Printers/Xerox/DLUT/$(DLUT_NAME)" "$(DESTDIR)$(DATADIR)/$(DLUT_NAME)"
	for m in 6000B 6010N; do \
	  sh tools/make-ppd.sh "$(XEROX_PAYLOAD)/Library/Printers/PPDs/Contents/Resources/Xerox Phaser $$m.gz" \
	     "$(DATADIR)/$(DLUT_NAME)" > "$(DESTDIR)$(PPDDIR)/Xerox-Phaser-$$m.ppd"; \
	done

uninstall:
	rm -f $(DESTDIR)$(CUPS_SERVERBIN)/filter/rastertoxrhbpl
	rm -rf $(DESTDIR)$(DATADIR) $(DESTDIR)$(PPDDIR)

clean:
	rm -rf rastertoxrhbpl *.dSYM test/al5filter test/ccfilter test/*.dSYM

.PHONY: all check install uninstall clean
