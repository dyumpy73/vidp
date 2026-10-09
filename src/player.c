#include "player.h"

void player_do_seek(PlayerContext *ctx, double target_sec) {
  if (target_sec < 0.0) target_sec = 0.0;

  double seek_from_sec = target_sec - SUBTITLE_PREROLL_SEC;
  if (seek_from_sec < 0.0) seek_from_sec = 0.0;

  int stream_idx = -1;
  int64_t seek_target = (int64_t)(seek_from_sec * AV_TIME_BASE);

  if (ctx->video_stream >= 0) {
    stream_idx = ctx->video_stream;
    AVRational tb = ctx->fmt_ctx->streams[stream_idx]->time_base;
    seek_target = av_rescale_q((int64_t)(seek_from_sec * AV_TIME_BASE),
                               AV_TIME_BASE_Q, tb);
  }

  if (av_seek_frame(ctx->fmt_ctx, stream_idx, seek_target,
                    AVSEEK_FLAG_BACKWARD) < 0) {
    fprintf(stderr, "[VidP] av_seek_frame failed\n");
    return;
  }

  if (ctx->v_codec_ctx) avcodec_flush_buffers(ctx->v_codec_ctx);
  if (ctx->a_codec_ctx) avcodec_flush_buffers(ctx->a_codec_ctx);

  for (int i = 0; i < ctx->subtitle_stream_count; i++) {
    if (ctx->ass_tracks[i]) ass_flush_events(ctx->ass_tracks[i]);
  }

  player_reset_audio_buffer(&ctx->audio_buf);
  if (ctx->swr_ctx) swr_init(ctx->swr_ctx);

  ctx->video_clock_started = 0;
  ctx->first_video_time    = 0.0;
  ctx->playback_start_wall = 0.0;
  ctx->last_video_time     = target_sec;
  ctx->discard_until       = target_sec;
  ctx->audio_discard_until = target_sec;
}

int player_open_file(PlayerContext *ctx, const char *filepath, int index, int total) {
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
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ctx->video_stream < 0)
      ctx->video_stream = i;
    if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_stream < 0)
      ctx->audio_stream = i;
    if (codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      if (codecpar->codec_id == AV_CODEC_ID_ASS ||
          codecpar->codec_id == AV_CODEC_ID_SSA) {
        if (ctx->subtitle_stream_count < MAX_SUBTITLE_STREAM) {
          ctx->subtitle_streams[ctx->subtitle_stream_count++] = i;
        } else {
          fprintf(stderr, "[VidP] Warning: subtitle stream limit reached, skipping #%u\n", i);
        }
      } else {
        fprintf(stderr, "[VidP] Skipping unsupported subtitle codec: %s\n",
                avcodec_get_name(codecpar->codec_id));
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
    if (ctx->is_hw_accel) break;
  }

  if (avcodec_open2(ctx->v_codec_ctx, v_codec, NULL) < 0) {
    if (!ctx->is_hw_accel) return 0;
    printf("[VidP] HW accel init failed, rebuilding fresh SW context...\n");

    avcodec_free_context(&ctx->v_codec_ctx);
    if (ctx->hw_device_ctx) {
      av_buffer_unref(&ctx->hw_device_ctx);
      ctx->hw_device_ctx = NULL;
    }
    ctx->is_hw_accel = 0;
    ctx->hw_pix_fmt  = AV_PIX_FMT_NONE;

    ctx->v_codec_ctx = avcodec_alloc_context3(v_codec);
    if (!ctx->v_codec_ctx) return 0;
    if (avcodec_parameters_to_context(ctx->v_codec_ctx, v_par) < 0) return 0;

    ctx->v_codec_ctx->thread_count = 2;
    ctx->v_codec_ctx->thread_type  = FF_THREAD_FRAME;
    ctx->v_codec_ctx->opaque       = ctx;

    if (avcodec_open2(ctx->v_codec_ctx, v_codec, NULL) < 0) return 0;
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
      if (sd->type == AV_PKT_DATA_DISPLAYMATRIX &&
          sd->size >= 9 * sizeof(int32_t)) {
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

void player_close_file(PlayerContext *ctx) {
  if (ctx->audio_dev > 0) {
    SDL_PauseAudioDevice(ctx->audio_dev, 1);
  }

  if (ctx->blank_cursor) {
    SDL_FreeCursor(ctx->blank_cursor);
    ctx->blank_cursor = NULL;
  }

  if (ctx->sws_ctx) {
    sws_freeContext(ctx->sws_ctx);
    ctx->sws_ctx = NULL;
  }
  if (ctx->frame_yuv) av_frame_free(&ctx->frame_yuv);

  for (int i = 0; i < MAX_SUBTITLE_STREAM; i++) {
    if (ctx->ass_tracks[i]) {
      ass_free_track(ctx->ass_tracks[i]);
      ctx->ass_tracks[i] = NULL;
    }
  }

  ctx->ass_track = NULL;
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

  free(ctx->sub_tmp);
  ctx->sub_tmp = NULL;
  ctx->sub_tmp_cap = 0;

  if (ctx->audio_dev > 0) {
    SDL_CloseAudioDevice(ctx->audio_dev);
    ctx->audio_dev = 0;
  }
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

void player_sleep_pumping(PlayerContext *ctx, double seconds) {
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

void player_present_frame(PlayerContext *ctx) {
  if (!ctx->texture) return;

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
    ASS_Image *sub_img = ass_render_frame(ctx->ass_renderer, ctx->ass_track,
                                          now_ms, &changed);
    if (sub_img) render_ass_overlay(ctx, sub_img, &dest_rect);
  }

  SDL_RenderPresent(ctx->renderer);
  ctx->last_dest_rect = dest_rect;
  ctx->has_last_frame = 1;
}

void player_handle_events(PlayerContext *ctx, int *global_quit, int *playlist_index) {
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
      if (event.window.event == SDL_WINDOWEVENT_ENTER || event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event.window.event == SDL_WINDOWEVENT_RESIZED || event.window.event == SDL_WINDOWEVENT_EXPOSED) {
        ctx->last_mouse_move = SDL_GetTicks();
        if (ctx->cursor_hidden) {
          SDL_SetCursor(SDL_GetDefaultCursor());
          ctx->cursor_hidden = 0;
        }

        if (ctx->paused && ctx->has_last_frame && ctx->texture) {
          player_present_frame(ctx);
        }
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

void player_run_loop(PlayerContext *ctx, int *global_quit, int *playlist_index) {
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

    int read_ret = av_read_frame(ctx->fmt_ctx, ctx->packet);
    if (read_ret < 0) {
      if (ctx->v_codec_ctx) {
        avcodec_send_packet(ctx->v_codec_ctx, NULL);
        player_receive_video_frames(ctx);
      }
      if (ctx->a_codec_ctx) {
        avcodec_send_packet(ctx->a_codec_ctx, NULL);
        player_receive_audio_frames(ctx);
      }

      double drain_start = get_monotonic_time();
      while (!*global_quit) {
        SDL_LockMutex(ctx->audio_buf.lock);
        size_t remaining = ctx->audio_buf.size;
        SDL_UnlockMutex(ctx->audio_buf.lock);

        if (remaining == 0) break;
        if (get_monotonic_time() - drain_start > 3.0) break;
        if (ctx->paused) break;

        player_handle_events(ctx, global_quit, playlist_index);
        SDL_Delay(20);
      }

      ctx->file_finished = 1;
      break;
    }

    if (ctx->packet->stream_index == ctx->video_stream) {
      player_process_video_packet(ctx);
    } else if (ctx->packet->stream_index == ctx->audio_stream) {
      player_process_audio_packet(ctx);
    } else {
      for (int i = 0; i < ctx->subtitle_stream_count; i++) {
        if (ctx->packet->stream_index == ctx->subtitle_streams[i]) {
          player_process_subtitle_packet(ctx);
          break;
        }
      }
    }

    av_packet_unref(ctx->packet);
  }
}

