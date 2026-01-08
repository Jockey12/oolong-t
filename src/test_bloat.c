#include <SDL2/SDL.h>
#include <stdio.h>

int main() {
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *w = SDL_CreateWindow("Test", 0, 0, 100, 100, SDL_WINDOW_SHOWN);
  SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);

  printf("Window created. Check btop now.\n");
  printf("Press Enter to quit...\n");
  getchar();

  SDL_Quit();
  return 0;
}
