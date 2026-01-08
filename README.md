# oolong-t

A lightweight, GPU-accelerated terminal emulator written in C using SDL2 and libvterm.

## Features

- **Rendering**: Hardware accelerated rendering via SDL2.
- **Font Support**: TrueType font support using `stb_truetype`.
  - Includes support for Box Drawing characters (Tmux borders).
  - Powerline symbols.
  - Nerd Font icons (DevIcons, FontAwesome).
- **Terminal Emulation**: Robust ANSI/xterm emulation powered by `libvterm`.
- **PTY Support**: Standard POSIX pseudo-terminal support.
- **Resizing**: Dynamic window and terminal resizing.

## Dependencies

To build and run `oolong-t`, you need the following libraries installed on your system:

- **SDL2** (`libsdl2-dev`)
- **libvterm** (`libvterm-dev`)
- **GCC** (or another C compiler)
- **Make**

## Building

1. Run `make` to compile the project.

```bash
make
```

This will produce an executable named `terminal-emulator-c`.

## Running

Ensure you have the required font file located at `src/font.ttf` relative to the executable (or update the `FONT_PATH` in `src/main.c`).

```bash
./terminal-emulator-c
```

## Configuration

Currently, configuration is done by modifying `src/main.c` directly and recompiling.

- **Font**: Change `FONT_PATH` and `FONT_SIZE`.
- **Colors**: Modify the default colors in the `main` function or the `render_term` function.

## License

See the LICENSE file for details.
