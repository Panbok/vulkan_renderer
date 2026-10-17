/**
 * @file vkr_ui_draw.h
 * @brief Allocation-free UI draw commands and single-stream geometry builder.
 */
#pragma once

#include "core/ui/vkr_ui_types.h"
#include "vkr_ui_draw_types.h"

#define VKR_UI_CLIP_STACK_CAPACITY 32u

/** One rectangle of a rect run, in pixels from the run's origin. */
typedef struct VkrUiDrawRect {
  VkrUiRect rect;
  /** Linear color, multiplied by the command's `color`. */
  Vec4 color;
} VkrUiDrawRect;

/** Batched geometry of a draw command: one command draws many primitives,
 * such as the bars of a chart. */
typedef enum VkrUiDrawRun {
  VKR_UI_DRAW_RUN_NONE = 0,
  /** `run_count` flat rectangles, each with its own color. */
  VKR_UI_DRAW_RUN_RECTS,
  /** An open polyline through `run_count` points, stroked `run_width_px`
   * wide in `color` with a one-device-pixel alpha fringe on both sides. */
  VKR_UI_DRAW_RUN_POLYLINE,
} VkrUiDrawRun;

typedef struct VkrUiDrawCommand {
  VkrUiRect rect_px;
  /** Prepared convex icon polygon: inner then outer coverage ring, Y-down.
   *
   * Three or four corners per ring; zero selects the ordinary rectangle. */
  Vec2 corners_px[8];
  uint32_t corner_count;
  VkrUiRect clip_rect_px;
  /** Top-left texture coordinates: u0, v0, u1, v1. */
  Vec4 uv_rect;
  Vec4 color;
  /** SDF box data; see VKR_UI_DRAW_MODE_BOX. */
  Vec4 corner_radius_px;
  Vec4 border_color;
  float32_t border_px;
  float32_t softness_px;
  VkrUiTextureRef texture;
  VkrUiDrawMode mode;
  float32_t screen_px_range;
  /** Normalized atlas distance range used by the MTSDF shader. */
  Vec2 sdf_unit_range;
  /** A plain quad's fill blends `color` into `border_color` across it:
   * VKR_UI_DRAW_GRADIENT_*. Vertex colors interpolate in linear space. */
  uint32_t gradient;
  /** Batched geometry, VKR_UI_DRAW_RUN_*. Rectangles and points are in
   * pixels from rect_px's origin, multiplied by run_scale, so moving or
   * scaling rect_px moves the run with it. The run draws only inside
   * rect_px, which is also its damage bound. The data is borrowed until the
   * draw list is built. */
  uint32_t run;
  uint32_t run_count;
  union {
    const VkrUiDrawRect *run_rects;
    const Vec2 *run_points;
  };
  float32_t run_scale;
  /** Polyline stroke width in pixels before run_scale. */
  float32_t run_width_px;
  /** Fingerprint of the borrowed run data; tile damage hashes it in place of
   * the data, so it must change whenever the data does. */
  uint64_t run_hash;
} VkrUiDrawCommand;

enum {
  VKR_UI_DRAW_GRADIENT_NONE = 0,
  VKR_UI_DRAW_GRADIENT_HORIZONTAL, /* Left to right. */
  VKR_UI_DRAW_GRADIENT_VERTICAL,   /* Top to bottom. */
};

typedef struct VkrUiDrawBuffer {
  VkrUiDrawCommand *commands;
  uint32_t command_count;
  uint32_t command_capacity;
  uint32_t dropped_command_count;
  VkrUiRect clip_stack[VKR_UI_CLIP_STACK_CAPACITY];
  uint32_t clip_count;
} VkrUiDrawBuffer;

typedef enum VkrUiDrawBuildStatus {
  VKR_UI_DRAW_BUILD_OK = 0,
  VKR_UI_DRAW_BUILD_TRUNCATED,
  VKR_UI_DRAW_BUILD_INVALID,
} VkrUiDrawBuildStatus;

typedef struct VkrUiDrawBuildResult {
  VkrUiDrawBuildStatus status;
  uint32_t dropped_command_count;
} VkrUiDrawBuildResult;

bool8_t vkr_ui_draw_buffer_begin(VkrUiDrawBuffer *buffer,
                                 VkrUiDrawCommand *commands,
                                 uint32_t command_capacity,
                                 VkrUiRect target_rect_px);
bool8_t vkr_ui_draw_buffer_push_clip(VkrUiDrawBuffer *buffer,
                                     VkrUiRect clip_rect_px);
bool8_t vkr_ui_draw_buffer_pop_clip(VkrUiDrawBuffer *buffer);
bool8_t vkr_ui_draw_buffer_push(VkrUiDrawBuffer *buffer,
                                VkrUiDrawCommand command);
bool8_t vkr_ui_draw_buffer_solid(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                 Vec4 color);
/** A plain quad blending `start` into `end`, left to right or top to bottom;
 * colors are linear like the other commands'. */
bool8_t vkr_ui_draw_buffer_gradient(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                    Vec4 start, Vec4 end, bool8_t vertical);
/** Convex icon quad with a one-device-pixel alpha fringe centered on its
 * edges.
 * Repeat the last corner for a triangle. Edges must support a 0.5px
 * inset. */
bool8_t vkr_ui_draw_buffer_polygon(VkrUiDrawBuffer *buffer,
                                   const Vec2 corners_px[4], Vec4 color);
bool8_t vkr_ui_draw_buffer_image(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                 Vec4 uv_rect, Vec4 color,
                                 VkrUiTextureRef texture);
bool8_t vkr_ui_draw_buffer_text_quad(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                     Vec4 uv_rect, Vec4 color,
                                     VkrUiTextureRef texture,
                                     VkrUiDrawMode text_mode,
                                     float32_t screen_px_range,
                                     Vec2 sdf_unit_range);
/** `count` flat rectangles in one command, placed from `bounds_px`'s origin
 * and clipped to it. `rects` stays borrowed until the draw list is built;
 * `content_hash` must change whenever the rectangles do. */
bool8_t vkr_ui_draw_buffer_rects(VkrUiDrawBuffer *buffer, VkrUiRect bounds_px,
                                 const VkrUiDrawRect *rects, uint32_t count,
                                 uint64_t content_hash);
/** An antialiased open polyline through `count` (at least two) points, placed
 * from `bounds_px`'s origin and clipped to it. A joint follows the mitre of
 * its two segments, limited to four times the width. `points` stays borrowed
 * until the draw list is built; `content_hash` must change whenever the
 * points do. Colors are linear. */
bool8_t vkr_ui_draw_buffer_polyline(VkrUiDrawBuffer *buffer,
                                    VkrUiRect bounds_px, const Vec2 *points,
                                    uint32_t count, float32_t width_px,
                                    Vec4 color, uint64_t content_hash);
/** Rounded box with an optional inner border. A positive softness feathers
 * the edge over that many pixels on each side for shadows and glows; the
 * emitted quad grows to cover the feather. Colors are linear. */
bool8_t vkr_ui_draw_buffer_box(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                               Vec4 color, Vec4 corner_radius_px,
                               Vec4 border_color, float32_t border_px,
                               float32_t softness_px);

VkrUiDrawBuildResult vkr_ui_draw_build(const VkrUiDrawBuffer *buffer,
                                       uint32_t target_width,
                                       uint32_t target_height,
                                       VkrUiDrawOutput *out_draws);
