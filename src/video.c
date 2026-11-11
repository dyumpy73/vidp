#include "player.h"
#include <stdatomic.h>
#include <stdlib.h>

static void player_fallback_to_sw(PlayerContext *ctx);

enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                 const enum AVPixelFormat *pix_fmts) {
  PlayerContext *p_ctx = (PlayerContext *)ctx->opaque;
  const enum AVPixelFormat *p;
  for (p = pix_fmts; *p != -1; p++)
    if (*p == p_ctx->hw_pix_fmt)
      return *p;
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

int player_init_sw_video(PlayerContext *ctx) {
  if (!ctx->fmt_ctx || ctx->video_stream < 0)
    return 0;
  AVCodecParameters *v_par = ctx->fmt_ctx->streams[ctx->video_stream]->codecpar;
  const AVCodec *v_codec = avcodec_find_decoder(v_par->codec_id);
  if (!v_codec)
    return 0;

  if (ctx->v_codec_ctx)
    avcodec_free_context(&ctx->v_codec_ctx);
  if (ctx->hw_device_ctx) {
    av_buffer_unref(&ctx->hw_device_ctx);
    ctx->hw_device_ctx = NULL;
  }
  ctx->is_hw_accel = 0;
  ctx->hw_pix_fmt = AV_PIX_FMT_NONE;
  ctx->hw_transfer_failures = 0;

  ctx->v_codec_ctx = avcodec_alloc_context3(v_codec);
  if (!ctx->v_codec_ctx)
    return 0;
  if (avcodec_parameters_to_context(ctx->v_codec_ctx, v_par) < 0) {
    avcodec_free_context(&ctx->v_codec_ctx);
    return 0;
  }
  player_configure_video_threads(ctx->v_codec_ctx, 0);
  ctx->v_codec_ctx->opaque = ctx;

  if (avcodec_open2(ctx->v_codec_ctx, v_codec, NULL) < 0) {
    avcodec_free_context(&ctx->v_codec_ctx);
    return 0;
  }
  return 1;
}

static void player_fallback_to_sw(PlayerContext *ctx) {
  printf("[VidP] HW decode failed. Falling back to software.\n");
  if (!player_init_sw_video(ctx))
    return;
  ctx->seek_target = ctx->last_video_time;
  ctx->seek_requested = 1;
}

int player_init_display(PlayerContext *ctx, const char *title) {
  double disp_w = (double)ctx->width * ctx->sar_ratio;
  double disp_h = (double)ctx->height;
  if (ctx->rotation == 90 || ctx->rotation == 270) {
    double t = disp_w;
    disp_w = disp_h;
    disp_h = t;
  }
  SDL_Rect usable;
  if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
      usable.w > 0 && usable.h > 0) {
    double sx = (usable.w * 0.9) / disp_w;
    double sy = (usable.h * 0.9) / disp_h;
    double scale = sx < sy ? sx : sy;
    if (scale < 1.0) {
      disp_w *= scale;
      disp_h *= scale;
    }
  }
  int win_w = (int)disp_w;
  if (win_w < 64)
    win_w = 64;
  int win_h = (int)disp_h;
  if (win_h < 64)
    win_h = 64;

  if (!ctx->window) {
    ctx->window = SDL_CreateWindow(title, win_w, win_h,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!ctx->window)
      return 0;
  } else {
    SDL_SetWindowTitle(ctx->window, title);
    SDL_WindowFlags flags = SDL_GetWindowFlags(ctx->window);
    int is_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    int is_fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    int is_tiling_wm = (getenv("SWAYSOCK") != NULL) ||
                       (getenv("HYPRLAND_INSTANCE_SIGNATURE") != NULL) ||
                       (getenv("NIRI_SOCKET") != NULL);
    if (!is_tiling_wm && !is_maximized && !is_fullscreen)
      SDL_SetWindowSize(ctx->window, win_w, win_h);
  }
  const char *drivers[] = {"opengl", "opengles2", "vulkan", "software", NULL};
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
    if (ctx->renderer)
      printf("[VidP] Fallback: Using default/software SDL renderer.\n");
  }
  if (!ctx->renderer)
    return 0;
  if (SDL_GetWindowFlags(ctx->window) & SDL_WINDOW_HIDDEN)
    SDL_ShowWindow(ctx->window);
  ctx->texture =
      SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV,
                        SDL_TEXTUREACCESS_STREAMING, ctx->width, ctx->height);
  if (ctx->texture)
    SDL_SetTextureScaleMode(ctx->texture, SDL_SCALEMODE_LINEAR);
  return ctx->texture != NULL;
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
  if (!downscale_enabled() || ctx->rotation != 0 || !ctx->window)
    return;
  SDL_FRect dest;
  player_compute_dest_rect(ctx, &dest);
  if (dest.w < 2.0f || dest.h < 2.0f)
    return;
  if ((double)ctx->width / dest.w < DOWNSCALE_MIN_RATIO)
    return;
  int w = (int)(dest.w + 0.5f) & ~1;
  int h = (int)(dest.h + 0.5f) & ~1;
  if (w < 2)
    w = 2;
  if (h < 2)
    h = 2;
  if (w >= ctx->width || h >= ctx->height)
    return;
  if (ctx->texture && ctx->tex_w > 0 && ctx->tex_w < ctx->width &&
      ctx->tex_h < ctx->height && abs(w - ctx->tex_w) * 10 <= ctx->tex_w) {
    w = ctx->tex_w;
    h = ctx->tex_h;
  }
  *tw = w;
  *th = h;
}

static SDL_Colorspace pick_colorspace(const AVFrame *src, int range_normalized,
                                      int height) {
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
    if (height > 576)
      return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
  }
}

static SDL_Texture *create_video_texture(SDL_Renderer *r, SDL_PixelFormat fmt,
                                         int w, int h, SDL_Colorspace cs) {
  SDL_PropertiesID p = SDL_CreateProperties();
  if (!p)
    return NULL;
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, fmt);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                        SDL_TEXTUREACCESS_STREAMING);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, cs);
  SDL_Texture *t = SDL_CreateTextureWithProperties(r, p);
  SDL_DestroyProperties(p);
  return t;
}

void player_render_current_frame(PlayerContext *ctx, AVFrame *render_frame) {
  AVFrame *final_frame = render_frame;
  int negative_stride = render_frame->linesize[0] < 0 ||
                        render_frame->linesize[1] < 0 ||
                        render_frame->linesize[2] < 0;
  int tw, th;
  player_pick_texture_size(ctx, &tw, &th);
  int resize = (tw != ctx->width || th != ctx->height);

  if (negative_stride || resize ||
      (render_frame->format != AV_PIX_FMT_YUV420P &&
       render_frame->format != AV_PIX_FMT_NV12)) {
    if (ctx->frame_yuv &&
        (ctx->frame_yuv->width != tw || ctx->frame_yuv->height != th))
      av_frame_free(&ctx->frame_yuv);
    if (!ctx->frame_yuv) {
      ctx->frame_yuv = av_frame_alloc();
      if (!ctx->frame_yuv)
        return;
      ctx->frame_yuv->format = AV_PIX_FMT_YUV420P;
      ctx->frame_yuv->width = tw;
      ctx->frame_yuv->height = th;
      if (av_frame_get_buffer(ctx->frame_yuv, 32) < 0) {
        av_frame_free(&ctx->frame_yuv);
        return;
      }
    }
    ctx->sws_ctx = sws_getCachedContext(
        ctx->sws_ctx, render_frame->width, render_frame->height,
        (enum AVPixelFormat)render_frame->format, tw, th, AV_PIX_FMT_YUV420P,
        resize ? SWS_AREA : SWS_BICUBIC, NULL, NULL, NULL);
    if (ctx->sws_ctx) {
      int scaled =
          sws_scale(ctx->sws_ctx, (const uint8_t *const *)render_frame->data,
                    render_frame->linesize, 0, render_frame->height,
                    ctx->frame_yuv->data, ctx->frame_yuv->linesize);
      if (scaled > 0)
        final_frame = ctx->frame_yuv;
      else {
        SDL_RenderClear(ctx->renderer);
        SDL_RenderPresent(ctx->renderer);
        return;
      }
    }
  }

  SDL_PixelFormat req_format = SDL_PIXELFORMAT_IYUV;
  if (final_frame->format == AV_PIX_FMT_NV12)
    req_format = SDL_PIXELFORMAT_NV12;
  int j_fmt = render_frame->format == AV_PIX_FMT_YUVJ420P ||
              render_frame->format == AV_PIX_FMT_YUVJ422P ||
              render_frame->format == AV_PIX_FMT_YUVJ444P ||
              render_frame->format == AV_PIX_FMT_YUVJ440P;
  SDL_Colorspace cs =
      pick_colorspace(render_frame, (final_frame != render_frame) && j_fmt,
                      render_frame->height);

  if (!ctx->texture || ctx->current_tex_format != req_format ||
      ctx->tex_cs != (uint32_t)cs || ctx->tex_w != final_frame->width ||
      ctx->tex_h != final_frame->height) {
    if (ctx->texture)
      SDL_DestroyTexture(ctx->texture);
    ctx->texture = create_video_texture(
        ctx->renderer, req_format, final_frame->width, final_frame->height, cs);
    ctx->current_tex_format = req_format;
    ctx->tex_cs = (uint32_t)cs;
    ctx->tex_w = final_frame->width;
    ctx->tex_h = final_frame->height;
    if (ctx->texture)
      SDL_SetTextureScaleMode(ctx->texture, SDL_SCALEMODE_LINEAR);
  }
  if (!ctx->texture) {
    SDL_RenderClear(ctx->renderer);
    SDL_RenderPresent(ctx->renderer);
    return;
  }

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
    SDL_UpdateYUVTexture(ctx->texture, NULL, y_ptr, y_stride, u_ptr, u_stride,
                         v_ptr, v_stride);
  }
  player_present_frame(ctx);
}

int thread_video_decode(void *data) {
  PlayerContext *ctx = (PlayerContext *)data;
  AVFrame *frame = av_frame_alloc();

  while (!atomic_load(&ctx->abort_request)) {
    AVPacket *pkt = packet_queue_get(&ctx->v_pkt_q);

    if (!pkt) {
      if (atomic_load(&ctx->abort_request))
        break;

      SDL_LockMutex(ctx->v_frame_q.mutex);
      atomic_store(&ctx->v_dec_eof, 1);
      SDL_BroadcastCondition(ctx->v_frame_q.cond);
      SDL_UnlockMutex(ctx->v_frame_q.mutex);

      SDL_LockMutex(ctx->v_pkt_q.mutex);
      while (!atomic_load(&ctx->abort_request) &&
             atomic_load(&ctx->v_dec_eof)) {
        SDL_WaitCondition(ctx->v_pkt_q.cond, ctx->v_pkt_q.mutex);
      }
      SDL_UnlockMutex(ctx->v_pkt_q.mutex);

      if (atomic_load(&ctx->abort_request))
        break;
      continue;
    }

    SDL_LockMutex(ctx->seek_mutex);
    avcodec_send_packet(ctx->v_codec_ctx, pkt);
    SDL_UnlockMutex(ctx->seek_mutex);
    av_packet_free(&pkt);

    while (!atomic_load(&ctx->abort_request)) {
      SDL_LockMutex(ctx->seek_mutex);
      int ret = avcodec_receive_frame(ctx->v_codec_ctx, frame);
      if (ret != 0) {
        SDL_UnlockMutex(ctx->seek_mutex);
        break;
      }

      AVFrame *render_frame = frame;
      AVFrame *sw_frame = NULL;

      if (ctx->is_hw_accel && frame->format == ctx->hw_pix_fmt) {
        sw_frame = av_frame_alloc();
        if (sw_frame && av_hwframe_transfer_data(sw_frame, frame, 0) >= 0) {
          sw_frame->pts = frame->pts;
          sw_frame->best_effort_timestamp = frame->best_effort_timestamp;
          sw_frame->color_range = frame->color_range;
          sw_frame->colorspace = frame->colorspace;
          sw_frame->color_primaries = frame->color_primaries;
          sw_frame->color_trc = frame->color_trc;
          sw_frame->chroma_location = frame->chroma_location;
          render_frame = sw_frame;
          ctx->hw_transfer_failures = 0;
        } else {
          if (sw_frame)
            av_frame_free(&sw_frame);
          sw_frame = NULL;
          render_frame = NULL;
          if (++ctx->hw_transfer_failures >= 10) {
            av_frame_unref(frame);
            SDL_UnlockMutex(ctx->seek_mutex);
            player_fallback_to_sw(ctx);
            break;
          }
        }
      }

      AVFrame *clone = NULL;
      if (render_frame) {
        clone = av_frame_clone(render_frame);
        if (clone) {
          clone->pts = frame->pts;
          clone->best_effort_timestamp = frame->best_effort_timestamp;
        }
      }

      av_frame_unref(frame);
      if (sw_frame)
        av_frame_free(&sw_frame);

      int stale = 0;
      if (clone && ctx->discard_until >= 0.0) {
        if (clone->best_effort_timestamp != AV_NOPTS_VALUE) {
          double pts =
              clone->best_effort_timestamp *
              av_q2d(ctx->fmt_ctx->streams[ctx->video_stream]->time_base);
          if (pts < ctx->discard_until - 0.2)
            stale = 1;
          else
            ctx->discard_until = -1.0;
        } else {
          ctx->discard_until = -1.0;
        }
      }

      SDL_UnlockMutex(ctx->seek_mutex);

      if (!clone || stale) {
        if (clone)
          av_frame_free(&clone);
        continue;
      }

      SDL_LockMutex(ctx->v_frame_q.mutex);
      while (ctx->v_frame_q.count >= ctx->frame_queue_cap &&
             !atomic_load(&ctx->v_frame_q.abort_request)) {
        SDL_WaitCondition(ctx->v_frame_q.cond, ctx->v_frame_q.mutex);
      }
      if (atomic_load(&ctx->v_frame_q.abort_request)) {
        SDL_UnlockMutex(ctx->v_frame_q.mutex);
        av_frame_free(&clone);
        break;
      }

      ctx->v_frame_q.frames[ctx->v_frame_q.w_idx] = clone;
      ctx->v_frame_q.w_idx = (ctx->v_frame_q.w_idx + 1) % ctx->frame_queue_cap;
      ctx->v_frame_q.count++;
      SDL_SignalCondition(ctx->v_frame_q.cond);
      SDL_UnlockMutex(ctx->v_frame_q.mutex);
    }
  }

  SDL_LockMutex(ctx->v_frame_q.mutex);
  atomic_store(&ctx->v_dec_eof, 1);
  SDL_BroadcastCondition(ctx->v_frame_q.cond);
  SDL_UnlockMutex(ctx->v_frame_q.mutex);

  av_frame_free(&frame);
  return 0;
}
