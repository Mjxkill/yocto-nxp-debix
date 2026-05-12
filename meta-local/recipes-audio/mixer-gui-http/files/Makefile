CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=
LIBS     = -lmicrohttpd -lpthread -lasound

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: mixer-gui-http

mixer-gui-http: mixer-gui-http.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

install: mixer-gui-http
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 mixer-gui-http $(DESTDIR)$(BINDIR)/mixer-gui-http

clean:
	rm -f mixer-gui-http

.PHONY: all install clean
