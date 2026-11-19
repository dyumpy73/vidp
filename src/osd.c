#include "player.h"

#define OSD_SHOW_DURATION 2.5
#define OSD_PAD 16
#define OSD_PANEL_H 72
#define OSD_SEEK_H 4
#define OSD_KNOB_R 7
#define OSD_BTN 36

static void compute_layout(PlayerContext *ctx, OSDLayout *L) {
  int ww = 1, wh = 1;
  SDL_GetWindowSize(ctx->window, &ww, &wh);

  float panel_y = wh - OSD_PANEL_H;
  L->panel = (SDL_FRect){0, panel_y, (float)ww, OSD_PANEL_H};

  L->seek_track =
      (SDL_FRect){OSD_PAD, panel_y + 14, (float)ww - 2 * OSD_PAD, OSD_SEEK_H};

  float by = panel_y + 32, bx = OSD_PAD;
  L->btn_play = (SDL_FRect){bx, by, OSD_BTN, OSD_BTN};
  bx += OSD_BTN + 4;
  L->btn_prev = (SDL_FRect){bx, by, OSD_BTN, OSD_BTN};
  bx += OSD_BTN + 4;
  L->btn_next = (SDL_FRect){bx, by, OSD_BTN, OSD_BTN};
  bx += OSD_BTN + 4;
  L->btn_cc = (SDL_FRect){bx, by, OSD_BTN, OSD_BTN};
  L->btn_mute =
      (SDL_FRect){(float)ww - OSD_PAD - OSD_BTN, by, OSD_BTN, OSD_BTN};

  double frac = 0.0;
  if (ctx->osd.seek_dragging) {
    frac = ctx->osd.seek_drag_frac;
  } else if (ctx->fmt_ctx && ctx->fmt_ctx->duration > 0 &&
             ctx->fmt_ctx->duration != AV_NOPTS_VALUE) {
    double start = ctx->fmt_ctx->start_time != AV_NOPTS_VALUE
                       ? (double)ctx->fmt_ctx->start_time / AV_TIME_BASE
                       : 0.0;
    double dur = (double)ctx->fmt_ctx->duration / AV_TIME_BASE;
    double pos = ctx->last_video_time;
    if (pos < start)
      pos = start;
    if (pos > start + dur)
      pos = start + dur;
    frac = dur > 0 ? (pos - start) / dur : 0.0;
  }
  float kx = L->seek_track.x + (float)(frac * L->seek_track.w);
  L->seek_knob = (SDL_FRect){kx - OSD_KNOB_R,
                             L->seek_track.y + L->seek_track.h / 2 - OSD_KNOB_R,
                             OSD_KNOB_R * 2, OSD_KNOB_R * 2};
}

int osd_is_visible(PlayerContext *ctx) {
  double now = get_monotonic_time();
  return (now < ctx->osd.visible_until) || ctx->osd.hover ||
         ctx->osd.seek_dragging;
}

static OSDHit hit_test(PlayerContext *ctx, float mx, float my) {
  OSDLayout L;
  compute_layout(ctx, &L);
  SDL_FPoint p = {mx, my};

  SDL_FRect seek_hit = {L.seek_track.x, L.seek_track.y - 10, L.seek_track.w,
                        L.seek_track.h + 20};
  if (SDL_PointInRectFloat(&p, &seek_hit))
    return OSD_SEEK;
  if (SDL_PointInRectFloat(&p, &L.btn_play))
    return OSD_PLAY;
  if (SDL_PointInRectFloat(&p, &L.btn_prev))
    return OSD_PREV;
  if (SDL_PointInRectFloat(&p, &L.btn_next))
    return OSD_NEXT;
  if (SDL_PointInRectFloat(&p, &L.btn_cc))
    return OSD_CC;
  if (SDL_PointInRectFloat(&p, &L.btn_mute))
    return OSD_MUTE;
  return OSD_NONE;
}

void osd_on_mouse_move(PlayerContext *ctx, float mx, float my) {
  double now = get_monotonic_time();

  if (ctx->osd.seek_dragging) {
    SDL_MouseButtonFlags btns = SDL_GetMouseState(NULL, NULL);
    if (!(btns & SDL_BUTTON_LMASK)) {
      osd_on_mouse_up(ctx, mx, my);
      return;
    }
  }

  OSDLayout L;
  compute_layout(ctx, &L);
  SDL_FPoint p = {mx, my};
  ctx->osd.hover = SDL_PointInRectFloat(&p, &L.panel);
  ctx->osd.visible_until = now + OSD_SHOW_DURATION;

  if (ctx->osd.seek_dragging) {
    float f = (mx - L.seek_track.x) / L.seek_track.w;
    if (f < 0)
      f = 0;
    if (f > 1)
      f = 1;
    ctx->osd.seek_drag_frac = f;
    ctx->osd.visible_until = now + 10.0;
  }
}

void osd_on_mouse_down(PlayerContext *ctx, float mx, float my) {
  double now = get_monotonic_time();

  if (!osd_is_visible(ctx)) {
    ctx->osd.visible_until = now + OSD_SHOW_DURATION;
    return;
  }

  OSDHit hit = hit_test(ctx, mx, my);
  OSDLayout L;
  compute_layout(ctx, &L);
  SDL_FPoint p = {mx, my};

  switch (hit) {
  case OSD_SEEK: {
    float f = (mx - L.seek_track.x) / L.seek_track.w;
    if (f < 0)
      f = 0;
    if (f > 1)
      f = 1;
    ctx->osd.seek_dragging = 1;
    ctx->osd.seek_drag_frac = f;
    ctx->osd.visible_until = now + 10.0;
    break;
  }
  case OSD_PLAY:
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
    ctx->osd.needs_redraw = 1;
    break;
  case OSD_PREV:
    if (ctx->playlist_index_ref && *ctx->playlist_index_ref > 0) {
      ctx->nav_action = NAV_PREV;
      ctx->file_finished = 1;
    } else {
      ctx->seek_target = 0.0;
      atomic_store(&ctx->seek_requested, 1);
    }
    break;
  case OSD_NEXT:
    if (ctx->playlist_index_ref &&
        *ctx->playlist_index_ref + 1 < ctx->playlist_count) {
      ctx->nav_action = NAV_NEXT;
      ctx->file_finished = 1;
    }
    break;
  case OSD_CC:
    if (ctx->subtitle_stream_count <= 0)
      break;
    if (!ctx->subtitle_visible) {
      ctx->subtitle_visible = 1;
      if (ctx->current_subtitle_idx < 0)
        player_switch_subtitle(ctx);
    } else {
      player_switch_subtitle(ctx);
    }
    ctx->osd.needs_redraw = 1;
    break;
  case OSD_MUTE:
    if (ctx->volume > 0.001f) {
      ctx->osd.pre_mute_volume = ctx->volume;
      player_set_volume(ctx, 0.0f);
    } else {
      player_set_volume(ctx, ctx->osd.pre_mute_volume > 0.001f
                                 ? ctx->osd.pre_mute_volume
                                 : 1.0f);
    }
    break;
  default:
    if (!SDL_PointInRectFloat(&p, &L.panel)) {
      ctx->osd.visible_until = 0.0;
      ctx->osd.hover = 0;
    }
    break;
  }
}

void osd_on_mouse_up(PlayerContext *ctx, float mx, float my) {
  (void)mx;
  (void)my;

  if (!ctx->osd.seek_dragging)
    return;
  ctx->osd.seek_dragging = 0;

  if (ctx->fmt_ctx && ctx->fmt_ctx->duration > 0 &&
      ctx->fmt_ctx->duration != AV_NOPTS_VALUE) {
    double start = ctx->fmt_ctx->start_time != AV_NOPTS_VALUE
                       ? (double)ctx->fmt_ctx->start_time / AV_TIME_BASE
                       : 0.0;
    double dur = (double)ctx->fmt_ctx->duration / AV_TIME_BASE;
    ctx->seek_target = start + ctx->osd.seek_drag_frac * dur;
    atomic_store(&ctx->seek_requested, 1);
    ctx->osd.needs_redraw = 1;
  }
  ctx->osd.visible_until = get_monotonic_time() + OSD_SHOW_DURATION;
}

/* ---------------- Rendering Helpers ---------------- */

static void draw_circle(SDL_Renderer *r, float cx, float cy, float rad) {
  for (int dy = -(int)rad; dy <= (int)rad; dy++) {
    float w = sqrtf(rad * rad - (float)(dy * dy));
    SDL_RenderLine(r, cx - w, cy + dy, cx + w, cy + dy);
  }
}

static void draw_icon_play_pause(SDL_Renderer *r, SDL_FRect btn, int paused) {
  float cx = btn.x + btn.w / 2;
  float cy = btn.y + btn.h / 2;
  SDL_FColor white = {1, 1, 1, 1};
  if (paused) {
    SDL_Vertex tri[3] = {{{cx - 5, cy - 8}, white, {0, 0}},
                         {{cx + 8, cy}, white, {0, 0}},
                         {{cx - 5, cy + 8}, white, {0, 0}}};
    SDL_RenderGeometry(r, NULL, tri, 3, NULL, 0);
  } else {
    SDL_FRect b1 = {cx - 6, cy - 8, 4, 16};
    SDL_FRect b2 = {cx + 2, cy - 8, 4, 16};
    SDL_RenderFillRect(r, &b1);
    SDL_RenderFillRect(r, &b2);
  }
}

static void draw_icon_prev_next(SDL_Renderer *r, SDL_FRect btn, int is_next) {
  float cx = btn.x + btn.w / 2;
  float cy = btn.y + btn.h / 2;
  SDL_FColor white = {1, 1, 1, 1};

  if (!is_next) {
    SDL_FRect pb = {cx - 8, cy - 7, 3, 14};
    SDL_RenderFillRect(r, &pb);
    SDL_Vertex tri[3] = {{{cx + 7, cy - 7}, white, {0, 0}},
                         {{cx + 7, cy + 7}, white, {0, 0}},
                         {{cx - 4, cy}, white, {0, 0}}};
    SDL_RenderGeometry(r, NULL, tri, 3, NULL, 0);
  } else {
    SDL_FRect nb = {cx + 6, cy - 7, 3, 14};
    SDL_RenderFillRect(r, &nb);
    SDL_Vertex tri[3] = {{{cx - 7, cy - 7}, white, {0, 0}},
                         {{cx + 4, cy}, white, {0, 0}},
                         {{cx - 7, cy + 7}, white, {0, 0}}};
    SDL_RenderGeometry(r, NULL, tri, 3, NULL, 0);
  }
}

static void draw_icon_speaker(SDL_Renderer *r, SDL_FRect btn, float volume) {
  float cx = btn.x + btn.w / 2;
  float cy = btn.y + btn.h / 2;
  SDL_FColor white = {1, 1, 1, 1};

  SDL_FRect sp = {cx - 8, cy - 3, 4, 6};
  SDL_RenderFillRect(r, &sp);

  SDL_Vertex cone[4] = {{{cx - 4, cy - 4}, white, {0, 0}},
                        {{cx - 4, cy + 4}, white, {0, 0}},
                        {{cx + 4, cy + 8}, white, {0, 0}},
                        {{cx + 4, cy - 8}, white, {0, 0}}};
  int indices[6] = {0, 1, 2, 0, 2, 3};
  SDL_RenderGeometry(r, NULL, cone, 4, indices, 6);

  if (volume <= 0.001f) {
    SDL_SetRenderDrawColor(r, 255, 70, 70, 255);
    SDL_RenderLine(r, cx + 5, cy - 5, cx + 10, cy + 5);
    SDL_RenderLine(r, cx + 10, cy - 5, cx + 5, cy + 5);
  } else {
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderLine(r, cx + 5, cy - 3, cx + 5, cy + 3);
    SDL_RenderLine(r, cx + 8, cy - 6, cx + 8, cy + 6);
  }
}

/* ---------------- Main Render ---------------- */

void osd_render(PlayerContext *ctx, const SDL_FRect *dest_rect) {
  if (!osd_is_visible(ctx) || !ctx->fmt_ctx)
    return;
  (void)dest_rect;

  OSDLayout L;
  compute_layout(ctx, &L);
  SDL_Renderer *r = ctx->renderer;
  SDL_Color white = {255, 255, 255, 255};

  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

  /* Panel & Seek Bar */
  SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
  SDL_RenderFillRect(r, &L.panel);
  SDL_SetRenderDrawColor(r, 70, 70, 82, 220);
  SDL_RenderFillRect(r, &L.seek_track);

  double frac = 0.0;
  if (ctx->osd.seek_dragging) {
    frac = ctx->osd.seek_drag_frac;
  } else if (ctx->fmt_ctx->duration > 0 &&
             ctx->fmt_ctx->duration != AV_NOPTS_VALUE) {
    double start = ctx->fmt_ctx->start_time != AV_NOPTS_VALUE
                       ? (double)ctx->fmt_ctx->start_time / AV_TIME_BASE
                       : 0.0;
    double dur = (double)ctx->fmt_ctx->duration / AV_TIME_BASE;
    double pos = ctx->last_video_time;
    if (pos < start)
      pos = start;
    if (pos > start + dur)
      pos = start + dur;
    frac = dur > 0 ? (pos - start) / dur : 0.0;
  }
  SDL_FRect fill = L.seek_track;
  fill.w *= (float)frac;
  SDL_SetRenderDrawColor(r, 0x38, 0xBD, 0xF8, 255);
  SDL_RenderFillRect(r, &fill);

  SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
  draw_circle(r, L.seek_knob.x + OSD_KNOB_R, L.seek_knob.y + OSD_KNOB_R,
              (float)OSD_KNOB_R);

  /* Buttons */
  draw_icon_play_pause(r, L.btn_play, ctx->paused);
  draw_icon_prev_next(r, L.btn_prev, 0);
  draw_icon_prev_next(r, L.btn_next, 1);
  draw_icon_speaker(r, L.btn_mute, ctx->volume);

  /* CC Text */
  char ccl[16];
  int have_subs = ctx->subtitle_stream_count > 0;
  int subs_on =
      have_subs && ctx->subtitle_visible && ctx->current_subtitle_idx >= 0;
  if (subs_on)
    snprintf(ccl, sizeof(ccl), "CC%d", ctx->current_subtitle_idx + 1);
  else
    snprintf(ccl, sizeof(ccl), "CC");

  SDL_Color col = have_subs ? (subs_on ? (SDL_Color){0x38, 0xBD, 0xF8, 255}
                                       : (SDL_Color){180, 180, 190, 255})
                            : (SDL_Color){70, 70, 78, 255};
  ui_text_box(r, ccl, col, L.btn_cc, TEXT_ALIGN_CENTER);

  /* Time Text */
  double cur = ctx->last_video_time;
  if (ctx->osd.seek_dragging && ctx->fmt_ctx->duration != AV_NOPTS_VALUE) {
    double start = ctx->fmt_ctx->start_time != AV_NOPTS_VALUE
                       ? (double)ctx->fmt_ctx->start_time / AV_TIME_BASE
                       : 0.0;
    double dur = (double)ctx->fmt_ctx->duration / AV_TIME_BASE;
    cur = start + ctx->osd.seek_drag_frac * dur;
  }
  double start_time_sec =
      (ctx->fmt_ctx && ctx->fmt_ctx->start_time != AV_NOPTS_VALUE)
          ? (double)ctx->fmt_ctx->start_time / AV_TIME_BASE
          : 0.0;
  double display_cur = cur - start_time_sec;
  if (display_cur < 0)
    display_cur = 0;
  double tot = ctx->fmt_ctx->duration != AV_NOPTS_VALUE
                   ? (double)ctx->fmt_ctx->duration / AV_TIME_BASE
                   : 0.0;

  char tb[64], bs[24], bt[24];
  int h1 = (int)display_cur / 3600, m1 = ((int)display_cur % 3600) / 60,
      s1 = (int)display_cur % 60;
  int h2 = (int)tot / 3600, m2 = ((int)tot % 3600) / 60, s2 = (int)tot % 60;
  if (h1)
    snprintf(bs, sizeof(bs), "%d:%02d:%02d", h1, m1, s1);
  else
    snprintf(bs, sizeof(bs), "%d:%02d", m1, s1);
  if (h2)
    snprintf(bt, sizeof(bt), "%d:%02d:%02d", h2, m2, s2);
  else
    snprintf(bt, sizeof(bt), "%d:%02d", m2, s2);
  snprintf(tb, sizeof(tb), "%s / %s", bs, bt);

  SDL_FRect time_box = {L.btn_cc.x + L.btn_cc.w + 16, L.btn_play.y,
                        L.panel.w - (L.btn_cc.x + L.btn_cc.w + 16) -
                            (OSD_PAD + OSD_BTN + 60),
                        OSD_BTN};
  ui_text_box(r, tb, white, time_box, TEXT_ALIGN_LEFT);

  /* Volume Text */
  char vb[16];
  snprintf(vb, sizeof(vb), "%d%%", (int)(ctx->volume * 100 + .5f));
  SDL_FRect vol_box = {L.btn_mute.x - 40, L.btn_mute.y, 44, OSD_BTN};
  ui_text_box(r, vb, white, vol_box, TEXT_ALIGN_RIGHT);

  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}
