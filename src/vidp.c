#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <malloc.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <ass/ass.h>

#define AUDIO_BUFFER_SIZE       (1024 * 1024)
#define MAX_AUDIO_SAMPLES       192000
#define SEEK_STEP_SEC           10.0
#define AV_SYNC_THRESHOLD_MAX   0.100
#define AV_SYNC_THRESHOLD_MIN  -0.100
#define AV_SYNC_DROP_THRESHOLD -0.020
#define AV_SYNC_DELAY_THRESHOLD  0.002

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
  int output_channels;

  ASS_Library *ass_library;
  ASS_Renderer *ass_renderer;
  ASS_Track *ass_track;
  SDL_Surface *sub_surf;

  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  int width;
  int height;

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
} PlayerContext;

static double get_monotonic_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
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
  *playlist = realloc(*playlist, sizeof(char *) * (*count + 1));
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
      SDL_MixAudioFormat(stream, audio_buf->buffer + audio_buf->read_pos, AUDIO_S16SYS, bytes_to_copy, (int)(SDL_MIX_MAXVOLUME * audio_buf->volume));
      audio_buf->read_pos = (audio_buf->read_pos + bytes_to_copy) % AUDIO_BUFFER_SIZE;
    } else {
      size_t first_part = AUDIO_BUFFER_SIZE - audio_buf->read_pos;
      size_t second_part = bytes_to_copy - first_part;
      SDL_MixAudioFormat(stream, audio_buf->buffer + audio_buf->read_pos, AUDIO_S16SYS, first_part, (int)(SDL_MIX_MAXVOLUME * audio_buf->volume));
      SDL_MixAudioFormat(stream + first_part, audio_buf->buffer, AUDIO_S16SYS, second_part, (int)(SDL_MIX_MAXVOLUME * audio_buf->volume));
      audio_buf->read_pos = second_part;
    }
    audio_buf->size -= bytes_to_copy;
  }

  SDL_UnlockMutex(audio_buf->lock);
}

static void push_audio_data(AudioBuffer *audio_buf, const uint8_t *data, size_t len) {
  SDL_LockMutex(audio_buf->lock);

  if (audio_buf->size + len <= AUDIO_BUFFER_SIZE) {
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
  }

  SDL_UnlockMutex(audio_buf->lock);
}

static void player_reset_audio_buffer(AudioBuffer *audio_buf) {
  SDL_LockMutex(audio_buf->lock);
  audio_buf->read_pos = 0;
  audio_buf->write_pos = 0;
  audio_buf->size = 0;
  SDL_UnlockMutex(audio_buf->lock);
}

static inline void blend_ass_pixel(uint32_t *pixel, SDL_PixelFormat *fmt, uint8_t r, uint8_t g, uint8_t b, uint8_t final_a) {
  uint8_t ex_r, ex_g, ex_b, ex_a;
  SDL_GetRGBA(*pixel, fmt, &ex_r, &ex_g, &ex_b, &ex_a);

  if (ex_a == 0) {
    *pixel = SDL_MapRGBA(fmt, r, g, b, final_a);
  } else {
    uint8_t out_a = final_a + ex_a * (255 - final_a) / 255;
    if (out_a > 0) {
      uint8_t out_r = (r * final_a + ex_r * ex_a * (255 - final_a) / 255) / out_a;
      uint8_t out_g = (g * final_a + ex_g * ex_a * (255 - final_a) / 255) / out_a;
      uint8_t out_b = (b * final_a + ex_b * ex_a * (255 - final_a) / 255) / out_a;
      *pixel = SDL_MapRGBA(fmt, out_r, out_g, out_b, out_a);
    }
  }
}

static void render_ass_overlay(PlayerContext *ctx, ASS_Image *img, SDL_Rect *dest_rect) {
  if (!img || dest_rect->w <= 0 || dest_rect->h <= 0) return;

  if (!ctx->sub_surf || ctx->sub_surf->w != dest_rect->w || ctx->sub_surf->h != dest_rect->h) {
    if (ctx->sub_surf) SDL_FreeSurface(ctx->sub_surf);
    ctx->sub_surf = SDL_CreateRGBSurfaceWithFormat(0, dest_rect->w, dest_rect->h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!ctx->sub_surf) return;
  }

  SDL_FillRect(ctx->sub_surf, NULL, 0);

  while (img) {
    if (img->type != 3 && img->w > 0 && img->h > 0) {
      uint8_t r = (img->color >> 24) & 0xFF;
      uint8_t g = (img->color >> 16) & 0xFF;
      uint8_t b = (img->color >> 8) & 0xFF;
      uint8_t a = 255 - (img->color & 0xFF);

      if (a > 0) {
        uint32_t *pixels = (uint32_t *)ctx->sub_surf->pixels;
        for (int y = 0; y < img->h; y++) {
          int dst_y = img->dst_y + y;
          if (dst_y < 0 || dst_y >= dest_rect->h) continue;

          for (int x = 0; x < img->w; x++) {
            int dst_x = img->dst_x + x;
            if (dst_x < 0 || dst_x >= dest_rect->w) continue;

            uint8_t alpha = img->bitmap[y * img->stride + x];
            if (alpha == 0) continue;

            uint8_t final_a = (uint8_t)((alpha * a) / 255);
            int idx = dst_y * dest_rect->w + dst_x;

            blend_ass_pixel(&pixels[idx], ctx->sub_surf->format, r, g, b, final_a);
          }
        }
      }
    }
    img = img->next;
  }

  SDL_Texture *sub_tex = SDL_CreateTextureFromSurface(ctx->renderer, ctx->sub_surf);
  if (sub_tex) {
    SDL_SetTextureBlendMode(sub_tex, SDL_BLENDMODE_BLEND);
    SDL_RenderCopy(ctx->renderer, sub_tex, NULL, dest_rect);
    SDL_DestroyTexture(sub_tex);
  }
}

static void player_do_seek(PlayerContext *ctx, double target_sec) {
  if (target_sec < 0.0) target_sec = 0.0;
  int64_t seek_target = (int64_t)(target_sec * AV_TIME_BASE);

  if (av_seek_frame(ctx->fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) return;

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
  avcodec_parameters_to_context(ctx->a_codec_ctx, par);

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
  av_samples_alloc(&ctx->audio_out_buffer, NULL, ctx->output_channels, MAX_AUDIO_SAMPLES, AV_SAMPLE_FMT_S16, 0);

  return 1;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
  const enum AVPixelFormat *p;
  for (p = pix_fmts; *p != -1; p++) {
    if (*p == AV_PIX_FMT_VAAPI) {
      return *p;
    }
  }

  fprintf(stderr, "Warning: VAAPI pixel format not found, falling back to software decoding.\n");
  return pix_fmts[0];
}

static void player_init_subtitles(PlayerContext *ctx) {
  if (ctx->subtitle_stream < 0) return;

  AVCodecParameters *sub_par = ctx->fmt_ctx->streams[ctx->subtitle_stream]->codecpar;
  ctx->ass_library = ass_library_init();
  if (!ctx->ass_library) return;

  ctx->ass_renderer = ass_renderer_init(ctx->ass_library);
  ass_set_hinting(ctx->ass_renderer, ASS_HINTING_LIGHT);
  ass_set_shaper(ctx->ass_renderer, ASS_SHAPING_COMPLEX);
  ass_set_fonts(ctx->ass_renderer, NULL, "Sans", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
  ctx->ass_track = ass_new_track(ctx->ass_library);

  if (sub_par->extradata && sub_par->extradata_size > 0) {
    ass_process_codec_private(ctx->ass_track, (char *)sub_par->extradata, sub_par->extradata_size);
  }
}

static int player_init_display(PlayerContext *ctx, const char *title) {
  ctx->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                ctx->width, ctx->height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
  if (!ctx->window) return 0;

  ctx->renderer = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ctx->renderer) ctx->renderer = SDL_CreateRenderer(ctx->window, -1, 0);
  if (!ctx->renderer) return 0;

  ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, ctx->width, ctx->height);
  return ctx->texture != NULL;
}

static int player_open_file(PlayerContext *ctx, const char *filepath, int index, int total) {
  memset(ctx, 0, sizeof(PlayerContext));
  ctx->audio_buf.lock = SDL_CreateMutex();
  ctx->audio_buf.volume = 1.0f;
  ctx->discard_until = -1.0;
  ctx->audio_discard_until = -1.0;
  ctx->video_stream = -1;
  ctx->audio_stream = -1;
  ctx->subtitle_stream = -1;

  if (avformat_open_input(&ctx->fmt_ctx, filepath, NULL, NULL) != 0) return 0;
  if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0) return 0;

  for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
    AVCodecParameters *codecpar = ctx->fmt_ctx->streams[i]->codecpar;
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ctx->video_stream < 0) ctx->video_stream = i;
    if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_stream < 0) ctx->audio_stream = i;
    if (codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && ctx->subtitle_stream < 0) ctx->subtitle_stream = i;
  }

  if (ctx->video_stream < 0) return 0;

  AVCodecParameters *v_par = ctx->fmt_ctx->streams[ctx->video_stream]->codecpar;
  const AVCodec *v_codec = avcodec_find_decoder(v_par->codec_id);
  ctx->v_codec_ctx = avcodec_alloc_context3(v_codec);
  avcodec_parameters_to_context(ctx->v_codec_ctx, v_par);

  ctx->v_codec_ctx->thread_count = 2;
  ctx->v_codec_ctx->thread_type = FF_THREAD_FRAME;

  if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) >= 0) {
    ctx->v_codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
    ctx->v_codec_ctx->get_format = get_hw_format;
    ctx->is_hw_accel = 1;
  }

  if (avcodec_open2(ctx->v_codec_ctx, v_codec, NULL) < 0) return 0;

  ctx->width = ctx->v_codec_ctx->width;
  ctx->height = ctx->v_codec_ctx->height;

  player_init_audio(ctx);
  player_init_subtitles(ctx);

  ctx->frame_video = av_frame_alloc();
  ctx->frame_audio = av_frame_alloc();
  ctx->packet = av_packet_alloc();

  ctx->frame_yuv = av_frame_alloc();
  ctx->frame_yuv->format = AV_PIX_FMT_YUV420P;
  ctx->frame_yuv->width = ctx->width;
  ctx->frame_yuv->height = ctx->height;
  if (av_frame_get_buffer(ctx->frame_yuv, 0) < 0) {
    fprintf(stderr, "[VidP] Failed to allocate frame_yuv buffer!\n");
    return 0;
  }

  char title[512];
  snprintf(title, sizeof(title), "VidP [%d/%d] - %s", index + 1, total, filepath);
  return player_init_display(ctx, title);
}

static void player_close_file(PlayerContext *ctx) {
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
  if (ctx->renderer) SDL_DestroyRenderer(ctx->renderer);
  if (ctx->window) SDL_DestroyWindow(ctx->window);
  if (ctx->audio_buf.lock) SDL_DestroyMutex(ctx->audio_buf.lock);

  malloc_trim(0);
}

static void player_render_current_frame(PlayerContext *ctx) {
  AVFrame *render_frame = ctx->frame_yuv ? ctx->frame_yuv : ctx->frame_video;

  SDL_UpdateYUVTexture(ctx->texture, NULL,
                       render_frame->data[0], render_frame->linesize[0],
                       render_frame->data[1], render_frame->linesize[1],
                       render_frame->data[2], render_frame->linesize[2]);

  int win_w, win_h;
  SDL_GetWindowSize(ctx->window, &win_w, &win_h);
  float video_aspect = (float)ctx->width / (float)ctx->height;
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
  SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, &dest_rect);

  if (ctx->ass_renderer && ctx->ass_track) {
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

static void player_process_video_packet(PlayerContext *ctx) {
  if (avcodec_send_packet(ctx->v_codec_ctx, ctx->packet) < 0) return;

  while (avcodec_receive_frame(ctx->v_codec_ctx, ctx->frame_video) == 0) {
    AVFrame *sw_frame = NULL;
    AVFrame *src_frame = ctx->frame_video;

    if (ctx->v_codec_ctx->pix_fmt == AV_PIX_FMT_VAAPI) {
      sw_frame = av_frame_alloc();
      if (av_hwframe_transfer_data(sw_frame, ctx->frame_video, 0) < 0) {
        fprintf(stderr, "[VidP] Failed transfer frame from GPU to CPU!\n");
        av_frame_free(&sw_frame);
        av_frame_unref(ctx->frame_video);
        continue;
      }
      src_frame = sw_frame;
    }

    if (ctx->frame_yuv && src_frame->data[0] != NULL) {
      ctx->sws_ctx = sws_getCachedContext(
        ctx->sws_ctx,
        ctx->width, ctx->height, src_frame->format,
        ctx->width, ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, NULL, NULL, NULL
      );

      if (ctx->sws_ctx) {
        sws_scale(
          ctx->sws_ctx,
          (const uint8_t * const *)src_frame->data, src_frame->linesize,
          0, ctx->height,
          ctx->frame_yuv->data, ctx->frame_yuv->linesize
        );
        ctx->frame_yuv->best_effort_timestamp = ctx->frame_video->best_effort_timestamp;
      }
    }

    AVStream *st = ctx->fmt_ctx->streams[ctx->video_stream];
    int64_t pts = ctx->frame_video->best_effort_timestamp;

    if (pts != AV_NOPTS_VALUE) {
      double video_time = pts * av_q2d(st->time_base);

      if (ctx->discard_until >= 0.0) {
        if (video_time < ctx->discard_until - 0.2) {
          av_frame_unref(ctx->frame_video);
          if (sw_frame) av_frame_free(&sw_frame);
          continue;
        } else {
          ctx->discard_until = -1.0;
        }
      }
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
        SDL_Delay((Uint32)(delay * 1000.0));
      }
    }

    player_render_current_frame(ctx);

    av_frame_unref(ctx->frame_video);
    if (sw_frame) {
      av_frame_free(&sw_frame);
    }
  }
}

static void player_process_audio_packet(PlayerContext *ctx) {
  if (!ctx->a_codec_ctx || ctx->audio_dev <= 0 || !ctx->swr_ctx) return;
  if (avcodec_send_packet(ctx->a_codec_ctx, ctx->packet) < 0) return;

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

    int len = swr_convert(ctx->swr_ctx, &ctx->audio_out_buffer, out_samples, (const uint8_t **)ctx->frame_audio->data, ctx->frame_audio->nb_samples);
    if (len <= 0) {
      av_frame_unref(ctx->frame_audio);
      continue;
    }

    int audio_data_size = av_samples_get_buffer_size(NULL, ctx->output_channels, len, AV_SAMPLE_FMT_S16, 1);
    if (audio_data_size > 0) {
      push_audio_data(&ctx->audio_buf, ctx->audio_out_buffer, audio_data_size);
    }

    av_frame_unref(ctx->frame_audio);
  }
}

static void player_process_subtitle_packet(PlayerContext *ctx) {
  if (!ctx->ass_track) return;
  if (ctx->packet->pts != AV_NOPTS_VALUE) {
    double sub_pts = ctx->packet->pts * av_q2d(ctx->fmt_ctx->streams[ctx->subtitle_stream]->time_base);
    double duration_sub = ctx->packet->duration * av_q2d(ctx->fmt_ctx->streams[ctx->subtitle_stream]->time_base);
    int64_t start_ms = (int64_t)(sub_pts * 1000);
    int64_t dur_ms = (int64_t)(duration_sub * 1000);
    if (dur_ms <= 0) dur_ms = 5000;

    ass_process_chunk(ctx->ass_track, (char *)ctx->packet->data, ctx->packet->size, start_ms, dur_ms);
  }
}

static void player_handle_events(PlayerContext *ctx, int *global_quit, int *playlist_index) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      *global_quit = 1;
    }

    if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
      switch (event.key.keysym.sym) {
        case SDLK_ESCAPE:
          *global_quit = 1;
          break;
        case SDLK_SPACE:
          ctx->paused = !ctx->paused;
          if (ctx->paused) ctx->paused_started_wall = get_monotonic_time();
          else ctx->playback_start_wall += (get_monotonic_time() - ctx->paused_started_wall);
          if (ctx->audio_dev > 0) SDL_PauseAudioDevice(ctx->audio_dev, ctx->paused);
          break;
        case SDLK_UP:
          ctx->audio_buf.volume = (ctx->audio_buf.volume + 0.1f > 1.0f) ? 1.0f : ctx->audio_buf.volume + 0.1f;
          break;
        case SDLK_DOWN:
          ctx->audio_buf.volume = (ctx->audio_buf.volume - 0.1f < 0.0f) ? 0.0f : ctx->audio_buf.volume - 0.1f;
          break;
        case SDLK_RIGHT:
          player_do_seek(ctx, ctx->last_video_time + SEEK_STEP_SEC);
          break;
        case SDLK_LEFT:
          player_do_seek(ctx, ctx->last_video_time - SEEK_STEP_SEC);
          break;
        case SDLK_n:
          ctx->file_finished = 1;
          break;
        case SDLK_p:
          *playlist_index = (*playlist_index > 0) ? *playlist_index - 2 : -1;
          ctx->file_finished = 1;
          break;
      }
    }
  }
}

static void player_run_loop(PlayerContext *ctx, int *global_quit, int *playlist_index) {
  while (!ctx->file_finished && !*global_quit) {
    player_handle_events(ctx, global_quit, playlist_index);

    if (*global_quit || ctx->file_finished) break;
    if (ctx->paused) {
      SDL_Delay(10);
      continue;
    }

    SDL_LockMutex(ctx->audio_buf.lock);
    size_t current_audio_size = ctx->audio_buf.size;
    SDL_UnlockMutex(ctx->audio_buf.lock);

    if (current_audio_size > (AUDIO_BUFFER_SIZE - 65536)) {
      SDL_Delay(5);
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

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    fprintf(stderr, "[VidP] Failed to initialize SDL: %s\n", SDL_GetError());
    free_playlist(playlist, playlist_count);
    return -1;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

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
    playlist_index++;
  }

  free_playlist(playlist, playlist_count);
  SDL_Quit();

  return 0;
}
