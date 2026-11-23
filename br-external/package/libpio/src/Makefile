# libpio.so and pioasm (cross: make CC=arm-...-gcc)
CC ?= cc
CFLAGS ?= -O2 -Wall

all: libpio.so pioasm

libpio.so: pio.c pioasm.c pio.h pioasm.h rp2350-pio.h
	$(CC) $(CFLAGS) -fPIC -shared -Wl,-soname,libpio.so -o $@ pio.c pioasm.c

pioasm: pioasm-main.c pioasm.h pio.h libpio.so
	$(CC) $(CFLAGS) -o $@ pioasm-main.c -L. -lpio

clean:
	rm -f libpio.so pioasm
