#define _XOPEN_SOURCE 600
#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#include <vterm.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// font config
#define FONT_PATH "src/font.ttf"
#define FONT_SIZE 20.0f
#define ATLAS_WIDTH 512
#define ATLAS_HEIGHT 512

// Globals
int master_fd;
pid_t child_pid;
VTerm *vterm;
VTermScreen *vterm_screen;
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *font_texture;
int dirty = 1;

// Font data
stbtt_bakedchar cdata[224]; // ASCII 32..255

// pty setup
void spawn_shell() {
  master_fd = posix_openpt(O_RDWR | O_NOCTTY);
  if (master_fd == -1) {
    perror("posix_openpt");
    exit(1);
  }

  if (grantpt(master_fd) == -1) {
    perror("grantpt");
    exit(1);
  }

  if (unlockpt(master_fd) == -1) {
    perror("unlockpt");
    exit(1);
  }

  char *slave_name = ptsname(master_fd);
  if (slave_name == NULL) {
    perror("ttyname");
    exit(1);
  }

  child_pid = fork();
  if (child_pid == -1) {
    perror("fork");
    exit(1);
  }

  if (child_pid == 0) {
    // --- CHILD PROCESS ---
    setsid();

    int slave_fd = open(slave_name, O_RDWR);
    if (slave_fd == -1) {
      perror("open slave");
      exit(1);
    }

// Set Controlling Terminal
#ifdef TIOCSCTTY
    ioctl(slave_fd, TIOCSCTTY, 0);
#endif

    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);

    // Close the master in the child (important!)
    close(master_fd);
    close(slave_fd);

    // --- SETUP ENVIRONMENT ---
    // Ensure we are xterm-256color
    setenv("TERM", "xterm-256color", 1);
    // Clear old size variables to force bash to ask the PTY
    unsetenv("COLUMNS");
    unsetenv("LINES");

    // --- EXECUTE SHELL ---
    // FIX 2: use -l to make it a login shell (loads config)
    execlp("/bin/bash", "bash", "-l", NULL);

    // Fallback if bash is missing
    execlp("/bin/sh", "sh", NULL);

    perror("execlp failed");
    exit(1);
  }
  // Parent doesn't need to do anything else
}

void load_font() {
  FILE *f = fopen(FONT_PATH, "rb");
  if (!f) {
    printf("error: could not open font file at %s\n", FONT_PATH);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *ttf_buffer = malloc(size);
  fread(ttf_buffer, 1, size, f);
  fclose(f);

  // create bitmap for ATLAS
  unsigned char *temp_bitmap = malloc(ATLAS_WIDTH * ATLAS_HEIGHT);

  // bake ascii characters 32..255 into bitmap
  int res = stbtt_BakeFontBitmap(ttf_buffer, 0, FONT_SIZE, temp_bitmap,
                                 ATLAS_WIDTH, ATLAS_HEIGHT, 32, 224, cdata);
  if (res <= 0) {
    printf("Warning: Font baking failed or not all chars fit. res=%d\n", res);
  }
  free(ttf_buffer);

  // convert 1-channel bitmap to 4-channel SDL texture
  Uint32 *pixels = malloc(ATLAS_WIDTH * ATLAS_HEIGHT * sizeof(Uint32));
  for (int i = 0; i < ATLAS_WIDTH * ATLAS_HEIGHT; i++) {
    Uint8 alpha = temp_bitmap[i];
    // pixels[i] = (val << 24) | (val << 16) | (val << 8) | val;
    pixels[i] = (Uint32)(alpha << 24 | 0x00FFFFFF);
  }
  free(temp_bitmap);

  font_texture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STATIC, ATLAS_WIDTH, ATLAS_HEIGHT);
  SDL_UpdateTexture(font_texture, NULL, pixels, ATLAS_WIDTH * 4);
  SDL_SetTextureBlendMode(font_texture, SDL_BLENDMODE_BLEND);
  free(pixels);
  if (cdata[0].xadvance == 0)
    cdata[0].xadvance = FONT_SIZE / 2;
}

void render_term() {
  // 1. Clear with the DEFAULT background color (usually black)
  VTermState *state = vterm_obtain_state(vterm);
  VTermColor default_fg, default_bg;
  vterm_state_get_default_colors(state, &default_fg, &default_bg);

  SDL_SetRenderDrawColor(renderer, default_bg.rgb.red, default_bg.rgb.green,
                         default_bg.rgb.blue, 255);
  SDL_RenderClear(renderer);

  int rows, cols;
  vterm_get_size(vterm, &rows, &cols);

  // 2. Iterate Cells
  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols; col++) {
      VTermScreenCell cell;
      VTermPos pos = {row, col};
      vterm_screen_get_cell(vterm_screen, pos, &cell);

      uint32_t code = cell.chars[0];

      // --- BACKGROUND ---
      // Convert VTerm color to RGB
      vterm_state_convert_color_to_rgb(state, &cell.bg);

      // Only draw background if it is NOT the default color
      if (cell.bg.rgb.red != default_bg.rgb.red ||
          cell.bg.rgb.green != default_bg.rgb.green ||
          cell.bg.rgb.blue != default_bg.rgb.blue) {

        SDL_SetRenderDrawColor(renderer, cell.bg.rgb.red, cell.bg.rgb.green,
                               cell.bg.rgb.blue, 255);
        SDL_Rect bg_rect = {col * cdata[0].xadvance, row * FONT_SIZE,
                            cdata[0].xadvance, FONT_SIZE};
        SDL_RenderFillRect(renderer, &bg_rect);
      }

      // --- FOREGROUND (TEXT) ---
      if (code < 32 || code > 126)
        continue; // Skip non-printable for now

      vterm_state_convert_color_to_rgb(state, &cell.fg);
      SDL_SetTextureColorMod(font_texture, cell.fg.rgb.red, cell.fg.rgb.green,
                             cell.fg.rgb.blue);

      stbtt_aligned_quad q;
      float x = col * cdata[0].xadvance;
      float y = (row * FONT_SIZE) + (FONT_SIZE * 0.75f); // Baseline adjustment

      stbtt_GetBakedQuad(cdata, ATLAS_WIDTH, ATLAS_HEIGHT, code - 32, &x, &y,
                         &q, 1);

      SDL_Rect src = {(int)(q.s0 * ATLAS_WIDTH), (int)(q.t0 * ATLAS_HEIGHT),
                      (int)((q.s1 - q.s0) * ATLAS_WIDTH),
                      (int)((q.t1 - q.t0) * ATLAS_HEIGHT)};
      SDL_Rect dst = {(int)q.x0, (int)q.y0, (int)(q.x1 - q.x0),
                      (int)(q.y1 - q.y0)};
      SDL_RenderCopy(renderer, font_texture, &src, &dst);
    }
  }

  // 3. Draw Cursor (Invert colors of the cell under it)
  VTermPos cursor_pos;
  vterm_state_get_cursorpos(state, &cursor_pos);

  // Blink logic: only draw if blink state is "ON"
  if ((SDL_GetTicks() / 500) % 2) {
    SDL_Rect cursor_rect = {cursor_pos.col * cdata[0].xadvance,
                            cursor_pos.row * FONT_SIZE, cdata[0].xadvance,
                            FONT_SIZE};

    // Draw a semi-transparent white box
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 128); // 50% opacity cursor
    SDL_RenderFillRect(renderer, &cursor_rect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  }

  SDL_RenderPresent(renderer);
  dirty = 0;
}

// vterm callbacks
static int screen_damage(VTermRect rect, void *user) {
  dirty = 1;
  return 1;
}

static void output_callback(const char *s, size_t len, void *user) {
  write(master_fd, s, len);
}

static VTermScreenCallbacks screen_callbacks = {
    .damage = screen_damage,
};

int main() {
  spawn_shell();

  // init libvterm
  vterm = vterm_new(24, 80);
  vterm_output_set_callback(vterm, output_callback, NULL);
  vterm_screen = vterm_obtain_screen(vterm);
  vterm_screen_set_callbacks(vterm_screen, &screen_callbacks, NULL);
  vterm_screen_reset(vterm_screen, 1);

  // Set default colors (White text on black background)
  VTermState *state = vterm_obtain_state(vterm);
  VTermColor default_fg = {0};
  default_fg.type = VTERM_COLOR_RGB;
  default_fg.rgb.red = 255;
  default_fg.rgb.green = 255;
  default_fg.rgb.blue = 255;

  VTermColor default_bg = {0};
  default_bg.type = VTERM_COLOR_RGB;
  default_bg.rgb.red = 0;
  default_bg.rgb.green = 0;
  default_bg.rgb.blue = 0;
  vterm_state_set_default_colors(state, &default_fg, &default_bg);
  vterm_set_utf8(vterm, 1);

  // Init SDL
  SDL_Init(SDL_INIT_VIDEO);
  window = SDL_CreateWindow("Term", 100, 100, 800, 400,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  load_font();

  // Initial resize to match window
  int w, h;
  SDL_GetWindowSize(window, &w, &h);
  int rows = h / FONT_SIZE;
  int cols = w / cdata[0].xadvance;
  vterm_set_size(vterm, rows, cols);
  vterm_screen_flush_damage(vterm_screen);

  // Update PTY size
  struct winsize ws = {rows, cols, 0, 0};
  ioctl(master_fd, TIOCSWINSZ, &ws);

  int running = 1;
  char buffer[4096];

  while (running) {

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        running = 0;

      if (ev.type == SDL_WINDOWEVENT) {
        if (ev.window.event == SDL_WINDOWEVENT_RESIZED) {
          int w = ev.window.data1;
          int h = ev.window.data2;
          int rows = h / FONT_SIZE;
          int cols = w / cdata[0].xadvance;
          vterm_set_size(vterm, rows, cols);
          vterm_screen_flush_damage(vterm_screen);
          struct winsize ws = {rows, cols, 0, 0};
          ioctl(master_fd, TIOCSWINSZ, &ws);
        }
      }

      // 1. Handle Regular Text (Typed characters)
      // We skip this if CTRL is held, because CTRL+C is not "text", it's a
      // command.
      if (ev.type == SDL_TEXTINPUT) {
        SDL_Keymod mod = SDL_GetModState();
        if (!(mod & KMOD_CTRL)) {
          write(master_fd, ev.text.text, strlen(ev.text.text));
        }
      }

      // 2. Handle Special Keys & Ctrl Shortcuts
      if (ev.type == SDL_KEYDOWN) {
        SDL_Keymod mod = SDL_GetModState();
        SDL_Keycode key = ev.key.keysym.sym;
        int ctrl_down = (mod & KMOD_CTRL);
        const char *seq = NULL;

        // --- Handle CTRL + Key ---
        if (ctrl_down) {
          // Ctrl + A-Z maps to bytes 1-26
          if (key >= SDLK_a && key <= SDLK_z) {
            char ch = key - SDLK_a + 1;
            write(master_fd, &ch, 1);
          }
          // Ctrl + [ is Escape (ASCII 27)
          else if (key == SDLK_LEFTBRACKET) {
            char ch = 27;
            write(master_fd, &ch, 1);
          }
          // Ctrl + \ is Quit (ASCII 28)
          else if (key == SDLK_BACKSLASH) {
            char ch = 28;
            write(master_fd, &ch, 1);
          }
        }
        // --- Handle Navigation & Special Keys ---
        else {
          switch (key) {
          case SDLK_RETURN:
            seq = "\r";
            break; // Enter
          case SDLK_BACKSPACE:
            seq = "\x7f";
            break; // Backspace (Del)
          case SDLK_TAB:
            seq = "\t";
            break; // Tab
          case SDLK_ESCAPE:
            seq = "\x1b";
            break; // Escape

          // Arrow Keys (ANSI sequences)
          case SDLK_UP:
            seq = "\x1b[A";
            break;
          case SDLK_DOWN:
            seq = "\x1b[B";
            break;
          case SDLK_RIGHT:
            seq = "\x1b[C";
            break;
          case SDLK_LEFT:
            seq = "\x1b[D";
            break;

          // Navigation
          case SDLK_HOME:
            seq = "\x1b[H";
            break;
          case SDLK_END:
            seq = "\x1b[F";
            break;
          case SDLK_PAGEUP:
            seq = "\x1b[5~";
            break;
          case SDLK_PAGEDOWN:
            seq = "\x1b[6~";
            break;
          case SDLK_DELETE:
            seq = "\x1b[3~";
            break; // Forward Delete
          }

          if (seq) {
            write(master_fd, seq, strlen(seq));
          }
        }
      }
    }
    // Read PTY
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(master_fd, &readfds);
    // struct timeval timeout = {0, 10000}; // 10ms poll
    struct timeval timeout = {0, dirty ? 0 : 10000}; // 0ms or 100ms

    if (select(master_fd + 1, &readfds, NULL, NULL, &timeout) > 0) {
      int len = read(master_fd, buffer, sizeof(buffer));
      if (len > 0) {
        printf("DEBUG: Read %d bytes from PTY: '%.*s'\n", len, len, buffer);
        fflush(stdout);
        vterm_input_write(vterm, buffer, len);
        vterm_screen_flush_damage(vterm_screen);
      } else if (len == -1) {
        perror("read pty");
        running = 0;
      } else {
        running = 0; // EOF
      }
      dirty = 1;
    }

    // render only if dirty
    static int last_blink = 0;
    int now = SDL_GetTicks();
    if (now / 500 != last_blink) {
      dirty = 1;
      last_blink = now / 500;
    }

    if (dirty) {
      render_term();
    }
  }

  SDL_DestroyTexture(font_texture);

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  vterm_free(vterm);
  return 0;
}
