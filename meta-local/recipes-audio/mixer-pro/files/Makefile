CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=
LIBS     = -lasound -lpthread -lm -lrt

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: mixer-pro mixerctl

mixer-pro: mixer-pro.c mixer-pro.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mixer-pro.c $(LIBS)

mixerctl: mixerctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mixerctl.c

install: mixer-pro mixerctl
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 mixer-pro $(DESTDIR)$(BINDIR)/mixer-pro
	install -m 0755 mixerctl  $(DESTDIR)$(BINDIR)/mixerctl

clean:
	rm -f mixer-pro mixerctl

.PHONY: all install clean
