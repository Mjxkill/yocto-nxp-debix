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
# V9.5.12 — librt = shm_open/mmap pour SHM tap audio export.
LIBS     = -lasound -lpthread -lm -lrt $(FFTW_LIBS)   # V9.5.20 : fx_spectral_env (FIR irfft)

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: mixer-pro mixerctl ml_features_test

# V9.5.12 — mixer-pro NE link plus tensorflow-lite (TFLite causait freeze
# kernel via VX delegate + galcore + audio_thread RT99). L'inférence ML
# est faite par le daemon mixer-ml-inference (process séparé).
# V14.0 étape 0 — modules extraits : state.h (état partagé), util.c (log +
# helpers ALSA), dsp_bq.c (biquads RBJ). ARCHI_V14_RESTRUCTURATION.md.
mixer-pro: mixer-pro.c mixer-pro.h effects.h analyzer.c analyzer.h \
           fx_dynamics.c fx_space.c fx_eq.c fx_chain.c \
           lv2_host.c lv2_load.c fx_lv2.h fx_internal.h \
           mixer_pro_shm_tap.c mixer_pro_shm_tap.h \
           state.h util.c util.h dsp_bq.c dsp_bq.h sampler.c sampler.h \
           looper.c looper.h midix.c midix.h strip_dyn.c strip_dyn.h \
           master.c master.h automix.c automix.h voice.c voice.h dsp_block.h \
           persist.c persist.h uac2_ring.c uac2_ring.h \
           audio_loop.c audio_loop.h automix_ops.c control.c control.h fx_ops.c tac_ops.c antilarsen.c antilarsen.h voice_clean.c voice_clean.h voice_clean_shm.h
	$(CC) $(CFLAGS) $(LILV_CFLAGS) $(LDFLAGS) -o $@ \
	    mixer-pro.c analyzer.c mixer_pro_shm_tap.c \
	    fx_dynamics.c fx_space.c fx_eq.c fx_chain.c lv2_host.c lv2_load.c \
	    util.c dsp_bq.c sampler.c looper.c midix.c strip_dyn.c master.c automix.c voice.c persist.c uac2_ring.c audio_loop.c automix_ops.c control.c fx_ops.c tac_ops.c antilarsen.c voice_clean.c \
	    $(LIBS) $(LILV_LIBS)

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
