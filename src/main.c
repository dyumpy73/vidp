#include "player.h"

int main(int argc, char *argv[]) {
#if defined(__linux__) && defined(__GLIBC__)
  mallopt(M_TRIM_THRESHOLD, 64 * 1024);
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif

  if (argc < 2) {
    printf("Usage: %s <file_or_directory> [file2 file3 ...]\n", argv[0]);
    return -1;
  }

  av_log_set_level(AV_LOG_QUIET);

  char **playlist = NULL;
  int playlist_count = 0;
  build_playlist(argc, argv, &playlist, &playlist_count);

  if (playlist_count == 0) {
    fprintf(stderr, "[VidP] No valid media files found to play.\n");
    return -1;
  }

  printf("[VidP] Playlist loaded with %d file(s).\n", playlist_count);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    fprintf(stderr, "[VidP] Failed to initialize SDL: %s\n", SDL_GetError());
    free_playlist(playlist, playlist_count);
    return -1;
  }

  int playlist_index = 0;
  int global_quit = 0;
  PlayerContext ctx;

  while (playlist_index < playlist_count && !global_quit) {
    char *current_file = playlist[playlist_index];
    printf("[VidP] (%d/%d) Playing: %s\n", playlist_index + 1, playlist_count, current_file);

    if (player_open_file(&ctx, current_file, playlist_index, playlist_count)) {
      player_run_loop(&ctx, &global_quit, &playlist_index);
    } else {
      fprintf(stderr, "[VidP] Failed to open file or initialize player: %s\n", current_file);
    }

    player_close_file(&ctx);
    if (playlist_index == -2) {
      playlist_index = 0;
    } else {
      playlist_index++;
    }
  }

  free_playlist(playlist, playlist_count);
  SDL_Quit();

  return 0;
}
