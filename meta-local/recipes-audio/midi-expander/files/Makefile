CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  += -lfluidsynth -lpthread

midi-expander: midi-expander.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags fluidsynth 2>/dev/null) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f midi-expander

.PHONY: clean
