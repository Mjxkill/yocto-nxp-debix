CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=
# V9.2 — lilv pour LV2 plugin host (pkg-config résolu cross-compile par Yocto).
# Note : on passe LILV_CFLAGS directement au gcc (pas via CFLAGS) car bitbake
# override CFLAGS via argument make → += dans Makefile ignoré.
LILV_CFLAGS := $(shell pkg-config --cflags lilv-0 2>/dev/null)
LILV_LIBS   := $(shell pkg-config --libs   lilv-0 2>/dev/null)
# V9.5.12 — fftw3f pour ml_features.c (FFT 1024 float32).
FFTW_LIBS := -lfftw3f
LIBS     = -lasound -lpthread -lm -lrt

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: mixer-pro mixerctl ml_features_test

mixer-pro: mixer-pro.c mixer-pro.h effects.c effects.h analyzer.c analyzer.h ml_features.c ml_features.h
	$(CC) $(CFLAGS) $(LILV_CFLAGS) $(LDFLAGS) -o $@ mixer-pro.c effects.c analyzer.c ml_features.c $(LIBS) $(LILV_LIBS) $(FFTW_LIBS)

mixerctl: mixerctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mixerctl.c

# V9.5.12 — programme standalone pour test parité Python ↔ C.
ml_features_test: ml_features_test.c ml_features.c ml_features.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ml_features_test.c ml_features.c -lm $(FFTW_LIBS)

install: mixer-pro mixerctl ml_features_test
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 mixer-pro        $(DESTDIR)$(BINDIR)/mixer-pro
	install -m 0755 mixerctl         $(DESTDIR)$(BINDIR)/mixerctl
	install -m 0755 ml_features_test $(DESTDIR)$(BINDIR)/ml_features_test

clean:
	rm -f mixer-pro mixerctl ml_features_test

.PHONY: all install clean
