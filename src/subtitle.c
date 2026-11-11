#include "player.h"

#define VIDP_MAX_TRACKED_FONTS 64
static uint64_t g_font_hashes[VIDP_MAX_TRACKED_FONTS];
static int g_font_hash_count = 0;

static ASS_Library *g_ass_lib = NULL;
static ASS_Renderer *g_ass_rend = NULL;

static uint64_t fnv1a_hash(const uint8_t *data, int size) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < size; i++) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

static int font_hash_seen(uint64_t h) {
  for (int i = 0; i < g_font_hash_count; i++)
    if (g_font_hashes[i] == h)
      return 1;
  return 0;
}

static inline uint8_t clamp8(int v) { return v > 255 ? 255 : (uint8_t)v; }

static inline void blend_ass_pixel(uint32_t *pixel, uint8_t r, uint8_t g,
                                   uint8_t b, uint8_t final_a) {
  uint32_t px = *pixel;
  uint8_t ex_a = (px >> 24) & 0xFF;
  if (final_a == 255 || ex_a == 0) {
    if (final_a == 0)
      return;
    *pixel = ((uint32_t)final_a << 24) | ((uint32_t)b << 16) |
             ((uint32_t)g << 8) | r;
    return;
  }
  uint8_t ex_r = px & 0xFF;
  uint8_t ex_g = (px >> 8) & 0xFF;
  uint8_t ex_b = (px >> 16) & 0xFF;
  uint8_t out_a = final_a + ex_a * (255 - final_a) / 255;
  if (out_a > 0) {
    uint8_t out_r =
        clamp8((r * final_a + ex_r * ex_a * (255 - final_a) / 255) / out_a);
    uint8_t out_g =
        clamp8((g * final_a + ex_g * ex_a * (255 - final_a) / 255) / out_a);
    uint8_t out_b =
        clamp8((b * final_a + ex_b * ex_a * (255 - final_a) / 255) / out_a);
    *pixel = ((uint32_t)out_a << 24) | ((uint32_t)out_b << 16) |
             ((uint32_t)out_g << 8) | out_r;
  }
}

void player_update_ass_storage(PlayerContext *ctx) {
  if (!ctx->ass_renderer)
    return;
  int w = ctx->width, h = ctx->height;
  if (ctx->rotation == 90 || ctx->rotation == 270) {
    int t = w;
    w = h;
    h = t;
  }
  ass_set_storage_size(ctx->ass_renderer, w, h);
}

static void overlay_draw(PlayerContext *ctx, const SDL_FRect *dest_rect) {
  SDL_FRect src = {0.0f, 0.0f, (float)ctx->sub_w, (float)ctx->sub_h};
  SDL_FRect dst = {dest_rect->x + (float)ctx->sub_x,
                   dest_rect->y + (float)ctx->sub_y, (float)ctx->sub_w,
                   (float)ctx->sub_h};
  SDL_RenderTexture(ctx->renderer, ctx->sub_tex, &src, &dst);
}

void render_ass_overlay(PlayerContext *ctx, ASS_Image *img,
                        SDL_FRect *dest_rect, int changed) {
  if (!img || dest_rect->w <= 0 || dest_rect->h <= 0)
    return;
  int frame_w = (int)dest_rect->w, frame_h = (int)dest_rect->h;
  if (frame_w < 1 || frame_h < 1)
    return;
  int rebuild = changed || ctx->sub_frame_w != frame_w ||
                ctx->sub_frame_h != frame_h ||
                (ctx->sub_has_pixels && !ctx->sub_tex);
  if (!rebuild) {
    if (ctx->sub_has_pixels && ctx->sub_tex)
      overlay_draw(ctx, dest_rect);
    return;
  }
  int x0 = frame_w, y0 = frame_h, x1 = 0, y1 = 0;
  for (ASS_Image *c = img; c; c = c->next) {
    if (c->w <= 0 || c->h <= 0 || (255 - (c->color & 0xFF)) == 0)
      continue;
    int ax0 = c->dst_x < 0 ? 0 : c->dst_x;
    int ay0 = c->dst_y < 0 ? 0 : c->dst_y;
    int ax1 = c->dst_x + c->w > frame_w ? frame_w : c->dst_x + c->w;
    int ay1 = c->dst_y + c->h > frame_h ? frame_h : c->dst_y + c->h;
    if (ax0 >= ax1 || ay0 >= ay1)
      continue;
    if (ax0 < x0)
      x0 = ax0;
    if (ay0 < y0)
      y0 = ay0;
    if (ax1 > x1)
      x1 = ax1;
    if (ay1 > y1)
      y1 = ay1;
  }
  ctx->sub_frame_w = frame_w;
  ctx->sub_frame_h = frame_h;
  if (x1 <= x0 || y1 <= y0) {
    ctx->sub_has_pixels = 0;
    return;
  }
  int bw = x1 - x0, bh = y1 - y0;
  if (!ctx->sub_tex || ctx->sub_tex_w < bw || ctx->sub_tex_h < bh) {
    int cw = (bw + 63) & ~63, ch = (bh + 63) & ~63;
    if (cw > frame_w)
      cw = frame_w;
    if (ch > frame_h)
      ch = frame_h;
    if (cw < ctx->sub_tex_w)
      cw = ctx->sub_tex_w;
    if (ch < ctx->sub_tex_h)
      ch = ctx->sub_tex_h;
    if (ctx->sub_tex) {
      SDL_DestroyTexture(ctx->sub_tex);
      ctx->sub_tex = NULL;
    }
    ctx->sub_tex = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STREAMING, cw, ch);
    if (!ctx->sub_tex) {
      ctx->sub_has_pixels = 0;
      ctx->sub_tex_w = ctx->sub_tex_h = 0;
      return;
    }
    SDL_SetTextureBlendMode(ctx->sub_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(ctx->sub_tex, SDL_SCALEMODE_NEAREST);
    ctx->sub_tex_w = cw;
    ctx->sub_tex_h = ch;
  }
  SDL_Rect lock = {0, 0, bw, bh};
  void *pix = NULL;
  int pitch = 0;
  if (!SDL_LockTexture(ctx->sub_tex, &lock, &pix, &pitch)) {
    ctx->sub_has_pixels = 0;
    return;
  }
  for (int y = 0; y < bh; y++)
    memset((uint8_t *)pix + (size_t)y * pitch, 0, (size_t)bw * 4);
  for (ASS_Image *c = img; c; c = c->next) {
    if (c->w <= 0 || c->h <= 0)
      continue;
    uint8_t a = 255 - (c->color & 0xFF);
    if (a == 0)
      continue;
    uint8_t r = (c->color >> 24) & 0xFF;
    uint8_t g = (c->color >> 16) & 0xFF;
    uint8_t b = (c->color >> 8) & 0xFF;
    int dx = c->dst_x, dy = c->dst_y;
    int ys = dy < y0 ? y0 - dy : 0;
    int ye = dy + c->h > y1 ? y1 - dy : c->h;
    int xs = dx < x0 ? x0 - dx : 0;
    int xe = dx + c->w > x1 ? x1 - dx : c->w;
    for (int y = ys; y < ye; y++) {
      const uint8_t *src = c->bitmap + (size_t)y * c->stride;
      uint32_t *row =
          (uint32_t *)((uint8_t *)pix + (size_t)(dy + y - y0) * pitch);
      for (int x = xs; x < xe; x++) {
        uint8_t alpha = src[x];
        if (alpha == 0)
          continue;
        blend_ass_pixel(&row[dx + x - x0], r, g, b,
                        (uint8_t)((alpha * a) / 255));
      }
    }
  }
  SDL_UnlockTexture(ctx->sub_tex);
  ctx->sub_x = x0;
  ctx->sub_y = y0;
  ctx->sub_w = bw;
  ctx->sub_h = bh;
  ctx->sub_has_pixels = 1;
  overlay_draw(ctx, dest_rect);
}

static int player_extract_attached_fonts(PlayerContext *ctx) {
  if (!ctx->fmt_ctx || !ctx->ass_library)
    return 0;
  int added = 0;
  for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
    AVStream *st = ctx->fmt_ctx->streams[i];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT)
      continue;
    AVDictionaryEntry *tag = av_dict_get(st->metadata, "filename", NULL, 0);
    if (!tag || !tag->value)
      continue;
    if (!st->codecpar->extradata || st->codecpar->extradata_size <= 0)
      continue;

    uint64_t h =
        fnv1a_hash(st->codecpar->extradata, st->codecpar->extradata_size);
    if (!font_hash_seen(h) && g_font_hash_count < VIDP_MAX_TRACKED_FONTS) {
      ass_add_font(ctx->ass_library, (char *)tag->value,
                   (char *)st->codecpar->extradata,
                   st->codecpar->extradata_size);
      g_font_hashes[g_font_hash_count++] = h;
      added = 1;
    }
    av_freep(&st->codecpar->extradata);
    st->codecpar->extradata_size = 0;
  }
  return added;
}

void player_init_subtitles(PlayerContext *ctx) {
  if (ctx->subtitle_stream_count <= 0)
    return;

  if (!g_ass_lib) {
    g_ass_lib = ass_library_init();
    if (!g_ass_lib) {
      fprintf(stderr, "[VidP] ass_library_init failed, subtitles disabled\n");
      return;
    }
  }
  ctx->ass_library = g_ass_lib;

  int new_font_added = player_extract_attached_fonts(ctx);

  if (!g_ass_rend) {
    g_ass_rend = ass_renderer_init(g_ass_lib);
    if (!g_ass_rend) {
      fprintf(stderr, "[VidP] ass_renderer_init failed, subtitles disabled\n");
      return;
    }
    ass_set_hinting(g_ass_rend, ASS_HINTING_NONE);
    ass_set_shaper(g_ass_rend, ASS_SHAPING_COMPLEX);
    ass_set_cache_limits(g_ass_rend, 500, 32);
    ass_set_fonts(g_ass_rend, NULL, "Sans", ASS_FONTPROVIDER_AUTODETECT, NULL,
                  1);
  } else if (new_font_added) {
    ass_set_fonts(g_ass_rend, NULL, "Sans", ASS_FONTPROVIDER_AUTODETECT, NULL,
                  1);
  }
  ctx->ass_renderer = g_ass_rend;

  player_update_ass_storage(ctx);

  for (int i = 0; i < ctx->subtitle_stream_count; i++) {
    ctx->ass_tracks[i] = ass_new_track(g_ass_lib);
    if (!ctx->ass_tracks[i])
      continue;
    int sidx = ctx->subtitle_streams[i];
    AVCodecParameters *sub_par = ctx->fmt_ctx->streams[sidx]->codecpar;
    if (sub_par->extradata && sub_par->extradata_size > 0) {
      ass_process_codec_private(ctx->ass_tracks[i], (char *)sub_par->extradata,
                                sub_par->extradata_size);
    }
  }

  if (ctx->current_subtitle_idx >= 0 &&
      ctx->current_subtitle_idx < ctx->subtitle_stream_count) {
    ctx->ass_track = ctx->ass_tracks[ctx->current_subtitle_idx];
  }
}

void player_shutdown_subtitles_global(void) {
  if (g_ass_rend) {
    ass_renderer_done(g_ass_rend);
    g_ass_rend = NULL;
  }
  if (g_ass_lib) {
    ass_library_done(g_ass_lib);
    g_ass_lib = NULL;
  }
  g_font_hash_count = 0;
}

void player_process_subtitle_packet(PlayerContext *ctx) {
  if (!ctx->ass_library || ctx->subtitle_stream_count <= 0)
    return;
  int pkt_stream = ctx->packet->stream_index;
  int sub_idx = -1;
  for (int i = 0; i < ctx->subtitle_stream_count; i++) {
    if (ctx->subtitle_streams[i] == pkt_stream) {
      sub_idx = i;
      break;
    }
  }
  if (sub_idx < 0 || !ctx->ass_tracks[sub_idx])
    return;
  AVCodecParameters *par = ctx->fmt_ctx->streams[pkt_stream]->codecpar;
  if (par->codec_id != AV_CODEC_ID_ASS && par->codec_id != AV_CODEC_ID_SSA)
    return;
  int64_t pts = (ctx->packet->pts != AV_NOPTS_VALUE) ? ctx->packet->pts
                                                     : ctx->packet->dts;
  if (pts == AV_NOPTS_VALUE)
    return;
  double sub_pts = pts * av_q2d(ctx->fmt_ctx->streams[pkt_stream]->time_base);
  double duration_sub = ctx->packet->duration *
                        av_q2d(ctx->fmt_ctx->streams[pkt_stream]->time_base);
  int64_t start_ms = (int64_t)(sub_pts * 1000);
  int64_t dur_ms = (int64_t)(duration_sub * 1000);
  if (dur_ms <= 0)
    dur_ms = 2000;
  if (ctx->last_video_time >= 0.0) {
    double sub_end = sub_pts + duration_sub;
    if (sub_end < ctx->last_video_time - 1.0)
      return;
  }
  int size = ctx->packet->size;
  if (size + 1 > ctx->sub_tmp_cap) {
    int new_cap = size + 256;
    char *nt = realloc(ctx->sub_tmp, new_cap);
    if (!nt)
      return;
    ctx->sub_tmp = nt;
    ctx->sub_tmp_cap = new_cap;
  }
  memcpy(ctx->sub_tmp, ctx->packet->data, size);
  ctx->sub_tmp[size] = '\0';
  ass_process_chunk(ctx->ass_tracks[sub_idx], ctx->sub_tmp, size, start_ms,
                    dur_ms);
}

void player_reset_subtitle_state(PlayerContext *ctx) {
  if (ctx->sub_tex) {
    SDL_DestroyTexture(ctx->sub_tex);
    ctx->sub_tex = NULL;
  }
  ctx->sub_tex_w = ctx->sub_tex_h = 0;
  ctx->sub_frame_w = ctx->sub_frame_h = 0;
  ctx->sub_has_pixels = 0;
  player_update_ass_storage(ctx);
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
    ctx->ass_track = NULL;
    printf("[VidP] Subtitle: OFF (Disabled)\n");
  } else {
    ctx->ass_track = ctx->ass_tracks[ctx->current_subtitle_idx];
    printf(
        "[VidP] Switched to Subtitle stream index: %d (Total available: %d)\n",
        ctx->current_subtitle_idx + 1, ctx->subtitle_stream_count);
  }
}
