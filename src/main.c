#define _XOPEN_SOURCE 600
#include <SDL2/SDL.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#include <vterm.h>

int master_fd;
pid_t child_pid;

VTerm *vterm;
VTermScreen *vterm_screen;

void spawn_shell() {
  master_fd = posix_openpt(O_RDWR | O_NOCTTY);
  grantpt(master_fd);
  unlockpt(master_fd);
  char *slave_name = ptsname(master_fd);

  child_pid = fork();
  if (child_pid == 0) {
    // child porcess (shell)
    int slave_fd = open(slave_name, O_RDWR);
    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);
    close(master_fd);
    close(slave_fd);

    setenv("TERM", "xterm-256color", 1);
    execlp("/bin/fish", "/bin/bash", NULL);
    exit(1);
  }
}

static int screen_damage(VTermRect rect, void *user) {
  // trigger gpu re-render.
  printf("DEBUG: Shell requested update at rect [%d,%d] to [%d,%d]\n",
         rect.start_col, rect.start_row, rect.end_col, rect.end_row);
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
}
