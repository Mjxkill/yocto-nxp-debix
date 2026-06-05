# V9.5 — Compile lv2_chain_host.so pour le wrapper Python.

CFLAGS  := -O2 -fPIC -Wall -Wextra $(shell pkg-config --cflags lilv-0)
LDFLAGS := -shared $(shell pkg-config --libs lilv-0)

lv2_chain_host.so: lv2_chain_host.c
	gcc $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

clean:
	rm -f lv2_chain_host.so

.PHONY: clean
