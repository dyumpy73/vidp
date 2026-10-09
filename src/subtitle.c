#include "player.h"

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

void render_ass_overlay(PlayerContext *ctx, ASS_Image *img, SDL_Rect *dest_rect) {
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
  }

  if (!ctx->sub_tex) {
    ctx->sub_tex = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STREAMING, target_w, target_h);
    if (!ctx->sub_tex) return;
    SDL_SetTextureBlendMode(ctx->sub_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(ctx->sub_tex, SDL_ScaleModeLinear);
  }

  SDL_FillRect(ctx->sub_surf, NULL, 0);

  uint32_t *pixels = (uint32_t *)ctx->sub_surf->pixels;
  int surf_w = ctx->sub_surf->w;
  int has_pixels = 0;

  ASS_Image *curr = img;
  while (curr) {
    if (curr->type != 3 && curr->w > 0 && curr->h > 0) {
      uint8_t r = (curr->color >> 24) & 0xFF;
      uint8_t g = (curr->color >> 16) & 0xFF;
      uint8_t b = (curr->color >> 8) & 0xFF;
      uint8_t a = 255 - (curr->color & 0xFF);

      if (a > 0) {
        has_pixels = 1;
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
    }
    curr = curr->next;
  }

  SDL_UpdateTexture(ctx->sub_tex, NULL, ctx->sub_surf->pixels, ctx->sub_surf->pitch);
  if (has_pixels) {
    SDL_RenderCopy(ctx->renderer, ctx->sub_tex, NULL, dest_rect);
  }
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
          ass_add_font(ctx->ass_library, (char *)filename,
                       (char *)st->codecpar->extradata,
                       st->codecpar->extradata_size);
        }
      }
    }
  }
}

void player_init_subtitles(PlayerContext *ctx) {
  if (ctx->subtitle_stream_count <= 0) return;

  ctx->ass_library = ass_library_init();
  if (!ctx->ass_library) {
    fprintf(stderr, "[VidP] ass_library_init failed, subtitles disabled\n");
    return;
  }

  player_extract_attached_fonts(ctx);

  ctx->ass_renderer = ass_renderer_init(ctx->ass_library);
  if (!ctx->ass_renderer) {
    fprintf(stderr, "[VidP] ass_renderer_init failed, subtitles disabled\n");
    return;
  }

  ass_set_storage_size(ctx->ass_renderer, ctx->width, ctx->height);
  ass_set_hinting(ctx->ass_renderer, ASS_HINTING_NONE);
  ass_set_shaper(ctx->ass_renderer, ASS_SHAPING_COMPLEX);
  ass_set_fonts(ctx->ass_renderer, NULL, "Sans",
                ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
  ass_set_cache_limits(ctx->ass_renderer, 20, 20);

  for (int i = 0; i < ctx->subtitle_stream_count; i++) {
    ctx->ass_tracks[i] = ass_new_track(ctx->ass_library);
    if (!ctx->ass_tracks[i]) continue;

    int sidx = ctx->subtitle_streams[i];
    AVCodecParameters *sub_par = ctx->fmt_ctx->streams[sidx]->codecpar;
    if (sub_par->extradata && sub_par->extradata_size > 0) {
      ass_process_codec_private(ctx->ass_tracks[i],
                                (char *)sub_par->extradata,
                                sub_par->extradata_size);
    }
  }

  if (ctx->current_subtitle_idx >= 0 &&
      ctx->current_subtitle_idx < ctx->subtitle_stream_count) {
    ctx->ass_track = ctx->ass_tracks[ctx->current_subtitle_idx];
  }
}

void player_process_subtitle_packet(PlayerContext *ctx) {
  if (!ctx->ass_library || ctx->subtitle_stream_count <= 0) return;

  int pkt_stream = ctx->packet->stream_index;

  int sub_idx = -1;
  for (int i = 0; i < ctx->subtitle_stream_count; i++) {
    if (ctx->subtitle_streams[i] == pkt_stream) {
      sub_idx = i;
      break;
    }
  }
  if (sub_idx < 0) return;
  if (!ctx->ass_tracks[sub_idx]) return;

  AVCodecParameters *par = ctx->fmt_ctx->streams[pkt_stream]->codecpar;
  if (par->codec_id != AV_CODEC_ID_ASS && par->codec_id != AV_CODEC_ID_SSA) return;

  int64_t pts = (ctx->packet->pts != AV_NOPTS_VALUE) ? ctx->packet->pts : ctx->packet->dts;
  if (pts == AV_NOPTS_VALUE) return;

  double sub_pts = pts * av_q2d(ctx->fmt_ctx->streams[pkt_stream]->time_base);
  double duration_sub = ctx->packet->duration * av_q2d(ctx->fmt_ctx->streams[pkt_stream]->time_base);
  int64_t start_ms = (int64_t)(sub_pts * 1000);
  int64_t dur_ms   = (int64_t)(duration_sub * 1000);
  if (dur_ms <= 0) dur_ms = 5000;

  int size = ctx->packet->size;
  if (size + 1 > ctx->sub_tmp_cap) {
    int new_cap = size + 256;
    char *nt = realloc(ctx->sub_tmp, new_cap);
    if (!nt) return;
    ctx->sub_tmp = nt;
    ctx->sub_tmp_cap = new_cap;
  }

  memcpy(ctx->sub_tmp, ctx->packet->data, size);
  ctx->sub_tmp[size] = '\0';
  ass_process_chunk(ctx->ass_tracks[sub_idx], ctx->sub_tmp, size, start_ms, dur_ms);
}

void player_reset_subtitle_state(PlayerContext *ctx) {
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

void player_switch_subtitle(PlayerContext *ctx) {
  if (ctx->subtitle_stream_count <= 0 || !ctx->ass_library) {
    printf("[VidP] No subtitles available in this file.\n");
    return;
  }

  player_reset_subtitle_state(ctx);

  ctx->current_subtitle_idx++;
  if (ctx->current_subtitle_idx >= ctx->subtitle_stream_count) {
    ctx->current_subtitle_idx = -1;
    ctx->subtitle_stream = -1;
    ctx->ass_track = NULL;
    printf("[VidP] Subtitle: OFF (Disabled)\n");
  } else {
    ctx->subtitle_stream = ctx->subtitle_streams[ctx->current_subtitle_idx];
    ctx->ass_track = ctx->ass_tracks[ctx->current_subtitle_idx];
    printf("[VidP] Switched to Subtitle stream index: %d (Total available: %d)\n", ctx->current_subtitle_idx + 1, ctx->subtitle_stream_count);
  }
}

