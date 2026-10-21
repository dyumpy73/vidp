#include "player.h"
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static TTF_Font *g_font = NULL;
static int       g_ttf_ready = 0;

static const char *g_body_fonts[] = {
  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  "/usr/share/fonts/TTF/DejaVuSans.ttf",
  "/usr/share/fonts/dejavu/DejaVuSans.ttf",
  "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
  "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
  NULL
};

static const char *find_font(const char **cands) {
  for (int i = 0; cands[i]; i++) {
    FILE *f = fopen(cands[i], "r");
    if (f) { fclose(f); return cands[i]; }
  }
  return NULL;
}

static int idle_init_ttf(void) {
  if (g_ttf_ready) return 1;
  if (TTF_Init() < 0) {
    fprintf(stderr, "[VidP] TTF_Init failed: %s\n", TTF_GetError());
    return 0;
  }
  const char *bp = find_font(g_body_fonts);
  if (bp) g_font = TTF_OpenFont(bp, 16);
  g_ttf_ready = 1;
  return 1;
}

static void idle_cleanup_ttf(void) {
  if (g_font) { TTF_CloseFont(g_font); g_font = NULL; }
  if (g_ttf_ready) { TTF_Quit(); g_ttf_ready = 0; }
}

static void render_text_centered(SDL_Renderer *r, const char *text, SDL_Color c, int cx, int y) {
  if (!g_font) return;
  SDL_Surface *s = TTF_RenderUTF8_Blended(g_font, text, c);
  if (!s) return;
  SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
  if (t) {
    SDL_Rect dst = { cx - s->w / 2, y, s->w, s->h };
    SDL_RenderCopy(r, t, NULL, &dst);
    SDL_DestroyTexture(t);
  }
  SDL_FreeSurface(s);
}

static void render_text_left(SDL_Renderer *r, const char *text, SDL_Color c, int x, int y) {
  if (!g_font) return;
  SDL_Surface *s = TTF_RenderUTF8_Blended(g_font, text, c);
  if (!s) return;
  SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
  if (t) {
    SDL_Rect dst = { x, y, s->w, s->h };
    SDL_RenderCopy(r, t, NULL, &dst);
    SDL_DestroyTexture(t);
  }
  SDL_FreeSurface(s);
}

static void truncate_left(const char *in, char *out, size_t out_sz, int max_px) {
  if (!g_font) { snprintf(out, out_sz, "%s", in); return; }
  int w = 0, h = 0;
  TTF_SizeUTF8(g_font, in, &w, &h);
  if (w <= max_px) { snprintf(out, out_sz, "%s", in); return; }
  const char *p = in + strlen(in);
  while (p > in) {
    p--;
    snprintf(out, out_sz, "...%s", p);
    TTF_SizeUTF8(g_font, out, &w, &h);
    if (w <= max_px) return;
  }
  snprintf(out, out_sz, "...");
}

typedef struct {
  char *path;
  char *name;
  int   is_dir;
  int   selected;
  int   permanent;
  int   temp_selected;
} PickEntry;

typedef struct {
  PickEntry *entries;
  int        count;
  int        cap;
  int        cursor;
  int        scroll;
  char       cwd[PATH_MAX];
  int        space_held;
  int        anchor;
  Uint32     ghost_until;
} Picker;

static int cmp_entry(const void *a, const void *b) {
  const PickEntry *ea = a, *eb = b;
  if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
  return compare_strings(&ea->name, &eb->name);
}

static void picker_free(Picker *p) {
  for (int i = 0; i < p->count; i++) {
    free(p->entries[i].path);
    free(p->entries[i].name);
  }
  free(p->entries);
  p->entries = NULL;
  p->count = p->cap = 0;
}

static void picker_clear_ghosts(Picker *p) {
  for (int i = 0; i < p->count; i++) {
    if (!p->entries[i].permanent) {
      p->entries[i].temp_selected = 0;
      p->entries[i].selected = 0;
    }
  }
  p->ghost_until = 0;
}

static void picker_apply_range(Picker *p) {
  if (!p->space_held) {
    for (int i = 0; i < p->count; i++) {
      p->entries[i].selected = p->entries[i].permanent;
    }
    return;
  }

  for (int i = 0; i < p->count; i++) {
    p->entries[i].selected = p->entries[i].permanent;
    p->entries[i].temp_selected = 0;
  }
  if (p->anchor < 0 || p->count == 0) return;

  int lo = p->anchor < p->cursor ? p->anchor : p->cursor;
  int hi = p->anchor > p->cursor ? p->anchor : p->cursor;
  if (lo < 0) lo = 0;
  if (hi >= p->count) hi = p->count - 1;

  for (int i = lo; i <= hi; i++) {
    if (!p->entries[i].is_dir && !p->entries[i].permanent) {
      p->entries[i].selected = 1;
      p->entries[i].temp_selected = 1;
    }
  }
}

static void picker_rollback(Picker *p) {
  for (int i = 0; i < p->count; i++) {
    p->entries[i].selected = p->entries[i].permanent;
  }
  p->space_held = 0;
  p->anchor = -1;
  p->ghost_until = SDL_GetTicks() + 600;
}

static int picker_load(Picker *p, const char *dir) {
  picker_free(p);
  snprintf(p->cwd, sizeof(p->cwd), "%s", dir);
  p->anchor = -1;
  p->space_held = 0;
  p->ghost_until = 0;

  DIR *d = opendir(dir);
  if (!d) {
    const char *h = getenv("HOME");
    if (h && strcmp(h, dir) != 0) return picker_load(p, h);
    if (strcmp(dir, "/") != 0) return picker_load(p, "/");
    return 0;
  }

  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') continue;
    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
    if (n < 0 || (size_t)n >= sizeof(full)) continue;
    struct stat st;
    if (stat(full, &st) != 0) continue;
    int is_dir = S_ISDIR(st.st_mode);
    if (!is_dir && !is_media_file(de->d_name)) continue;
    if (p->count >= p->cap) {
      p->cap = p->cap ? p->cap * 2 : 32;
      PickEntry *ne = realloc(p->entries, p->cap * sizeof(PickEntry));
      if (!ne) break;
      p->entries = ne;
    }
    p->entries[p->count].path = strdup(full);
    p->entries[p->count].name = strdup(de->d_name);
    p->entries[p->count].is_dir = is_dir;
    p->entries[p->count].selected = 0;
    p->entries[p->count].permanent = 0;
    p->entries[p->count].temp_selected = 0;
    p->count++;
  }
  closedir(d);

  qsort(p->entries, p->count, sizeof(PickEntry), cmp_entry);
  p->cursor = 0;
  p->scroll = 0;
  return 1;
}

static void picker_draw(SDL_Renderer *ren, Picker *p, int ww, int wh) {
  SDL_Color white = { 230, 230, 235, 255 };
  SDL_Color dim   = { 110, 110, 120, 255 };
  SDL_Color cyan  = { 0x38, 0xBD, 0xF8, 255 };
  SDL_Color orange= { 0xFF, 0x5C, 0x38, 255 };

  SDL_SetRenderDrawColor(ren, 0x10, 0x10, 0x12, 0xFF);
  SDL_RenderClear(ren);

  char hdr[PATH_MAX + 32];
  truncate_left(p->cwd, hdr, sizeof(hdr), ww - 48);
  render_text_left(ren, hdr, cyan, 24, 20);

  const int row_h    = 26;
  const int list_y   = 56;
  const int footer_h = 36;
  int vis = (wh - list_y - footer_h) / row_h;
  if (vis < 1) vis = 1;
  if (vis > 24) vis = 24;

  if (p->cursor < p->scroll) p->scroll = p->cursor;
  if (p->cursor >= p->scroll + vis) p->scroll = p->cursor - vis + 1;
  if (p->scroll < 0) p->scroll = 0;

  for (int i = 0; i < vis; i++) {
    int idx = p->scroll + i;
    if (idx >= p->count) break;
    int y = list_y + i * row_h;

    if (idx == p->cursor) {
      SDL_SetRenderDrawColor(ren, 0x22, 0x22, 0x2E, 255);
      SDL_Rect hl = { 16, y - 2, ww - 32, row_h };
      SDL_RenderFillRect(ren, &hl);
    }

    PickEntry *e = &p->entries[idx];
    char line[PATH_MAX + 8];
    if (e->is_dir) {
      snprintf(line, sizeof(line), "%s/", e->name);
      render_text_left(ren, line, cyan, 32, y);
    } else {
      const char *mark = "  ";
      SDL_Color col = dim;
      if (e->permanent) {
        mark = "* "; col = white;
      } else if (e->temp_selected) {
        if (p->space_held) {
          mark = "* "; col = white;
        } else {
          mark = "~ "; col = orange;
        }
      }
      snprintf(line, sizeof(line), "%s%s", mark, e->name);
      render_text_left(ren, line, col, 32, y);
    }
  }

  if (p->count > vis) {
    int total_h = wh - list_y - footer_h;
    int bar_h = (vis * total_h) / p->count;
    if (bar_h < 20) bar_h = 20;
    int bar_y = list_y + (p->scroll * total_h) / p->count;
    SDL_SetRenderDrawColor(ren, 60, 60, 70, 255);
    SDL_Rect bar = { ww - 8, bar_y, 3, bar_h };
    SDL_RenderFillRect(ren, &bar);
  }

  int nperm = 0, ntemp = 0;
  for (int i = 0; i < p->count; i++) {
    if (p->entries[i].permanent) nperm++;
    if (p->entries[i].temp_selected) ntemp++;
  }

  char footer[256];
  if (p->space_held) {
    snprintf(footer, sizeof(footer),
             "extending +%d  ·  arrows resize  ·  enter commit  ·  release rollback", ntemp);
  } else if (nperm > 0) {
    snprintf(footer, sizeof(footer),
             "%d selected  ·  enter play  ·  space extend  ·  ctrl+enter scan  ·  esc cancel", nperm);
  } else {
    snprintf(footer, sizeof(footer),
             "enter open  ·  space select+extend  ·  ctrl+enter scan folder  ·  esc cancel");
  }
  render_text_left(ren, footer, dim, 24, wh - 28);
}

static int picker_run(SDL_Window *win, SDL_Renderer *ren,
                      char ***out_files, int *out_count) {
  *out_files = NULL;
  *out_count = 0;

  const char *start = getenv("HOME");
  if (!start) start = "/";

  Picker p = {0};
  p.anchor = -1;
  if (!picker_load(&p, start)) return 0;

  int confirmed = 0;
  int done = 0;

  while (!done) {
    const Uint8 *kbd = SDL_GetKeyboardState(NULL);
    if (!kbd[SDL_SCANCODE_SPACE] && p.space_held) {
      picker_rollback(&p);
    }

    if (p.ghost_until && SDL_GetTicks() > p.ghost_until) {
      picker_clear_ghosts(&p);
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        done = 1;
      }
      else if (e.type == SDL_KEYUP) {
        if (e.key.keysym.sym == SDLK_SPACE && p.space_held) {
          picker_rollback(&p);
        }
      }
      else if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;

        if (k == SDLK_ESCAPE) {
          done = 1;
        }
        else if (k == SDLK_UP) {
          if (p.cursor > 0) p.cursor--;
          picker_apply_range(&p);
        }
        else if (k == SDLK_DOWN) {
          if (p.cursor < p.count - 1) p.cursor++;
          picker_apply_range(&p);
        }
        else if (k == SDLK_PAGEUP) {
          p.cursor -= 10; if (p.cursor < 0) p.cursor = 0;
          picker_apply_range(&p);
        }
        else if (k == SDLK_PAGEDOWN) {
          p.cursor += 10;
          if (p.cursor >= p.count) p.cursor = p.count - 1;
          if (p.cursor < 0) p.cursor = 0;
          picker_apply_range(&p);
        }
        else if (k == SDLK_HOME) {
          p.cursor = 0;
          picker_apply_range(&p);
        }
        else if (k == SDLK_END) {
          p.cursor = p.count - 1; if (p.cursor < 0) p.cursor = 0;
          picker_apply_range(&p);
        }
        else if (k == SDLK_BACKSPACE) {
          picker_clear_ghosts(&p);
          char parent[PATH_MAX];
          snprintf(parent, sizeof(parent), "%s", p.cwd);
          char *slash = strrchr(parent, '/');
          if (slash && slash != parent) {
            *slash = 0;
            picker_load(&p, parent);
          } else if (strcmp(parent, "/") != 0) {
            picker_load(&p, "/");
          }
        }
        else if (k == SDLK_a && (e.key.keysym.mod & KMOD_CTRL)) {
          picker_clear_ghosts(&p);
          int all_perm = 1;
          for (int i = 0; i < p.count; i++) {
            if (!p.entries[i].is_dir && !p.entries[i].permanent) {
              all_perm = 0; break;
            }
          }
          for (int i = 0; i < p.count; i++) {
            if (!p.entries[i].is_dir) {
              p.entries[i].permanent = !all_perm;
              p.entries[i].selected = !all_perm;
              p.entries[i].temp_selected = 0;
            }
          }
          p.space_held = 0;
          p.anchor = -1;
        }
        else if (k == SDLK_SPACE) {
          if (e.key.repeat) continue;
          if (p.count > 0 && !p.entries[p.cursor].is_dir) {
            if (!p.space_held) picker_clear_ghosts(&p);

            if (p.entries[p.cursor].permanent) {
              p.entries[p.cursor].permanent = 0;
              p.entries[p.cursor].selected = 0;
              p.entries[p.cursor].temp_selected = 0;
              p.space_held = 0;
              p.anchor = -1;
            } else {
              p.entries[p.cursor].permanent = 1;
              p.entries[p.cursor].selected = 1;
              p.entries[p.cursor].temp_selected = 0;
              p.anchor = p.cursor;
              p.space_held = 1;
              picker_apply_range(&p);
            }
          }
        }
        else if (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_CTRL)) {
          picker_clear_ghosts(&p);
          p.space_held = 0;
          p.anchor = -1;
          char target[PATH_MAX];
          if (p.count > 0 && p.entries[p.cursor].is_dir) {
            snprintf(target, sizeof(target), "%s", p.entries[p.cursor].path);
          } else {
            snprintf(target, sizeof(target), "%s", p.cwd);
          }
          playlist_collect_path(target, out_files, out_count);
          if (*out_count > 0) {
            confirmed = 1;
            done = 1;
          }
        }
        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
          if (p.count == 0) continue;
          PickEntry *cur = &p.entries[p.cursor];
          if (cur->is_dir) {
            picker_clear_ghosts(&p);
            char newpath[PATH_MAX];
            snprintf(newpath, sizeof(newpath), "%s", cur->path);
            picker_load(&p, newpath);
          } else {
            int nsel = 0;
            for (int i = 0; i < p.count; i++) if (p.entries[i].selected) nsel++;
            if (nsel > 0) {
              *out_files = malloc(nsel * sizeof(char *));
              int k2 = 0;
              for (int i = 0; i < p.count; i++)
                if (p.entries[i].selected)
                  (*out_files)[k2++] = strdup(p.entries[i].path);
              *out_count = nsel;
            } else {
              *out_files = malloc(sizeof(char *));
              (*out_files)[0] = strdup(cur->path);
              *out_count = 1;
            }
            confirmed = 1;
            done = 1;
          }
        }
      }
    }

    if (done) break;

    int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    picker_draw(ren, &p, ww, wh);
    SDL_RenderPresent(ren);
    SDL_Delay(16);
  }

  picker_free(&p);
  return confirmed;
}

int player_idle_run(char ***out_playlist, int *out_count) {
  *out_playlist = NULL;
  *out_count = 0;

  idle_init_ttf();

  SDL_Window *win = SDL_CreateWindow("VidP",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      900, 600, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!win) { idle_cleanup_ttf(); return 0; }

  SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  if (!ren) { SDL_DestroyWindow(win); idle_cleanup_ttf(); return 0; }

  SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

  int got_files = 0;
  int quit = 0;

  while (!quit && !got_files) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        quit = 1;
      } else if (e.type == SDL_DROPFILE) {
        playlist_collect_path(e.drop.file, out_playlist, out_count);
        SDL_free(e.drop.file);
        if (*out_count > 0) got_files = 1;
      } else if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_ESCAPE) {
          quit = 1;
        } else if ((e.key.keysym.sym == SDLK_o) &&
                   (e.key.keysym.mod & KMOD_CTRL) && !e.key.repeat) {
          if (picker_run(win, ren, out_playlist, out_count)) {
            got_files = 1;
          }
        }
      }
    }

    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);

    SDL_SetRenderDrawColor(ren, 0x10, 0x10, 0x12, 0xFF);
    SDL_RenderClear(ren);

    SDL_Color dim = { 80, 80, 90, 255 };
    render_text_centered(ren, "ctrl+o open   ·   esc quit", dim, ww/2, wh - 32);

    SDL_RenderPresent(ren);
    SDL_Delay(16);
  }

  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  idle_cleanup_ttf();

  return got_files;
}
