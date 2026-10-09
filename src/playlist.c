#include "player.h"

static void add_to_playlist(char ***playlist, int *count, const char *filepath) {
  char *dup = strdup(filepath);
  if (!dup) {
    fprintf(stderr, "[VidP] Warning: strdup failed for '%s'\n", filepath);
    return;
  }

  char **new_playlist = realloc(*playlist, sizeof(char *) * (*count + 1));
  if (!new_playlist) {
    fprintf(stderr, "[VidP] Warning: realloc failed, cannot add '%s'\n", filepath);
    free(dup);
    return;
  }

  *playlist = new_playlist;
  (*playlist)[*count] = dup;
  (*count)++;
}

static void scan_directory(const char *dirpath, char ***playlist, int *count) {
  DIR *d = opendir(dirpath);
  if (!d) return;

  struct dirent *dir;
  char fullpath[1024];
  int added_here = 0;

  while ((dir = readdir(d)) != NULL) {
    if (dir->d_name[0] == '.') continue;
    if (!is_media_file(dir->d_name)) continue;

    int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, dir->d_name);
    if (n < 0 || (size_t)n >= sizeof(fullpath)) {
      fprintf(stderr, "[VidP] Path too long, skipped: %s/%s\n",
              dirpath, dir->d_name);
      continue;
    }

    struct stat st;
    if (stat(fullpath, &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;

    int before = *count;
    add_to_playlist(playlist, count, fullpath);
    if (*count > before) added_here++;
  }
  closedir(d);

  if (added_here > 0) {
    qsort(*playlist, *count, sizeof(char *), compare_strings);
  }
}

void build_playlist(int argc, char *argv[], char ***playlist, int *count) {
  *playlist = NULL;
  *count = 0;

  for (int i = 1; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        scan_directory(argv[i], playlist, count);
      } else if (S_ISREG(st.st_mode) && is_media_file(argv[i])) {
        add_to_playlist(playlist, count, argv[i]);
      }
    }
  }
}

void free_playlist(char **playlist, int count) {
  if (!playlist) return;
  for (int i = 0; i < count; i++) {
    free(playlist[i]);
  }
  free(playlist);
}
