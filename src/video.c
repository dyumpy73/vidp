#include "player.h"

enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
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

int player_init_display(PlayerContext *ctx, const char *title) {
  ctx->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                ctx->width, ctx->height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
  if (!ctx->window) return 0;

  const char *drivers[] = { "opengl", "opengles2", "vulkan", NULL };
  ctx->renderer = NULL;

  for (int i = 0; drivers[i] != NULL; i++) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, drivers[i]);
    ctx->renderer = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_ACCELERATED);
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

  ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   ctx->width, ctx->height);
  if (ctx->texture) {
    SDL_SetTextureScaleMode(ctx->texture, SDL_ScaleModeLinear);
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

  if (ctx->frame_yuv) {
    av_frame_free(&ctx->frame_yuv);
    ctx->frame_yuv = NULL;
  }

  if (ctx->sws_ctx) {
    sws_freeContext(ctx->sws_ctx);
    ctx->sws_ctx = NULL;
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

void player_render_current_frame(PlayerContext *ctx, AVFrame *render_frame) {
  AVFrame *final_frame = render_frame;

  if (render_frame->format != AV_PIX_FMT_YUV420P &&
      render_frame->format != AV_PIX_FMT_NV12) {
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
        render_frame->width, render_frame->height,
        (enum AVPixelFormat)render_frame->format,
        ctx->width, ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BICUBIC, NULL, NULL, NULL);

    if (ctx->sws_ctx) {
      int scaled = sws_scale(ctx->sws_ctx,
                             (const uint8_t *const *)render_frame->data,
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
    ctx->texture = SDL_CreateTexture(ctx->renderer, req_format,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     ctx->width, ctx->height);
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
  player_present_frame(ctx);
}

void player_receive_video_frames(PlayerContext *ctx) {
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

void player_process_video_packet(PlayerContext *ctx) {
  int ret = avcodec_send_packet(ctx->v_codec_ctx, ctx->packet);
  if (ret == AVERROR(EAGAIN)) {
    player_receive_video_frames(ctx);
    ret = avcodec_send_packet(ctx->v_codec_ctx, ctx->packet);
  }
  if (ret < 0 && ret != AVERROR_EOF) return;
  player_receive_video_frames(ctx);
}

