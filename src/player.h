#ifndef VIDP_PLAYER_H
#define VIDP_PLAYER_H

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <dirent.h>
#include <libavcodec/packet.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <ass/ass.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define SEEK_STEP_SEC 10.0
#define AV_SYNC_THRESHOLD_MAX 0.100
#define AV_SYNC_THRESHOLD_MIN -0.100
#define AV_SYNC_DROP_THRESHOLD -0.020
#define AV_SYNC_DELAY_THRESHOLD 0.002
#define MAX_SUBTITLE_STREAM 32
#define SUBTITLE_PREROLL_SEC 1.5
#define AV_SYNC_DROP_AUDIO -0.050
#define DOWNSCALE_MIN_RATIO 1.3

#define MAX_FRAME_QUEUE_HARD_LIMIT 8
#define MAX_PACKET_QUEUE_HARD_LIMIT 32

static inline void player_configure_video_threads(AVCodecContext *c,
                                                  int is_hw_accel) {
  int logical = SDL_GetNumLogicalCPUCores();
  if (logical < 1)
    logical = 1;

  int cap;
  if (is_hw_accel) {
    cap = 2;
  } else {
    cap = logical > 4 ? 4 : logical;
  }

  const char *e = getenv("VIDP_THREADS");
  if (e) {
    int v = atoi(e);
    if (v > 0)
      cap = v;
  }

  if (logical > cap)
    logical = cap;
  c->thread_count = logical;
  c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
}

typedef enum { NAV_NEXT = 0, NAV_PREV } NavAction;

typedef struct {
  AVPacket *pkts[MAX_PACKET_QUEUE_HARD_LIMIT];
  int r_idx, w_idx, count;
  int capacity;
  SDL_Mutex *mutex;
  SDL_Condition *cond;
  atomic_int abort_request;
} PacketQueue;

typedef struct {
  AVFrame *frames[MAX_FRAME_QUEUE_HARD_LIMIT];
  int r_idx, w_idx, count;
  int capacity;
  SDL_Mutex *mutex;
  SDL_Condition *cond;
  atomic_int abort_request;
} FrameQueue;

typedef struct {
  AVFormatContext *fmt_ctx;
  int video_stream;
  int audio_stream;

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
  AVFrame *last_rendered_frame;
  AVPacket *packet;

  PacketQueue v_pkt_q;
  PacketQueue a_pkt_q;
  FrameQueue v_frame_q;

  int frame_queue_cap;
  int packet_queue_cap;

  SDL_Thread *demux_th, *v_dec_th, *a_dec_th;
  atomic_int abort_request;
  atomic_int demux_eof;
  atomic_int v_dec_eof;
  atomic_int a_dec_eof;
  atomic_int audio_generation;
  atomic_int seek_generation;
  SDL_Mutex *seek_mutex;

  SDL_AudioStream *sdl_stream;
  SDL_AudioDeviceID audio_dev;
  uint8_t *audio_out_buffer;
  unsigned int audio_out_buffer_size;
  int output_channels;
  int bytes_per_sec;
  int out_freq;
  float volume;
  double audio_hw_delay;

  _Atomic double audio_starve_since;
  _Atomic double pending_audio_pts;
  _Atomic double last_audio_pts;
  _Atomic double discard_until;
  _Atomic double audio_discard_until;

  double last_clock_valid_time;

  ASS_Library *ass_library;
  ASS_Renderer *ass_renderer;
  ASS_Track *ass_track;
  ASS_Track *ass_tracks[MAX_SUBTITLE_STREAM];
  SDL_Texture *sub_tex;
  int sub_tex_w, sub_tex_h;
  int sub_x, sub_y, sub_w, sub_h;
  int sub_frame_w, sub_frame_h;

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
  double playback_start_wall;
  double first_video_time;
  int video_clock_started;
  double paused_started_wall;
  AVBufferRef *hw_device_ctx;
  int is_hw_accel;
  enum AVPixelFormat hw_pix_fmt;
  int hw_transfer_failures;
  int subtitle_visible;
  uint32_t current_tex_format;
  uint32_t tex_cs;
  int tex_w;
  int tex_h;

  uint32_t last_mouse_move;
  int cursor_hidden;
  SDL_Cursor *blank_cursor;

  int has_last_frame;
  double volume_overlay_until;

  int *global_quit_ref;
  int *playlist_index_ref;
  int playlist_count;
  int playlist_index;
  NavAction nav_action;

  int seek_requested;
  double seek_target;
  int step_pending;
  int consecutive_drops;
  int sub_has_pixels;

  char *sub_tmp;
  int sub_tmp_cap;
} PlayerContext;

/* ---- utils.c ---- */
double get_monotonic_time(void);
void trim_memory(void);
int is_media_file(const char *filename);
int compare_strings(const void *a, const void *b);
SDL_Cursor *create_blank_cursor(void);
void ui_set_font(TTF_Font *font);
void ui_clear_cache(void);
void ui_text_left(SDL_Renderer *r, const char *text, SDL_Color c, int x, int y);
void ui_text_center(SDL_Renderer *r, const char *text, SDL_Color c, int cx,
                    int y);

/* ---- playlist.c ---- */
void build_playlist(int argc, char *argv[], char ***playlist, int *count);
void free_playlist(char **playlist, int count);
void playlist_collect_path(const char *path, char ***out, int *count);

/* ---- idle.c ---- */
int player_idle_run(char ***out_playlist, int *out_count);
int player_init_font(void);
void player_cleanup_font(void);

/* ---- video.c ---- */
enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                 const enum AVPixelFormat *pix_fmts);
int player_init_display(PlayerContext *ctx, const char *title);
int player_init_sw_video(PlayerContext *ctx);
void player_render_current_frame(PlayerContext *ctx, AVFrame *render_frame);
int thread_video_decode(void *data);

/* ---- audio.c ---- */
int player_init_audio(PlayerContext *ctx);
void player_reset_audio_buffer(PlayerContext *ctx);
double player_get_audio_clock(PlayerContext *ctx);
void player_apply_volume(PlayerContext *ctx);
int thread_audio_decode(void *data);

/* ---- subtitle.c ---- */
void player_init_subtitles(PlayerContext *ctx);
void player_process_subtitle_packet(PlayerContext *ctx);
void player_reset_subtitle_state(PlayerContext *ctx);
void player_switch_subtitle(PlayerContext *ctx);
void render_ass_overlay(PlayerContext *ctx, ASS_Image *img,
                        SDL_FRect *dest_rect, int changed);
void player_update_ass_storage(PlayerContext *ctx);

/* ---- player.c ---- */
void packet_queue_init(PacketQueue *q, int capacity);
void packet_queue_put(PacketQueue *q, AVPacket *pkt);
AVPacket *packet_queue_get(PacketQueue *q);
void packet_queue_flush(PacketQueue *q);
int thread_demux(void *data);
void player_detect_device_tuning(PlayerContext *ctx);
int player_open_file(PlayerContext *ctx, const char *filepath, int index,
                     int total);
void player_close_file(PlayerContext *ctx);
void player_shutdown_display(void);
void player_run_loop(PlayerContext *ctx, int *global_quit, int *playlist_index);
void player_do_seek(PlayerContext *ctx, double target_sec);
void player_handle_events(PlayerContext *ctx, int *global_quit,
                          int *playlist_index);
void player_present_frame(PlayerContext *ctx);
void player_compute_dest_rect(PlayerContext *ctx, SDL_FRect *dest_rect);

#endif
