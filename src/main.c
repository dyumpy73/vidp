#include "player.h"

int main(int argc, char *argv[]) {
#if defined(__linux__) && defined(__GLIBC__)
  mallopt(M_TRIM_THRESHOLD, 64 * 1024);
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif

  av_log_set_level(AV_LOG_ERROR);

  char **playlist = NULL;
  int playlist_count = 0;
  build_playlist(argc, argv, &playlist, &playlist_count);

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS)) {
    fprintf(stderr, "[VidP] Failed to initialize SDL: %s\n", SDL_GetError());
    free_playlist(playlist, playlist_count);
    return -1;
  }

  if (playlist_count == 0) {
    printf("[VidP] No files given, showing idle screen.\n");
    if (!player_idle_run(&playlist, &playlist_count) || playlist_count == 0) {
        printf("[VidP] No file selected, exiting.\n");
        SDL_Quit();
        return 0;
    }
    printf("[VidP] Idle screen selected %d file(s).\n", playlist_count);
  }

  printf("[VidP] Playlist loaded with %d file(s).\n", playlist_count);

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
    if (ctx.nav_action == NAV_PREV && playlist_index > 0) {
      playlist_index--;
    } else {
      playlist_index++;
    }
  }

  free_playlist(playlist, playlist_count);
  SDL_Quit();

  return 0;
}
