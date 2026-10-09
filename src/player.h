#ifndef VIDP_PLAYER_H
#define VIDP_PLAYER_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <math.h>

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <ass/ass.h>

#define AUDIO_BUFFER_SIZE       (256 * 1024)
#define SEEK_STEP_SEC           10.0
#define AV_SYNC_THRESHOLD_MAX   0.100
#define AV_SYNC_THRESHOLD_MIN  -0.100
#define AV_SYNC_DROP_THRESHOLD -0.020
#define AV_SYNC_DELAY_THRESHOLD  0.002
#define SUBTITLE_RENDER_SCALE   1
#define MAX_SUBTITLE_STREAM     32
#define SUBTITLE_PREROLL_SEC    1.5

typedef struct {
  uint8_t buffer[AUDIO_BUFFER_SIZE];
  size_t write_pos;
  size_t read_pos;
  size_t size;
  SDL_mutex *lock;
  float volume;
} AudioBuffer;

typedef struct {
  AVFormatContext *fmt_ctx;
  int video_stream;
  int audio_stream;
  int subtitle_stream;

  int subtitle_streams[MAX_SUBTITLE_STREAM];
  int subtitle_stream_count;
  int current_subtitle_idx;

  AVCodecContext *v_codec_ctx;
  AVCodecContext *a_codec_ctx;

  struct SwsContext *sws_ctx;
  struct SwrContext *swr_ctx;

  AVFrame *frame_video;
  AVFrame *frame_audio;
  AVFrame *frame_yuv;
  AVPacket *packet;

  AudioBuffer audio_buf;
  SDL_AudioDeviceID audio_dev;
  SDL_AudioSpec audio_spec;
  uint8_t *audio_out_buffer;
  unsigned int audio_out_buffer_size;
  int output_channels;

  ASS_Library *ass_library;
  ASS_Renderer *ass_renderer;
  ASS_Track *ass_track;
  ASS_Track *ass_tracks[MAX_SUBTITLE_STREAM];
  SDL_Surface *sub_surf;
  SDL_Texture *sub_tex;

  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  int width;
  int height;
  double sar_ratio;
  int rotation;

  int file_finished;
  int paused;
  double last_video_time;
  double discard_until;
  double audio_discard_until;
  double playback_start_wall;
  double first_video_time;
  int video_clock_started;
  double paused_started_wall;
  AVBufferRef *hw_device_ctx;
  int is_hw_accel;
  enum AVPixelFormat hw_pix_fmt;
  int frame_counter;
  int subtitle_visible;
  uint32_t current_tex_format;

  uint32_t last_mouse_move;
  int cursor_hidden;
  SDL_Cursor *blank_cursor;

  int has_last_frame;
  SDL_Rect last_dest_rect;

  int *global_quit_ref;
  int *playlist_index_ref;
  int playlist_count;

  char *sub_tmp;
  int sub_tmp_cap;
} PlayerContext;

/* ---- utils.c ---- */
double get_monotonic_time(void);
void trim_memory(void);
int is_media_file(const char *filename);
int compare_strings(const void *a, const void *b);
SDL_Cursor* create_blank_cursor(void);

/* ---- playlist.c ---- */
void build_playlist(int argc, char *argv[], char ***playlist, int *count);
void free_playlist(char **playlist, int count);

/* ---- video.c ---- */
enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
int player_init_display(PlayerContext *ctx, const char *title);
void player_check_dynamic_resolution(PlayerContext *ctx, AVFrame *render_frame);
void player_render_current_frame(PlayerContext *ctx, AVFrame *render_frame);
void player_receive_video_frames(PlayerContext *ctx);
void player_process_video_packet(PlayerContext *ctx);

/* ---- audio.c ---- */
int player_init_audio(PlayerContext *ctx);
void player_receive_audio_frames(PlayerContext *ctx);
void player_process_audio_packet(PlayerContext *ctx);
void player_reset_audio_buffer(AudioBuffer *audio_buf);

/* ---- subtitle.c ---- */
void player_init_subtitles(PlayerContext *ctx);
void player_process_subtitle_packet(PlayerContext *ctx);
void player_reset_subtitle_state(PlayerContext *ctx);
void player_switch_subtitle(PlayerContext *ctx);
void render_ass_overlay(PlayerContext *ctx, ASS_Image *img, SDL_Rect *dest_rect);

/* ---- player.c ---- */
int  player_open_file(PlayerContext *ctx, const char *filepath, int index, int total);
void player_close_file(PlayerContext *ctx);
void player_run_loop(PlayerContext *ctx, int *global_quit, int *playlist_index);
void player_do_seek(PlayerContext *ctx, double target_sec);
void player_sleep_pumping(PlayerContext *ctx, double seconds);
void player_handle_events(PlayerContext *ctx, int *global_quit, int *playlist_index);
void player_present_frame(PlayerContext *ctx);

#endif /* VIDP_PLAYER_H */
