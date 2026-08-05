CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LIBS     = -lpthread -lm -lrt -lfftw3f -ltensorflow-lite -ldl

voice-clean: voice-clean.c voice_clean_shm.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ voice-clean.c $(LIBS)

clean:
	rm -f voice-clean
