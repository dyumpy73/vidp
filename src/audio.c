#include "player.h"

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

void player_reset_audio_buffer(AudioBuffer *audio_buf) {
  SDL_LockMutex(audio_buf->lock);
  audio_buf->read_pos = 0;
  audio_buf->write_pos = 0;
  audio_buf->size = 0;
  SDL_UnlockMutex(audio_buf->lock);
}

int player_init_audio(PlayerContext *ctx) {
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

void player_receive_audio_frames(PlayerContext *ctx) {
  while (avcodec_receive_frame(ctx->a_codec_ctx, ctx->frame_audio) == 0) {
    if (ctx->audio_discard_until >= 0.0) {
      int64_t apts = ctx->frame_audio->best_effort_timestamp;
      if (apts != AV_NOPTS_VALUE) {
        double a_time = apts * av_q2d(ctx->fmt_ctx->streams[ctx->audio_stream]->time_base);
        if (a_time < ctx->audio_discard_until - 0.2) {
          av_frame_unref(ctx->frame_audio);
          continue;
        }
        ctx->audio_discard_until = -1.0;
      } else {
        ctx->audio_discard_until = -1.0;
      }
    }

    int out_samples = av_rescale_rnd(swr_get_delay(ctx->swr_ctx, ctx->a_codec_ctx->sample_rate) + ctx->frame_audio->nb_samples, ctx->audio_spec.freq, ctx->a_codec_ctx->sample_rate, AV_ROUND_UP);
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
