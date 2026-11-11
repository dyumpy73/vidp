#include "player.h"
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_timer.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static float g_master_volume = 1.0f;
static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Cursor *g_blank_cursor = NULL;
static int g_cursor_hidden = 0;

void player_detect_device_tuning(PlayerContext *ctx) {
  int ram_mb = SDL_GetSystemRAM();
  if (ram_mb <= 0)
    ram_mb = 2048;
  int logical = SDL_GetNumLogicalCPUCores();
  if (logical < 1)
    logical = 1;

  int frame_q, packet_q;
  if (ram_mb < 2048) {
    frame_q = 2;
    packet_q = 8;
  } else if (ram_mb < 4096) {
    frame_q = 2;
    packet_q = 12;
  } else if (ram_mb < 8192) {
    frame_q = 3;
    packet_q = 16;
  } else if (ram_mb < 16384) {
    frame_q = 4;
    packet_q = 24;
  } else {
    frame_q = 6;
    packet_q = 32;
  }
  if (logical <= 1) {
    frame_q = 2;
    packet_q = 8;
  }

  const char *e;
  if ((e = getenv("VIDP_FRAME_QUEUE"))) {
    int v = atoi(e);
    if (v >= 2 && v <= MAX_FRAME_QUEUE_HARD_LIMIT)
      frame_q = v;
  }
  if ((e = getenv("VIDP_PACKET_QUEUE"))) {
    int v = atoi(e);
    if (v >= 4 && v <= MAX_PACKET_QUEUE_HARD_LIMIT)
      packet_q = v;
  }
  const char *lowmem = getenv("VIDP_LOWMEM");
  if (lowmem && (strcmp(lowmem, "1") == 0 || strcasecmp(lowmem, "true") == 0)) {
    frame_q = 2;
    packet_q = 8;
  }

  ctx->frame_queue_cap = frame_q;
  ctx->packet_queue_cap = packet_q;
  printf("[VidP] Device: %d MB RAM, %d CPU cores\n", ram_mb, logical);
  printf("[VidP] Tuning: frame_queue=%d, packet_queue=%d\n", frame_q, packet_q);
}

void packet_queue_init(PacketQueue *q, int capacity) {
  memset(q, 0, sizeof(PacketQueue));
  q->capacity = capacity;
  q->mutex = SDL_CreateMutex();
  q->cond = SDL_CreateCondition();
  if (!q->mutex || !q->cond) {
    fprintf(stderr, "[VidP] Failed to init packet queue!\n");
    return;
  }
  atomic_store(&q->abort_request, 0);
}

void packet_queue_put(PacketQueue *q, AVPacket *pkt) {
  if (!q->mutex || !q->cond) {
    if (pkt)
      av_packet_free(&pkt);
    return;
  }
  SDL_LockMutex(q->mutex);
  while (q->count >= q->capacity && !atomic_load(&q->abort_request)) {
    SDL_WaitCondition(q->cond, q->mutex);
  }
  if (atomic_load(&q->abort_request)) {
    SDL_UnlockMutex(q->mutex);
    if (pkt)
      av_packet_free(&pkt);
    return;
  }
  q->pkts[q->w_idx] = pkt;
  q->w_idx = (q->w_idx + 1) % q->capacity;
  q->count++;
  SDL_SignalCondition(q->cond);
  SDL_UnlockMutex(q->mutex);
}

AVPacket *packet_queue_get(PacketQueue *q) {
  if (!q->mutex || !q->cond)
    return NULL;
  SDL_LockMutex(q->mutex);
  while (q->count == 0 && !atomic_load(&q->abort_request)) {
    SDL_WaitCondition(q->cond, q->mutex);
  }
  if (atomic_load(&q->abort_request)) {
    SDL_UnlockMutex(q->mutex);
    return NULL;
  }
  AVPacket *pkt = q->pkts[q->r_idx];
  q->r_idx = (q->r_idx + 1) % q->capacity;
  q->count--;
  SDL_SignalCondition(q->cond);
  SDL_UnlockMutex(q->mutex);
  return pkt;
}

void packet_queue_flush(PacketQueue *q) {
  if (!q->mutex || !q->cond)
    return;
  SDL_LockMutex(q->mutex);
  while (q->count > 0) {
    if (q->pkts[q->r_idx])
      av_packet_free(&q->pkts[q->r_idx]);
    q->r_idx = (q->r_idx + 1) % q->capacity;
    q->count--;
  }
  SDL_BroadcastCondition(q->cond);
  SDL_UnlockMutex(q->mutex);
}

int thread_demux(void *data) {
  PlayerContext *ctx = (PlayerContext *)data;
  AVPacket *pkt = av_packet_alloc();

  while (!atomic_load(&ctx->abort_request)) {
    int gen_before = atomic_load(&ctx->seek_generation);

    SDL_LockMutex(ctx->seek_mutex);
    int ret = av_read_frame(ctx->fmt_ctx, pkt);
    SDL_UnlockMutex(ctx->seek_mutex);

    if (ret < 0) {
      if (atomic_load(&ctx->abort_request))
        break;

      atomic_store(&ctx->demux_eof, 1);
      packet_queue_put(&ctx->v_pkt_q, NULL);
      packet_queue_put(&ctx->a_pkt_q, NULL);

      SDL_LockMutex(ctx->v_pkt_q.mutex);
      while (!atomic_load(&ctx->abort_request) &&
             atomic_load(&ctx->demux_eof)) {
        SDL_WaitCondition(ctx->v_pkt_q.cond, ctx->v_pkt_q.mutex);
      }
      SDL_UnlockMutex(ctx->v_pkt_q.mutex);

      if (atomic_load(&ctx->abort_request))
        break;
      continue;
    }

    if (atomic_load(&ctx->seek_generation) != gen_before) {
      av_packet_unref(pkt);
      continue;
    }

    if (pkt->stream_index == ctx->video_stream) {
      AVPacket *clone = av_packet_clone(pkt);
      if (clone)
        packet_queue_put(&ctx->v_pkt_q, clone);
    } else if (pkt->stream_index == ctx->audio_stream) {
      AVPacket *clone = av_packet_clone(pkt);
      if (clone)
        packet_queue_put(&ctx->a_pkt_q, clone);
    } else {
      for (int i = 0; i < ctx->subtitle_stream_count; i++) {
        if (pkt->stream_index == ctx->subtitle_streams[i]) {
          ctx->packet = pkt;
          player_process_subtitle_packet(ctx);
          ctx->packet = NULL;
          break;
        }
      }
    }
    av_packet_unref(pkt);
  }

  packet_queue_put(&ctx->v_pkt_q, NULL);
  packet_queue_put(&ctx->a_pkt_q, NULL);
  av_packet_free(&pkt);
  return 0;
}

void player_do_seek(PlayerContext *ctx, double target_sec) {
  if (atomic_load(&ctx->abort_request) || ctx->file_finished)
    return;

  atomic_fetch_add(&ctx->seek_generation, 1);
  atomic_fetch_add(&ctx->audio_generation, 1);

  if (target_sec < 0.0)
    target_sec = 0.0;
  if (ctx->audio_dev > 0)
    SDL_PauseAudioDevice(ctx->audio_dev);

  if (ctx->fmt_ctx->duration != AV_NOPTS_VALUE && ctx->fmt_ctx->duration > 0) {
    double start = (ctx->fmt_ctx->start_time != AV_NOPTS_VALUE)
                       ? (double)ctx->fmt_ctx->start_time / AV_TIME_BASE
                       : 0.0;
    double dur = (double)ctx->fmt_ctx->duration / AV_TIME_BASE;
    double max_target = start + dur - 0.5;
    if (max_target < 0.0)
      max_target = 0.0;
    if (target_sec > max_target)
      target_sec = max_target;
  }

  double seek_from_sec = target_sec - SUBTITLE_PREROLL_SEC;
  if (seek_from_sec < 0.0)
    seek_from_sec = 0.0;

  int stream_idx = -1;
  int64_t seek_av_time = (int64_t)(seek_from_sec * AV_TIME_BASE);
  if (ctx->fmt_ctx->start_time != AV_NOPTS_VALUE &&
      ctx->fmt_ctx->start_time > 0)
    seek_av_time += ctx->fmt_ctx->start_time;

  int64_t seek_target = seek_av_time;
  if (ctx->video_stream >= 0) {
    stream_idx = ctx->video_stream;
    AVRational tb = ctx->fmt_ctx->streams[stream_idx]->time_base;
    seek_target = av_rescale_q(seek_av_time, AV_TIME_BASE_Q, tb);
  }

  SDL_LockMutex(ctx->seek_mutex);

  if (av_seek_frame(ctx->fmt_ctx, stream_idx, seek_target,
                    AVSEEK_FLAG_BACKWARD) < 0) {
    fprintf(stderr, "[VidP] av_seek_frame failed\n");
    SDL_UnlockMutex(ctx->seek_mutex);
    if (ctx->audio_dev > 0) {
      if (ctx->paused)
        SDL_PauseAudioDevice(ctx->audio_dev);
      else
        SDL_ResumeAudioDevice(ctx->audio_dev);
    }
    return;
  }

  if (ctx->v_codec_ctx)
    avcodec_flush_buffers(ctx->v_codec_ctx);
  if (ctx->a_codec_ctx)
    avcodec_flush_buffers(ctx->a_codec_ctx);

  packet_queue_flush(&ctx->v_pkt_q);
  packet_queue_flush(&ctx->a_pkt_q);

  player_reset_audio_buffer(ctx);
  if (ctx->swr_ctx) {
    swr_close(ctx->swr_ctx);
    swr_init(ctx->swr_ctx);
  }

  atomic_store(&ctx->demux_eof, 0);
  atomic_store(&ctx->v_dec_eof, 0);
  atomic_store(&ctx->a_dec_eof, 0);

  ctx->discard_until = target_sec;
  ctx->audio_discard_until = target_sec;

  SDL_UnlockMutex(ctx->seek_mutex);

  SDL_LockMutex(ctx->v_pkt_q.mutex);
  SDL_BroadcastCondition(ctx->v_pkt_q.cond);
  SDL_UnlockMutex(ctx->v_pkt_q.mutex);
  SDL_LockMutex(ctx->a_pkt_q.mutex);
  SDL_BroadcastCondition(ctx->a_pkt_q.cond);
  SDL_UnlockMutex(ctx->a_pkt_q.mutex);

  SDL_LockMutex(ctx->v_frame_q.mutex);
  while (ctx->v_frame_q.count > 0) {
    av_frame_free(&ctx->v_frame_q.frames[ctx->v_frame_q.r_idx]);
    ctx->v_frame_q.r_idx = (ctx->v_frame_q.r_idx + 1) % ctx->frame_queue_cap;
    ctx->v_frame_q.count--;
  }
  SDL_BroadcastCondition(ctx->v_frame_q.cond);
  SDL_UnlockMutex(ctx->v_frame_q.mutex);

  player_reset_subtitle_state(ctx);

  ctx->video_clock_started = 0;
  ctx->first_video_time = 0.0;
  ctx->playback_start_wall = 0.0;
  ctx->last_video_time = target_sec;
  ctx->last_audio_pts = -1.0;
  ctx->pending_audio_pts = -1.0;
  ctx->consecutive_drops = 0;
  ctx->sub_frame_w = 0;
  ctx->audio_starve_since = 0.0;
  ctx->last_clock_valid_time = 0.0;

  if (ctx->last_rendered_frame)
    av_frame_unref(ctx->last_rendered_frame);
  if (ctx->sws_ctx) {
    sws_freeContext(ctx->sws_ctx);
    ctx->sws_ctx = NULL;
  }

  if (ctx->audio_dev > 0) {
    if (ctx->paused)
      SDL_PauseAudioDevice(ctx->audio_dev);
    else
      SDL_ResumeAudioDevice(ctx->audio_dev);
  }
  if (ctx->paused)
    ctx->step_pending = 1;
  trim_memory();
}

int player_open_file(PlayerContext *ctx, const char *filepath, int index,
                     int total) {
  memset(ctx, 0, sizeof(PlayerContext));
  ctx->seek_mutex = SDL_CreateMutex();
  player_detect_device_tuning(ctx);
  ctx->window = g_window;
  ctx->renderer = g_renderer;
  ctx->playlist_count = total;
  ctx->playlist_index = index;
  ctx->nav_action = NAV_NEXT;
  ctx->volume = g_master_volume;
  ctx->discard_until = -1.0;
  ctx->audio_discard_until = -1.0;
  ctx->last_audio_pts = -1.0;
  ctx->pending_audio_pts = -1.0;
  ctx->video_stream = -1;
  ctx->audio_stream = -1;
  ctx->subtitle_visible = 1;
  ctx->subtitle_stream_count = 0;
  ctx->current_subtitle_idx = -1;
  atomic_store(&ctx->audio_generation, 0);
  atomic_store(&ctx->seek_generation, 0);

  if (!g_blank_cursor)
    g_blank_cursor = create_blank_cursor();
  ctx->blank_cursor = g_blank_cursor;
  ctx->last_mouse_move = SDL_GetTicks();
  ctx->cursor_hidden = g_cursor_hidden;

  ctx->fmt_ctx = avformat_alloc_context();
  if (!ctx->fmt_ctx)
    goto fail;
  ctx->fmt_ctx->probesize = 4 * 1024 * 1024;
  ctx->fmt_ctx->max_analyze_duration = 3 * AV_TIME_BASE;
  if (avformat_open_input(&ctx->fmt_ctx, filepath, NULL, NULL) != 0)
    goto fail;
  if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0)
    goto fail;

  for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
    AVCodecParameters *codecpar = ctx->fmt_ctx->streams[i]->codecpar;
    int is_cover = (ctx->fmt_ctx->streams[i]->disposition &
                    AV_DISPOSITION_ATTACHED_PIC) != 0;
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ctx->video_stream < 0 &&
        !is_cover)
      ctx->video_stream = i;
    if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_stream < 0)
      ctx->audio_stream = i;
    if (codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      if (codecpar->codec_id == AV_CODEC_ID_ASS ||
          codecpar->codec_id == AV_CODEC_ID_SSA) {
        if (ctx->subtitle_stream_count < MAX_SUBTITLE_STREAM)
          ctx->subtitle_streams[ctx->subtitle_stream_count++] = i;
      }
    }
  }

  if (ctx->subtitle_stream_count > 0)
    ctx->current_subtitle_idx = 0;
  if (ctx->video_stream < 0)
    goto fail;

  AVCodecParameters *v_par = ctx->fmt_ctx->streams[ctx->video_stream]->codecpar;
  const AVCodec *v_codec = avcodec_find_decoder(v_par->codec_id);
  if (!v_codec)
    goto fail;

  ctx->v_codec_ctx = avcodec_alloc_context3(v_codec);
  if (!ctx->v_codec_ctx)
    goto fail;
  if (avcodec_parameters_to_context(ctx->v_codec_ctx, v_par) < 0)
    goto fail;
  ctx->v_codec_ctx->opaque = ctx;
  ctx->hw_pix_fmt = AV_PIX_FMT_NONE;

  static const enum AVHWDeviceType linux_hw_priority[] = {
      AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VDPAU,
      AV_HWDEVICE_TYPE_NONE};
  for (int i = 0; linux_hw_priority[i] != AV_HWDEVICE_TYPE_NONE; i++) {
    for (int j = 0;; j++) {
      const AVCodecHWConfig *config = avcodec_get_hw_config(v_codec, j);
      if (!config)
        break;
      if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
          config->device_type == linux_hw_priority[i]) {
        if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, linux_hw_priority[i],
                                   NULL, NULL, 0) >= 0) {
          ctx->hw_pix_fmt = config->pix_fmt;
          ctx->v_codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
          ctx->v_codec_ctx->get_format = get_hw_format;
          ctx->is_hw_accel = 1;
          printf("[VidP] HW Acceleration active: %s\n",
                 av_hwdevice_get_type_name(linux_hw_priority[i]));
          break;
        }
      }
    }
    if (ctx->is_hw_accel)
      break;
  }

  player_configure_video_threads(ctx->v_codec_ctx, ctx->is_hw_accel);

  if (avcodec_open2(ctx->v_codec_ctx, v_codec, NULL) < 0) {
    if (!ctx->is_hw_accel)
      goto fail;
    if (!player_init_sw_video(ctx))
      goto fail;
  }

  ctx->width = ctx->v_codec_ctx->width;
  ctx->height = ctx->v_codec_ctx->height;

  AVStream *vst = ctx->fmt_ctx->streams[ctx->video_stream];
  AVRational sar = av_guess_sample_aspect_ratio(ctx->fmt_ctx, vst, NULL);
  if (sar.num <= 0 || sar.den <= 0)
    sar = (AVRational){1, 1};
  ctx->sar_ratio = (double)sar.num / (double)sar.den;
  ctx->rotation = 0;

  AVDictionaryEntry *rot = av_dict_get(vst->metadata, "rotate", NULL, 0);
  if (rot) {
    int r = atoi(rot->value);
    r = ((r % 360) + 360) % 360;
    if (r == 90 || r == 180 || r == 270)
      ctx->rotation = r;
  }

  if (ctx->audio_stream >= 0) {
    if (!player_init_audio(ctx)) {
      fprintf(stderr, "[VidP] Audio init failed, continuing without audio.\n");
      ctx->audio_stream = -1;
    }
  }
  player_init_subtitles(ctx);

  ctx->frame_video = av_frame_alloc();
  ctx->frame_audio = av_frame_alloc();
  ctx->last_rendered_frame = av_frame_alloc();
  ctx->packet = av_packet_alloc();
  if (!ctx->frame_video || !ctx->frame_audio || !ctx->packet ||
      !ctx->last_rendered_frame)
    goto fail;

  const char *base = strrchr(filepath, '/');
  base = base ? base + 1 : filepath;
  char title[512];
  snprintf(title, sizeof(title), "VidP [%d/%d] - %s", index + 1, total, base);

  if (!player_init_display(ctx, title))
    goto fail;
  trim_memory();
  return 1;

fail:
  player_close_file(ctx);
  return 0;
}

void player_close_file(PlayerContext *ctx) {
  if (ctx->audio_dev > 0)
    SDL_PauseAudioDevice(ctx->audio_dev);
  if (ctx->blank_cursor) {
    g_cursor_hidden = ctx->cursor_hidden;
    ctx->blank_cursor = NULL;
  }
  if (ctx->sws_ctx) {
    sws_freeContext(ctx->sws_ctx);
    ctx->sws_ctx = NULL;
  }
  if (ctx->frame_yuv)
    av_frame_free(&ctx->frame_yuv);

  for (int i = 0; i < MAX_SUBTITLE_STREAM; i++) {
    if (ctx->ass_tracks[i]) {
      ass_free_track(ctx->ass_tracks[i]);
      ctx->ass_tracks[i] = NULL;
    }
  }
  ctx->ass_track = NULL;
  if (ctx->ass_renderer) {
    ass_renderer_done(ctx->ass_renderer);
    ctx->ass_renderer = NULL;
  }
  if (ctx->ass_library) {
    ass_library_done(ctx->ass_library);
    ctx->ass_library = NULL;
  }
  if (ctx->sub_tex) {
    SDL_DestroyTexture(ctx->sub_tex);
    ctx->sub_tex = NULL;
  }

  free(ctx->sub_tmp);
  ctx->sub_tmp = NULL;
  ctx->sub_tmp_cap = 0;

  if (ctx->sdl_stream) {
    SDL_DestroyAudioStream(ctx->sdl_stream);
    ctx->sdl_stream = NULL;
  }
  if (ctx->audio_dev > 0) {
    SDL_CloseAudioDevice(ctx->audio_dev);
    ctx->audio_dev = 0;
  }
  if (ctx->swr_ctx)
    swr_free(&ctx->swr_ctx);
  if (ctx->audio_out_buffer)
    av_freep(&ctx->audio_out_buffer);

  for (int i = 0; i < MAX_FRAME_QUEUE_HARD_LIMIT; i++) {
    if (ctx->v_frame_q.frames[i])
      av_frame_free(&ctx->v_frame_q.frames[i]);
  }

  if (ctx->frame_video)
    av_frame_free(&ctx->frame_video);
  if (ctx->frame_audio)
    av_frame_free(&ctx->frame_audio);
  if (ctx->last_rendered_frame)
    av_frame_free(&ctx->last_rendered_frame);
  if (ctx->packet)
    av_packet_free(&ctx->packet);

  if (ctx->v_codec_ctx)
    avcodec_free_context(&ctx->v_codec_ctx);
  if (ctx->a_codec_ctx)
    avcodec_free_context(&ctx->a_codec_ctx);
  if (ctx->hw_device_ctx) {
    av_buffer_unref(&ctx->hw_device_ctx);
    ctx->hw_device_ctx = NULL;
  }
  if (ctx->fmt_ctx)
    avformat_close_input(&ctx->fmt_ctx);
  if (ctx->texture)
    SDL_DestroyTexture(ctx->texture);

  ctx->texture = NULL;
  ctx->current_tex_format = 0;
  if (ctx->window)
    g_window = ctx->window;
  if (ctx->renderer)
    g_renderer = ctx->renderer;

  if (ctx->seek_mutex) {
    SDL_DestroyMutex(ctx->seek_mutex);
    ctx->seek_mutex = NULL;
  }
  trim_memory();
}

void player_shutdown_display(void) {
  if (g_blank_cursor) {
    SDL_SetCursor(SDL_GetDefaultCursor());
    SDL_DestroyCursor(g_blank_cursor);
    g_blank_cursor = NULL;
  }
  if (g_renderer) {
    SDL_DestroyRenderer(g_renderer);
    g_renderer = NULL;
  }
  if (g_window) {
    SDL_DestroyWindow(g_window);
    g_window = NULL;
  }
}

void player_compute_dest_rect(PlayerContext *ctx, SDL_FRect *dest_rect) {
  int win_w = 0, win_h = 0;
  SDL_GetWindowSize(ctx->window, &win_w, &win_h);
  if (win_w < 1)
    win_w = 1;
  if (win_h < 1)
    win_h = 1;
  double video_aspect =
      ((double)ctx->width * ctx->sar_ratio) / (double)ctx->height;
  if (ctx->rotation == 90 || ctx->rotation == 270)
    video_aspect = 1.0 / video_aspect;
  float window_aspect = (float)win_w / (float)win_h;
  if (window_aspect > video_aspect) {
    dest_rect->h = (float)win_h;
    dest_rect->w = (float)(win_h * video_aspect);
    dest_rect->x = (float)(win_w - dest_rect->w) / 2.0f;
    dest_rect->y = 0.0f;
  } else {
    dest_rect->w = (float)win_w;
    dest_rect->h = (float)(win_w / video_aspect);
    dest_rect->x = 0.0f;
    dest_rect->y = (float)(win_h - dest_rect->h) / 2.0f;
  }
}

void player_present_frame(PlayerContext *ctx) {
  if (!ctx->texture)
    return;
  SDL_FRect dest_rect;
  player_compute_dest_rect(ctx, &dest_rect);
  SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
  SDL_RenderClear(ctx->renderer);

  if (ctx->rotation != 0) {
    SDL_FRect r = dest_rect;
    if (ctx->rotation == 90 || ctx->rotation == 270) {
      r.w = dest_rect.h;
      r.h = dest_rect.w;
      r.x = dest_rect.x + (dest_rect.w - r.w) / 2.0f;
      r.y = dest_rect.y + (dest_rect.h - r.h) / 2.0f;
    }
    SDL_RenderTextureRotated(ctx->renderer, ctx->texture, NULL, &r,
                             (double)ctx->rotation, NULL, SDL_FLIP_NONE);
  } else {
    SDL_RenderTexture(ctx->renderer, ctx->texture, NULL, &dest_rect);
  }

  if (ctx->subtitle_visible && ctx->ass_renderer && ctx->ass_track) {
    int changed = 0;
    int64_t now_ms = (int64_t)(ctx->last_video_time * 1000);
    ass_set_frame_size(ctx->ass_renderer, (int)dest_rect.w, (int)dest_rect.h);
    ASS_Image *sub_img =
        ass_render_frame(ctx->ass_renderer, ctx->ass_track, now_ms, &changed);
    if (sub_img)
      render_ass_overlay(ctx, sub_img, &dest_rect, changed);
  }

  if (ctx->volume_overlay_until > 0.0 &&
      get_monotonic_time() < ctx->volume_overlay_until) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Volume: %d%%", (int)(ctx->volume * 100 + 0.5f));
    int ww, wh;
    SDL_GetWindowSize(ctx->window, &ww, &wh);
    SDL_Color white = {255, 255, 255, 255};
    SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);
    SDL_FRect box = {20, (float)wh - 60, 170, 34};
    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 190);
    SDL_RenderFillRect(ctx->renderer, &box);
    ui_text_left(ctx->renderer, buf, white, 30, wh - 54);
    SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_NONE);
  }

  SDL_RenderPresent(ctx->renderer);
  ctx->has_last_frame = 1;
}

void player_handle_events(PlayerContext *ctx, int *global_quit,
                          int *playlist_index) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT)
      *global_quit = 1;
    if (event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
      ctx->last_mouse_move = SDL_GetTicks();
      if (ctx->cursor_hidden) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        ctx->cursor_hidden = 0;
      }
    }
    if (event.type == SDL_EVENT_WINDOW_MOUSE_ENTER ||
        event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
        event.type == SDL_EVENT_WINDOW_RESIZED ||
        event.type == SDL_EVENT_WINDOW_EXPOSED) {
      ctx->last_mouse_move = SDL_GetTicks();
      if (ctx->cursor_hidden) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        ctx->cursor_hidden = 0;
      }
      if (ctx->paused) {
        if (ctx->last_rendered_frame && ctx->last_rendered_frame->width > 0)
          player_render_current_frame(ctx, ctx->last_rendered_frame);
        else if (ctx->has_last_frame && ctx->texture)
          player_present_frame(ctx);
      }
    }

    if (event.type == SDL_EVENT_KEY_DOWN) {
      if (event.key.repeat && event.key.key != SDLK_UP &&
          event.key.key != SDLK_DOWN)
        continue;
      switch (event.key.key) {
      case SDLK_ESCAPE:
        *global_quit = 1;
        break;
      case SDLK_F: {
        SDL_WindowFlags flags = SDL_GetWindowFlags(ctx->window);
        if ((flags & SDL_WINDOW_FULLSCREEN) == SDL_WINDOW_FULLSCREEN)
          SDL_SetWindowFullscreen(ctx->window, false);
        else
          SDL_SetWindowFullscreen(ctx->window, true);
        ctx->last_mouse_move = SDL_GetTicks();
        SDL_SetCursor(SDL_GetDefaultCursor());
        ctx->cursor_hidden = 0;
        break;
      }
      case SDLK_SPACE:
        ctx->paused = !ctx->paused;
        if (ctx->paused)
          ctx->paused_started_wall = get_monotonic_time();
        else
          ctx->playback_start_wall +=
              (get_monotonic_time() - ctx->paused_started_wall);
        if (ctx->audio_dev > 0) {
          if (ctx->paused)
            SDL_PauseAudioDevice(ctx->audio_dev);
          else
            SDL_ResumeAudioDevice(ctx->audio_dev);
        }
        break;
      case SDLK_UP:
        ctx->volume = (ctx->volume + 0.1f > 1.0f) ? 1.0f : ctx->volume + 0.1f;
        g_master_volume = ctx->volume;
        player_apply_volume(ctx);
        ctx->volume_overlay_until = get_monotonic_time() + 1.0;
        if (ctx->paused && ctx->has_last_frame && ctx->texture)
          player_present_frame(ctx);
        break;
      case SDLK_DOWN:
        ctx->volume = (ctx->volume - 0.1f < 0.0f) ? 0.0f : ctx->volume - 0.1f;
        g_master_volume = ctx->volume;
        player_apply_volume(ctx);
        ctx->volume_overlay_until = get_monotonic_time() + 1.0;
        if (ctx->paused && ctx->has_last_frame && ctx->texture)
          player_present_frame(ctx);
        break;
      case SDLK_RIGHT:
      case SDLK_LEFT: {
        double base =
            ctx->seek_requested ? ctx->seek_target : ctx->last_video_time;
        double step =
            (event.key.key == SDLK_RIGHT) ? SEEK_STEP_SEC : -SEEK_STEP_SEC;
        ctx->seek_target = base + step;
        if (ctx->seek_target < 0.0)
          ctx->seek_target = 0.0;
        if (ctx->fmt_ctx && ctx->fmt_ctx->duration != AV_NOPTS_VALUE &&
            ctx->fmt_ctx->duration > 0) {
          double start = (ctx->fmt_ctx->start_time != AV_NOPTS_VALUE)
                             ? (double)ctx->fmt_ctx->start_time / AV_TIME_BASE
                             : 0.0;
          double dur = (double)ctx->fmt_ctx->duration / AV_TIME_BASE;
          if (ctx->seek_target >= start + dur - 0.5) {
            ctx->file_finished = 1;
            break;
          }
        }
        ctx->seek_requested = 1;
        break;
      }
      case SDLK_N:
        if (*playlist_index + 1 < ctx->playlist_count) {
          ctx->nav_action = NAV_NEXT;
          ctx->file_finished = 1;
        }
        break;
      case SDLK_P:
        if (*playlist_index > 0) {
          ctx->nav_action = NAV_PREV;
          ctx->file_finished = 1;
        } else {
          ctx->seek_target = 0.0;
          ctx->seek_requested = 1;
        }
        break;
      case SDLK_V:
        ctx->subtitle_visible = !ctx->subtitle_visible;
        break;
      case SDLK_S:
        player_switch_subtitle(ctx);
        break;
      }
    }
  }
}

void player_run_loop(PlayerContext *ctx, int *global_quit,
                     int *playlist_index) {
  ctx->global_quit_ref = global_quit;
  ctx->playlist_index_ref = playlist_index;
  SDL_FlushEvent(SDL_EVENT_KEY_DOWN);
  SDL_FlushEvent(SDL_EVENT_KEY_UP);

  packet_queue_init(&ctx->v_pkt_q, ctx->packet_queue_cap);
  packet_queue_init(&ctx->a_pkt_q, ctx->packet_queue_cap);
  memset(&ctx->v_frame_q, 0, sizeof(FrameQueue));
  ctx->v_frame_q.capacity = ctx->frame_queue_cap;
  ctx->v_frame_q.mutex = SDL_CreateMutex();
  ctx->v_frame_q.cond = SDL_CreateCondition();

  if (!ctx->v_pkt_q.mutex || !ctx->v_pkt_q.cond || !ctx->a_pkt_q.mutex ||
      !ctx->a_pkt_q.cond || !ctx->v_frame_q.mutex || !ctx->v_frame_q.cond) {
    fprintf(stderr, "[VidP] Failed to create queues!\n");
    goto cleanup;
  }

  atomic_store(&ctx->abort_request, 0);
  atomic_store(&ctx->demux_eof, 0);
  atomic_store(&ctx->v_dec_eof, 0);
  atomic_store(&ctx->a_dec_eof, 0);
  atomic_store(&ctx->audio_generation, 0);
  atomic_store(&ctx->seek_generation, 0);

  ctx->demux_th = SDL_CreateThread(thread_demux, "Demuxer", ctx);
  ctx->v_dec_th = SDL_CreateThread(thread_video_decode, "VideoDec", ctx);
  ctx->a_dec_th = SDL_CreateThread(thread_audio_decode, "AudioDec", ctx);

  if (!ctx->demux_th || !ctx->v_dec_th || !ctx->a_dec_th) {
    fprintf(stderr, "[VidP] Failed to create threads!\n");
    goto cleanup;
  }

  while (!ctx->file_finished && !*global_quit) {
    player_handle_events(ctx, global_quit, playlist_index);

    if (!ctx->cursor_hidden && (SDL_GetTicks() - ctx->last_mouse_move > 2000)) {
      if (ctx->blank_cursor)
        SDL_SetCursor(ctx->blank_cursor);
      ctx->cursor_hidden = 1;
    }
    if (*global_quit || ctx->file_finished)
      break;

    if (ctx->seek_requested) {
      ctx->seek_requested = 0;
      player_do_seek(ctx, ctx->seek_target);
    }

    if (ctx->paused && !ctx->step_pending) {
      if (ctx->has_last_frame && ctx->texture &&
          ctx->volume_overlay_until > 0.0 &&
          get_monotonic_time() >= ctx->volume_overlay_until) {
        player_present_frame(ctx);
        ctx->volume_overlay_until = 0.0;
      }
      SDL_Delay(10);
      continue;
    }

    int got_frame = 0;
    AVFrame *frame = NULL;

    SDL_LockMutex(ctx->v_frame_q.mutex);
    while (ctx->v_frame_q.count == 0 && !atomic_load(&ctx->abort_request) &&
           !atomic_load(&ctx->v_dec_eof) && !ctx->file_finished &&
           !*global_quit) {
      if (!SDL_WaitConditionTimeout(ctx->v_frame_q.cond, ctx->v_frame_q.mutex,
                                    100))
        break;
    }
    if (atomic_load(&ctx->abort_request)) {
      SDL_UnlockMutex(ctx->v_frame_q.mutex);
      break;
    }
    if (ctx->file_finished || *global_quit) {
      SDL_UnlockMutex(ctx->v_frame_q.mutex);
      break;
    }
    if (ctx->v_frame_q.count > 0) {
      frame = ctx->v_frame_q.frames[ctx->v_frame_q.r_idx];
      got_frame = 1;
    }
    SDL_UnlockMutex(ctx->v_frame_q.mutex);

    if (!got_frame) {
      if (atomic_load(&ctx->v_dec_eof)) {
        double drain_start = get_monotonic_time();
        int seek_interrupt = 0;
        while (!*global_quit && !ctx->file_finished) {
          player_handle_events(ctx, global_quit, playlist_index);
          if (*global_quit || ctx->file_finished)
            break;

          if (ctx->seek_requested) {
            seek_interrupt = 1;
            break;
          }

          int audio_drained = !ctx->sdl_stream ||
                              SDL_GetAudioStreamQueued(ctx->sdl_stream) <= 0;
          if (audio_drained && atomic_load(&ctx->a_dec_eof))
            break;
          if (get_monotonic_time() - drain_start > 3.0)
            break;
          SDL_Delay(20);
        }
        if (seek_interrupt)
          continue;
        ctx->file_finished = 1;
        break;
      }
      SDL_Delay(10);
      continue;
    }

    if (frame) {
      int64_t pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
      if (pts == AV_NOPTS_VALUE)
        pts = 0;

      double frame_pts =
          pts * av_q2d(ctx->fmt_ctx->streams[ctx->video_stream]->time_base);
      double audio_clock = player_get_audio_clock(ctx);
      double delay;
      int drop = 0;

      double now = get_monotonic_time();
      if (audio_clock >= 0.0) {
        if (ctx->last_clock_valid_time > 0.0 &&
            now - ctx->last_clock_valid_time > 0.5)
          ctx->video_clock_started = 0;
        ctx->last_clock_valid_time = now;
        delay = frame_pts - audio_clock;
        if (delay < AV_SYNC_DROP_AUDIO)
          drop = 1;
      } else {
        ctx->last_clock_valid_time = 0.0;
        if (!ctx->video_clock_started) {
          ctx->first_video_time = frame_pts;
          ctx->playback_start_wall = get_monotonic_time();
          ctx->video_clock_started = 1;
        }
        double target_time =
            ctx->playback_start_wall + (frame_pts - ctx->first_video_time);
        delay = target_time - get_monotonic_time();
        if (delay < AV_SYNC_DROP_THRESHOLD)
          drop = 1;
        if (delay > AV_SYNC_THRESHOLD_MAX) {
          ctx->playback_start_wall = get_monotonic_time();
          ctx->first_video_time = frame_pts;
          delay = 0.0;
          drop = 0;
        }
      }

      if (drop) {
        SDL_LockMutex(ctx->v_frame_q.mutex);
        av_frame_free(&ctx->v_frame_q.frames[ctx->v_frame_q.r_idx]);
        ctx->v_frame_q.r_idx =
            (ctx->v_frame_q.r_idx + 1) % ctx->frame_queue_cap;
        ctx->v_frame_q.count--;
        SDL_SignalCondition(ctx->v_frame_q.cond);
        SDL_UnlockMutex(ctx->v_frame_q.mutex);
        continue;
      }

      if (delay > 0.0) {
        if (delay > 0.002)
          SDL_Delay(1);
        else
          SDL_DelayNS((Uint64)(delay * 1000000000.0));
        continue;
      }

      ctx->last_video_time = frame_pts;
      player_render_current_frame(ctx, frame);
      ctx->step_pending = 0;

      SDL_LockMutex(ctx->v_frame_q.mutex);
      av_frame_free(&ctx->v_frame_q.frames[ctx->v_frame_q.r_idx]);
      ctx->v_frame_q.r_idx = (ctx->v_frame_q.r_idx + 1) % ctx->frame_queue_cap;
      ctx->v_frame_q.count--;
      SDL_SignalCondition(ctx->v_frame_q.cond);
      SDL_UnlockMutex(ctx->v_frame_q.mutex);
    }
  }

cleanup:
  atomic_store(&ctx->abort_request, 1);
  atomic_store(&ctx->v_pkt_q.abort_request, 1);
  atomic_store(&ctx->a_pkt_q.abort_request, 1);
  atomic_store(&ctx->v_frame_q.abort_request, 1);

  if (ctx->v_pkt_q.cond)
    SDL_BroadcastCondition(ctx->v_pkt_q.cond);
  if (ctx->a_pkt_q.cond)
    SDL_BroadcastCondition(ctx->a_pkt_q.cond);
  if (ctx->v_frame_q.cond)
    SDL_BroadcastCondition(ctx->v_frame_q.cond);

  if (ctx->demux_th) {
    SDL_WaitThread(ctx->demux_th, NULL);
    ctx->demux_th = NULL;
  }
  if (ctx->v_dec_th) {
    SDL_WaitThread(ctx->v_dec_th, NULL);
    ctx->v_dec_th = NULL;
  }
  if (ctx->a_dec_th) {
    SDL_WaitThread(ctx->a_dec_th, NULL);
    ctx->a_dec_th = NULL;
  }

  if (ctx->v_pkt_q.cond) {
    SDL_DestroyCondition(ctx->v_pkt_q.cond);
    ctx->v_pkt_q.cond = NULL;
  }
  if (ctx->v_pkt_q.mutex) {
    SDL_DestroyMutex(ctx->v_pkt_q.mutex);
    ctx->v_pkt_q.mutex = NULL;
  }
  if (ctx->a_pkt_q.cond) {
    SDL_DestroyCondition(ctx->a_pkt_q.cond);
    ctx->a_pkt_q.cond = NULL;
  }
  if (ctx->a_pkt_q.mutex) {
    SDL_DestroyMutex(ctx->a_pkt_q.mutex);
    ctx->a_pkt_q.mutex = NULL;
  }
  if (ctx->v_frame_q.cond) {
    SDL_DestroyCondition(ctx->v_frame_q.cond);
    ctx->v_frame_q.cond = NULL;
  }
  if (ctx->v_frame_q.mutex) {
    SDL_DestroyMutex(ctx->v_frame_q.mutex);
    ctx->v_frame_q.mutex = NULL;
  }
}
