#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "player.h"
#include <ctype.h>
#include <stdlib.h>
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
  "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
  "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
  "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
  "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
  "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
  "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
  "/usr/share/fonts/noto/NotoSans-Regular.ttf",
  NULL
};

static const char *find_font(const char **cands) {
  for (int i = 0; cands[i]; i++) {
    FILE *f = fopen(cands[i], "r");
    if (f) { fclose(f); return cands[i]; }
  }
  return NULL;
}

static char g_fc_font[PATH_MAX];
static const char *find_font_fc(void) {
  FILE *p = popen("fc-match -f '%{file}' sans 2>/dev/null", "r");
  if (!p) return NULL;
  size_t n = fread(g_fc_font, 1, sizeof(g_fc_font) - 1, p);
  pclose(p);
  g_fc_font[n] = '\0';
  return n ? g_fc_font : NULL;
}

int player_init_font(void) {
  if (g_ttf_ready) return 1;
  if (!TTF_Init()) {
    fprintf(stderr, "[VidP] TTF_Init failed: %s\n", SDL_GetError());
    return 0;
  }
  const char *bp = find_font(g_body_fonts);
  if (!bp) bp = find_font_fc();
  if (bp) g_font = TTF_OpenFont(bp, 16);
  ui_set_font(g_font);
  if (!g_font)
    fprintf(stderr, "[VidP] Warning: no UI font found; idle/picker text will be blank. "
                    "Install DejaVu/Liberation/Noto fonts.\n");
  g_ttf_ready = 1;
  return 1;
}

void player_cleanup_font(void) {
  ui_set_font(NULL);
  if (g_font) { TTF_CloseFont(g_font); g_font = NULL; }
  if (g_ttf_ready) { TTF_Quit(); g_ttf_ready = 0; }
}

static void truncate_left(const char *in, char *out, size_t out_sz, int max_px) {
  if (!g_font) { snprintf(out, out_sz, "%s", in); return; }
  int w = 0, h = 0;
  TTF_GetStringSize(g_font, in, 0, &w, &h);
  if (w <= max_px) { snprintf(out, out_sz, "%s", in); return; }
  const char *p = in;
  while (*p) {
    p++;
    while ((*p & 0xC0) == 0x80) p++;
    snprintf(out, out_sz, "...%s", p);
    TTF_GetStringSize(g_font, out, 0, &w, &h);
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
  int        op_select;
} Picker;

/* Permanents survive folder navigation within the picker session.
   Insertion order is preserved; removal uses memmove. */
static char **g_permanent_paths = NULL;
static int    g_permanent_count = 0;

static int picker_is_permanent(const char *path) {
  for (int i = 0; i < g_permanent_count; i++)
    if (strcmp(g_permanent_paths[i], path) == 0) return 1;
  return 0;
}

static void picker_set_permanent(const char *path, int perm) {
  int found = -1;
  for (int i = 0; i < g_permanent_count; i++)
    if (strcmp(g_permanent_paths[i], path) == 0) { found = i; break; }

  if (perm && found < 0) {
    char **np = realloc(g_permanent_paths, (g_permanent_count + 1) * sizeof(char *));
    if (!np) return;
    g_permanent_paths = np;
    char *dup = strdup(path);
    if (!dup) return;
    g_permanent_paths[g_permanent_count++] = dup;
  } else if (!perm && found >= 0) {
    free(g_permanent_paths[found]);
    memmove(&g_permanent_paths[found], &g_permanent_paths[found + 1],
            (size_t)(g_permanent_count - found - 1) * sizeof(char *));
    g_permanent_count--;
  }
}

static void picker_free_permanents(void) {
  for (int i = 0; i < g_permanent_count; i++) free(g_permanent_paths[i]);
  free(g_permanent_paths);
  g_permanent_paths = NULL;
  g_permanent_count = 0;
}

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

static void picker_apply_range(Picker *p) {
  for (int i = 0; i < p->count; i++) {
    p->entries[i].selected = p->entries[i].permanent;
  }
  if (!p->space_held || p->anchor < 0 || p->count == 0) return;

  int lo = p->anchor < p->cursor ? p->anchor : p->cursor;
  int hi = p->anchor > p->cursor ? p->anchor : p->cursor;
  if (lo < 0) lo = 0;
  if (hi >= p->count) hi = p->count - 1;

  for (int i = lo; i <= hi; i++) {
    if (!p->entries[i].is_dir) {
      p->entries[i].selected = p->op_select;
    }
  }
}

static void picker_commit(Picker *p) {
  for (int i = 0; i < p->count; i++) {
    p->entries[i].permanent = p->entries[i].selected;
    if (!p->entries[i].is_dir)
      picker_set_permanent(p->entries[i].path, p->entries[i].selected);
  }
  p->space_held = 0;
  p->anchor = -1;
}

static int picker_load(Picker *p, const char *dir) {
  picker_free(p);
  snprintf(p->cwd, sizeof(p->cwd), "%s", dir);
  p->anchor = -1;
  p->space_held = 0;

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

    char *pdup = strdup(full);
    char *ndup = strdup(de->d_name);
    if (!pdup || !ndup) { free(pdup); free(ndup); continue; }

    if (p->count >= p->cap) {
      int new_cap = p->cap ? p->cap * 2 : 32;
      PickEntry *ne = realloc(p->entries, (size_t)new_cap * sizeof(PickEntry));
      if (!ne) { free(pdup); free(ndup); break; }
      p->entries = ne;
      p->cap = new_cap;
    }
    p->entries[p->count].path      = pdup;
    p->entries[p->count].name      = ndup;
    p->entries[p->count].is_dir    = is_dir;
    p->entries[p->count].permanent = is_dir ? 0 : picker_is_permanent(full);
    p->entries[p->count].selected  = p->entries[p->count].permanent;
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

  SDL_SetRenderDrawColor(ren, 0x10, 0x10, 0x12, 0xFF);
  SDL_RenderClear(ren);

  char hdr[PATH_MAX + 32];
  truncate_left(p->cwd, hdr, sizeof(hdr), ww - 48);
  ui_text_left(ren, hdr, cyan, 24, 20);

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
      SDL_FRect hl = { 16, (float)y - 2, (float)ww - 32, (float)row_h };
      SDL_RenderFillRect(ren, &hl);
    }

    PickEntry *e = &p->entries[idx];
    char line[PATH_MAX + 8];
    if (e->is_dir) {
      snprintf(line, sizeof(line), "%s/", e->name);
      ui_text_left(ren, line, cyan, 32, y);
    } else {
      const char *mark = "  ";
      SDL_Color col = dim;
      if (e->selected) { mark = "* "; col = white; }
      snprintf(line, sizeof(line), "%s%s", mark, e->name);
      ui_text_left(ren, line, col, 32, y);
    }
  }

  if (p->count > vis) {
    int total_h = wh - list_y - footer_h;
    int bar_h = (vis * total_h) / p->count;
    if (bar_h < 20) bar_h = 20;
    int bar_y = list_y + (p->scroll * total_h) / p->count;
    SDL_SetRenderDrawColor(ren, 60, 60, 70, 255);
    SDL_FRect bar = { (float)ww - 8, (float)bar_y, 3, (float)bar_h };
    SDL_RenderFillRect(ren, &bar);
  }

  int nsel = 0;
  for (int i = 0; i < p->count; i++) if (p->entries[i].selected) nsel++;

  char footer[256];
  if (p->space_held) {
    snprintf(footer, sizeof(footer),
             "%s range  ·  arrows resize  ·  release to commit",
             p->op_select ? "selecting" : "unselecting");
  } else if (nsel > 0) {
    snprintf(footer, sizeof(footer),
             "%d selected  ·  enter play  ·  space select/unselect  ·  ctrl+enter scan  ·  esc cancel", nsel);
  } else {
    snprintf(footer, sizeof(footer),
             "enter open  ·  space select+extend  ·  ctrl+enter scan folder  ·  esc cancel");
  }
  ui_text_left(ren, footer, dim, 24, wh - 28);
}

static int picker_run(SDL_Window *win, SDL_Renderer *ren, char ***out_files, int *out_count, int *quit_flag) {
  *out_files = NULL;
  *out_count = 0;

  const char *start = getenv("HOME");
  if (!start) start = "/";

  Picker p = {0};
  p.anchor = -1;
  if (!picker_load(&p, start)) return 0;

  int confirmed = 0;
  int done = 0;
  int dirty = 1;

  while (!done) {
    const bool *kbd = SDL_GetKeyboardState(NULL);
    if (!kbd[SDL_SCANCODE_SPACE] && p.space_held) {
      picker_commit(&p);
      dirty = 1;
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      dirty = 1;
      if (e.type == SDL_EVENT_QUIT) {
        if (quit_flag) *quit_flag = 1;
        done = 1;
      }
      else if (e.type == SDL_EVENT_KEY_UP) {
        if ((e.key.key == SDLK_SPACE ||
             e.key.scancode == SDL_SCANCODE_SPACE) && p.space_held) {
          picker_commit(&p);
        }
      }
      else if (e.type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode k = e.key.key;

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
          picker_commit(&p);
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
        else if (k == SDLK_A && (e.key.mod & SDL_KMOD_CTRL)) {
          picker_commit(&p);
          int all_perm = 1;
          for (int i = 0; i < p.count; i++) {
            if (!p.entries[i].is_dir && !p.entries[i].permanent) { all_perm = 0; break; }
          }
          for (int i = 0; i < p.count; i++) {
            if (!p.entries[i].is_dir) {
              p.entries[i].permanent = !all_perm;
              p.entries[i].selected  = !all_perm;
              picker_set_permanent(p.entries[i].path, !all_perm);
            }
          }
          p.space_held = 0;
          p.anchor = -1;
        }
        else if (k == SDLK_SPACE) {
          if (e.key.repeat) continue;
          if (p.count > 0 && !p.entries[p.cursor].is_dir) {
            p.op_select = !p.entries[p.cursor].selected;
            p.anchor = p.cursor;
            p.space_held = 1;
            picker_apply_range(&p);
          }
        }
        else if (k == SDLK_RETURN && (e.key.mod & SDL_KMOD_CTRL)) {
          picker_commit(&p);
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
            picker_commit(&p);
            char newpath[PATH_MAX];
            snprintf(newpath, sizeof(newpath), "%s", cur->path);
            picker_load(&p, newpath);
          } else {
            /* Sync any in-flight space selection into the permanents set
               before we build the output. */
            picker_commit(&p);

            if (g_permanent_count > 0) {
              /* Cross-folder selection: play every permanently-selected
                 file regardless of which folder it lives in. */
              *out_files = malloc((size_t)g_permanent_count * sizeof(char *));
              if (*out_files) {
                int k2 = 0;
                for (int i = 0; i < g_permanent_count; i++) {
                  char *dup = strdup(g_permanent_paths[i]);
                  if (dup) (*out_files)[k2++] = dup;
                }
                *out_count = k2;
              }
              if (*out_count == 0) { free(*out_files); *out_files = NULL; }
            } else {
              /* Nothing selected anywhere: play the cursor file. */
              *out_files = malloc(sizeof(char *));
              if (*out_files) {
                (*out_files)[0] = strdup(cur->path);
                *out_count = (*out_files)[0] ? 1 : 0;
                if (*out_count == 0) { free(*out_files); *out_files = NULL; }
              }
            }

            if (*out_count > 0) {
              confirmed = 1;
              done = 1;
            }
          }
        }
      }
    }

    if (done) break;

    if (dirty) {
      int ww, wh;
      SDL_GetWindowSize(win, &ww, &wh);
      picker_draw(ren, &p, ww, wh);
      SDL_RenderPresent(ren);
      dirty = 0;
    }
    SDL_Delay(16);
  }

  picker_free(&p);
  picker_free_permanents();
  return confirmed;
}

static void url_decode(const char *in, char *out, size_t out_sz) {
  size_t i = 0;
  while (*in && i < out_sz - 1) {
    if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
      char hex[3] = { in[1], in[2], '\0' };
      out[i++] = (char)strtol(hex, NULL, 16);
      in += 3;
    } else {
      out[i++] = *in++;
    }
  }
  out[i] = '\0';
}

static void clean_drop_path(const char *raw, char *out, size_t out_sz) {
  char temp[PATH_MAX];
  snprintf(temp, sizeof(temp), "%s", raw);
  char *p = temp;
  int is_uri = 0;
  if (strncmp(p, "file://", 7) == 0) { p += 7; is_uri = 1; }

  char decoded[PATH_MAX];
  if (is_uri) url_decode(p, decoded, sizeof(decoded));
  else snprintf(decoded, sizeof(decoded), "%s", p);

  size_t plen = strlen(decoded);
  while (plen > 0 && (decoded[plen - 1] == '\n' || decoded[plen - 1] == '\r')) {
    decoded[--plen] = '\0';
  }
  snprintf(out, out_sz, "%s", decoded);
}

int player_idle_run(char ***out_playlist, int *out_count) {
  *out_playlist = NULL;
  *out_count = 0;

  SDL_Window *win = SDL_CreateWindow("VidP", 900, 600,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
  if (!win) return 0;

  SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
  if (!ren) { SDL_DestroyWindow(win); return 0; }

  SDL_ShowWindow(win);

  int got_files = 0;
  int quit = 0;
  int dirty = 1;
  int from_drop = 0;
  Uint64 last_drop = 0;

  while (!quit && !got_files) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      dirty = 1;
      if (e.type == SDL_EVENT_QUIT) {
        quit = 1;
      }
      else if (e.type == SDL_EVENT_DROP_FILE || e.type == SDL_EVENT_DROP_TEXT) {
        if (e.drop.data && e.drop.data[0] != '\0') {
          char clean[PATH_MAX];
          clean_drop_path(e.drop.data, clean, sizeof(clean));
          playlist_collect_path(clean, out_playlist, out_count);
          last_drop = SDL_GetTicks();
          from_drop = 1;
        }
      }
      else if (e.type == SDL_EVENT_DROP_COMPLETE) {
        if (*out_count > 0) got_files = 1;
      }
      else if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_ESCAPE) {
          quit = 1;
        }
        else if (e.key.key == SDLK_O && (e.key.mod & SDL_KMOD_CTRL) && !e.key.repeat) {
          if (picker_run(win, ren, out_playlist, out_count, &quit)) {
            got_files = 1;
          }
          dirty = 1;
        }
      }
    }

    if (!got_files && from_drop && *out_count > 0 && SDL_GetTicks() - last_drop > 500)
      got_files = 1;
    if (got_files && from_drop && *out_count > 1)
      qsort(*out_playlist, (size_t)*out_count, sizeof(char *), compare_strings);

    if (dirty && !got_files && !quit) {
      int ww = 0, wh = 0;
      SDL_GetWindowSize(win, &ww, &wh);

      SDL_SetRenderDrawColor(ren, 0x10, 0x10, 0x12, 0xFF);
      SDL_RenderClear(ren);

      SDL_Color dim = { 80, 80, 90, 255 };
      ui_text_center(ren, "ctrl+o open   ·   esc quit   ·   drop files here", dim, ww/2, wh - 32);

      SDL_RenderPresent(ren);
      dirty = 0;
    }
    SDL_Delay(16);
  }

  ui_clear_cache();
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);

  return got_files;
}
