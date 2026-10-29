#include "player.h"

static void push_audio_data(PlayerContext *ctx, const uint8_t *data, size_t len, int frame_size, double start_pts) {
  (void)start_pts;
  if (frame_size <= 0) frame_size = 4;
  len = (len / frame_size) * frame_size;
  if (len == 0) return;
  SDL_PutAudioStreamData(ctx->sdl_stream, data, (int)len);
}

void player_reset_audio_buffer(PlayerContext *ctx) {
  if (ctx->sdl_stream) {
    SDL_ClearAudioStream(ctx->sdl_stream);
  }
}

double player_get_audio_clock(PlayerContext *ctx) {
  if (ctx->audio_dev == 0 || !ctx->sdl_stream || ctx->last_audio_pts < 0.0) return -1.0;
  int queued = SDL_GetAudioStreamQueued(ctx->sdl_stream);

  if (queued <= 0 && !ctx->paused) {
    double now = get_monotonic_time();
    if (ctx->audio_starve_since == 0.0) ctx->audio_starve_since = now;
    if (now - ctx->audio_starve_since > 0.25) return -1.0;
  } else {
    ctx->audio_starve_since = 0.0;
  }

  if (ctx->bytes_per_sec > 0) {
    double delay = (double)queued / ctx->bytes_per_sec;
    delay += ctx->audio_hw_delay;

    return ctx->last_audio_pts - delay;
  }
  return ctx->last_audio_pts;
}

int player_init_audio(PlayerContext *ctx) {
  if (ctx->audio_stream < 0) return 0;

  AVStream *st = ctx->fmt_ctx->streams[ctx->audio_stream];
  AVCodecParameters *par = st->codecpar;
  const AVCodec *codec = avcodec_find_decoder(par->codec_id);
  if (!codec) return 0;

  ctx->a_codec_ctx = avcodec_alloc_context3(codec);
  if (!ctx->a_codec_ctx) return 0;
  if (avcodec_parameters_to_context(ctx->a_codec_ctx, par) < 0) goto fail;
  if (avcodec_open2(ctx->a_codec_ctx, codec, NULL) < 0) goto fail;

  int input_channels = ctx->a_codec_ctx->ch_layout.nb_channels;
  if (input_channels <= 0) input_channels = 2;

  SDL_AudioSpec wanted_spec;
  SDL_zero(wanted_spec);
  wanted_spec.freq = ctx->a_codec_ctx->sample_rate;
  wanted_spec.format = SDL_AUDIO_S16;
  wanted_spec.channels = input_channels > 2 ? 2 : input_channels;
  ctx->out_freq = wanted_spec.freq;

  ctx->audio_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &wanted_spec);
  if (ctx->audio_dev == 0) goto fail;

  int hw_sample_frames = 1024;
  SDL_GetAudioDeviceFormat(ctx->audio_dev, &ctx->audio_spec, &hw_sample_frames);

  if (ctx->audio_spec.freq > 0) {
    ctx->audio_hw_delay = (double)hw_sample_frames / (double)ctx->audio_spec.freq;
  } else {
    ctx->audio_hw_delay = 0.0;
  }

  ctx->sdl_stream = SDL_CreateAudioStream(&wanted_spec, NULL);
  if (!ctx->sdl_stream) goto fail;

  if (!SDL_BindAudioStream(ctx->audio_dev, ctx->sdl_stream)) goto fail;
  if (!SDL_ResumeAudioDevice(ctx->audio_dev)) goto fail;

  SDL_SetAudioStreamGain(ctx->sdl_stream, ctx->volume);

  ctx->output_channels = wanted_spec.channels;
  ctx->bytes_per_sec = wanted_spec.freq * ctx->output_channels * (int)sizeof(int16_t);

  AVChannelLayout out_ch_layout;
  av_channel_layout_default(&out_ch_layout, ctx->output_channels);

  AVChannelLayout in_layout = {0};
  av_channel_layout_copy(&in_layout, &ctx->a_codec_ctx->ch_layout);
  if (in_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in_layout, in_layout.nb_channels);

  int swr_ret = swr_alloc_set_opts2(&ctx->swr_ctx, &out_ch_layout, AV_SAMPLE_FMT_S16,
                                    wanted_spec.freq,
                                    &in_layout, ctx->a_codec_ctx->sample_fmt,
                                    ctx->a_codec_ctx->sample_rate, 0, NULL);
  av_channel_layout_uninit(&out_ch_layout);
  av_channel_layout_uninit(&in_layout);

  if (swr_ret < 0 || !ctx->swr_ctx || swr_init(ctx->swr_ctx) < 0) goto fail;

  return 1;

fail:
  if (ctx->sdl_stream) { SDL_DestroyAudioStream(ctx->sdl_stream); ctx->sdl_stream = NULL; }
  if (ctx->audio_dev > 0) { SDL_CloseAudioDevice(ctx->audio_dev); ctx->audio_dev = 0; }
  if (ctx->swr_ctx) swr_free(&ctx->swr_ctx);
  if (ctx->a_codec_ctx) avcodec_free_context(&ctx->a_codec_ctx);
  ctx->bytes_per_sec = 0;
  ctx->output_channels = 0;
  return 0;
}

void player_receive_audio_frames(PlayerContext *ctx) {
  while (avcodec_receive_frame(ctx->a_codec_ctx, ctx->frame_audio) == 0) {
    double frame_pts = -1.0;
    int64_t apts = ctx->frame_audio->best_effort_timestamp;
    if (apts != AV_NOPTS_VALUE) {
      frame_pts = apts * av_q2d(ctx->fmt_ctx->streams[ctx->audio_stream]->time_base);
    }

    if (ctx->audio_discard_until >= 0.0) {
      if (apts != AV_NOPTS_VALUE) {
        if (frame_pts < ctx->audio_discard_until - 0.2) {
          av_frame_unref(ctx->frame_audio);
          continue;
        }
        ctx->audio_discard_until = -1.0;
      } else {
        ctx->audio_discard_until = -1.0;
      }
    }

    int out_samples = av_rescale_rnd(swr_get_delay(ctx->swr_ctx, ctx->a_codec_ctx->sample_rate) + ctx->frame_audio->nb_samples, ctx->out_freq, ctx->a_codec_ctx->sample_rate, AV_ROUND_UP);
    if (out_samples <= 0) {
      av_frame_unref(ctx->frame_audio);
      continue;
    }

    int req_buf_size = av_samples_get_buffer_size(NULL, ctx->output_channels, out_samples, AV_SAMPLE_FMT_S16, 1);
    if (req_buf_size > 0) {
      av_fast_malloc(&ctx->audio_out_buffer, &ctx->audio_out_buffer_size, req_buf_size);
      if (ctx->audio_out_buffer) {
        int len = swr_convert(ctx->swr_ctx, &ctx->audio_out_buffer, out_samples, (const uint8_t **)ctx->frame_audio->extended_data, ctx->frame_audio->nb_samples);
        if (len > 0) {
          int real_size = av_samples_get_buffer_size(NULL, ctx->output_channels, len, AV_SAMPLE_FMT_S16, 1);
          if (real_size > 0) {
            int sample_frame_size = ctx->output_channels * (int)sizeof(int16_t);
            push_audio_data(ctx, ctx->audio_out_buffer, real_size, sample_frame_size, frame_pts);
            if (frame_pts >= 0.0 && ctx->bytes_per_sec > 0) {
              ctx->last_audio_pts = frame_pts + (double)real_size / ctx->bytes_per_sec;
            }
          }
        }
      }
    }
    av_frame_unref(ctx->frame_audio);
  }
}

void player_process_audio_packet(PlayerContext *ctx) {
  if (!ctx->a_codec_ctx || ctx->audio_dev <= 0 || !ctx->swr_ctx) return;

  int ret = avcodec_send_packet(ctx->a_codec_ctx, ctx->packet);
  if (ret == AVERROR(EAGAIN)) {
    player_receive_audio_frames(ctx);
    ret = avcodec_send_packet(ctx->a_codec_ctx, ctx->packet);
  }
  if (ret < 0 && ret != AVERROR_EOF) return;
  player_receive_audio_frames(ctx);
}
