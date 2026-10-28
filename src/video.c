#include "player.h"

enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
  PlayerContext *p_ctx = (PlayerContext *)ctx->opaque;
  const enum AVPixelFormat *p;
  for (p = pix_fmts; *p != -1; p++) {
    if (*p == p_ctx->hw_pix_fmt) return *p;
  }
  for (p = pix_fmts; *p != -1; p++) {
    switch (*p) {
      case AV_PIX_FMT_YUV420P:
      case AV_PIX_FMT_NV12:
      case AV_PIX_FMT_YUVJ420P:
        return *p;
      default: break;
    }
  }
  return pix_fmts[0];
}

int player_init_display(PlayerContext *ctx, const char *title) {
  double disp_w = (double)ctx->width * ctx->sar_ratio;
  double disp_h = (double)ctx->height;
  if (ctx->rotation == 90 || ctx->rotation == 270) {
    double t = disp_w; disp_w = disp_h; disp_h = t;
  }
  SDL_Rect usable;
  if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
    double sx = (usable.w * 0.9) / disp_w;
    double sy = (usable.h * 0.9) / disp_h;
    double scale = sx < sy ? sx : sy;
    if (scale < 1.0) { disp_w *= scale; disp_h *= scale; }
  }
  int win_w = (int)disp_w; if (win_w < 64) win_w = 64;
  int win_h = (int)disp_h; if (win_h < 64) win_h = 64;

    if (!ctx->window) {
    ctx->window = SDL_CreateWindow(title, win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!ctx->window) return 0;
  } else {
    SDL_SetWindowTitle(ctx->window, title);
    
    Uint32 flags = SDL_GetWindowFlags(ctx->window);
    int is_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    int is_fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;

    int is_tiling_wm = (getenv("SWAYSOCK") != NULL) || 
                       (getenv("HYPRLAND_INSTANCE_SIGNATURE") != NULL) || 
                       (getenv("NIRI_SOCKET") != NULL);

    if (!is_tiling_wm && !is_maximized && !is_fullscreen) {
      SDL_SetWindowSize(ctx->window, win_w, win_h);
    }
  }
  const char *drivers[] = { 
    "opengl",
    "opengles2",
    "vulkan",
    "software",
    NULL 
  };

  for (int i = 0; ctx->renderer == NULL && drivers[i] != NULL; i++) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, drivers[i]);
    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (ctx->renderer) {
      printf("[VidP] Active renderer using backend: %s\n", drivers[i]);
      break;
    }
  }

  if (!ctx->renderer) {
    SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (ctx->renderer) {
      printf("[VidP] Fallback: Using default/software SDL renderer.\n");
    }
  }

  if (!ctx->renderer) return 0;
  if (SDL_GetWindowFlags(ctx->window) & SDL_WINDOW_HIDDEN) SDL_ShowWindow(ctx->window);
  ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV,
                                   SDL_TEXTUREACCESS_STREAMING, ctx->width, ctx->height);
  if (ctx->texture) {
    SDL_SetTextureScaleMode(ctx->texture, SDL_SCALEMODE_LINEAR);
  }
  return ctx->texture != NULL;
}

void player_check_dynamic_resolution(PlayerContext *ctx, AVFrame *render_frame) {
  if (!render_frame || render_frame->width <= 0 || render_frame->height <= 0) return;
  if (render_frame->width == ctx->width && render_frame->height == ctx->height) return;

  printf("[VidP] Dynamic resolution change: %dx%d -> %dx%d\n",
         ctx->width, ctx->height, render_frame->width, render_frame->height);

  ctx->width  = render_frame->width;
  ctx->height = render_frame->height;

  if (ctx->frame_yuv) { av_frame_free(&ctx->frame_yuv); ctx->frame_yuv = NULL; }
  if (ctx->sws_ctx) { sws_freeContext(ctx->sws_ctx); ctx->sws_ctx = NULL; }
  if (ctx->texture) { SDL_DestroyTexture(ctx->texture); ctx->texture = NULL; }
  
  ctx->current_tex_format = 0;
  if (ctx->sub_surf) { SDL_DestroySurface(ctx->sub_surf); ctx->sub_surf = NULL; }
  if (ctx->sub_tex) { SDL_DestroyTexture(ctx->sub_tex); ctx->sub_tex = NULL; }

  player_update_ass_storage(ctx);
}

static int downscale_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("VIDP_SCALE");
    enabled = !(e && (strcmp(e, "off") == 0 || strcmp(e, "0") == 0));
  }
  return enabled;
}

static void player_pick_texture_size(PlayerContext *ctx, int *tw, int *th) {
  *tw = ctx->width;
  *th = ctx->height;
  if (!downscale_enabled() || ctx->rotation != 0 || !ctx->window) return;

  SDL_FRect dest;
  player_compute_dest_rect(ctx, &dest);
  if (dest.w < 2.0f || dest.h < 2.0f) return;
  if ((double)ctx->width / dest.w < DOWNSCALE_MIN_RATIO) return;

  int w = (int)(dest.w + 0.5f) & ~1;
  int h = (int)(dest.h + 0.5f) & ~1;
  if (w < 2) w = 2;
  if (h < 2) h = 2;
  if (w >= ctx->width || h >= ctx->height) return;

  if (ctx->texture && ctx->tex_w > 0 && ctx->tex_w < ctx->width &&
      ctx->tex_h < ctx->height && abs(w - ctx->tex_w) * 10 <= ctx->tex_w) {
    w = ctx->tex_w;
    h = ctx->tex_h;
  }
  *tw = w;
  *th = h;
}

static SDL_Colorspace pick_colorspace(const AVFrame *src, int range_normalized, int height) {
  int full = (src->color_range == AVCOL_RANGE_JPEG) && !range_normalized;
  switch (src->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      return full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
    case AVCOL_SPC_BT709:
      return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    default:
      if (height > 576) return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
      return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
  }
}

static SDL_Texture *create_video_texture(SDL_Renderer *r, SDL_PixelFormat fmt,
                                         int w, int h, SDL_Colorspace cs) {
  SDL_PropertiesID p = SDL_CreateProperties();
  if (!p) return NULL;
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, fmt);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, cs);
  SDL_Texture *t = SDL_CreateTextureWithProperties(r, p);
  SDL_DestroyProperties(p);
  return t;
}

void player_render_current_frame(PlayerContext *ctx, AVFrame *render_frame) {
  AVFrame *final_frame = render_frame;
  int negative_stride = render_frame->linesize[0] < 0 || render_frame->linesize[1] < 0 || render_frame->linesize[2] < 0;

  int tw, th;
  player_pick_texture_size(ctx, &tw, &th);
  int resize = (tw != ctx->width || th != ctx->height);

  if (negative_stride || resize || (render_frame->format != AV_PIX_FMT_YUV420P && render_frame->format != AV_PIX_FMT_NV12)) {
    if (ctx->frame_yuv && (ctx->frame_yuv->width != tw || ctx->frame_yuv->height != th)) {
      av_frame_free(&ctx->frame_yuv);
    }
    if (!ctx->frame_yuv) {
      ctx->frame_yuv = av_frame_alloc();
      if (!ctx->frame_yuv) return;
      ctx->frame_yuv->format = AV_PIX_FMT_YUV420P;
      ctx->frame_yuv->width = tw;
      ctx->frame_yuv->height = th;
      if (av_frame_get_buffer(ctx->frame_yuv, 32) < 0) { av_frame_free(&ctx->frame_yuv); return; }
    }

    ctx->sws_ctx = sws_getCachedContext(ctx->sws_ctx, render_frame->width, render_frame->height,
        (enum AVPixelFormat)render_frame->format, tw, th, AV_PIX_FMT_YUV420P,
        resize ? SWS_AREA : SWS_BICUBIC, NULL, NULL, NULL);

    if (ctx->sws_ctx) {
      int scaled = sws_scale(ctx->sws_ctx, (const uint8_t *const *)render_frame->data,
                             render_frame->linesize, 0, render_frame->height,
                             ctx->frame_yuv->data, ctx->frame_yuv->linesize);
      if (scaled > 0) final_frame = ctx->frame_yuv;
      else { SDL_RenderClear(ctx->renderer); SDL_RenderPresent(ctx->renderer); return; }
    }
  }

  SDL_PixelFormat req_format = SDL_PIXELFORMAT_IYUV;
  if (final_frame->format == AV_PIX_FMT_NV12) req_format = SDL_PIXELFORMAT_NV12;

  int j_fmt = render_frame->format == AV_PIX_FMT_YUVJ420P ||
              render_frame->format == AV_PIX_FMT_YUVJ422P ||
              render_frame->format == AV_PIX_FMT_YUVJ444P ||
              render_frame->format == AV_PIX_FMT_YUVJ440P;
  SDL_Colorspace cs = pick_colorspace(render_frame, (final_frame != render_frame) && j_fmt,
                                      render_frame->height);

  if (!ctx->texture || ctx->current_tex_format != req_format || ctx->tex_cs != (uint32_t)cs ||
      ctx->tex_w != final_frame->width || ctx->tex_h != final_frame->height) {
    if (ctx->texture) SDL_DestroyTexture(ctx->texture);
    ctx->texture = create_video_texture(ctx->renderer, req_format,
                                        final_frame->width, final_frame->height, cs);
    ctx->current_tex_format = req_format;
    ctx->tex_cs = (uint32_t)cs;
    ctx->tex_w = final_frame->width;
    ctx->tex_h = final_frame->height;
    if (ctx->texture) SDL_SetTextureScaleMode(ctx->texture, SDL_SCALEMODE_LINEAR);
  }

  if (!ctx->texture) { SDL_RenderClear(ctx->renderer); SDL_RenderPresent(ctx->renderer); return; }

  int y_stride = final_frame->linesize[0];
  uint8_t *y_ptr = final_frame->data[0];

  if (req_format == SDL_PIXELFORMAT_NV12) {
    int uv_stride = final_frame->linesize[1];
    uint8_t *uv_ptr = final_frame->data[1];

    SDL_UpdateNVTexture(ctx->texture, NULL, y_ptr, y_stride, uv_ptr, uv_stride);
  } else {
    int u_stride = final_frame->linesize[1];
    int v_stride = final_frame->linesize[2];
    uint8_t *u_ptr = final_frame->data[1];
    uint8_t *v_ptr = final_frame->data[2];

    SDL_UpdateYUVTexture(ctx->texture, NULL, y_ptr, y_stride, u_ptr, u_stride, v_ptr, v_stride);
  }
  player_present_frame(ctx);
}

static void player_fallback_to_sw(PlayerContext *ctx) {
  if (!ctx->fmt_ctx || ctx->video_stream < 0) return;
  AVCodecParameters *v_par = ctx->fmt_ctx->streams[ctx->video_stream]->codecpar;
  const AVCodec *v_codec = avcodec_find_decoder(v_par->codec_id);
  if (!v_codec) return;

  avcodec_free_context(&ctx->v_codec_ctx);
  if (ctx->hw_device_ctx) { av_buffer_unref(&ctx->hw_device_ctx); ctx->hw_device_ctx = NULL; }
  ctx->is_hw_accel = 0; ctx->hw_pix_fmt = AV_PIX_FMT_NONE; ctx->hw_transfer_failures = 0;

  ctx->v_codec_ctx = avcodec_alloc_context3(v_codec);
  if (!ctx->v_codec_ctx) return;
  if (avcodec_parameters_to_context(ctx->v_codec_ctx, v_par) < 0) { avcodec_free_context(&ctx->v_codec_ctx); return; }
  ctx->v_codec_ctx->thread_count = 0;
  ctx->v_codec_ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
  ctx->v_codec_ctx->opaque       = ctx;

  if (avcodec_open2(ctx->v_codec_ctx, v_codec, NULL) < 0) { 
    avcodec_free_context(&ctx->v_codec_ctx);
  } else {
    ctx->seek_target = ctx->last_video_time;
    ctx->seek_requested = 1;
  }
}

static int should_stop_video(PlayerContext *ctx) {
  if (ctx->seek_requested || ctx->file_finished) return 1;
  if (ctx->global_quit_ref && *ctx->global_quit_ref) return 1;
  if (ctx->paused && !ctx->step_pending) return 1;
  return 0;
}

void player_receive_video_frames(PlayerContext *ctx) {
  if (!ctx->v_codec_ctx) return;
  while (!should_stop_video(ctx) && avcodec_receive_frame(ctx->v_codec_ctx, ctx->frame_video) == 0) {
    AVStream *st = ctx->fmt_ctx->streams[ctx->video_stream];
    int64_t pts = ctx->frame_video->best_effort_timestamp;
    double video_time = (pts != AV_NOPTS_VALUE) ? pts * av_q2d(st->time_base) : -1.0;

    if (video_time >= 0.0 && ctx->discard_until >= 0.0) {
      if (video_time < ctx->discard_until - 0.2) { av_frame_unref(ctx->frame_video); continue; }
      else ctx->discard_until = -1.0;
    }

    AVFrame *sw_frame = NULL;
    AVFrame *render_frame = ctx->frame_video;

    if (ctx->is_hw_accel && ctx->frame_video->format == ctx->hw_pix_fmt) {
      sw_frame = av_frame_alloc();
      if (!sw_frame) { av_frame_unref(ctx->frame_video); continue; }
      if (av_hwframe_transfer_data(sw_frame, ctx->frame_video, 0) < 0) {
        av_frame_free(&sw_frame); av_frame_unref(ctx->frame_video);
        if (++ctx->hw_transfer_failures >= 10) { player_fallback_to_sw(ctx); return; }
        continue;
      }
      ctx->hw_transfer_failures = 0;
      render_frame = sw_frame;
    }

    player_check_dynamic_resolution(ctx, render_frame);

    if (video_time >= 0.0) {
      ctx->last_video_time = video_time;
      double delay, drop_threshold;
      double audio_clock = player_get_audio_clock(ctx);

      if (audio_clock >= 0.0) {
        delay = video_time - audio_clock;
        drop_threshold = AV_SYNC_DROP_AUDIO;
        if (delay > AV_SYNC_THRESHOLD_MAX) delay = AV_SYNC_THRESHOLD_MAX;
        ctx->playback_start_wall = get_monotonic_time() + (delay > 0.0 ? delay : 0.0);
        ctx->first_video_time = video_time;
        ctx->video_clock_started = 1;
      } else {
        if (!ctx->video_clock_started) {
          ctx->first_video_time = video_time;
          ctx->playback_start_wall = get_monotonic_time();
          ctx->video_clock_started = 1;
        }
        double target_time = ctx->playback_start_wall + (video_time - ctx->first_video_time);
        delay = target_time - get_monotonic_time();
        drop_threshold = AV_SYNC_DROP_THRESHOLD;
        if (delay > AV_SYNC_THRESHOLD_MAX || delay < AV_SYNC_THRESHOLD_MIN) {
          ctx->playback_start_wall = get_monotonic_time();
          ctx->first_video_time = video_time;
          delay = 0.0;
        }
      }

      if (delay < drop_threshold && ctx->consecutive_drops < 15) {
        ctx->consecutive_drops++;
        av_frame_unref(ctx->frame_video);
        if (sw_frame) av_frame_free(&sw_frame);
        continue;
      }
      ctx->consecutive_drops = 0;

      if (delay > AV_SYNC_DELAY_THRESHOLD) {
        int has_audio = audio_clock >= 0.0 &&
                        ctx->audio_dev > 0 && ctx->sdl_stream && ctx->bytes_per_sec > 0;
        if (has_audio) {
          int queued = SDL_GetAudioStreamQueued(ctx->sdl_stream);
          double queued_sec = (queued > 0) ? (double)queued / ctx->bytes_per_sec : 0.0;

          if (queued_sec < 0.1) delay = 0.0;
          else if (delay > queued_sec) delay = queued_sec;
        }

        if (delay > 0.0) {
          player_sleep_pumping(ctx, delay);

          if (ctx->seek_requested || ctx->file_finished ||
              (ctx->global_quit_ref && *ctx->global_quit_ref)) {
            av_frame_unref(ctx->frame_video);
            if (sw_frame) av_frame_free(&sw_frame);
            return;
          }
        }
      }
    }

    player_render_current_frame(ctx, render_frame);
    ctx->step_pending = 0;
    av_frame_unref(ctx->frame_video);
    if (sw_frame) av_frame_free(&sw_frame);
  }
}

void player_process_video_packet(PlayerContext *ctx) {
  if (!ctx->v_codec_ctx) { ctx->file_finished = 1; return; }
  int ret = avcodec_send_packet(ctx->v_codec_ctx, ctx->packet);
  if (ret == AVERROR(EAGAIN)) {
    player_receive_video_frames(ctx);
    ret = avcodec_send_packet(ctx->v_codec_ctx, ctx->packet);
  }
  if (ret < 0 && ret != AVERROR_EOF) return;
  player_receive_video_frames(ctx);
}
