CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  += -lfluidsynth -lpthread -lm

midi-expander: midi-expander.c ala-synth.c ala-synth.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags fluidsynth 2>/dev/null) $(LDFLAGS) -o $@ midi-expander.c ala-synth.c $(LDLIBS)

clean:
	rm -f midi-expander

.PHONY: clean
