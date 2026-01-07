CC = gcc
CFLAGS = -Wall -O2 $(shell sdl2-config --cflags)
LIBS = $(shell sdl2-config --libs) -lvterm -lm

.PHONY: all clean

all: terminal-emulator-c

terminal-emulator-c: src/main.c src/stb_truetype.h
	$(CC) $(CFLAGS) -o terminal-emulator-c src/main.c $(LIBS)

clean:
	rm -f terminal-emulator-c
