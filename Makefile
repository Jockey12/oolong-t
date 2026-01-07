CC = gcc
CFLAGS = -Wall -O2 `sdl2-config --cflags`
LIBS = `sdl2-config --libs` -lvterm

all: terminal-emulator-c

terminal-emulator-c: src/main.c
										$(CC) $(CFLAGS) -o terminal-emulator-c src/main.c $(LIBS)

clean:
	rm -f terminal-emulator-c
