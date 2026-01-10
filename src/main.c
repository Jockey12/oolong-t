#define _GNU_SOURCE
#include "libs.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// --- Config ---
#define FONT_PATH "src/font.ttf"
#define FONT_SIZE 25.0f
#define ATLAS_WIDTH 2048
#define ATLAS_HEIGHT 2048

// --- Globals ---
int master_fd;
pid_t child_pid;
VTerm *vterm;
VTermScreen *vterm_screen;
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *font_texture;
int dirty = 1;

int cell_width = 0;
int cell_height = 0;

// --- Font Data Ranges ---
stbtt_packedchar ascii_chars[96];      // 32..126 (Standard Text)
stbtt_packedchar box_chars[128];       // U+2500..U+257F (Borders)
stbtt_packedchar powerline_chars[128]; // U+E0A0..U+E0FF (Powerline triangles)
stbtt_packedchar
    icon_chars[4096]; // U+E5FA..U+F500 (DevIcons, FontAwesome, etc)

// --- PTY Setup (Standard) ---
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
  if (!slave_name) {
    perror("ptsname");
    exit(1);
  }

  child_pid = fork();
  if (child_pid == 0) {
    setsid();
    int slave_fd = open(slave_name, O_RDWR);
    ioctl(slave_fd, TIOCSCTTY, 0);
    dup2(slave_fd, 0);
    dup2(slave_fd, 1);
    dup2(slave_fd, 2);
    close(master_fd);
    close(slave_fd);

    setenv("TERM", "xterm-256color", 1);
    unsetenv("COLUMNS");
    unsetenv("LINES");
    execlp("bash", "bash", NULL);
    exit(1);
  }
}

// --- Font Loading ---
void load_font() {
  int fd = open(FONT_PATH, O_RDONLY);
  if (fd == -1) {
    printf("Error opening font: %s\n", FONT_PATH);
    exit(1);
  }
  struct stat sb;
  fstat(fd, &sb);
  unsigned char *ttf_buffer =
      mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  unsigned char *temp_bitmap = calloc(1, ATLAS_WIDTH * ATLAS_HEIGHT);

  stbtt_pack_context spc;
  if (!stbtt_PackBegin(&spc, temp_bitmap, ATLAS_WIDTH, ATLAS_HEIGHT, 0, 1,
                       NULL)) {
    printf("Failed to init font packer\n");
    exit(1);
  }

  // 1. Pack ASCII
  stbtt_PackFontRange(&spc, ttf_buffer, 0, FONT_SIZE, 32, 96, ascii_chars);

  // 2. Pack Box Drawing (Tmux borders)
  stbtt_PackFontRange(&spc, ttf_buffer, 0, FONT_SIZE, 0x2500, 128, box_chars);

  // 3. Pack Powerline Symbols (The triangles in shell prompts)
  stbtt_PackFontRange(&spc, ttf_buffer, 0, FONT_SIZE, 0xE0A0, 96,
                      powerline_chars);

  // 4. Pack Common Nerd Font Icons (Folder icons, git logos, etc)
  // This range (E5FA to F500) covers Seti-UI, Devicons, and FontAwesome
  stbtt_PackFontRange(&spc, ttf_buffer, 0, FONT_SIZE, 0xE5FA, 3800, icon_chars);

  stbtt_PackEnd(&spc);
  munmap(ttf_buffer, sb.st_size);

  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
      0, ATLAS_WIDTH, ATLAS_HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);
  SDL_LockSurface(surface);
  Uint32 *pixels = (Uint32 *)surface->pixels;

  for (int i = 0; i < ATLAS_WIDTH * ATLAS_HEIGHT; i++) {
    Uint8 alpha = temp_bitmap[i];
    pixels[i] = (alpha > 0) ? SDL_MapRGBA(surface->format, 255, 255, 255, alpha)
                            : SDL_MapRGBA(surface->format, 0, 0, 0, 0);
  }

  SDL_UnlockSurface(surface);
  free(temp_bitmap);

  font_texture = SDL_CreateTextureFromSurface(renderer, surface);
  SDL_FreeSurface(surface);
  SDL_SetTextureBlendMode(font_texture, SDL_BLENDMODE_BLEND);

  // Metrics
  if (ascii_chars[0].xadvance == 0)
    ascii_chars[0].xadvance = FONT_SIZE / 2;
  cell_width = (int)ceilf(ascii_chars[0].xadvance);
  cell_height = (int)FONT_SIZE;

  printf("Font loaded. Cell size: %dx%d\n", cell_width, cell_height);
}

// --- Rendering ---
void render_term() {
  VTermState *state = vterm_obtain_state(vterm);
  VTermColor default_fg, default_bg;
  vterm_state_get_default_colors(state, &default_fg, &default_bg);
  SDL_SetRenderDrawColor(renderer, default_bg.rgb.red, default_bg.rgb.green,
                         default_bg.rgb.blue, 255);
  SDL_RenderClear(renderer);

  int rows, cols;
  vterm_get_size(vterm, &rows, &cols);

  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols; col++) {
      VTermScreenCell cell;
      VTermPos pos = {row, col};
      vterm_screen_get_cell(vterm_screen, pos, &cell);

      uint32_t code = cell.chars[0];

      // Draw Background
      vterm_state_convert_color_to_rgb(state, &cell.bg);
      if (cell.bg.rgb.red != default_bg.rgb.red ||
          cell.bg.rgb.green != default_bg.rgb.green ||
          cell.bg.rgb.blue != default_bg.rgb.blue) {
        SDL_SetRenderDrawColor(renderer, cell.bg.rgb.red, cell.bg.rgb.green,
                               cell.bg.rgb.blue, 255);
        SDL_Rect bg_rect = {col * cell_width, row * cell_height, cell_width,
                            cell_height};
        SDL_RenderFillRect(renderer, &bg_rect);
      }

      // Resolve Glyph
      stbtt_packedchar *b = NULL;

      if (code >= 32 && code < 128) {
        b = &ascii_chars[code - 32];
      } else if (code >= 0x2500 && code < 0x2580) {
        b = &box_chars[code - 0x2500];
      } else if (code >= 0xE0A0 && code < 0xE100) {
        b = &powerline_chars[code - 0xE0A0];
      } else if (code >= 0xE5FA && code < 0xF500) {
        b = &icon_chars[code - 0xE5FA];
      } else {
        continue;
      }

      // Draw Glyph
      vterm_state_convert_color_to_rgb(state, &cell.fg);
      SDL_SetTextureColorMod(font_texture, cell.fg.rgb.red, cell.fg.rgb.green,
                             cell.fg.rgb.blue);

      stbtt_aligned_quad q;
      float x = col * cell_width;
      float y = (row * cell_height) + (cell_height * 0.75f);

      stbtt_GetPackedQuad(b, ATLAS_WIDTH, ATLAS_HEIGHT, 0, &x, &y, &q, 1);

      SDL_Rect src = {(int)(q.s0 * ATLAS_WIDTH), (int)(q.t0 * ATLAS_HEIGHT),
                      (int)((q.s1 - q.s0) * ATLAS_WIDTH),
                      (int)((q.t1 - q.t0) * ATLAS_HEIGHT)};
      SDL_Rect dst = {(int)q.x0, (int)q.y0, (int)(q.x1 - q.x0),
                      (int)(q.y1 - q.y0)};
      SDL_RenderCopy(renderer, font_texture, &src, &dst);
    }
  }

  // Cursor
  VTermPos cursor_pos;
  vterm_state_get_cursorpos(state, &cursor_pos);
  if ((SDL_GetTicks() / 500) % 2) {
    SDL_Rect cursor_rect = {cursor_pos.col * cell_width,
                            cursor_pos.row * cell_height, cell_width,
                            cell_height};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 128);
    SDL_RenderFillRect(renderer, &cursor_rect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  }

  SDL_RenderPresent(renderer);
  dirty = 0;
}

// --- Main ---
static int damage(VTermRect r, void *u) {
  dirty = 1;
  return 1;
}
static void out_cb(const char *s, size_t l, void *u) { write(master_fd, s, l); }
static VTermScreenCallbacks cbs = {.damage = damage};

int main() {
  spawn_shell();

  vterm = vterm_new(24, 80);
  vterm_output_set_callback(vterm, out_cb, NULL);
  vterm_screen = vterm_obtain_screen(vterm);
  vterm_screen_set_callbacks(vterm_screen, &cbs, NULL);
  vterm_screen_reset(vterm_screen, 1);
  vterm_set_utf8(vterm, 1);

  VTermState *state = vterm_obtain_state(vterm);
  VTermColor fg = {.type = VTERM_COLOR_RGB, .rgb = {255, 255, 255}};
  VTermColor bg = {.type = VTERM_COLOR_RGB, .rgb = {0, 0, 0}};
  vterm_state_set_default_colors(state, &fg, &bg);

  SDL_Init(SDL_INIT_VIDEO);
  window =
      SDL_CreateWindow("Term", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                       800, 600, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  load_font();

  // Initial Sizing
  int w, h;
  SDL_GetWindowSize(window, &w, &h);
  int rows = h / cell_height;
  int cols = w / cell_width;
  vterm_set_size(vterm, rows, cols);
  struct winsize ws = {rows, cols, 0, 0};
  ioctl(master_fd, TIOCSWINSZ, &ws);

  int running = 1;
  char buffer[4096];

  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        running = 0;

      if (ev.type == SDL_WINDOWEVENT &&
          ev.window.event == SDL_WINDOWEVENT_RESIZED) {
        int w = ev.window.data1;
        int h = ev.window.data2;
        int rows = h / cell_height;
        int cols = w / cell_width;
        vterm_set_size(vterm, rows, cols);
        vterm_screen_flush_damage(vterm_screen);
        struct winsize ws = {rows, cols, 0, 0};
        ioctl(master_fd, TIOCSWINSZ, &ws);
        dirty = 1;
      }
      if (ev.type == SDL_TEXTINPUT && !(SDL_GetModState() & KMOD_CTRL)) {
        write(master_fd, ev.text.text, strlen(ev.text.text));
      }
      if (ev.type == SDL_KEYDOWN) {
        SDL_Keycode key = ev.key.keysym.sym;
        if (SDL_GetModState() & KMOD_CTRL) {
          if (key >= SDLK_a && key <= SDLK_z) {
            char c = key - SDLK_a + 1;
            write(master_fd, &c, 1);
          } else if (key == SDLK_c) {
            char c = 3;
            write(master_fd, &c, 1);
          } else if (key == SDLK_LEFTBRACKET) {
            char c = 27;
            write(master_fd, &c, 1);
          }
        } else {
          const char *seq = NULL;
          if (key == SDLK_RETURN)
            seq = "\r";
          else if (key == SDLK_BACKSPACE)
            seq = "\x7f";
          else if (key == SDLK_ESCAPE)
            seq = "\x1b";
          else if (key == SDLK_TAB)
            seq = "\t";
          else if (key == SDLK_UP)
            seq = "\x1b[A";
          else if (key == SDLK_DOWN)
            seq = "\x1b[B";
          else if (key == SDLK_RIGHT)
            seq = "\x1b[C";
          else if (key == SDLK_LEFT)
            seq = "\x1b[D";
          else if (key == SDLK_PAGEUP)
            seq = "\x1b[5~";
          else if (key == SDLK_PAGEDOWN)
            seq = "\x1b[6~";
          else if (key == SDLK_HOME)
            seq = "\x1b[H";
          else if (key == SDLK_END)
            seq = "\x1b[F";

          if (seq)
            write(master_fd, seq, strlen(seq));
        }
      }
    }

    fd_set rfd;
    FD_ZERO(&rfd);
    FD_SET(master_fd, &rfd);
    struct timeval tv = {0, dirty ? 0 : 10000};

    if (select(master_fd + 1, &rfd, NULL, NULL, &tv) > 0) {
      int len = read(master_fd, buffer, sizeof(buffer));
      if (len > 0) {
        vterm_input_write(vterm, buffer, len);
        vterm_screen_flush_damage(vterm_screen);
        dirty = 1;
      } else if (len < 0 && errno != EIO)
        running = 0;
    }

    if (dirty)
      render_term();
    static int last_blink = 0;
    if (SDL_GetTicks() / 500 != last_blink) {
      dirty = 1;
      last_blink = SDL_GetTicks() / 500;
    }
  }

  SDL_Quit();
  vterm_free(vterm);
  return 0;
}
