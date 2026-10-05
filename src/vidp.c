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
#define SUBTITLE_RENDER_SCALE  1

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

  int subtitle_streams[10];
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
  int prev_sub_min_x, prev_sub_min_y, prev_sub_max_x, prev_sub_max_y;
  int prev_sub_valid;
  uint32_t current_tex_format;

  uint32_t last_mouse_move;
  int cursor_hidden;
  SDL_Cursor *blank_cursor;

  int *global_quit_ref;
  int *playlist_index_ref;
  int playlist_count;
} PlayerContext;

static void player_handle_events(PlayerContext *ctx, int *global_quit, int *playlist_index);

static inline void trim_memory(void) {
#if defined(__linux__) && defined(__GLIBC__)
  malloc_trim(0);
#endif
}

static double get_monotonic_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static SDL_Cursor* create_blank_cursor(void) {
  SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surf) return NULL;
  SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 0, 0, 0, 0));
  SDL_Cursor *cursor = SDL_CreateColorCursor(surf, 0, 0);
  SDL_FreeSurface(surf);
  return cursor;
}

static int is_media_file(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot) return 0;
  return (strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".mp4") == 0 ||
          strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".webm") == 0 ||
          strcasecmp(dot, ".mov") == 0 || strcasecmp(dot, ".flv") == 0);
}

static int compare_strings(const void *a, const void *b) {
  return strcasecmp(*(const char **)a, *(const char **)b);
}

static void add_to_playlist(char ***playlist, int *count, const char *filepath) {
  char **new_playlist = realloc(*playlist, sizeof(char *) * (*count + 1));
  if (!new_playlist) return;
  *playlist = new_playlist;
  (*playlist)[*count] = strdup(filepath);
  (*count)++;
}

static void scan_directory(const char *dirpath, char ***playlist, int *count) {
  DIR *d = opendir(dirpath);
  if (!d) return;

  struct dirent *dir;
  char fullpath[1024];

  while ((dir = readdir(d)) != NULL) {
    if (dir->d_name[0] == '.') continue;
    if (is_media_file(dir->d_name)) {
      snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, dir->d_name);
      add_to_playlist(playlist, count, fullpath);
    }
  }
  closedir(d);

  if (*count > 0) {
    qsort(*playlist, *count, sizeof(char *), compare_strings);
  }
}

static void build_playlist(int argc, char *argv[], char ***playlist, int *count) {
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

static void free_playlist(char **playlist, int count) {
  if (!playlist) return;
  for (int i = 0; i < count; i++) {
    free(playlist[i]);
  }
  free(playlist);
}

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
  AudioBuffer *audio_buf = (AudioBuffer *)userdata;
  SDL_memset(stream, 0, len);
  SDL_LockMutex(audio_buf->lock);

  if (audio_buf->size > 0) {
    size_t bytes_to_copy = (size_t)len < audio_buf->size ? (size_t)len : audio_buf->size;

    if (audio_buf->read_pos + bytes_to_copy <= AUDIO_BUFFER_SIZE) {
      memcpy(stream, audio_buf->buffer + audio_buf->read_pos, bytes_to_copy);
      audio_buf->read_pos = (audio_buf->read_pos + bytes_to_copy) % AUDIO_BUFFER_SIZE;
    } else {
      size_t first_part = AUDIO_BUFFER_SIZE - audio_buf->read_pos;
      size_t second_part = bytes_to_copy - first_part;
      memcpy(stream, audio_buf->buffer + audio_buf->read_pos, first_part);
      memcpy(stream + first_part, audio_buf->buffer, second_part);
      audio_buf->read_pos = second_part;
    }
    audio_buf->size -= bytes_to_copy;

    float vol = audio_buf->volume;
    if (vol < 0.999f) {
      Sint16 *s = (Sint16 *)stream;
      int n = (int)(bytes_to_copy / sizeof(Sint16));
      for (int i = 0; i < n; i++) {
        s[i] = (Sint16)(s[i] * vol);
      }
    }
  }

  SDL_UnlockMutex(audio_buf->lock);
}

static void push_audio_data(AudioBuffer *audio_buf, const uint8_t *data, size_t len, int frame_size) {
  if (frame_size <= 0) frame_size = 4;
  len = (len / frame_size) * frame_size;
  if (len == 0) return;

  if (len >= AUDIO_BUFFER_SIZE) {
    size_t max_bytes = (AUDIO_BUFFER_SIZE / frame_size) * frame_size;
    data += (len - max_bytes);
    len = max_bytes;
  }

  SDL_LockMutex(audio_buf->lock);

  if (audio_buf->size + len > AUDIO_BUFFER_SIZE) {
    size_t to_drop = audio_buf->size + len - AUDIO_BUFFER_SIZE;
    to_drop = ((to_drop + frame_size - 1) / frame_size) * frame_size;
    if (to_drop > audio_buf->size) to_drop = audio_buf->size;
    audio_buf->read_pos = (audio_buf->read_pos + to_drop) % AUDIO_BUFFER_SIZE;
    audio_buf->size -= to_drop;
  }

  if (audio_buf->write_pos + len <= AUDIO_BUFFER_SIZE) {
    memcpy(audio_buf->buffer + audio_buf->write_pos, data, len);
    audio_buf->write_pos = (audio_buf->write_pos + len) % AUDIO_BUFFER_SIZE;
  } else {
    size_t first_part = AUDIO_BUFFER_SIZE - audio_buf->write_pos;
    size_t second_part = len - first_part;
    memcpy(audio_buf->buffer + audio_buf->write_pos, data, first_part);
    memcpy(audio_buf->buffer, data + first_part, second_part);
    audio_buf->write_pos = second_part;
  }
  audio_buf->size += len;

  SDL_UnlockMutex(audio_buf->lock);
}

static void player_reset_audio_buffer(AudioBuffer *audio_buf) {
  SDL_LockMutex(audio_buf->lock);
  audio_buf->read_pos = 0;
  audio_buf->write_pos = 0;
  audio_buf->size = 0;
  SDL_UnlockMutex(audio_buf->lock);
}

static inline void blend_ass_pixel(uint32_t *pixel, uint8_t r, uint8_t g, uint8_t b, uint8_t final_a) {
  uint32_t px = *pixel;
  uint8_t ex_a = (px >> 24) & 0xFF;

  if (final_a == 255 || ex_a == 0) {
    if (final_a == 0) return;
    *pixel = ((uint32_t)final_a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    return;
  }

  uint8_t ex_r = px & 0xFF;
  uint8_t ex_g = (px >> 8) & 0xFF;
  uint8_t ex_b = (px >> 16) & 0xFF;

  uint8_t out_a = final_a + ex_a * (255 - final_a) / 255;
  if (out_a > 0) {
    uint8_t out_r = (r * final_a + ex_r * ex_a * (255 - final_a) / 255) / out_a;
    uint8_t out_g = (g * final_a + ex_g * ex_a * (255 - final_a) / 255) / out_a;
    uint8_t out_b = (b * final_a + ex_b * ex_a * (255 - final_a) / 255) / out_a;
    *pixel = ((uint32_t)out_a << 24) | ((uint32_t)out_b << 16) | ((uint32_t)out_g << 8) | out_r;
  }
}

static void render_ass_overlay(PlayerContext *ctx, ASS_Image *img, SDL_Rect *dest_rect) {
  if (!img || dest_rect->w <= 0 || dest_rect->h <= 0) return;

  int target_w = dest_rect->w / SUBTITLE_RENDER_SCALE;
  int target_h = dest_rect->h / SUBTITLE_RENDER_SCALE;
  if (target_w < 1) target_w = 1;
  if (target_h < 1) target_h = 1;

  if (!ctx->sub_surf || ctx->sub_surf->w != target_w || ctx->sub_surf->h != target_h) {
    if (ctx->sub_surf) SDL_FreeSurface(ctx->sub_surf);
    ctx->sub_surf = SDL_CreateRGBSurfaceWithFormat(0, target_w, target_h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!ctx->sub_surf) return;

    if (ctx->sub_tex) {
      SDL_DestroyTexture(ctx->sub_tex);
      ctx->sub_tex = NULL;
    }
    ctx->prev_sub_valid = 0;
  }

  if (!ctx->sub_tex) {
    ctx->sub_tex = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STREAMING, target_w, target_h);
    if (!ctx->sub_tex) {
      fprintf(stderr, "[VidP] Failed to create subtitle texture: %s\n", SDL_GetError());
      return;
    }
    SDL_SetTextureBlendMode(ctx->sub_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(ctx->sub_tex, SDL_ScaleModeLinear);
    ctx->prev_sub_valid = 0;
  }

  int min_x = target_w, min_y = target_h;
  int max_x = 0, max_y = 0;

  ASS_Image *curr = img;
  while (curr) {
    if (curr->type != 3 && curr->w > 0 && curr->h > 0) {
      uint8_t a = 255 - (curr->color & 0xFF);
      if (a > 0) {
        int dx = curr->dst_x / SUBTITLE_RENDER_SCALE;
        int dy = curr->dst_y / SUBTITLE_RENDER_SCALE;
        int dw = curr->w / SUBTITLE_RENDER_SCALE;
        int dh = curr->h / SUBTITLE_RENDER_SCALE;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        if (dx < min_x) min_x = dx;
        if (dy < min_y) min_y = dy;
        if (dx + dw > max_x) max_x = dx + dw;
        if (dy + dh > max_y) max_y = dy + dh;
      }
    }
    curr = curr->next;
  }

  if (max_x <= min_x || max_y <= min_y) {
    if (ctx->prev_sub_valid) {
      SDL_Rect old_rect = {
        ctx->prev_sub_min_x, ctx->prev_sub_min_y,
        ctx->prev_sub_max_x - ctx->prev_sub_min_x,
        ctx->prev_sub_max_y - ctx->prev_sub_min_y
      };
      SDL_FillRect(ctx->sub_surf, &old_rect, 0);
      SDL_UpdateTexture(ctx->sub_tex, &old_rect,
                        (uint8_t *)ctx->sub_surf->pixels
                          + old_rect.y * ctx->sub_surf->pitch
                          + old_rect.x * 4,
                        ctx->sub_surf->pitch);
      ctx->prev_sub_valid = 0;
    }
    return;
  }

  if (min_x < 0) min_x = 0;
  if (min_y < 0) min_y = 0;
  if (max_x > target_w) max_x = target_w;
  if (max_y > target_h) max_y = target_h;

  int u_min_x = min_x, u_min_y = min_y, u_max_x = max_x, u_max_y = max_y;
  if (ctx->prev_sub_valid) {
    if (ctx->prev_sub_min_x < u_min_x) u_min_x = ctx->prev_sub_min_x;
    if (ctx->prev_sub_min_y < u_min_y) u_min_y = ctx->prev_sub_min_y;
    if (ctx->prev_sub_max_x > u_max_x) u_max_x = ctx->prev_sub_max_x;
    if (ctx->prev_sub_max_y > u_max_y) u_max_y = ctx->prev_sub_max_y;
  }

  int u_w = u_max_x - u_min_x;
  int u_h = u_max_y - u_min_y;
  if (u_w <= 0 || u_h <= 0) return;

  SDL_Rect clear_rect = { u_min_x, u_min_y, u_w, u_h };
  SDL_FillRect(ctx->sub_surf, &clear_rect, 0);

  uint32_t *pixels = (uint32_t *)ctx->sub_surf->pixels;
  int surf_w = ctx->sub_surf->w;

  curr = img;
  while (curr) {
    if (curr->type != 3 && curr->w > 0 && curr->h > 0) {
      uint8_t r = (curr->color >> 24) & 0xFF;
      uint8_t g = (curr->color >> 16) & 0xFF;
      uint8_t b = (curr->color >> 8) & 0xFF;
      uint8_t a = 255 - (curr->color & 0xFF);

      if (a == 0) { curr = curr->next; continue; }

      int layer_dx = curr->dst_x / SUBTITLE_RENDER_SCALE;
      int layer_dy = curr->dst_y / SUBTITLE_RENDER_SCALE;
      int layer_w  = curr->w / SUBTITLE_RENDER_SCALE;
      int layer_h  = curr->h / SUBTITLE_RENDER_SCALE;
      if (layer_w < 1) layer_w = 1;
      if (layer_h < 1) layer_h = 1;

      for (int y = 0; y < layer_h; y++) {
        int dst_y = layer_dy + y;
        if (dst_y < 0 || dst_y >= target_h) continue;

        int src_y = y * SUBTITLE_RENDER_SCALE;
        if (src_y >= curr->h) src_y = curr->h - 1;
        const uint8_t *bmp_row = curr->bitmap + src_y * curr->stride;

        uint32_t *row = pixels + dst_y * surf_w;

        for (int x = 0; x < layer_w; x++) {
          int dst_x = layer_dx + x;
          if (dst_x < 0 || dst_x >= target_w) continue;

          int src_x = x * SUBTITLE_RENDER_SCALE;
          if (src_x >= curr->w) src_x = curr->w - 1;

          uint8_t alpha = bmp_row[src_x];
          if (alpha == 0) continue;

          uint8_t final_a = (uint8_t)((alpha * a) / 255);
          blend_ass_pixel(&row[dst_x], r, g, b, final_a);
        }
      }
    }
    curr = curr->next;
  }

  SDL_Rect update_rect = { u_min_x, u_min_y, u_w, u_h };
  SDL_UpdateTexture(ctx->sub_tex, &update_rect,
                    (uint8_t *)ctx->sub_surf->pixels
                      + u_min_y * ctx->sub_surf->pitch
                      + u_min_x * 4,
                    ctx->sub_surf->pitch);

  SDL_RenderCopy(ctx->renderer, ctx->sub_tex, NULL, dest_rect);

  ctx->prev_sub_min_x = min_x;
  ctx->prev_sub_min_y = min_y;
  ctx->prev_sub_max_x = max_x;
  ctx->prev_sub_max_y = max_y;
  ctx->prev_sub_valid = 1;
}

static void player_do_seek(PlayerContext *ctx, double target_sec) {
  if (target_sec < 0.0) target_sec = 0.0;

  int stream_idx = -1;
  int64_t seek_target = (int64_t)(target_sec * AV_TIME_BASE);

  if (ctx->video_stream >= 0) {
    stream_idx = ctx->video_stream;
    AVRational tb = ctx->fmt_ctx->streams[stream_idx]->time_base;
    seek_target = av_rescale_q((int64_t)(target_sec * AV_TIME_BASE), AV_TIME_BASE_Q, tb);
  }

  if (av_seek_frame(ctx->fmt_ctx, stream_idx, seek_target, AVSEEK_FLAG_BACKWARD) < 0) return;

  if (ctx->v_codec_ctx) avcodec_flush_buffers(ctx->v_codec_ctx);
  if (ctx->a_codec_ctx) avcodec_flush_buffers(ctx->a_codec_ctx);
  if (ctx->ass_track) ass_flush_events(ctx->ass_track);

  player_reset_audio_buffer(&ctx->audio_buf);

  if (ctx->swr_ctx) swr_convert(ctx->swr_ctx, NULL, 0, NULL, 0);

  ctx->video_clock_started = 0;
  ctx->discard_until = target_sec;
  ctx->audio_discard_until = target_sec;
}

static int player_init_audio(PlayerContext *ctx) {
  if (ctx->audio_stream < 0) return 0;

  AVStream *st = ctx->fmt_ctx->streams[ctx->audio_stream];
  AVCodecParameters *par = st->codecpar;
  const AVCodec *codec = avcodec_find_decoder(par->codec_id);

  if (!codec) return 0;

  ctx->a_codec_ctx = avcodec_alloc_context3(codec);
  if (!ctx->a_codec_ctx) return 0;
  if (avcodec_parameters_to_context(ctx->a_codec_ctx, par) < 0) return 0;

  if (avcodec_open2(ctx->a_codec_ctx, codec, NULL) < 0) return 0;

  int input_channels = ctx->a_codec_ctx->ch_layout.nb_channels;
  if (input_channels <= 0) input_channels = 2;

  SDL_AudioSpec wanted_spec;
  SDL_zero(wanted_spec);
  wanted_spec.freq = ctx->a_codec_ctx->sample_rate;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.channels = input_channels;
  wanted_spec.silence = 0;
  wanted_spec.samples = 2048;
  wanted_spec.callback = sdl_audio_callback;
  wanted_spec.userdata = &ctx->audio_buf;

  ctx->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &ctx->audio_spec, 0);
  if (ctx->audio_dev == 0) return 0;

  ctx->output_channels = ctx->audio_spec.channels;
  AVChannelLayout out_ch_layout;
  av_channel_layout_default(&out_ch_layout, ctx->output_channels);

  swr_alloc_set_opts2(&ctx->swr_ctx, &out_ch_layout, AV_SAMPLE_FMT_S16, ctx->audio_spec.freq,
                      &ctx->a_codec_ctx->ch_layout, ctx->a_codec_ctx->sample_fmt,
                      ctx->a_codec_ctx->sample_rate, 0, NULL);
  av_channel_layout_uninit(&out_ch_layout);

  if (!ctx->swr_ctx || swr_init(ctx->swr_ctx) < 0) return 0;

  SDL_PauseAudioDevice(ctx->audio_dev, 0);
  return 1;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
  PlayerContext *p_ctx = (PlayerContext *)ctx->opaque;
  const enum AVPixelFormat *p;
  for (p = pix_fmts; *p != -1; p++) {
    if (*p == p_ctx->hw_pix_fmt) {
      return *p;
    }
  }

  for (p = pix_fmts; *p != -1; p++) {
    switch (*p) {
      case AV_PIX_FMT_YUV420P:
      case AV_PIX_FMT_NV12:
      case AV_PIX_FMT_YUVJ420P:
        return *p;
      default:
        break;
    }
  }

  return pix_fmts[0];
}

static void player_extract_attached_fonts(PlayerContext *ctx) {
  if (!ctx->fmt_ctx || !ctx->ass_library) return;

  for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
    AVStream *st = ctx->fmt_ctx->streams[i];
    if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
      AVDictionaryEntry *filename_tag = av_dict_get(st->metadata, "filename", NULL, 0);
      if (filename_tag && filename_tag->value) {
        const char *filename = filename_tag->value;
        if (st->codecpar->extradata && st->codecpar->extradata_size > 0) {
          ass_add_font(ctx->ass_library, (char *)filename, (char *)st->codecpar->extradata, st->codecpar->extradata_size);
        }
      }
    }
  }
}

static void player_init_subtitles(PlayerContext *ctx) {
  if (ctx->subtitle_stream < 0) return;

  AVCodecParameters *sub_par = ctx->fmt_ctx->streams[ctx->subtitle_stream]->codecpar;
  ctx->ass_library = ass_library_init();
  if (!ctx->ass_library) return;

  player_extract_attached_fonts(ctx);

  ctx->ass_renderer = ass_renderer_init(ctx->ass_library);
  if (!ctx->ass_renderer) return;

  ass_set_storage_size(ctx->ass_renderer, ctx->width, ctx->height);
  ass_set_hinting(ctx->ass_renderer, ASS_HINTING_NONE);
  ass_set_shaper(ctx->ass_renderer, ASS_SHAPING_COMPLEX);
  ass_set_fonts(ctx->ass_renderer, NULL, "Sans", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
  ass_set_cache_limits(ctx->ass_renderer, 20, 20);
  ctx->ass_track = ass_new_track(ctx->ass_library);
  if (!ctx->ass_track) return;

  if (sub_par->extradata && sub_par->extradata_size > 0) {
    ass_process_codec_private(ctx->ass_track, (char *)sub_par->extradata, sub_par->extradata_size);
  }
}

static int player_init_display(PlayerContext *ctx, const char *title) {
  ctx->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                ctx->width, ctx->height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
  if (!ctx->window) return 0;

  const char *drivers[] = { "opengl", "opengles2", "vulkan", NULL };
  ctx->renderer = NULL;

  for (int i = 0; drivers[i] != NULL; i++) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, drivers[i]);
    ctx->renderer = SDL_CreateRenderer(ctx->window, -1,
                                       SDL_RENDERER_ACCELERATED);

    if (ctx->renderer) {
      printf("[VidP] Active renderer using backend: %s\n", drivers[i]);
      break;
    }
  }

  if (!ctx->renderer) {
    SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
    ctx->renderer = SDL_CreateRenderer(ctx->window, -1, 0);
    if (ctx->renderer) {
      printf("[VidP] Fallback: Using default/software SDL renderer.\n");
    }
  }

  if (!ctx->renderer) return 0;

  ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, ctx->width, ctx->height);
  if (ctx->texture) {
    SDL_SetTextureScaleMode(ctx->texture, SDL_ScaleModeLinear);
  }

  return ctx->texture != NULL;
}

static int player_open_file(PlayerContext *ctx, const char *filepath, int index, int total) {
  memset(ctx, 0, sizeof(PlayerContext));
  ctx->playlist_count = total;
  ctx->audio_buf.lock = SDL_CreateMutex();
  if (!ctx->audio_buf.lock) return 0;
  ctx->audio_buf.volume = 1.0f;

  ctx->discard_until = -1.0;
  ctx->audio_discard_until = -1.0;
  ctx->video_stream = -1;
  ctx->audio_stream = -1;
  ctx->subtitle_stream = -1;
  ctx->frame_counter = 0;
  ctx->subtitle_visible = 1;
  ctx->subtitle_stream_count = 0;
  ctx->current_subtitle_idx = -1;

  ctx->blank_cursor = create_blank_cursor();
  ctx->last_mouse_move = SDL_GetTicks();
  ctx->cursor_hidden = 0;
  SDL_SetCursor(SDL_GetDefaultCursor());

  ctx->fmt_ctx = avformat_alloc_context();
  if (!ctx->fmt_ctx) return 0; 
  ctx->fmt_ctx->probesize = 1024 * 1024;
  ctx->fmt_ctx->max_analyze_duration = 1000000;

  if (avformat_open_input(&ctx->fmt_ctx, filepath, NULL, NULL) != 0) return 0;
  if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0) return 0;

  for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
    AVCodecParameters *codecpar = ctx->fmt_ctx->streams[i]->codecpar;
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ctx->video_stream < 0) ctx->video_stream = i;
    if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_stream < 0) ctx->audio_stream = i;
    if (codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      if (ctx->subtitle_stream_count < 10) {
        ctx->subtitle_streams[ctx->subtitle_stream_count++] = i;
      }
    }
  }

  if (ctx->subtitle_stream_count > 0) {
    ctx->current_subtitle_idx = 0;
    ctx->subtitle_stream = ctx->subtitle_streams[0];
  }

  if (ctx->video_stream < 0) return 0;

  AVCodecParameters *v_par = ctx->fmt_ctx->streams[ctx->video_stream]->codecpar;
  const AVCodec *v_codec = avcodec_find_decoder(v_par->codec_id);
  if (!v_codec) return 0;

  ctx->v_codec_ctx = avcodec_alloc_context3(v_codec);
  if (!ctx->v_codec_ctx) return 0;
  if (avcodec_parameters_to_context(ctx->v_codec_ctx, v_par) < 0) return 0;

  ctx->v_codec_ctx->thread_count = 2;
  ctx->v_codec_ctx->thread_type = FF_THREAD_FRAME;

  ctx->v_codec_ctx->opaque = ctx;
  ctx->hw_pix_fmt = AV_PIX_FMT_NONE;

  static const enum AVHWDeviceType linux_hw_priority[] = {
      AV_HWDEVICE_TYPE_VAAPI,
      AV_HWDEVICE_TYPE_CUDA,
      AV_HWDEVICE_TYPE_VDPAU,
      AV_HWDEVICE_TYPE_NONE
  };

  for (int i = 0; linux_hw_priority[i] != AV_HWDEVICE_TYPE_NONE; i++) {
    for (int j = 0;; j++) {
      const AVCodecHWConfig *config = avcodec_get_hw_config(v_codec, j);
      if (!config) break;

      if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
          config->device_type == linux_hw_priority[i]) {

        if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, linux_hw_priority[i], NULL, NULL, 0) >= 0) {
          ctx->hw_pix_fmt = config->pix_fmt;
          ctx->v_codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
          ctx->v_codec_ctx->get_format = get_hw_format;
          ctx->is_hw_accel = 1;
          printf("[VidP] HW Acceleration active: %s\n", av_hwdevice_get_type_name(linux_hw_priority[i]));
          break;
        }
      }
    }
    if (ctx->is_hw_accel) break;
  }

  if (avcodec_open2(ctx->v_codec_ctx, v_codec, NULL) < 0) {
    if (ctx->is_hw_accel) {
      printf("[VidP] HW accel initialization failed, falling back to SW decoding...\n");
      if (ctx->hw_device_ctx) {
        av_buffer_unref(&ctx->hw_device_ctx);
        ctx->hw_device_ctx = NULL;
      }
      if (ctx->v_codec_ctx->hw_device_ctx) {
        av_buffer_unref(&ctx->v_codec_ctx->hw_device_ctx);
        ctx->v_codec_ctx->hw_device_ctx = NULL;
      }
      ctx->v_codec_ctx->get_format = NULL;
      ctx->is_hw_accel = 0;
      ctx->hw_pix_fmt = AV_PIX_FMT_NONE;
      if (avcodec_open2(ctx->v_codec_ctx, v_codec, NULL) < 0) return 0;
    } else {
      return 0;
    }
  }

  ctx->width = ctx->v_codec_ctx->width;
  ctx->height = ctx->v_codec_ctx->height;

  AVStream *vst = ctx->fmt_ctx->streams[ctx->video_stream];
  AVRational sar = av_guess_sample_aspect_ratio(ctx->fmt_ctx, vst, NULL);
  if (sar.num <= 0 || sar.den <= 0) sar = (AVRational){1, 1};
  ctx->sar_ratio = (double)sar.num / (double)sar.den;

  ctx->rotation = 0;
  AVDictionaryEntry *rot = av_dict_get(vst->metadata, "rotate", NULL, 0);
  if (rot) {
    int r = atoi(rot->value);
    r = ((r % 360) + 360) % 360;
    if (r == 90 || r == 180 || r == 270) ctx->rotation = r;
  }

  if (ctx->rotation == 0) {
    for (int i = 0; i < vst->codecpar->nb_coded_side_data; i++) {
      const AVPacketSideData *sd = &vst->codecpar->coded_side_data[i];
      if (sd->type == AV_PKT_DATA_DISPLAYMATRIX && sd->size >= 9 * sizeof(int32_t)) {
        double theta = av_display_rotation_get((const int32_t *)sd->data);
        if (!isnan(theta)) {
          int r = (int)lround(-theta);
          r = ((r % 360) + 360) % 360;
          if (r == 90 || r == 180 || r == 270) ctx->rotation = r;
        }
        break;
      }
    }
  }

  if (!player_init_audio(ctx)) {
    fprintf(stderr, "[VidP] Audio init failed, continuing without audio.\n");
  }
  player_init_subtitles(ctx);

  ctx->frame_video = av_frame_alloc();
  ctx->frame_audio = av_frame_alloc();
  ctx->packet = av_packet_alloc();
  if (!ctx->frame_video || !ctx->frame_audio || !ctx->packet) return 0;

  char title[512];
  snprintf(title, sizeof(title), "VidP [%d/%d] - %s", index + 1, total, filepath);
  return player_init_display(ctx, title);
}

static void player_close_file(PlayerContext *ctx) {
  if (ctx->blank_cursor) {
    SDL_FreeCursor(ctx->blank_cursor);
    ctx->blank_cursor = NULL;
  }

  if (ctx->sws_ctx) {
    sws_freeContext(ctx->sws_ctx);
    ctx->sws_ctx = NULL;
  }
  if (ctx->frame_yuv) av_frame_free(&ctx->frame_yuv);

  if (ctx->ass_track) ass_free_track(ctx->ass_track);
  if (ctx->ass_renderer) ass_renderer_done(ctx->ass_renderer);
  if (ctx->ass_library) ass_library_done(ctx->ass_library);
  if (ctx->sub_surf) {
    SDL_FreeSurface(ctx->sub_surf);
    ctx->sub_surf = NULL;
  }
  if (ctx->sub_tex) {
    SDL_DestroyTexture(ctx->sub_tex);
    ctx->sub_tex = NULL;
  }

  if (ctx->audio_dev > 0) SDL_CloseAudioDevice(ctx->audio_dev);
  if (ctx->swr_ctx) swr_free(&ctx->swr_ctx);
  if (ctx->audio_out_buffer) av_freep(&ctx->audio_out_buffer);

  if (ctx->frame_video) av_frame_free(&ctx->frame_video);
  if (ctx->frame_audio) av_frame_free(&ctx->frame_audio);
  if (ctx->packet) av_packet_free(&ctx->packet);

  if (ctx->v_codec_ctx) avcodec_free_context(&ctx->v_codec_ctx);
  if (ctx->a_codec_ctx) avcodec_free_context(&ctx->a_codec_ctx);

  if (ctx->hw_device_ctx) {
    av_buffer_unref(&ctx->hw_device_ctx);
    ctx->hw_device_ctx = NULL;
  }

  if (ctx->fmt_ctx) avformat_close_input(&ctx->fmt_ctx);
  if (ctx->texture) SDL_DestroyTexture(ctx->texture);

  ctx->texture = NULL;
  ctx->current_tex_format = 0;
  if (ctx->renderer) SDL_DestroyRenderer(ctx->renderer);
  if (ctx->window) SDL_DestroyWindow(ctx->window);
  if (ctx->audio_buf.lock) SDL_DestroyMutex(ctx->audio_buf.lock);

  trim_memory();
}

static void player_check_dynamic_resolution(PlayerContext *ctx, AVFrame *render_frame) {
  if (!render_frame || render_frame->width <= 0 || render_frame->height <= 0) return;
  if (render_frame->width == ctx->width && render_frame->height == ctx->height) return;

  printf("[VidP] Dynamic resolution change: %dx%d -> %dx%d\n",
         ctx->width, ctx->height, render_frame->width, render_frame->height);

  ctx->width  = render_frame->width;
  ctx->height = render_frame->height;

  if (ctx->frame_yuv) {
    av_frame_free(&ctx->frame_yuv);
    ctx->frame_yuv = NULL;
  }

  if (ctx->texture) {
    SDL_DestroyTexture(ctx->texture);
    ctx->texture = NULL;
  }
  ctx->current_tex_format = 0;

  if (ctx->sub_surf) {
    SDL_FreeSurface(ctx->sub_surf);
    ctx->sub_surf = NULL;
  }
  if (ctx->sub_tex) {
    SDL_DestroyTexture(ctx->sub_tex);
    ctx->sub_tex = NULL;
  }

  if (ctx->ass_renderer) {
    ass_set_storage_size(ctx->ass_renderer, ctx->width, ctx->height);
  }
}

static void player_render_current_frame(PlayerContext *ctx, AVFrame *render_frame) {
  AVFrame *final_frame = render_frame;

  if (render_frame->format != AV_PIX_FMT_YUV420P && render_frame->format != AV_PIX_FMT_NV12) {
    if (!ctx->frame_yuv) {
      ctx->frame_yuv = av_frame_alloc();
      if (!ctx->frame_yuv) return;
      ctx->frame_yuv->format = AV_PIX_FMT_YUV420P;
      ctx->frame_yuv->width = ctx->width;
      ctx->frame_yuv->height = ctx->height;
      if (av_frame_get_buffer(ctx->frame_yuv, 32) < 0) {
        av_frame_free(&ctx->frame_yuv);
        return;
      }
    }

    ctx->sws_ctx = sws_getCachedContext(
        ctx->sws_ctx,
        render_frame->width, render_frame->height, (enum AVPixelFormat)render_frame->format,
        ctx->width, ctx->height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, NULL, NULL, NULL
    );

    if (ctx->sws_ctx) {
      int scaled = sws_scale(ctx->sws_ctx, (const uint8_t *const *)render_frame->data,
                             render_frame->linesize, 0, render_frame->height,
                             ctx->frame_yuv->data, ctx->frame_yuv->linesize);
      if (scaled > 0) {
        final_frame = ctx->frame_yuv;
      } else {
        SDL_RenderClear(ctx->renderer);
        SDL_RenderPresent(ctx->renderer);
        return;
      }
    }
  }

  uint32_t req_format = SDL_PIXELFORMAT_IYUV;
  if (final_frame->format == AV_PIX_FMT_NV12) {
    req_format = SDL_PIXELFORMAT_NV12;
  }

  if (!ctx->texture || ctx->current_tex_format != req_format) {
    if (ctx->texture) SDL_DestroyTexture(ctx->texture);
    ctx->texture = SDL_CreateTexture(ctx->renderer, req_format, SDL_TEXTUREACCESS_STREAMING, ctx->width, ctx->height);
    ctx->current_tex_format = req_format;
    if (ctx->texture) {
      SDL_SetTextureScaleMode(ctx->texture, SDL_ScaleModeLinear);
    }
  }

  if (!ctx->texture) {
    SDL_RenderClear(ctx->renderer);
    SDL_RenderPresent(ctx->renderer);
    return;
  }

  int y_stride = final_frame->linesize[0];
  uint8_t *y_ptr = final_frame->data[0];
  if (y_stride < 0) {
    y_ptr += y_stride * (ctx->height - 1);
    y_stride = -y_stride;
  }

  if (req_format == SDL_PIXELFORMAT_NV12) {
    int uv_stride = final_frame->linesize[1];
    uint8_t *uv_ptr = final_frame->data[1];
    if (uv_stride < 0) {
      uv_ptr += uv_stride * (ctx->height / 2 - 1);
      uv_stride = -uv_stride;
    }
    SDL_UpdateNVTexture(ctx->texture, NULL, y_ptr, y_stride, uv_ptr, uv_stride);
  } else {
    int u_stride = final_frame->linesize[1];
    int v_stride = final_frame->linesize[2];
    uint8_t *u_ptr = final_frame->data[1];
    uint8_t *v_ptr = final_frame->data[2];
    if (u_stride < 0) {
      u_ptr += u_stride * (ctx->height / 2 - 1);
      u_stride = -u_stride;
    }
    if (v_stride < 0) {
      v_ptr += v_stride * (ctx->height / 2 - 1);
      v_stride = -v_stride;
    }
    SDL_UpdateYUVTexture(ctx->texture, NULL,
                         y_ptr, y_stride,
                         u_ptr, u_stride,
                         v_ptr, v_stride);
  }

  int win_w, win_h;
  SDL_GetWindowSize(ctx->window, &win_w, &win_h);
  double video_aspect = ((double)ctx->width * ctx->sar_ratio) / (double)ctx->height;
  if (ctx->rotation == 90 || ctx->rotation == 270) {
    video_aspect = 1.0 / video_aspect;
  }
  float window_aspect = (float)win_w / (float)win_h;
  SDL_Rect dest_rect;

  if (window_aspect > video_aspect) {
    dest_rect.h = win_h;
    dest_rect.w = (int)(win_h * video_aspect);
    dest_rect.x = (win_w - dest_rect.w) / 2;
    dest_rect.y = 0;
  } else {
    dest_rect.w = win_w;
    dest_rect.h = (int)(win_w / video_aspect);
    dest_rect.x = 0;
    dest_rect.y = (win_h - dest_rect.h) / 2;
  }

  SDL_RenderClear(ctx->renderer);
  if (ctx->rotation != 0) {
    SDL_RenderCopyEx(ctx->renderer, ctx->texture, NULL, &dest_rect,
                     (double)ctx->rotation, NULL, SDL_FLIP_NONE);
  } else {
    SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, &dest_rect);
  }

  if (ctx->subtitle_visible && ctx->ass_renderer && ctx->ass_track) {
    int changed = 0;
    int64_t now_ms = (int64_t)(ctx->last_video_time * 1000);
    ass_set_frame_size(ctx->ass_renderer, dest_rect.w, dest_rect.h);
    ASS_Image *sub_img = ass_render_frame(ctx->ass_renderer, ctx->ass_track, now_ms, &changed);
    if (sub_img) {
      render_ass_overlay(ctx, sub_img, &dest_rect);
    }
  }

  SDL_RenderPresent(ctx->renderer);
}

static void player_sleep_pumping(PlayerContext *ctx, double seconds) {
  double target = get_monotonic_time() + seconds;
  while (get_monotonic_time() < target) {
    if (ctx->file_finished) return;
    if (ctx->global_quit_ref && *ctx->global_quit_ref) return;
    if (ctx->global_quit_ref && ctx->playlist_index_ref) {
      player_handle_events(ctx, ctx->global_quit_ref, ctx->playlist_index_ref);
    }
    double remain = target - get_monotonic_time();
    if (remain <= 0.0) break;
    SDL_Delay(remain > 0.005 ? 5 : 1);
  }
}

static void player_receive_video_frames(PlayerContext *ctx) {
  while (avcodec_receive_frame(ctx->v_codec_ctx, ctx->frame_video) == 0) {
    AVStream *st = ctx->fmt_ctx->streams[ctx->video_stream];
    int64_t pts = ctx->frame_video->best_effort_timestamp;
    double video_time = (pts != AV_NOPTS_VALUE) ? pts * av_q2d(st->time_base) : -1.0;

    if (video_time >= 0.0 && ctx->discard_until >= 0.0) {
      if (video_time < ctx->discard_until - 0.2) {
        av_frame_unref(ctx->frame_video);
        continue;
      } else {
        ctx->discard_until = -1.0;
      }
    }

    AVFrame *sw_frame = NULL;
    AVFrame *render_frame = ctx->frame_video;

    if (ctx->is_hw_accel && ctx->frame_video->format == ctx->hw_pix_fmt) {
      sw_frame = av_frame_alloc();
      if (!sw_frame) {
        av_frame_unref(ctx->frame_video);
        continue;
      }
      if (av_hwframe_transfer_data(sw_frame, ctx->frame_video, 0) < 0) {
        av_frame_free(&sw_frame);
        av_frame_unref(ctx->frame_video);
        continue;
      }
      render_frame = sw_frame;
    }

    player_check_dynamic_resolution(ctx, render_frame);

    if (video_time >= 0.0) {
      ctx->last_video_time = video_time;

      if (!ctx->video_clock_started) {
        ctx->first_video_time = video_time;
        ctx->playback_start_wall = get_monotonic_time();
        ctx->video_clock_started = 1;
      }

      double target_time = ctx->playback_start_wall + (video_time - ctx->first_video_time);
      double delay = target_time - get_monotonic_time();

      if (delay > AV_SYNC_THRESHOLD_MAX || delay < AV_SYNC_THRESHOLD_MIN) {
        ctx->playback_start_wall = get_monotonic_time();
        ctx->first_video_time = video_time;
        delay = 0.0;
      }

      if (delay < AV_SYNC_DROP_THRESHOLD) {
        av_frame_unref(ctx->frame_video);
        if (sw_frame) av_frame_free(&sw_frame);
        continue;
      }

      if (delay > AV_SYNC_DELAY_THRESHOLD) {
        player_sleep_pumping(ctx, delay);
      }
    }

    player_render_current_frame(ctx, render_frame);

    av_frame_unref(ctx->frame_video);
    if (sw_frame) {
      av_frame_free(&sw_frame);
    }

    if (++ctx->frame_counter % 300 == 0) {
      trim_memory();
    }
  }
}

static void player_process_video_packet(PlayerContext *ctx) {
  int ret = avcodec_send_packet(ctx->v_codec_ctx, ctx->packet);
  if (ret == AVERROR(EAGAIN)) {
    player_receive_video_frames(ctx);
    ret = avcodec_send_packet(ctx->v_codec_ctx, ctx->packet);
  }
  if (ret < 0 && ret != AVERROR_EOF) return;
  player_receive_video_frames(ctx);
}

static void player_receive_audio_frames(PlayerContext *ctx) {
  while (avcodec_receive_frame(ctx->a_codec_ctx, ctx->frame_audio) == 0) {
    if (ctx->audio_discard_until >= 0.0) {
      int64_t apts = ctx->frame_audio->best_effort_timestamp;
      if (apts != AV_NOPTS_VALUE) {
        double a_time = apts * av_q2d(ctx->fmt_ctx->streams[ctx->audio_stream]->time_base);
        if (a_time < ctx->audio_discard_until - 0.2) {
          av_frame_unref(ctx->frame_audio);
          continue;
        } else {
          ctx->audio_discard_until = -1.0;
        }
      }
    }

    int out_samples = av_rescale_rnd(swr_get_delay(ctx->swr_ctx, ctx->a_codec_ctx->sample_rate) + ctx->frame_audio->nb_samples,
                                     ctx->audio_spec.freq, ctx->a_codec_ctx->sample_rate, AV_ROUND_UP);
    if (out_samples <= 0) {
      av_frame_unref(ctx->frame_audio);
      continue;
    }

    int req_buf_size = av_samples_get_buffer_size(NULL, ctx->output_channels, out_samples, AV_SAMPLE_FMT_S16, 1);
    if (req_buf_size > 0) {
      av_fast_malloc(&ctx->audio_out_buffer, &ctx->audio_out_buffer_size, req_buf_size);
      if (ctx->audio_out_buffer) {
        int len = swr_convert(ctx->swr_ctx, &ctx->audio_out_buffer, out_samples, (const uint8_t **)ctx->frame_audio->data, ctx->frame_audio->nb_samples);
        if (len > 0) {
          int real_size = av_samples_get_buffer_size(NULL, ctx->output_channels, len, AV_SAMPLE_FMT_S16, 1);
          if (real_size > 0) {
            int sample_frame_size = ctx->output_channels * (int)sizeof(int16_t);
            push_audio_data(&ctx->audio_buf, ctx->audio_out_buffer, real_size, sample_frame_size);
          }
        }
      }
    }

    av_frame_unref(ctx->frame_audio);
  }
}

static void player_process_audio_packet(PlayerContext *ctx) {
  if (!ctx->a_codec_ctx || ctx->audio_dev <= 0 || !ctx->swr_ctx) return;

  int ret = avcodec_send_packet(ctx->a_codec_ctx, ctx->packet);
  if (ret == AVERROR(EAGAIN)) {
    player_receive_audio_frames(ctx);
    ret = avcodec_send_packet(ctx->a_codec_ctx, ctx->packet);
  }
  if (ret < 0 && ret != AVERROR_EOF) return;
  player_receive_audio_frames(ctx);
}

static void player_process_subtitle_packet(PlayerContext *ctx) {
  if (!ctx->ass_track || ctx->subtitle_stream < 0) return;

  if (ctx->packet->stream_index != ctx->subtitle_stream) return;
  int64_t pts = (ctx->packet->pts != AV_NOPTS_VALUE) ? ctx->packet->pts : ctx->packet->dts;

  if (pts != AV_NOPTS_VALUE) {
    double sub_pts = pts * av_q2d(ctx->fmt_ctx->streams[ctx->subtitle_stream]->time_base);
    double duration_sub = ctx->packet->duration * av_q2d(ctx->fmt_ctx->streams[ctx->subtitle_stream]->time_base);
    int64_t start_ms = (int64_t)(sub_pts * 1000);
    int64_t dur_ms = (int64_t)(duration_sub * 1000);
    if (dur_ms <= 0) dur_ms = 5000;

    ass_process_chunk(ctx->ass_track, (char *)ctx->packet->data, ctx->packet->size, start_ms, dur_ms);
  }
}

static void player_reset_subtitle_state(PlayerContext *ctx) {
  if (ctx->sub_surf) {
    SDL_FreeSurface(ctx->sub_surf);
    ctx->sub_surf = NULL;
  }
  if (ctx->sub_tex) {
    SDL_DestroyTexture(ctx->sub_tex);
    ctx->sub_tex = NULL;
  }
  ctx->prev_sub_valid = 0;
  ctx->prev_sub_min_x = ctx->prev_sub_min_y = 0;
  ctx->prev_sub_max_x = ctx->prev_sub_max_y = 0;

  if (ctx->ass_renderer) {
    ass_set_storage_size(ctx->ass_renderer, ctx->width, ctx->height);
  }
}

static void player_switch_subtitle(PlayerContext *ctx) {
  if (ctx->subtitle_stream_count <= 0 || !ctx->ass_library) {
    printf("[VidP] No subtitles available in this file.\n");
    return;
  }

  player_reset_subtitle_state(ctx);

  ctx->current_subtitle_idx++;
  if (ctx->current_subtitle_idx >= ctx->subtitle_stream_count) {
    ctx->current_subtitle_idx = -1;
    ctx->subtitle_stream = -1;
    printf("[VidP] Subtitle: OFF (Disabled)\n");
  } else {
    ctx->subtitle_stream = ctx->subtitle_streams[ctx->current_subtitle_idx];
    printf("[VidP] Switched to Subtitle stream index: %d (Total available: %d)\n", ctx->current_subtitle_idx + 1, ctx->subtitle_stream_count);
  }

  if (ctx->ass_track) {
    ass_free_track(ctx->ass_track);
    ctx->ass_track = NULL;
  }

  if (ctx->current_subtitle_idx >= 0) {
    ctx->ass_track = ass_new_track(ctx->ass_library);
    if (!ctx->ass_track) {
      fprintf(stderr, "[VidP] Failed to create subtitle track, disabling subtitle.\n");
      ctx->current_subtitle_idx = -1;
      ctx->subtitle_stream = -1;
      return;
    }
    int stream_idx = ctx->subtitle_streams[ctx->current_subtitle_idx];
    AVCodecParameters *sub_par = ctx->fmt_ctx->streams[stream_idx]->codecpar;
    if (sub_par->extradata && sub_par->extradata_size > 0) {
      ass_process_codec_private(ctx->ass_track, (char *)sub_par->extradata, sub_par->extradata_size);
    }
    player_do_seek(ctx, ctx->last_video_time);
  }
}

static void player_handle_events(PlayerContext *ctx, int *global_quit, int *playlist_index) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      *global_quit = 1;
    }

    if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN) {
      ctx->last_mouse_move = SDL_GetTicks();
      if (ctx->cursor_hidden) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        ctx->cursor_hidden = 0;
      }
    }

    if (event.type == SDL_WINDOWEVENT) {
      if (event.window.event == SDL_WINDOWEVENT_ENTER ||
          event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
          event.window.event == SDL_WINDOWEVENT_RESIZED ||
          event.window.event == SDL_WINDOWEVENT_EXPOSED) {
        ctx->last_mouse_move = SDL_GetTicks();
        if (ctx->cursor_hidden) {
          SDL_SetCursor(SDL_GetDefaultCursor());
          ctx->cursor_hidden = 0;
        }
        ctx->prev_sub_valid = 0;
      }
    }

    if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
      switch (event.key.keysym.sym) {
        case SDLK_ESCAPE:
          *global_quit = 1;
          break;
        case SDLK_f: {
          Uint32 flags = SDL_GetWindowFlags(ctx->window);
          if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
            SDL_SetWindowFullscreen(ctx->window, 0);
          } else {
            SDL_SetWindowFullscreen(ctx->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
          }
          ctx->last_mouse_move = SDL_GetTicks();
          SDL_SetCursor(SDL_GetDefaultCursor());
          ctx->cursor_hidden = 0;
          break;
        }
        case SDLK_SPACE:
          ctx->paused = !ctx->paused;
          if (ctx->paused) ctx->paused_started_wall = get_monotonic_time();
          else ctx->playback_start_wall += (get_monotonic_time() - ctx->paused_started_wall);
          if (ctx->audio_dev > 0) SDL_PauseAudioDevice(ctx->audio_dev, ctx->paused);
          break;
        case SDLK_UP:
          SDL_LockMutex(ctx->audio_buf.lock);
          ctx->audio_buf.volume = (ctx->audio_buf.volume + 0.1f > 1.0f) ? 1.0f : ctx->audio_buf.volume + 0.1f;
          SDL_UnlockMutex(ctx->audio_buf.lock);
          break;
        case SDLK_DOWN:
          SDL_LockMutex(ctx->audio_buf.lock);
          ctx->audio_buf.volume = (ctx->audio_buf.volume - 0.1f < 0.0f) ? 0.0f : ctx->audio_buf.volume - 0.1f;
          SDL_UnlockMutex(ctx->audio_buf.lock);
          break;
        case SDLK_RIGHT:
          player_do_seek(ctx, ctx->last_video_time + SEEK_STEP_SEC);
          break;
        case SDLK_LEFT:
          player_do_seek(ctx, ctx->last_video_time - SEEK_STEP_SEC);
          break;
        case SDLK_n:
          if (*playlist_index + 1 >= ctx->playlist_count) {
            *playlist_index = -2;
          }
          ctx->file_finished = 1;
          break;
        case SDLK_p:
          if (*playlist_index > 0) {
            *playlist_index = (*playlist_index > 0) ? *playlist_index - 2 : -2;
            ctx->file_finished = 1;
          } else {
            player_do_seek(ctx, 0.0);
          }
          break;
        case SDLK_v:
          ctx->subtitle_visible = !ctx->subtitle_visible;
          printf("[VidP] Subtitle %s\n", ctx->subtitle_visible ? "Visible (ON)" : "Hidden (OFF)");
          break;
        case SDLK_s:
          player_switch_subtitle(ctx);
          break;
      }
    }
  }
}

static void player_run_loop(PlayerContext *ctx, int *global_quit, int *playlist_index) {
  ctx->global_quit_ref = global_quit;
  ctx->playlist_index_ref = playlist_index;

  while (!ctx->file_finished && !*global_quit) {
    player_handle_events(ctx, global_quit, playlist_index);

    if (!ctx->cursor_hidden && (SDL_GetTicks() - ctx->last_mouse_move > 2000)) {
      if (ctx->blank_cursor) {
        SDL_SetCursor(ctx->blank_cursor);
      }
      ctx->cursor_hidden = 1;
    }

    if (*global_quit || ctx->file_finished) break;
    if (ctx->paused) {
      SDL_Delay(10);
      continue;
    }

    if (av_read_frame(ctx->fmt_ctx, ctx->packet) < 0) {
      ctx->file_finished = 1;
      break;
    }

    if (ctx->packet->stream_index == ctx->video_stream) {
      player_process_video_packet(ctx);
    } else if (ctx->packet->stream_index == ctx->audio_stream) {
      player_process_audio_packet(ctx);
    } else if (ctx->packet->stream_index == ctx->subtitle_stream) {
      player_process_subtitle_packet(ctx);
    }

    av_packet_unref(ctx->packet);
  }
}

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

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  SDL_SetHint("SDL_RENDER_YUV_COLOR_SPACE", "bt709");

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
