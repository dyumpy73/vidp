#include "player.h"

double get_monotonic_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

void trim_memory(void) {
#if defined(__linux__) && defined(__GLIBC__)
  malloc_trim(0);
#endif
}

int is_media_file(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot) return 0;
  return (strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".mp4") == 0 ||
          strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".webm") == 0 ||
          strcasecmp(dot, ".mov") == 0 || strcasecmp(dot, ".flv") == 0);
}

int compare_strings(const void *a, const void *b) {
  return strcasecmp(*(const char **)a, *(const char **)b);
}

SDL_Cursor* create_blank_cursor(void) {
  SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32,
                                                     SDL_PIXELFORMAT_RGBA32);
  if (!surf) return NULL;
  SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 0, 0, 0, 0));
  SDL_Cursor *cursor = SDL_CreateColorCursor(surf, 0, 0);
  SDL_FreeSurface(surf);
  return cursor;
}
