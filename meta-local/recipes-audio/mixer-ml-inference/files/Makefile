CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=

LIBS = -lpthread -lm -lrt -lfftw3f -ltensorflow-lite -ldl

all: mixer-ml-inference

mixer-ml-inference: mixer-ml-inference.c ml_features.c ml_features.h \
                    mixer_pro_shm_tap.h imx-audio-tap-uapi.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ \
	    mixer-ml-inference.c ml_features.c $(LIBS)

clean:
	rm -f mixer-ml-inference

.PHONY: all clean
