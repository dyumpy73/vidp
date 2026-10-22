#include "player.h"
#include <ctype.h>

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
          strcasecmp(dot, ".mov") == 0 || strcasecmp(dot, ".flv") == 0 ||
          strcasecmp(dot, ".m4v") == 0 || strcasecmp(dot, ".ts") == 0 ||
          strcasecmp(dot, ".wmv") == 0 || strcasecmp(dot, ".mpg") == 0 ||
          strcasecmp(dot, ".mpeg") == 0 || strcasecmp(dot, ".ogv") == 0);
}

int compare_strings(const void *a, const void *b) {
  const unsigned char *s1 = (const unsigned char *)*(const char **)a;
  const unsigned char *s2 = (const unsigned char *)*(const char **)b;

  while (*s1 && *s2) {
    if (isdigit(*s1) && isdigit(*s2)) {
      const unsigned char *z1 = s1, *z2 = s2;
      while (*z1 == '0') z1++;
      while (*z2 == '0') z2++;
      const unsigned char *e1 = z1, *e2 = z2;
      while (isdigit(*e1)) e1++;
      while (isdigit(*e2)) e2++;
      size_t l1 = (size_t)(e1 - z1), l2 = (size_t)(e2 - z2);
      if (l1 != l2) return l1 < l2 ? -1 : 1;
      int c = memcmp(z1, z2, l1);
      if (c) return c < 0 ? -1 : 1;
      s1 = e1;
      s2 = e2;
    } else {
      int c1 = tolower(*s1), c2 = tolower(*s2);
      if (c1 != c2) return c1 < c2 ? -1 : 1;
      s1++;
      s2++;
    }
  }
  return (*s1 ? 1 : 0) - (*s2 ? 1 : 0);
}

SDL_Cursor* create_blank_cursor(void) {
  SDL_Surface *surf = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
  if (!surf) return NULL;
  SDL_FillSurfaceRect(surf, NULL, SDL_MapSurfaceRGBA(surf, 0, 0, 0, 0));
  SDL_Cursor *cursor = SDL_CreateColorCursor(surf, 0, 0);
  SDL_DestroySurface(surf);
  return cursor;
}
