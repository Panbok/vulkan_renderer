#include "editor_recording.h"

#include "editor_internal.h"
#include "editor_ui.h"

/* The Thread timeline (ADR-126): for the open playback, a flame chart of
   each thread's CPU zones and tracks of the GPU's frames and timed passes
   around the playhead, the editor's view of `vkr_rec export --perfetto`.
   The UI has no canvas, so the spans of every track are the strokes of one
   path widget, their names are labels placed over them, and transparent
   plots over the tracks take hover, clicks, drags and the wheel. */

#define EDITOR_FLAME_HEADER_PT 26.0f
#define EDITOR_FLAME_RULER_PT 32.0f
#define EDITOR_FLAME_FOOTER_PT 16.0f
#define EDITOR_FLAME_GAP_PT 4.0f
#define EDITOR_FLAME_PAD_X_PT 12.0f
#define EDITOR_FLAME_PAD_Y_PT 8.0f
/* The track names' column, a row of one nesting depth, and the room above
   and below a track's rows. */
#define EDITOR_FLAME_LABEL_PT 88.0f
#define EDITOR_FLAME_ROW_PT 18.0f
#define EDITOR_FLAME_TRACK_PAD_PT 3.0f
/* Span names: their size, and the room either side of one in its span. */
#define EDITOR_FLAME_NAME_PT 10.5f
#define EDITOR_FLAME_NAME_PAD_PT 4.0f
/* Main, Render and 16 job workers, and the GPU frames and passes, each
   with at most this many rows; job workers start at this thread. */
#define EDITOR_FLAME_THREAD_MAX 18u
#define EDITOR_FLAME_FIRST_JOB_THREAD 2u
#define EDITOR_FLAME_TRACK_MAX (EDITOR_FLAME_THREAD_MAX + 2u)
#define EDITOR_FLAME_DEPTH_MAX 24u
#define EDITOR_FLAME_GPU_LANES 4u
#define EDITOR_FLAME_ROW_MAX (EDITOR_FLAME_TRACK_MAX * EDITOR_FLAME_DEPTH_MAX)
/* Spans one build strokes and names at most; spans narrower than a pixel
   that share one merge into a single mark. */
#define EDITOR_FLAME_SPAN_MAX 2400u
#define EDITOR_FLAME_NAME_MAX 160u
#define EDITOR_FLAME_PALETTE 8u
/* Frame starts and ruler ticks one build marks at most. */
#define EDITOR_FLAME_MARK_MAX 64u
/* One wheel notch zooms by this factor around the pointer; the view spans
   at least and at most these. */
#define EDITOR_FLAME_ZOOM 0.8
#define EDITOR_FLAME_VIEW_MIN_NS 20000ll
#define EDITOR_FLAME_VIEW_MAX_NS 4000000000ll
/* A ruler tick wants this much room, a frame name this much. */
#define EDITOR_FLAME_TICK_PT 84.0f
#define EDITOR_FLAME_FRAME_NAME_PT 72.0f
/* Frame starts closer than this draw only as short ruler ticks. */
#define EDITOR_FLAME_DENSE_PT 24.0f
/* Columns of the input plot; the pointer's own position places it. */
#define EDITOR_FLAME_INPUT_SAMPLES 64u
/* Builds the view waits for the playhead to reach a seek of its own. */
#define EDITOR_FLAME_SEEK_BUILDS 30u

typedef enum EditorFlameTrackKind {
  EDITOR_FLAME_TRACK_CPU = 0,
  EDITOR_FLAME_TRACK_GPU_FRAMES,
  EDITOR_FLAME_TRACK_GPU_PASSES,
} EditorFlameTrackKind;

/* One track: a thread's zones by depth, or the GPU's frames or passes in
   lanes; its rows are global rows [first_row, first_row + rows) and its
   top is in the tracks' content, before scrolling. */
typedef struct EditorFlameTrack {
  EditorFlameTrackKind kind;
  uint32_t thread;
  uint32_t rows;
  uint32_t first_row;
  float32_t top_pt;
} EditorFlameTrack;

/* One build's tracks and what the view holds: the frames whose Frame zone
   starts in it, the frames it overlaps and its span counts. */
typedef struct EditorFlameLayout {
  EditorFlameTrack tracks[EDITOR_FLAME_TRACK_MAX];
  uint32_t track_count;
  uint32_t thread_track[EDITOR_FLAME_THREAD_MAX];
  uint32_t gpu_frames_track;
  uint32_t gpu_passes_track;
  float32_t content_pt;
  int64_t mark_ns[EDITOR_FLAME_MARK_MAX];
  uint64_t mark_frame[EDITOR_FLAME_MARK_MAX];
  uint32_t mark_count;
  bool8_t has_frames;
  uint64_t frame_first;
  uint64_t frame_last;
  uint32_t zones;
  uint32_t gpu_frames;
  uint32_t passes;
} EditorFlameLayout;

/* The view in points: the canvas below the header, the tracks' width right
   of the names, and the scale from ns. */
typedef struct EditorFlameView {
  int64_t begin;
  int64_t end;
  float32_t width_pt;
  float32_t height_pt;
  float32_t area_pt;
  float64_t pt_per_ns;
  float32_t pixel_pt;
} EditorFlameView;

typedef struct EditorFlameName {
  float32_t x_pt;
  float32_t y_pt;
  String8 text;
} EditorFlameName;

/* The spans' strokes and names as one build collects them, and the span
   under the pointer and the selected one as drawn. */
typedef struct EditorFlameDraw {
  const EditorFlameLayout *layout;
  const EditorFlameView *view;
  float32_t scroll_pt;
  Vec2 cell_pt;
  /* The UI's frame allocator, for shortened names. */
  VkrAllocator *allocator;
  Vec2 *points;
  uint8_t *colors;
  uint32_t point_count;
  uint32_t point_capacity;
  /* Where each row's last stroke ends, so narrow spans merge. */
  float32_t row_end[EDITOR_FLAME_ROW_MAX];
  uint32_t drawn;
  uint32_t merged;
  bool8_t capped;
  EditorFlameName names[EDITOR_FLAME_NAME_MAX];
  uint32_t name_count;
  /* The pointer over the tracks, in the tracks panel's points. */
  bool8_t pointer_in;
  Vec2 pointer_pt;
  bool8_t hovered;
  bool8_t hovered_gpu;
  uint32_t hovered_index;
  uint32_t hovered_track;
  VkrUiRect hovered_rect;
  bool8_t has_selection;
  bool8_t selected_gpu;
  uint32_t selected_index;
  bool8_t selected_shown;
  VkrUiRect selected_rect;
} EditorFlameDraw;

/* Legible with dark names on both themes; a name's hash picks one. */
static const Vec4 s_flame_palette[EDITOR_FLAME_PALETTE] = {
    {0.45f, 0.62f, 0.88f, 1.0f}, {0.38f, 0.75f, 0.66f, 1.0f},
    {0.86f, 0.68f, 0.38f, 1.0f}, {0.70f, 0.56f, 0.90f, 1.0f},
    {0.90f, 0.56f, 0.52f, 1.0f}, {0.55f, 0.78f, 0.44f, 1.0f},
    {0.45f, 0.76f, 0.86f, 1.0f}, {0.86f, 0.58f, 0.74f, 1.0f},
};
static const Vec4 s_flame_name_color = {0.07f, 0.08f, 0.10f, 1.0f};
/* The input plot's values: it draws nothing. */
static const float32_t s_flame_input_values[EDITOR_FLAME_INPUT_SAMPLES] = {0};

/* ------------------------------------------------------------------------ */
/* Spans and names                                                           */

vkr_internal uint8_t editor_flame_color(const char *name) {
  uint32_t hash = 2166136261u;
  for (const char *c = name; *c; ++c) {
    hash ^= (uint8_t)*c;
    hash *= 16777619u;
  }
  return (uint8_t)(hash % EDITOR_FLAME_PALETTE);
}

vkr_internal const char *
editor_flame_zone_name(const VkrRecordingPlayback *playback,
                       const VkrRecordingPlaybackZone *zone) {
  if (zone->zone < playback->zone_name_count &&
      playback->zone_names[zone->zone][0]) {
    return playback->zone_names[zone->zone];
  }
  return "zone";
}

vkr_internal const char *
editor_flame_gpu_name(const VkrRecordingPlayback *playback,
                      const VkrRecordingPlaybackGpuSpan *span) {
  if (span->pass == VKR_RECORDING_PLAYBACK_GPU_FRAME) {
    return "GPU frame";
  }
  if (span->pass < playback->pass_name_count &&
      playback->pass_names[span->pass][0]) {
    return playback->pass_names[span->pass];
  }
  return "pass";
}

vkr_internal String8 editor_flame_track_name(VkrUiSystem *ui,
                                             const EditorFlameTrack *track) {
  if (track->kind == EDITOR_FLAME_TRACK_GPU_FRAMES) {
    return string8_lit("GPU frames");
  }
  if (track->kind == EDITOR_FLAME_TRACK_GPU_PASSES) {
    return string8_lit("GPU passes");
  }
  if (track->thread == 0u) {
    return string8_lit("Main");
  }
  if (track->thread == 1u) {
    return string8_lit("Render");
  }
  return string8_create_formatted(ui->frame_allocator, "Worker %u",
                                  track->thread -
                                      EDITOR_FLAME_FIRST_JOB_THREAD);
}

/* The first of the lanes free at `begin`, else the last; it stays busy
   until `end`. */
vkr_internal uint32_t editor_flame_lane(int64_t *lane_end, int64_t begin,
                                        int64_t end) {
  uint32_t lane = EDITOR_FLAME_GPU_LANES - 1u;
  for (uint32_t i = 0u; i < EDITOR_FLAME_GPU_LANES; ++i) {
    if (lane_end[i] <= begin) {
      lane = i;
      break;
    }
  }
  lane_end[lane] = Max(lane_end[lane], end);
  return lane;
}

/* ------------------------------------------------------------------------ */
/* View                                                                      */

/* Where frame `index` ran on the recording's clock: from its Frame zone's
   start to the next frame's, else for its recorded length. A frame without
   a Frame zone is placed from the nearest one that has one, else from its
   GPU frame. False when nothing places it. */
vkr_internal bool8_t
editor_flame_frame_range(const VkrRecordingPlayback *playback, uint64_t index,
                         int64_t *out_begin, int64_t *out_end) {
  if (playback->frame_count == 0u || !playback->frame_begin_ns) {
    return false_v;
  }
  const uint64_t count = playback->frame_count;
  const uint64_t frame = Min(index, count - 1u);
  int64_t begin = (int64_t)playback->frame_begin_ns[frame];
  for (uint64_t step = 1u; begin == 0 && step < 256u; ++step) {
    uint64_t near = UINT64_MAX;
    if (frame >= step && playback->frame_begin_ns[frame - step]) {
      near = frame - step;
    } else if (frame + step < count && playback->frame_begin_ns[frame + step]) {
      near = frame + step;
    }
    if (near != UINT64_MAX) {
      const float64_t offset =
          (playback->times[frame] - playback->times[near]) * 1.0e9;
      begin = Max((int64_t)playback->frame_begin_ns[near] + (int64_t)offset,
                  (int64_t)1);
    }
  }
  for (uint32_t i = 0u; begin == 0 && i < playback->gpu_span_count; ++i) {
    const VkrRecordingPlaybackGpuSpan *span = &playback->gpu_spans[i];
    if (span->frame == frame &&
        span->pass == VKR_RECORDING_PLAYBACK_GPU_FRAME) {
      begin = (int64_t)span->begin_ns;
    }
  }
  if (begin == 0) {
    return false_v;
  }
  const int64_t length = Max(
      (int64_t)(playback->frames[frame].display_dt * 1.0e9), (int64_t)1000000);
  int64_t end = begin + length;
  if (frame + 1u < count &&
      (int64_t)playback->frame_begin_ns[frame + 1u] > begin) {
    end = (int64_t)playback->frame_begin_ns[frame + 1u];
  }
  *out_begin = begin;
  *out_end = end;
  return true_v;
}

/* Shows `length` ns from `begin`, within the zoom limits and no further
   than half a view past the recording's spans. */
vkr_internal void editor_flame_set_view(VkrEditorRecordingFlame *flame,
                                        const VkrRecordingPlayback *playback,
                                        int64_t begin, int64_t length) {
  length = Clamp(length, EDITOR_FLAME_VIEW_MIN_NS, EDITOR_FLAME_VIEW_MAX_NS);
  int64_t low = INT64_MAX;
  int64_t high = 0;
  if (playback->zone_count > 0u) {
    low = Min(low, (int64_t)playback->zones[0].begin_ns);
    high =
        Max(high, (int64_t)playback->zones[playback->zone_count - 1u].end_ns);
  }
  if (playback->gpu_span_count > 0u) {
    low = Min(low, (int64_t)playback->gpu_spans[0].begin_ns);
    high =
        Max(high,
            (int64_t)playback->gpu_spans[playback->gpu_span_count - 1u].end_ns);
  }
  if (high > low) {
    begin = Clamp(begin, low - length / 2, high - length / 2);
  }
  flame->view_begin_ns = begin;
  flame->view_end_ns = begin + length;
}

/* Shows frame `index` with `around` frames either side; a lone frame keeps
   a little room at its edges. */
vkr_internal void editor_flame_show_frames(VkrEditorRecordingFlame *flame,
                                           const VkrRecordingPlayback *playback,
                                           uint64_t index, uint64_t around) {
  int64_t begin = 0;
  int64_t end = 0;
  int64_t last_begin = 0;
  int64_t last_end = 0;
  const uint64_t first = index > around ? index - around : 0u;
  if (!editor_flame_frame_range(playback, first, &begin, &end) ||
      !editor_flame_frame_range(playback, index + around, &last_begin,
                                &last_end)) {
    /* Nothing places frames: the first 50 ms of the spans. */
    const int64_t start = playback->zone_count > 0u
                              ? (int64_t)playback->zones[0].begin_ns
                          : playback->gpu_span_count > 0u
                              ? (int64_t)playback->gpu_spans[0].begin_ns
                              : 0;
    editor_flame_set_view(flame, playback, start, 50000000);
    return;
  }
  const int64_t length = Max(last_end - begin, EDITOR_FLAME_VIEW_MIN_NS);
  const int64_t margin = around == 0u ? length / 40 : 0;
  editor_flame_set_view(flame, playback, begin - margin, length + margin * 2);
}

/* Another bundle shows its playhead frame and the frames either side. */
vkr_internal void editor_flame_reset(VkrEditorRecordingFlame *flame,
                                     const VkrRecordingPlayback *playback,
                                     uint64_t playhead) {
  if (string_equals(flame->bundle, playback->bundle_path)) {
    return;
  }
  string_format(flame->bundle, sizeof(flame->bundle), "%s",
                playback->bundle_path);
  flame->selected = false_v;
  flame->dragging = false_v;
  flame->scroll_pt = 0.0f;
  flame->followed_frame = playhead;
  flame->awaited_builds = 0u;
  editor_flame_show_frames(flame, playback, playhead, 1u);
}

/* The view moves with the playhead: a playhead frame wholly outside it is
   centred, at the same zoom. */
vkr_internal void editor_flame_follow(VkrEditorRecordingFlame *flame,
                                      const VkrRecordingPlayback *playback,
                                      uint64_t playhead) {
  /* A seek of the window's own has placed the view already; the playhead
     reaches it a few builds later. */
  if (flame->awaited_builds > 0u) {
    flame->awaited_builds--;
    if (playhead == flame->awaited_frame) {
      flame->awaited_builds = 0u;
      flame->followed_frame = playhead;
    }
    return;
  }
  if (playhead == flame->followed_frame) {
    return;
  }
  flame->followed_frame = playhead;
  int64_t begin = 0;
  int64_t end = 0;
  if (!editor_flame_frame_range(playback, playhead, &begin, &end) ||
      (end > flame->view_begin_ns && begin < flame->view_end_ns)) {
    return;
  }
  const int64_t length = flame->view_end_ns - flame->view_begin_ns;
  editor_flame_set_view(flame, playback, begin + (end - begin) / 2 - length / 2,
                        length);
}

/* Moves the playhead to `frame` without the view following it. */
vkr_internal void editor_flame_seek(VkrEditorUi *editor, uint64_t frame) {
  VkrEditorRecordingFlame *flame = &editor->recording.flame;
  VkrSampleRecordingRequest seek = {
      .command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
      .frame = frame,
  };
  if (vkr_editor_recording_send(editor, seek, false_v) != 0u) {
    flame->awaited_frame = frame;
    flame->awaited_builds = EDITOR_FLAME_SEEK_BUILDS;
  }
}

/* Moves the playhead a frame and the view with it. */
vkr_internal void editor_flame_step(VkrEditorUi *editor,
                                    const VkrRecordingPlayback *playback,
                                    uint64_t playhead, bool8_t forward) {
  VkrEditorRecordingFlame *flame = &editor->recording.flame;
  if (forward ? playhead + 1u >= playback->frame_count : playhead == 0u) {
    return;
  }
  const uint64_t next = forward ? playhead + 1u : playhead - 1u;
  int64_t from = 0;
  int64_t to = 0;
  int64_t unused = 0;
  if (editor_flame_frame_range(playback, playhead, &from, &unused) &&
      editor_flame_frame_range(playback, next, &to, &unused)) {
    editor_flame_set_view(flame, playback, flame->view_begin_ns + (to - from),
                          flame->view_end_ns - flame->view_begin_ns);
  }
  editor_flame_seek(editor, next);
}

/* ------------------------------------------------------------------------ */
/* Layout                                                                    */

vkr_internal uint32_t editor_flame_add_track(EditorFlameLayout *layout,
                                             EditorFlameTrackKind kind,
                                             uint32_t thread) {
  if (layout->track_count == EDITOR_FLAME_TRACK_MAX) {
    return UINT32_MAX;
  }
  layout->tracks[layout->track_count] = (EditorFlameTrack){
      .kind = kind,
      .thread = thread,
      .rows = 1u,
  };
  return layout->track_count++;
}

vkr_internal void editor_flame_note_frame(EditorFlameLayout *layout,
                                          uint64_t frame) {
  if (!layout->has_frames) {
    layout->has_frames = true_v;
    layout->frame_first = frame;
    layout->frame_last = frame;
    return;
  }
  layout->frame_first = Min(layout->frame_first, frame);
  layout->frame_last = Max(layout->frame_last, frame);
}

/* A track per thread that recorded zones and two for the GPU, each as tall
   as the deepest zone or busiest lane in the view, and the frames whose
   Frame zones the view holds. */
vkr_internal void editor_flame_layout(const VkrRecordingPlayback *playback,
                                      const VkrEditorRecordingFlame *flame,
                                      EditorFlameLayout *layout) {
  MemZero(layout, sizeof(*layout));
  /* Main and Render, then the GPU's tracks beside the Render thread that
     feeds them, then the job workers, so that the GPU stays in view
     however many workers recorded zones. */
  for (uint32_t t = 0u; t < EDITOR_FLAME_THREAD_MAX; ++t) {
    layout->thread_track[t] = UINT32_MAX;
  }
  for (uint32_t t = 0u; t < EDITOR_FLAME_FIRST_JOB_THREAD; ++t) {
    if ((playback->zone_threads >> t) & 1u) {
      layout->thread_track[t] =
          editor_flame_add_track(layout, EDITOR_FLAME_TRACK_CPU, t);
    }
  }
  layout->gpu_frames_track = UINT32_MAX;
  layout->gpu_passes_track = UINT32_MAX;
  if (playback->gpu_span_count > 0u) {
    layout->gpu_frames_track =
        editor_flame_add_track(layout, EDITOR_FLAME_TRACK_GPU_FRAMES, 0u);
    layout->gpu_passes_track =
        editor_flame_add_track(layout, EDITOR_FLAME_TRACK_GPU_PASSES, 0u);
  }
  for (uint32_t t = EDITOR_FLAME_FIRST_JOB_THREAD; t < EDITOR_FLAME_THREAD_MAX;
       ++t) {
    if ((playback->zone_threads >> t) & 1u) {
      layout->thread_track[t] =
          editor_flame_add_track(layout, EDITOR_FLAME_TRACK_CPU, t);
    }
  }

  const int64_t view_begin = flame->view_begin_ns;
  const int64_t view_end = flame->view_end_ns;
  uint32_t first = 0u;
  const uint32_t zones = vkr_recording_playback_zones_in(
      playback, (uint64_t)view_begin, (uint64_t)view_end, &first);
  for (uint32_t i = first; i < first + zones; ++i) {
    const VkrRecordingPlaybackZone *zone = &playback->zones[i];
    if ((int64_t)zone->end_ns <= view_begin) {
      continue;
    }
    layout->zones++;
    if (zone->thread < EDITOR_FLAME_THREAD_MAX &&
        layout->thread_track[zone->thread] != UINT32_MAX) {
      EditorFlameTrack *track =
          &layout->tracks[layout->thread_track[zone->thread]];
      track->rows =
          Max(track->rows, Min(zone->depth + 1u, EDITOR_FLAME_DEPTH_MAX));
    }
    if (zone->thread != 0u || zone->depth != 0u) {
      continue;
    }
    /* The view's frames: those it shows half of, or a twentieth of the
       view of. */
    const int64_t shown = Min((int64_t)zone->end_ns, view_end) -
                          Max((int64_t)zone->begin_ns, view_begin);
    if (shown * 2 >= (int64_t)(zone->end_ns - zone->begin_ns) ||
        shown * 20 >= view_end - view_begin) {
      editor_flame_note_frame(layout, zone->frame);
    }
    if (playback->frame_begin_ns[zone->frame] == zone->begin_ns &&
        (int64_t)zone->begin_ns >= view_begin &&
        layout->mark_count < EDITOR_FLAME_MARK_MAX) {
      layout->mark_ns[layout->mark_count] = (int64_t)zone->begin_ns;
      layout->mark_frame[layout->mark_count] = zone->frame;
      layout->mark_count++;
    }
  }

  int64_t frame_lanes[EDITOR_FLAME_GPU_LANES] = {0};
  int64_t pass_lanes[EDITOR_FLAME_GPU_LANES] = {0};
  const uint32_t spans = vkr_recording_playback_gpu_spans_in(
      playback, (uint64_t)view_begin, (uint64_t)view_end, &first);
  for (uint32_t i = first; i < first + spans; ++i) {
    const VkrRecordingPlaybackGpuSpan *span = &playback->gpu_spans[i];
    if ((int64_t)span->end_ns <= view_begin) {
      continue;
    }
    const bool8_t whole = span->pass == VKR_RECORDING_PLAYBACK_GPU_FRAME;
    const uint32_t lane =
        editor_flame_lane(whole ? frame_lanes : pass_lanes,
                          (int64_t)span->begin_ns, (int64_t)span->end_ns);
    const uint32_t index =
        whole ? layout->gpu_frames_track : layout->gpu_passes_track;
    layout->tracks[index].rows = Max(layout->tracks[index].rows, lane + 1u);
    if (whole) {
      layout->gpu_frames++;
    } else {
      layout->passes++;
    }
    if (playback->zone_count == 0u) {
      editor_flame_note_frame(layout, span->frame);
    }
  }

  float32_t top = 0.0f;
  uint32_t row = 0u;
  for (uint32_t i = 0u; i < layout->track_count; ++i) {
    EditorFlameTrack *track = &layout->tracks[i];
    track->first_row = row;
    track->top_pt = top;
    row += track->rows;
    top += EDITOR_FLAME_TRACK_PAD_PT * 2.0f +
           (float32_t)track->rows * EDITOR_FLAME_ROW_PT;
  }
  layout->content_pt = top;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */

/* Strokes one span on global row `row`, whose top is `top_pt` in the
   tracks' content, names it when the name fits and notes it under the
   pointer. */
vkr_internal void editor_flame_span(EditorFlameDraw *draw, uint32_t track,
                                    uint32_t row, float32_t top_pt,
                                    int64_t begin, int64_t end,
                                    const char *name, bool8_t gpu,
                                    uint32_t index) {
  const EditorFlameView *view = draw->view;
  const float32_t top = top_pt - draw->scroll_pt;
  if (top + EDITOR_FLAME_ROW_PT <= 0.0f || top >= view->height_pt) {
    return;
  }
  float32_t x0 =
      (float32_t)((float64_t)(begin - view->begin) * view->pt_per_ns);
  float32_t x1 = (float32_t)((float64_t)(end - view->begin) * view->pt_per_ns);
  if (x1 <= 0.0f || x0 >= view->area_pt) {
    return;
  }
  x0 = Max(x0, 0.0f);
  x1 = Min(x1, view->area_pt);
  const float32_t left = EDITOR_FLAME_LABEL_PT;

  /* A narrow span still takes the pointer two points wide. */
  if (draw->pointer_in && !draw->hovered) {
    const Vec2 pointer = draw->pointer_pt;
    const float32_t reach = Max(x1, x0 + 2.0f);
    if (pointer.x >= left + x0 && pointer.x < left + reach &&
        pointer.y >= top && pointer.y < top + EDITOR_FLAME_ROW_PT) {
      draw->hovered = true_v;
      draw->hovered_gpu = gpu;
      draw->hovered_index = index;
      draw->hovered_track = track;
      draw->hovered_rect =
          (VkrUiRect){left + x0, top + 1.0f, Max(x1 - x0, view->pixel_pt),
                      EDITOR_FLAME_ROW_PT - 2.0f};
    }
  }

  const bool8_t selected = draw->has_selection && draw->selected_gpu == gpu &&
                           draw->selected_index == index;
  if (x1 - x0 < view->pixel_pt) {
    x0 = vkr_floor_f32(x0 / view->pixel_pt) * view->pixel_pt;
    if (x0 < draw->row_end[row] && !selected) {
      draw->merged++;
      return;
    }
    x1 = x0 + view->pixel_pt;
  } else if (x1 - x0 > 3.0f) {
    /* Neighbours keep a hairline between them. */
    x0 += 0.5f;
    x1 -= 0.5f;
  }
  draw->row_end[row] = Max(draw->row_end[row], x1);
  if (selected) {
    draw->selected_shown = true_v;
    draw->selected_rect =
        (VkrUiRect){left + x0, top + 1.0f, x1 - x0, EDITOR_FLAME_ROW_PT - 2.0f};
  }
  if (draw->point_count + 3u > draw->point_capacity) {
    draw->capped = true_v;
    return;
  }

  const float32_t y = top + EDITOR_FLAME_ROW_PT * 0.5f;
  const uint8_t color = editor_flame_color(name);
  draw->points[draw->point_count] = (Vec2){left + x0, y};
  draw->colors[draw->point_count] = color;
  draw->points[draw->point_count + 1u] = (Vec2){left + x1, y};
  draw->colors[draw->point_count + 1u] = color;
  draw->points[draw->point_count + 2u] =
      (Vec2){VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY};
  draw->colors[draw->point_count + 2u] = 0u;
  draw->point_count += 3u;
  draw->drawn++;

  const float32_t room = x1 - x0 - EDITOR_FLAME_NAME_PAD_PT * 2.0f;
  if (draw->name_count < EDITOR_FLAME_NAME_MAX &&
      room >= draw->cell_pt.x * 3.0f) {
    const uint64_t length =
        string_length_bounded(name, VKR_RECORDING_PLAYBACK_NAME_MAX);
    const uint64_t fit = (uint64_t)(room / draw->cell_pt.x);
    /* A name longer than its span ends in three dots, which the mono face
       has where it lacks an ellipsis; a span too short for two letters and
       the dots stays unnamed. */
    if (length > fit && fit < 5u) {
      return;
    }
    const String8 text =
        length <= fit ? string8_create_from_cstr((const uint8_t *)name, length)
                      : string8_create_formatted(draw->allocator, "%.*s...",
                                                 (int32_t)(fit - 3u), name);
    draw->names[draw->name_count++] = (EditorFlameName){
        .x_pt = left + x0 + EDITOR_FLAME_NAME_PAD_PT,
        .y_pt = top + (EDITOR_FLAME_ROW_PT - draw->cell_pt.y) * 0.5f,
        .text = text,
    };
  }
}

/* Every zone and GPU span in the view, in the rows the layout gave them. */
vkr_internal void editor_flame_spans(const VkrRecordingPlayback *playback,
                                     EditorFlameDraw *draw) {
  const EditorFlameLayout *layout = draw->layout;
  const int64_t view_begin = draw->view->begin;
  const int64_t view_end = draw->view->end;
  uint32_t first = 0u;
  const uint32_t zones = vkr_recording_playback_zones_in(
      playback, (uint64_t)view_begin, (uint64_t)view_end, &first);
  for (uint32_t i = first; i < first + zones; ++i) {
    const VkrRecordingPlaybackZone *zone = &playback->zones[i];
    if ((int64_t)zone->end_ns <= view_begin ||
        zone->thread >= EDITOR_FLAME_THREAD_MAX ||
        layout->thread_track[zone->thread] == UINT32_MAX) {
      continue;
    }
    const uint32_t index = layout->thread_track[zone->thread];
    const EditorFlameTrack *track = &layout->tracks[index];
    const uint32_t depth = Min(zone->depth, track->rows - 1u);
    editor_flame_span(draw, index, track->first_row + depth,
                      track->top_pt + EDITOR_FLAME_TRACK_PAD_PT +
                          (float32_t)depth * EDITOR_FLAME_ROW_PT,
                      (int64_t)zone->begin_ns, (int64_t)zone->end_ns,
                      editor_flame_zone_name(playback, zone), false_v, i);
  }

  /* Lanes as the layout assigned them, in the same order. */
  int64_t frame_lanes[EDITOR_FLAME_GPU_LANES] = {0};
  int64_t pass_lanes[EDITOR_FLAME_GPU_LANES] = {0};
  const uint32_t spans = vkr_recording_playback_gpu_spans_in(
      playback, (uint64_t)view_begin, (uint64_t)view_end, &first);
  for (uint32_t i = first; i < first + spans; ++i) {
    const VkrRecordingPlaybackGpuSpan *span = &playback->gpu_spans[i];
    if ((int64_t)span->end_ns <= view_begin) {
      continue;
    }
    const bool8_t whole = span->pass == VKR_RECORDING_PLAYBACK_GPU_FRAME;
    const uint32_t lane =
        editor_flame_lane(whole ? frame_lanes : pass_lanes,
                          (int64_t)span->begin_ns, (int64_t)span->end_ns);
    const uint32_t index =
        whole ? layout->gpu_frames_track : layout->gpu_passes_track;
    const EditorFlameTrack *track = &layout->tracks[index];
    editor_flame_span(draw, index, track->first_row + lane,
                      track->top_pt + EDITOR_FLAME_TRACK_PAD_PT +
                          (float32_t)lane * EDITOR_FLAME_ROW_PT,
                      (int64_t)span->begin_ns, (int64_t)span->end_ns,
                      editor_flame_gpu_name(playback, span), true_v, i);
  }
}

/* A path widget over the whole tracks or ruler panel. */
vkr_internal void editor_flame_path(VkrUiSystem *ui, String8 id,
                                    const EditorFlameView *view,
                                    float32_t height_pt, const Vec2 *points,
                                    const uint8_t *colors, uint32_t count,
                                    const Vec4 *palette, uint32_t palette_count,
                                    float32_t width_pt) {
  if (count < 2u) {
    return;
  }
  const VkrUiPathConfig path = {
      .points = points,
      .colors = colors,
      .count = count,
      .palette = palette,
      .palette_count = palette_count,
      .width_pt = width_pt,
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = VKR_UI_PLACEMENT_DEFAULT;
  config.placement.column = config.placement.row = 0u;
  config.placement.justify = config.placement.align = VKR_UI_ALIGN_START;
  config.style.min_size_pt = config.style.max_size_pt =
      (Vec2){view->width_pt, height_pt};
  (void)vkr_ui_path(ui, id, &path, &config);
}

/* A label placed at `x_pt`, `y_pt` in its panel. */
vkr_internal void editor_flame_text(VkrUiSystem *ui, uint64_t slot,
                                    String8 text, float32_t x_pt,
                                    float32_t y_pt, VkrUiWidgetConfig config) {
  config.placement.column = config.placement.row = 0u;
  config.placement.justify = config.placement.align = VKR_UI_ALIGN_START;
  config.placement.margin_pt = (VkrUiEdges){y_pt, 0.0f, 0.0f, x_pt};
  (void)vkr_ui_push_id_u64(ui, slot);
  vkr_ui_label(ui, string8_lit("text"), text, &config);
  (void)vkr_ui_pop_id(ui);
}

/* Appends a closed rectangle outline to `points`, starting mid-edge so
   every corner joins. */
vkr_internal uint32_t editor_flame_outline(Vec2 *points, uint8_t *colors,
                                           uint32_t count, VkrUiRect rect,
                                           uint8_t color) {
  const float32_t right = rect.x + rect.width;
  const float32_t bottom = rect.y + rect.height;
  const float32_t middle = rect.x + rect.width * 0.5f;
  const Vec2 corners[] = {
      {middle, rect.y},
      {right, rect.y},
      {right, bottom},
      {rect.x, bottom},
      {rect.x, rect.y},
      {middle, rect.y},
      {VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY},
  };
  for (uint32_t i = 0u; i < ArrayCount(corners); ++i) {
    points[count + i] = corners[i];
    colors[count + i] = color;
  }
  return count + (uint32_t)ArrayCount(corners);
}

/* ------------------------------------------------------------------------ */
/* Header, ruler and footer                                                  */

vkr_internal VkrUiPlacement editor_flame_cell(uint32_t column, uint32_t row) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_CENTER};
}

/* The view's frames and length, frame steps and zoom. */
vkr_internal void editor_flame_header(VkrEditorUi *editor, VkrUiSystem *ui,
                                      const VkrRecordingPlayback *playback,
                                      const EditorFlameLayout *layout,
                                      uint64_t playhead) {
  VkrEditorRecordingFlame *flame = &editor->recording.flame;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack button = {.value = theme->control_height,
                             .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                button,
                                button,
                                {.value = 8.0f, .unit = VKR_UI_TRACK_PX},
                                button,
                                button,
                                button};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = editor_flame_cell(0u, 0u);
  header.columns = columns;
  header.column_count = ArrayCount(columns);
  header.rows = &row;
  header.row_count = 1u;
  header.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    return;
  }
  const float64_t length_ms =
      (float64_t)(flame->view_end_ns - flame->view_begin_ns) / 1.0e6;
  VkrUiWidgetConfig caption =
      vkr_editor_section_label_config(editor->heading_font);
  caption.placement = editor_flame_cell(0u, 0u);
  caption.placement.justify = VKR_UI_ALIGN_START;
  caption.tooltip = string8_lit(
      "Scroll over the tracks to zoom, with Shift to pan, or drag them; "
      "scroll over the thread names to see more tracks. Click a span to "
      "select it and move the playhead to its frame.");
  String8 text = string8_create_formatted(
      ui->frame_allocator, "Timeline  \xc2\xb7  %.2f ms", length_ms);
  if (layout->has_frames && layout->frame_first != layout->frame_last) {
    text = string8_create_formatted(
        ui->frame_allocator,
        "Timeline  \xc2\xb7  frames %llu\xe2\x80\x93%llu  \xc2\xb7  %.2f ms",
        (unsigned long long)layout->frame_first,
        (unsigned long long)layout->frame_last, length_ms);
  } else if (layout->has_frames) {
    text = string8_create_formatted(
        ui->frame_allocator,
        "Timeline  \xc2\xb7  frame %llu  \xc2\xb7  %.2f ms",
        (unsigned long long)layout->frame_first, length_ms);
  }
  vkr_ui_label(ui, string8_lit("caption"), text, &caption);

  VkrUiWidgetConfig back = vkr_editor_icon_button_config(
      1u, 0u, VKR_UI_ICON_CHEVRON_LEFT,
      string8_lit("Previous frame; the view moves with the playhead"));
  back.disabled = playhead == 0u;
  if (vkr_ui_button(ui, string8_lit("back"), (String8){0}, &back)) {
    editor_flame_step(editor, playback, playhead, false_v);
  }
  VkrUiWidgetConfig next = vkr_editor_icon_button_config(
      2u, 0u, VKR_UI_ICON_CHEVRON_RIGHT,
      string8_lit("Next frame; the view moves with the playhead"));
  next.disabled = playhead + 1u >= playback->frame_count;
  if (vkr_ui_button(ui, string8_lit("next"), (String8){0}, &next)) {
    editor_flame_step(editor, playback, playhead, true_v);
  }

  const int64_t length = flame->view_end_ns - flame->view_begin_ns;
  const int64_t middle = flame->view_begin_ns + length / 2;
  VkrUiWidgetConfig out = vkr_editor_icon_button_config(
      4u, 0u, VKR_UI_ICON_ZOOM_OUT, string8_lit("Zoom out"));
  if (vkr_ui_button(ui, string8_lit("out"), (String8){0}, &out)) {
    editor_flame_set_view(flame, playback, middle - length, length * 2);
  }
  VkrUiWidgetConfig fit = vkr_editor_icon_button_config(
      5u, 0u, VKR_UI_ICON_MAXIMIZE, string8_lit("Fit the playhead frame"));
  if (vkr_ui_button(ui, string8_lit("fit"), (String8){0}, &fit)) {
    editor_flame_show_frames(flame, playback, playhead, 0u);
  }
  VkrUiWidgetConfig in = vkr_editor_icon_button_config(
      6u, 0u, VKR_UI_ICON_ZOOM_IN, string8_lit("Zoom in"));
  if (vkr_ui_button(ui, string8_lit("in"), (String8){0}, &in)) {
    editor_flame_set_view(flame, playback, middle - length / 4, length / 2);
  }
  (void)vkr_ui_panel_end(ui);
}

/* Whether frame starts crowd the view: closer than EDITOR_FLAME_DENSE_PT
   apart, or more than the ruler keeps. Crowded ones draw only as short
   ticks on the ruler. */
vkr_internal bool8_t editor_flame_marks_dense(const EditorFlameLayout *layout,
                                              const EditorFlameView *view) {
  if (layout->mark_count == EDITOR_FLAME_MARK_MAX) {
    return true_v;
  }
  if (layout->mark_count < 2u) {
    return false_v;
  }
  const float64_t spread =
      (float64_t)(layout->mark_ns[layout->mark_count - 1u] -
                  layout->mark_ns[0]);
  return spread * view->pt_per_ns <
         (float64_t)(EDITOR_FLAME_DENSE_PT *
                     (float32_t)(layout->mark_count - 1u));
}

/* A round tick step: 1, 2 or 5 times a power of ten ns, at least
   `target`. */
vkr_internal int64_t editor_flame_tick_step(float64_t target) {
  int64_t step = 1;
  while ((float64_t)(step * 10) <= target) {
    step *= 10;
  }
  if ((float64_t)step < target) {
    step = (float64_t)(step * 2) >= target   ? step * 2
           : (float64_t)(step * 5) >= target ? step * 5
                                             : step * 10;
  }
  return step;
}

/* Frame starts on the upper line and times from the playhead frame's
   start on the lower one, with their ticks. */
vkr_internal void editor_flame_ruler(VkrUiSystem *ui,
                                     const EditorFlameLayout *layout,
                                     const EditorFlameView *view,
                                     uint64_t playhead, int64_t origin) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig ruler = vkr_ui_panel_config_default();
  ruler.placement = editor_flame_cell(0u, 1u);
  ruler.placement.align = VKR_UI_ALIGN_STRETCH;
  ruler.columns = &fill;
  ruler.column_count = 1u;
  ruler.rows = &fill;
  ruler.row_count = 1u;
  ruler.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("ruler"), &ruler)) {
    return;
  }
  const float32_t height = EDITOR_FLAME_RULER_PT;
  const float32_t upper = 2.0f;
  const float32_t lower = 16.0f;
  VkrUiWidgetConfig side =
      vkr_editor_text_config(theme->font_caption, theme->text_disabled);
  editor_flame_text(ui, 0u, string8_lit("Frame"), 0.0f, upper, side);
  editor_flame_text(ui, 1u, string8_lit("Time"), 0.0f, lower, side);

  Vec2 points[EDITOR_FLAME_MARK_MAX * 6u + 3u];
  uint8_t colors[ArrayCount(points)];
  uint32_t count = 0u;
  points[count] = (Vec2){EDITOR_FLAME_LABEL_PT, height - 0.5f};
  colors[count++] = 0u;
  points[count] = (Vec2){view->width_pt, height - 0.5f};
  colors[count++] = 0u;
  points[count] = (Vec2){VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY};
  colors[count++] = 0u;

  /* Frame starts: a tick, full height unless frames crowd the view, and
     the frame's number where it has room, the playhead frame's in the
     accent. */
  const float32_t mark_bottom =
      editor_flame_marks_dense(layout, view) ? 6.0f : height;
  VkrUiWidgetConfig frame_text =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  float32_t last_name = -1.0e9f;
  for (uint32_t i = 0u; i < layout->mark_count; ++i) {
    const float32_t x =
        EDITOR_FLAME_LABEL_PT +
        (float32_t)((float64_t)(layout->mark_ns[i] - view->begin) *
                    view->pt_per_ns);
    points[count] = (Vec2){x, 0.0f};
    colors[count++] = 1u;
    points[count] = (Vec2){x, mark_bottom};
    colors[count++] = 1u;
    points[count] = (Vec2){VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY};
    colors[count++] = 0u;
    if (x - last_name < EDITOR_FLAME_FRAME_NAME_PT) {
      continue;
    }
    last_name = x;
    frame_text.style.text_color = layout->mark_frame[i] == playhead
                                      ? theme->accent_hover
                                      : theme->text_secondary;
    editor_flame_text(
        ui, 16u + i,
        string8_create_formatted(ui->frame_allocator, "Frame %llu",
                                 (unsigned long long)layout->mark_frame[i]),
        x + 4.0f, upper, frame_text);
  }

  /* Times: ticks at a round step from the playhead frame's start. */
  const int64_t step =
      editor_flame_tick_step(EDITOR_FLAME_TICK_PT / view->pt_per_ns);
  int64_t k = (view->begin - origin) / step;
  while (origin + k * step < view->begin) {
    ++k;
  }
  const int32_t decimals = step >= 1000000  ? 0
                           : step >= 100000 ? 1
                           : step >= 10000  ? 2
                                            : 3;
  VkrUiWidgetConfig tick_text =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  for (uint32_t i = 0u; i < EDITOR_FLAME_MARK_MAX; ++i, ++k) {
    const int64_t at = origin + k * step;
    if (at >= view->end) {
      break;
    }
    const float32_t x =
        EDITOR_FLAME_LABEL_PT +
        (float32_t)((float64_t)(at - view->begin) * view->pt_per_ns);
    points[count] = (Vec2){x, height - 5.0f};
    colors[count++] = 0u;
    points[count] = (Vec2){x, height};
    colors[count++] = 0u;
    points[count] = (Vec2){VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY};
    colors[count++] = 0u;
    const float64_t ms = (float64_t)(at - origin) / 1.0e6;
    editor_flame_text(ui, 16u + EDITOR_FLAME_MARK_MAX + i,
                      at == origin
                          ? string8_lit("0 ms")
                          : string8_create_formatted(ui->frame_allocator,
                                                     "%+.*f ms", decimals, ms),
                      x + 4.0f, lower, tick_text);
  }

  const Vec4 palette[] = {vkr_ui_color_alpha(theme->text_secondary, 0.45f),
                          vkr_ui_color_alpha(theme->accent_hover, 0.7f)};
  editor_flame_path(ui, string8_lit("ticks"), view, height, points, colors,
                    count, palette, ArrayCount(palette), 1.0f);
  (void)vkr_ui_panel_end(ui);
}

/* What the view holds and what it left out. */
vkr_internal void editor_flame_footer(VkrUiSystem *ui,
                                      const VkrRecordingPlayback *playback,
                                      const EditorFlameLayout *layout,
                                      const EditorFlameDraw *draw) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char notes[256] = {0};
  int32_t length = 0;
  if (draw->merged > 0u) {
    length += string_format(notes + length, sizeof(notes) - (uint64_t)length,
                            "  \xc2\xb7  %u narrow spans merged", draw->merged);
  }
  if (draw->capped && length >= 0 && length < (int32_t)sizeof(notes)) {
    length += string_format(notes + length, sizeof(notes) - (uint64_t)length,
                            "  \xc2\xb7  span limit reached; zoom in");
  }
  if (layout->gpu_frames > 0u && layout->passes == 0u && length >= 0 &&
      length < (int32_t)sizeof(notes)) {
    length += string_format(notes + length, sizeof(notes) - (uint64_t)length,
                            "  \xc2\xb7  no timed GPU passes here (tiled GPUs "
                            "time 1 frame in 8)");
  }
  if (playback->zone_count == 0u && length >= 0 &&
      length < (int32_t)sizeof(notes)) {
    (void)string_format(notes + length, sizeof(notes) - (uint64_t)length,
                        "  \xc2\xb7  no CPU zones recorded");
  }
  VkrUiWidgetConfig footer =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  footer.placement = editor_flame_cell(0u, 3u);
  footer.placement.justify = VKR_UI_ALIGN_START;
  vkr_ui_label(ui, string8_lit("footer"),
               string8_create_formatted(
                   ui->frame_allocator,
                   "%u zones  \xc2\xb7  %u GPU frames  \xc2\xb7  %u GPU "
                   "passes%s",
                   layout->zones, layout->gpu_frames, layout->passes, notes),
               &footer);
}

/* ------------------------------------------------------------------------ */
/* Tracks                                                                    */

/* The playhead frame's band, frame starts and the lines between tracks. */
vkr_internal void editor_flame_grid(VkrUiSystem *ui,
                                    const VkrRecordingPlayback *playback,
                                    const EditorFlameLayout *layout,
                                    const EditorFlameView *view,
                                    float32_t scroll_pt, uint64_t playhead) {
  const VkrUiTheme *theme = vkr_ui_theme();
  /* The band and frame lines stop where the tracks do. */
  const float32_t bottom = Min(view->height_pt, layout->content_pt - scroll_pt);
  int64_t begin = 0;
  int64_t end = 0;
  if (bottom > 0.0f &&
      editor_flame_frame_range(playback, playhead, &begin, &end) &&
      end > view->begin && begin < view->end) {
    const float32_t x0 =
        (float32_t)((float64_t)(Max(begin, view->begin) - view->begin) *
                    view->pt_per_ns);
    const float32_t x1 =
        (float32_t)((float64_t)(Min(end, view->end) - view->begin) *
                    view->pt_per_ns);
    const Vec2 band[] = {
        {EDITOR_FLAME_LABEL_PT + x0, bottom * 0.5f},
        {EDITOR_FLAME_LABEL_PT + x1, bottom * 0.5f},
    };
    const Vec4 band_color = vkr_ui_color_alpha(theme->accent, 0.04f);
    editor_flame_path(ui, string8_lit("band"), view, view->height_pt, band,
                      NULL, ArrayCount(band), &band_color, 1u, bottom);
  }

  Vec2 points[(EDITOR_FLAME_MARK_MAX + EDITOR_FLAME_TRACK_MAX) * 3u];
  uint8_t colors[ArrayCount(points)];
  uint32_t count = 0u;
  const uint32_t marks =
      editor_flame_marks_dense(layout, view) ? 0u : layout->mark_count;
  for (uint32_t i = 0u; i < marks && bottom > 0.0f; ++i) {
    const float32_t x =
        EDITOR_FLAME_LABEL_PT +
        (float32_t)((float64_t)(layout->mark_ns[i] - view->begin) *
                    view->pt_per_ns);
    points[count] = (Vec2){x, 0.0f};
    colors[count++] = 1u;
    points[count] = (Vec2){x, bottom};
    colors[count++] = 1u;
    points[count] = (Vec2){VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY};
    colors[count++] = 0u;
  }
  for (uint32_t i = 0u; i + 1u < layout->track_count; ++i) {
    const EditorFlameTrack *track = &layout->tracks[i + 1u];
    const float32_t y = vkr_floor_f32(track->top_pt - scroll_pt) + 0.5f;
    if (y <= 0.0f || y >= view->height_pt) {
      continue;
    }
    points[count] = (Vec2){0.0f, y};
    colors[count++] = 0u;
    points[count] = (Vec2){view->width_pt, y};
    colors[count++] = 0u;
    points[count] = (Vec2){VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY};
    colors[count++] = 0u;
  }
  const Vec4 palette[] = {theme->separator,
                          vkr_ui_color_alpha(theme->accent_hover, 0.35f)};
  editor_flame_path(ui, string8_lit("grid"), view, view->height_pt, points,
                    colors, count, palette, ArrayCount(palette), 1.0f);
}

/* The thread and GPU names beside their tracks' first rows. */
vkr_internal void editor_flame_track_names(VkrEditorUi *editor, VkrUiSystem *ui,
                                           const EditorFlameLayout *layout,
                                           const EditorFlameView *view,
                                           float32_t scroll_pt) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t line =
      vkr_ui_code_cell_size(ui, editor->mono_font, theme->font_caption).y;
  VkrUiWidgetConfig name =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  name.text.font = editor->heading_font;
  for (uint32_t i = 0u; i < layout->track_count; ++i) {
    const EditorFlameTrack *track = &layout->tracks[i];
    /* The heading face sits a point lower than the mono line it is
       centred by. */
    const float32_t top = track->top_pt - scroll_pt +
                          EDITOR_FLAME_TRACK_PAD_PT +
                          (EDITOR_FLAME_ROW_PT - line) * 0.5f - 1.0f;
    if (top + EDITOR_FLAME_ROW_PT <= 0.0f || top >= view->height_pt) {
      continue;
    }
    editor_flame_text(ui, 4096u + i, editor_flame_track_name(ui, track),
                      theme->space_sm, top, name);
  }
}

/* Zoom and pan with the wheel, pan with a drag, and select with a click. */
vkr_internal void editor_flame_input(VkrEditorUi *editor, VkrUiSystem *ui,
                                     const VkrRecordingPlayback *playback,
                                     const EditorFlameView *view,
                                     const EditorFlameDraw *draw,
                                     VkrUiPlotResult result) {
  VkrEditorRecordingFlame *flame = &editor->recording.flame;
  const int64_t length = view->end - view->begin;
  if (result.hovered != VKR_UI_PLOT_NONE && ui->mouse_wheel != 0) {
    const bool8_t shift = input_is_key_down(ui->input, KEY_LSHIFT) ||
                          input_is_key_down(ui->input, KEY_RSHIFT);
    if (shift) {
      editor_flame_set_view(flame, playback,
                            view->begin - (int64_t)ui->mouse_wheel * length / 8,
                            length);
    } else {
      /* The time under the pointer stays under it. */
      const float64_t factor =
          ui->mouse_wheel > 0 ? EDITOR_FLAME_ZOOM : 1.0 / EDITOR_FLAME_ZOOM;
      const float64_t offset =
          (float64_t)(draw->pointer_pt.x - EDITOR_FLAME_LABEL_PT) /
          view->pt_per_ns;
      const int64_t anchor = view->begin + (int64_t)offset;
      editor_flame_set_view(flame, playback,
                            anchor - (int64_t)(offset * factor),
                            (int64_t)((float64_t)length * factor));
    }
  }

  const float32_t scale = Max(ui->content_scale, 0.001f);
  if (result.brushing) {
    const Vec2 mouse = {(float32_t)ui->mouse_x, (float32_t)ui->mouse_y};
    if (!flame->dragging) {
      int32_t press_x = ui->mouse_x;
      int32_t press_y = ui->mouse_y;
      input_get_button_press_position(ui->input, BUTTON_LEFT, &press_x,
                                      &press_y);
      flame->drag_px = (Vec2){(float32_t)press_x, (float32_t)press_y};
      flame->dragging = true_v;
    }
    const float32_t dx = (mouse.x - flame->drag_px.x) / scale;
    const float32_t dy = (mouse.y - flame->drag_px.y) / scale;
    editor_flame_set_view(flame, playback,
                          flame->view_begin_ns -
                              (int64_t)((float64_t)dx / view->pt_per_ns),
                          flame->view_end_ns - flame->view_begin_ns);
    flame->scroll_pt -= dy;
    flame->drag_px = mouse;
  } else {
    flame->dragging = false_v;
  }

  if (result.clicked == VKR_UI_PLOT_NONE) {
    return;
  }
  flame->selected = draw->hovered;
  if (!draw->hovered) {
    return;
  }
  flame->selected_gpu = draw->hovered_gpu;
  flame->selected_index = draw->hovered_index;
  const uint64_t frame = draw->hovered_gpu
                             ? playback->gpu_spans[draw->hovered_index].frame
                             : playback->zones[draw->hovered_index].frame;
  /* The span stays in view, so the view does not follow the seek. */
  editor_flame_seek(editor, frame);
}

/* The hovered span's name, length, frame and thread. */
vkr_internal String8 editor_flame_tooltip(VkrUiSystem *ui,
                                          const VkrRecordingPlayback *playback,
                                          const EditorFlameDraw *draw) {
  if (!draw->hovered) {
    return (String8){0};
  }
  const char *name = NULL;
  uint64_t begin = 0u;
  uint64_t end = 0u;
  uint64_t frame = 0u;
  if (draw->hovered_gpu) {
    const VkrRecordingPlaybackGpuSpan *span =
        &playback->gpu_spans[draw->hovered_index];
    name = editor_flame_gpu_name(playback, span);
    begin = span->begin_ns;
    end = span->end_ns;
    frame = span->frame;
  } else {
    const VkrRecordingPlaybackZone *zone =
        &playback->zones[draw->hovered_index];
    name = editor_flame_zone_name(playback, zone);
    begin = zone->begin_ns;
    end = zone->end_ns;
    frame = zone->frame;
  }
  /* Where it ran: "Main thread", "Worker 2", or the GPU. */
  const EditorFlameTrack *track = &draw->layout->tracks[draw->hovered_track];
  const String8 where = draw->hovered_gpu ? string8_lit("GPU")
                                          : editor_flame_track_name(ui, track);
  const bool8_t named_thread = !draw->hovered_gpu && track->thread < 2u;
  return string8_create_formatted(
      ui->frame_allocator,
      "%s  \xc2\xb7  %.3f ms  \xc2\xb7  frame %llu  \xc2\xb7  %.*s%s", name,
      (float64_t)(end - begin) / 1.0e6, (unsigned long long)frame,
      (int32_t)where.length, (const char *)where.str,
      named_thread ? " thread" : "");
}

/* Transparent plots over each track, and one over the room below them,
   take the pointer, so a span's tooltip shows under its track; the
   pointer's own position finds the span. Their results combine. */
vkr_internal VkrUiPlotResult editor_flame_input_plots(
    VkrUiSystem *ui, const VkrRecordingPlayback *playback,
    const VkrEditorRecordingFlame *flame, const EditorFlameView *view,
    const EditorFlameDraw *draw) {
  const EditorFlameLayout *layout = draw->layout;
  const VkrUiPlotSeries none = {.values = s_flame_input_values};
  const VkrUiPlotConfig plot = {
      .sample_count = EDITOR_FLAME_INPUT_SAMPLES,
      .y_min = 0.0f,
      .y_max = 1.0f,
      .bars = &none,
      .bar_count = 1u,
      .cursor = VKR_UI_PLOT_NONE,
      .range_first = VKR_UI_PLOT_NONE,
      .range_last = VKR_UI_PLOT_NONE,
  };
  const String8 tooltip = editor_flame_tooltip(ui, playback, draw);
  VkrUiPlotResult combined = {
      .hovered = VKR_UI_PLOT_NONE,
      .clicked = VKR_UI_PLOT_NONE,
      .brush_first = VKR_UI_PLOT_NONE,
      .brush_last = VKR_UI_PLOT_NONE,
  };
  for (uint32_t i = 0u; i <= layout->track_count; ++i) {
    /* Track i, then the room below the last track. */
    float32_t top = layout->content_pt - draw->scroll_pt;
    float32_t bottom = view->height_pt;
    if (i < layout->track_count) {
      const EditorFlameTrack *track = &layout->tracks[i];
      top = track->top_pt - draw->scroll_pt;
      bottom = top + EDITOR_FLAME_TRACK_PAD_PT * 2.0f +
               (float32_t)track->rows * EDITOR_FLAME_ROW_PT;
    }
    top = Max(top, 0.0f);
    bottom = Min(bottom, view->height_pt);
    if (bottom - top < 1.0f) {
      continue;
    }
    VkrUiWidgetConfig input = vkr_ui_widget_config_default();
    input.placement = editor_flame_cell(0u, 0u);
    input.placement.justify = input.placement.align = VKR_UI_ALIGN_START;
    input.placement.margin_pt =
        (VkrUiEdges){top, 0.0f, 0.0f, EDITOR_FLAME_LABEL_PT};
    input.style.min_size_pt = input.style.max_size_pt =
        (Vec2){view->area_pt, bottom - top};
    const bool8_t hovered = draw->hovered && draw->hovered_track == i;
    input.tooltip = hovered ? tooltip : (String8){0};
    input.cursor = flame->dragging ? VKR_WINDOW_CURSOR_GRABBING
                   : hovered       ? VKR_WINDOW_CURSOR_HAND
                                   : VKR_WINDOW_CURSOR_ARROW;
    (void)vkr_ui_push_id_u64(ui, i);
    const VkrUiPlotResult result =
        vkr_ui_plot(ui, string8_lit("input"), &plot, &input);
    (void)vkr_ui_pop_id(ui);
    if (result.hovered != VKR_UI_PLOT_NONE) {
      combined.hovered = result.hovered;
    }
    if (result.clicked != VKR_UI_PLOT_NONE) {
      combined.clicked = result.clicked;
    }
    combined.brushing = combined.brushing || result.brushing;
  }
  return combined;
}

/* The tracks: names, the playhead frame's band, the spans and their
   names, the hover and selection outlines, and the plots taking input. */
vkr_internal void
editor_flame_tracks(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                    const VkrRecordingPlayback *playback,
                    const EditorFlameLayout *layout, EditorFlameView *view,
                    EditorFlameDraw *draw, uint64_t playhead) {
  VkrEditorRecordingFlame *flame = &editor->recording.flame;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = Max(ui->content_scale, 0.001f);

  /* The panel's last rectangle places the pointer and sizes the tracks. */
  VkrUiRect rect_px = {0};
  const VkrUiId panel_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("tracks"));
  const bool8_t placed =
      vkr_ui_widget_rect(ui, panel_id, &rect_px) && rect_px.height > 0.0f;
  if (placed) {
    view->height_pt = rect_px.height / scale;
  }
  const Vec2 pointer = {((float32_t)ui->mouse_x - rect_px.x) / scale,
                        ((float32_t)ui->mouse_y - rect_px.y) / scale};
  const bool8_t over = placed && !ui->mouse_captured &&
                       ui->mouse_input_layer == ui->input_layer &&
                       pointer.x >= 0.0f && pointer.x < view->width_pt &&
                       pointer.y >= 0.0f && pointer.y < view->height_pt;
  /* The wheel over the names scrolls the tracks. */
  if (over && pointer.x < EDITOR_FLAME_LABEL_PT && ui->mouse_wheel != 0) {
    flame->scroll_pt -= (float32_t)ui->mouse_wheel * 40.0f;
  }
  flame->scroll_pt = Clamp(flame->scroll_pt, 0.0f,
                           Max(0.0f, layout->content_pt - view->height_pt));

  *draw = (EditorFlameDraw){
      .layout = layout,
      .view = view,
      .scroll_pt = flame->scroll_pt,
      .allocator = ui->frame_allocator,
      .cell_pt =
          vkr_ui_code_cell_size(ui, editor->mono_font, EDITOR_FLAME_NAME_PT),
      .point_capacity = EDITOR_FLAME_SPAN_MAX * 3u,
      .pointer_in = over && pointer.x >= EDITOR_FLAME_LABEL_PT,
      .pointer_pt = pointer,
      .has_selection = flame->selected,
      .selected_gpu = flame->selected_gpu,
      .selected_index = flame->selected_index,
  };
  draw->points = vkr_allocator_alloc(ui->frame_allocator,
                                     sizeof(Vec2) * draw->point_capacity,
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  draw->colors = vkr_allocator_alloc(ui->frame_allocator, draw->point_capacity,
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!draw->points || !draw->colors) {
    draw->point_capacity = 0u;
  }

  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig tracks = vkr_ui_panel_config_default();
  tracks.placement = editor_flame_cell(0u, 2u);
  tracks.placement.align = VKR_UI_ALIGN_STRETCH;
  tracks.columns = &fill;
  tracks.column_count = 1u;
  tracks.rows = &fill;
  tracks.row_count = 1u;
  tracks.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("tracks"), &tracks)) {
    return;
  }

  VkrUiWidgetConfig lane = vkr_ui_widget_config_default();
  lane.style.background_color = vkr_ui_color_alpha(theme->window, 0.6f);
  lane.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  lane.style.min_size_pt = lane.style.max_size_pt =
      (Vec2){view->area_pt, view->height_pt};
  editor_flame_text(ui, 0u, (String8){0}, EDITOR_FLAME_LABEL_PT, 0.0f, lane);
  editor_flame_grid(ui, playback, layout, view, flame->scroll_pt, playhead);

  if (draw->point_capacity > 0u) {
    editor_flame_spans(playback, draw);
    editor_flame_path(ui, string8_lit("spans"), view, view->height_pt,
                      draw->points, draw->colors, draw->point_count,
                      s_flame_palette, EDITOR_FLAME_PALETTE,
                      EDITOR_FLAME_ROW_PT - 2.0f);
  }
  VkrUiWidgetConfig name =
      vkr_editor_text_config(EDITOR_FLAME_NAME_PT, s_flame_name_color);
  name.text.font = editor->mono_font;
  for (uint32_t i = 0u; i < draw->name_count; ++i) {
    editor_flame_text(ui, 1u + i, draw->names[i].text, draw->names[i].x_pt,
                      draw->names[i].y_pt, name);
  }

  Vec2 outline[14];
  uint8_t outline_colors[ArrayCount(outline)];
  uint32_t outline_count = 0u;
  if (draw->hovered) {
    outline_count = editor_flame_outline(outline, outline_colors, outline_count,
                                         draw->hovered_rect, 0u);
  }
  if (draw->selected_shown) {
    outline_count = editor_flame_outline(outline, outline_colors, outline_count,
                                         draw->selected_rect, 1u);
  }
  const Vec4 outline_palette[] = {vkr_ui_color_alpha(theme->text, 0.55f),
                                  theme->text};
  editor_flame_path(ui, string8_lit("outline"), view, view->height_pt, outline,
                    outline_colors, outline_count, outline_palette,
                    ArrayCount(outline_palette), 1.5f);
  editor_flame_track_names(editor, ui, layout, view, flame->scroll_pt);

  const VkrUiPlotResult result =
      editor_flame_input_plots(ui, playback, flame, view, draw);
  (void)vkr_ui_panel_end(ui);
  editor_flame_input(editor, ui, playback, view, draw, result);
}

/* ------------------------------------------------------------------------ */
/* Window                                                                    */

/* A centred icon, title and line for a window with nothing to show. */
vkr_internal void editor_flame_empty(VkrEditorUi *editor, VkrUiSystem *ui,
                                     String8 title, String8 text) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack rows[] = {auto_track, auto_track, auto_track};
  VkrUiPanelConfig body = vkr_ui_panel_config_default();
  body.placement = editor_flame_cell(0u, 0u);
  body.placement.align = VKR_UI_ALIGN_STRETCH;
  body.columns = &fill;
  body.column_count = 1u;
  body.rows = &fill;
  body.row_count = 1u;
  if (!vkr_ui_panel_begin(ui, string8_lit("timeline.empty"), &body)) {
    return;
  }
  VkrUiPanelConfig stack = vkr_ui_panel_config_default();
  stack.placement = editor_flame_cell(0u, 0u);
  stack.placement.justify = stack.placement.align = VKR_UI_ALIGN_CENTER;
  stack.columns = &auto_track;
  stack.column_count = 1u;
  stack.rows = rows;
  stack.row_count = ArrayCount(rows);
  stack.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("stack"), &stack)) {
    VkrUiWidgetConfig icon = vkr_ui_widget_config_default();
    icon.placement = editor_flame_cell(0u, 0u);
    icon.placement.justify = VKR_UI_ALIGN_CENTER;
    icon.icon = VKR_UI_ICON_LAYERS;
    icon.icon_size_pt = 32.0f;
    icon.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("icon"), (String8){0}, &icon);
    VkrUiWidgetConfig heading =
        vkr_editor_text_config(theme->font_body, theme->text);
    heading.placement = editor_flame_cell(0u, 1u);
    heading.placement.justify = VKR_UI_ALIGN_CENTER;
    heading.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("title"), title, &heading);
    VkrUiWidgetConfig line =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    line.placement = editor_flame_cell(0u, 2u);
    line.placement.justify = VKR_UI_ALIGN_CENTER;
    line.text.layout.word_wrap = true_v;
    line.text.layout.max_width = 380.0f;
    line.text.layout.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("text"), text, &line);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_recording_flame_build(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  const VkrRecordingPlayback *playback =
      frame->recording ? frame->recording->playback : NULL;
  if (!playback || playback->frame_count == 0u) {
    editor_flame_empty(editor, ui, string8_lit("No recording open"),
                       string8_lit("Open a recording to see its timeline."));
    return;
  }
  if (!playback->has_timeline ||
      (playback->zone_count == 0u && playback->gpu_span_count == 0u)) {
    editor_flame_empty(
        editor, ui, string8_lit("No CPU zones"),
        string8_lit("This recording has no CPU zones; record with zones "
                    "(rec.start zones, or All measurements / Keep last "
                    "30 s)."));
    return;
  }

  VkrEditorRecordingFlame *flame = &editor->recording.flame;
  const uint64_t playhead =
      vkr_recording_playback_frame_at(playback, playback->time);
  editor_flame_reset(flame, playback, playhead);
  editor_flame_follow(flame, playback, playhead);

  /* The canvas from the body's size; the tracks' height comes from their
     last layout once they have one. */
  const float32_t scale = Max(ui->content_scale, 0.001f);
  EditorFlameView view = {
      .begin = flame->view_begin_ns,
      .end = flame->view_end_ns,
      .width_pt = bounds.width / scale - EDITOR_FLAME_PAD_X_PT * 2.0f,
      .height_pt = bounds.height / scale - EDITOR_FLAME_PAD_Y_PT * 2.0f -
                   EDITOR_FLAME_HEADER_PT - EDITOR_FLAME_RULER_PT -
                   EDITOR_FLAME_FOOTER_PT - EDITOR_FLAME_GAP_PT * 3.0f,
      .pixel_pt = 1.0f / scale,
  };
  view.width_pt = Max(view.width_pt, EDITOR_FLAME_LABEL_PT + 1.0f);
  view.height_pt = Max(view.height_pt, 1.0f);
  view.area_pt = view.width_pt - EDITOR_FLAME_LABEL_PT;
  view.pt_per_ns =
      (float64_t)view.area_pt / (float64_t)Max(view.end - view.begin, 1);
  EditorFlameLayout layout;
  editor_flame_layout(playback, flame, &layout);
  int64_t origin = view.begin;
  int64_t unused = 0;
  (void)editor_flame_frame_range(playback, playhead, &origin, &unused);

  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = EDITOR_FLAME_HEADER_PT, .unit = VKR_UI_TRACK_PX},
      {.value = EDITOR_FLAME_RULER_PT, .unit = VKR_UI_TRACK_PX},
      fill,
      {.value = EDITOR_FLAME_FOOTER_PT, .unit = VKR_UI_TRACK_PX},
  };
  VkrUiPanelConfig body = vkr_ui_panel_config_default();
  body.placement = editor_flame_cell(0u, 0u);
  body.placement.align = VKR_UI_ALIGN_STRETCH;
  body.columns = &fill;
  body.column_count = 1u;
  body.rows = rows;
  body.row_count = ArrayCount(rows);
  body.style.padding_pt =
      (VkrUiEdges){EDITOR_FLAME_PAD_Y_PT, EDITOR_FLAME_PAD_X_PT,
                   EDITOR_FLAME_PAD_Y_PT, EDITOR_FLAME_PAD_X_PT};
  body.style.gap_pt = EDITOR_FLAME_GAP_PT;
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("timeline.view"), &body)) {
    return;
  }
  editor_flame_header(editor, ui, playback, &layout, playhead);
  editor_flame_ruler(ui, &layout, &view, playhead, origin);
  EditorFlameDraw draw;
  editor_flame_tracks(editor, frame, playback, &layout, &view, &draw, playhead);
  editor_flame_footer(ui, playback, &layout, &draw);
  (void)vkr_ui_panel_end(ui);
}
