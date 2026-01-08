#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <vterm.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define FONT_PATH "src/font.ttf"
#define FONT_SIZE 16.0f
#define WIN_WIDTH 800
#define WIN_HEIGHT 600

// --- Globals ---
Display *dpy;
Window win;
int screen;
GC gc;
XImage *ximage;
char *buffer_data; // The raw pixel buffer

// PTY & VTerm globals (same as before)
int master_fd;
VTerm *vterm;
VTermScreen *vterm_screen;
stbtt_bakedchar cdata[224];
int dirty = 1;

// --- Helper: Get time in ms ---
long get_time_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

// --- 1. PTY Logic (Identical to your SDL version) ---
void spawn_shell() {
  master_fd = posix_openpt(O_RDWR | O_NOCTTY);
  grantpt(master_fd);
  unlockpt(master_fd);
  char *slave_name = ptsname(master_fd);

  if (fork() == 0) {
    setsid();
    int slave = open(slave_name, O_RDWR);
    ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    close(master_fd);
    close(slave);
    setenv("TERM", "xterm-256color", 1);
    unsetenv("COLUMNS");
    unsetenv("LINES");
    execlp("bash", "bash", "-l", NULL);
    exit(1);
  }
}

// --- 2. Font Loading (Modified for Raw Buffer) ---
// We don't create an SDL Texture. We just keep the raw bitmap.
unsigned char *font_bitmap;

void load_font() {
  FILE *f = fopen(FONT_PATH, "rb");
  if (!f)
    exit(1);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *ttf = malloc(size);
  fread(ttf, 1, size, f);
  fclose(f);

  font_bitmap = malloc(512 * 512);
  stbtt_BakeFontBitmap(ttf, 0, FONT_SIZE, font_bitmap, 512, 512, 32, 224,
                       cdata);
  free(ttf);
  if (cdata[0].xadvance == 0)
    cdata[0].xadvance = FONT_SIZE / 2;
}

// --- 3. Software Rendering to XImage ---
// This writes pixels directly to CPU memory (buffer_data)
void put_pixel(int x, int y, uint32_t color) {
  if (x < 0 || x >= WIN_WIDTH || y < 0 || y >= WIN_HEIGHT)
    return;
  uint32_t *pixels = (uint32_t *)buffer_data;
  pixels[y * WIN_WIDTH + x] = color;
}

void render_term() {
  int rows, cols;
  vterm_get_size(vterm, &rows, &cols);

  // Clear Screen (Black)
  for (int i = 0; i < WIN_WIDTH * WIN_HEIGHT; i++)
    ((uint32_t *)buffer_data)[i] = 0x000000;

  VTermState *state = vterm_obtain_state(vterm);

  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols; col++) {
      VTermScreenCell cell;
      VTermPos pos = {row, col};
      vterm_screen_get_cell(vterm_screen, pos, &cell);

      uint32_t code = cell.chars[0];
      if (code <= 32 || code > 255)
        continue;

      // Simple white text color
      uint32_t fg_color = 0xFFFFFF;

      // Draw Glyph from Font Bitmap
      stbtt_aligned_quad q;
      float x = col * cdata[0].xadvance;
      float y = (row * FONT_SIZE) + (FONT_SIZE * 0.75f);
      stbtt_GetBakedQuad(cdata, 512, 512, code - 32, &x, &y, &q, 1);

      int x0 = (int)q.x0;
      int x1 = (int)q.x1;
      int y0 = (int)q.y0;
      int y1 = (int)q.y1;
      int s0 = (int)(q.s0 * 512);
      int t0 = (int)(q.t0 * 512);

      // Manual Blit Loop
      for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
          int tex_x = s0 + (px - x0);
          int tex_y = t0 + (py - y0);
          if (font_bitmap[tex_y * 512 + tex_x] > 0) {
            put_pixel(px, py, fg_color);
          }
        }
      }
    }
  }

  // Blit buffer to X Window
  XPutImage(dpy, win, gc, ximage, 0, 0, 0, 0, WIN_WIDTH, WIN_HEIGHT);
  dirty = 0;
}

// --- VTerm Callbacks ---
static int damage(VTermRect r, void *u) {
  dirty = 1;
  return 1;
}
static void out_cb(const char *s, size_t l, void *u) { write(master_fd, s, l); }
static VTermScreenCallbacks cbs = {.damage = damage};

int main() {
  spawn_shell();

  // Init VTerm
  vterm = vterm_new(24, 80);
  vterm_output_set_callback(vterm, out_cb, NULL);
  vterm_screen = vterm_obtain_screen(vterm);
  vterm_screen_set_callbacks(vterm_screen, &cbs, NULL);
  vterm_screen_reset(vterm_screen, 1);
  load_font();

  // --- Init X11 ---
  dpy = XOpenDisplay(NULL);
  if (!dpy)
    exit(1);
  screen = DefaultScreen(dpy);
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 10, 10, WIN_WIDTH,
                            WIN_HEIGHT, 1, BlackPixel(dpy, screen),
                            BlackPixel(dpy, screen));
  XSelectInput(dpy, win, ExposureMask | KeyPressMask);
  XMapWindow(dpy, win);
  gc = XCreateGC(dpy, win, 0, NULL);

  // Create XImage buffer for software rendering
  buffer_data = malloc(WIN_WIDTH * WIN_HEIGHT * 4);
  ximage = XCreateImage(dpy, DefaultVisual(dpy, screen), 24, ZPixmap, 0,
                        buffer_data, WIN_WIDTH, WIN_HEIGHT, 32, 0);

  // Main Loop
  while (1) {
    // 1. Check X11 Events (Non-blocking)
    while (XPending(dpy) > 0) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      if (ev.type == KeyPress) {
        // Handle Key Input (Basic)
        char buf[32];
        KeySym keysym;
        int len = XLookupString(&ev.xkey, buf, sizeof(buf), &keysym, NULL);
        if (len > 0)
          write(master_fd, buf, len);
      }
    }

    // 2. Check PTY
    fd_set rfd;
    FD_ZERO(&rfd);
    FD_SET(master_fd, &rfd);
    struct timeval tv = {0, 10000};
    if (select(master_fd + 1, &rfd, NULL, NULL, &tv) > 0) {
      char b[4096];
      int l = read(master_fd, b, sizeof(b));
      if (l > 0)
        vterm_input_write(vterm, b, l);
    }

    // 3. Render
    if (dirty)
      render_term();
  }
}
