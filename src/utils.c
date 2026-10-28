#include "player.h"
#include <ctype.h>

#define TC_N 96
typedef struct { char *text; SDL_Color c; SDL_Texture *t; int w, h; unsigned long used; } TextEnt;
static TextEnt g_tc[TC_N];
static unsigned long g_tc_tick = 0;
static TTF_Font *g_ui_font = NULL;

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

void ui_set_font(TTF_Font *font) { g_ui_font = font; }

void ui_clear_cache(void) {
  for (int i = 0; i < TC_N; i++) {
    if (g_tc[i].t) SDL_DestroyTexture(g_tc[i].t);
    free(g_tc[i].text);
    memset(&g_tc[i], 0, sizeof(g_tc[i]));
  }
}

static TextEnt *text_cache_get(SDL_Renderer *r, const char *text, SDL_Color c) {
  if (!g_ui_font) return NULL;
  for (int i = 0; i < TC_N; i++) {
    TextEnt *e = &g_tc[i];
    if (e->t && e->text && !strcmp(e->text, text) &&
        e->c.r == c.r && e->c.g == c.g && e->c.b == c.b && e->c.a == c.a) {
      e->used = ++g_tc_tick;
      return e;
    }
  }
  int victim = 0;
  for (int i = 0; i < TC_N; i++) {
    if (!g_tc[i].t) { victim = i; break; }
    if (g_tc[i].used < g_tc[victim].used) victim = i;
  }
  TextEnt *e = &g_tc[victim];
  if (e->t) SDL_DestroyTexture(e->t);
  free(e->text);
  memset(e, 0, sizeof(*e));

  SDL_Surface *s = TTF_RenderText_Blended(g_ui_font, text, 0, c);
  if (!s) return NULL;
  e->t = SDL_CreateTextureFromSurface(r, s);
  e->w = s->w; e->h = s->h;
  SDL_DestroySurface(s);
  e->text = strdup(text);
  if (!e->t || !e->text) { ui_clear_cache(); return NULL; }
  e->c = c;
  e->used = ++g_tc_tick;
  return e;
}

void ui_text_left(SDL_Renderer *r, const char *text, SDL_Color c, int x, int y) {
  TextEnt *e = text_cache_get(r, text, c);
  if (!e) return;
  SDL_FRect dst = { (float)x, (float)y, (float)e->w, (float)e->h };
  SDL_RenderTexture(r, e->t, NULL, &dst);
}

void ui_text_center(SDL_Renderer *r, const char *text, SDL_Color c, int cx, int y) {
  TextEnt *e = text_cache_get(r, text, c);
  if (!e) return;
  SDL_FRect dst = { (float)(cx - e->w / 2), (float)y, (float)e->w, (float)e->h };
  SDL_RenderTexture(r, e->t, NULL, &dst);
}
