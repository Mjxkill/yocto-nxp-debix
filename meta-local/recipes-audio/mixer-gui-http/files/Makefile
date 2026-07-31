CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=
LIBS     = -lmicrohttpd -lpthread -lasound

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: mixer-gui-http

# V14.0 étape 6 — 5 modules (voir gui_http.h)
SRCS = http_core.c mixer_bridge.c sse_state.c alsa_ctl.c api_routes.c
mixer-gui-http: $(SRCS) gui_http.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS)

install: mixer-gui-http
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 mixer-gui-http $(DESTDIR)$(BINDIR)/mixer-gui-http

clean:
	rm -f mixer-gui-http

.PHONY: all install clean
