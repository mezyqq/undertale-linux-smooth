# 32-bit, like the game's runner. Needs gcc with multilib (gcc -m32) and 32-bit glibc/libX11 headers.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wno-unused-function

all: lib/smooth.so lib/layoutfix.so

lib/smooth.so: src/smooth.c
	@mkdir -p lib
	$(CC) -m32 $(CFLAGS) -shared -fPIC $< -o $@ -ldl -lm -lpthread

lib/layoutfix.so: src/layoutfix.c
	@mkdir -p lib
	$(CC) -m32 $(CFLAGS) -shared -fPIC $< -o $@ -ldl

clean:
	rm -f lib/*.so

.PHONY: all clean
