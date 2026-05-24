CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=
# V9.2 — lilv pour LV2 plugin host (pkg-config résolu cross-compile par Yocto).
# Note : on passe LILV_CFLAGS directement au gcc (pas via CFLAGS) car bitbake
# override CFLAGS via argument make → += dans Makefile ignoré.
LILV_CFLAGS := $(shell pkg-config --cflags lilv-0 2>/dev/null)
LILV_LIBS   := $(shell pkg-config --libs   lilv-0 2>/dev/null)
LIBS     = -lasound -lpthread -lm -lrt

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: mixer-pro mixerctl

mixer-pro: mixer-pro.c mixer-pro.h effects.c effects.h analyzer.c analyzer.h
	$(CC) $(CFLAGS) $(LILV_CFLAGS) $(LDFLAGS) -o $@ mixer-pro.c effects.c analyzer.c $(LIBS) $(LILV_LIBS)

mixerctl: mixerctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mixerctl.c

install: mixer-pro mixerctl
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 mixer-pro $(DESTDIR)$(BINDIR)/mixer-pro
	install -m 0755 mixerctl  $(DESTDIR)$(BINDIR)/mixerctl

clean:
	rm -f mixer-pro mixerctl

.PHONY: all install clean
