#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_math.h"

/*
 * Debug drawing
 * (docs/adr/127-crash-reports-gpu-capture-and-graph-description.md):
 * world-space lines, boxes, spheres, frusta and labels that tools and agents
 * add and the editor draws over the Scene, on top of the image without depth.
 * Fixed storage owned by the runtime; one thread (the frame loop) adds,
 * expires and reads it. Each shape belongs to a named group, so a tool clears
 * only what it drew, and lives until its expiry time or until cleared.
 */

#define VKR_DEBUG_DRAW_SEGMENT_MAX 8192u
#define VKR_DEBUG_DRAW_LABEL_MAX 64u
#define VKR_DEBUG_DRAW_LABEL_TEXT 48u
/* Segments of each circle of a sphere. */
#define VKR_DEBUG_DRAW_CIRCLE_SEGMENTS 24u

/* The colours a shape takes; the editor maps them to its theme. */
typedef enum VkrDebugDrawColor {
  VKR_DEBUG_DRAW_RED = 0,
  VKR_DEBUG_DRAW_GREEN,
  VKR_DEBUG_DRAW_BLUE,
  VKR_DEBUG_DRAW_YELLOW,
  VKR_DEBUG_DRAW_CYAN,
  VKR_DEBUG_DRAW_MAGENTA,
  VKR_DEBUG_DRAW_WHITE,
  VKR_DEBUG_DRAW_ORANGE,
  VKR_DEBUG_DRAW_COLOR_COUNT,
} VkrDebugDrawColor;

/* How long a shape lasts: the absolute time (vkr_platform_get_absolute_time)
   it expires at, zero until cleared; its group; and its colour. */
typedef struct VkrDebugDrawStyle {
  float64_t expires;
  uint32_t group;
  VkrDebugDrawColor color;
} VkrDebugDrawStyle;

typedef struct VkrDebugDrawSegment {
  Vec3 from;
  Vec3 to;
  VkrDebugDrawStyle style;
} VkrDebugDrawSegment;

typedef struct VkrDebugDrawLabel {
  Vec3 at;
  char text[VKR_DEBUG_DRAW_LABEL_TEXT];
  VkrDebugDrawStyle style;
} VkrDebugDrawLabel;

typedef struct VkrDebugDraw {
  VkrDebugDrawSegment segments[VKR_DEBUG_DRAW_SEGMENT_MAX];
  uint32_t segment_count;
  VkrDebugDrawLabel labels[VKR_DEBUG_DRAW_LABEL_MAX];
  uint32_t label_count;
  /* Shapes refused because storage was full, since the last clear. */
  uint32_t dropped;
  /* Bumped by every change, so a reader can tell the list moved. */
  uint64_t revision;
} VkrDebugDraw;

/** A group id from a name; an empty or NULL name is the default group. */
uint32_t vkr_debug_draw_group(const char *name);

/** A colour by name (red, green, blue, yellow, cyan, magenta, white,
 * orange); false for another name. */
bool8_t vkr_debug_draw_color_from_name(const char *name,
                                       VkrDebugDrawColor *out_color);

/* Each adder returns the segments or labels it stored; a full list stores
   what fits and counts the rest in `dropped`. */
uint32_t vkr_debug_draw_line(VkrDebugDraw *draw, Vec3 from, Vec3 to,
                             VkrDebugDrawStyle style);
uint32_t vkr_debug_draw_box(VkrDebugDraw *draw, Vec3 min, Vec3 max,
                            VkrDebugDrawStyle style);
/** Three great circles: one per axis plane. */
uint32_t vkr_debug_draw_sphere(VkrDebugDraw *draw, Vec3 center,
                               float32_t radius, VkrDebugDrawStyle style);
/** The frustum whose clip space `inverse_view_projection` maps back to the
 * world, with depth from 0 to 1. */
uint32_t vkr_debug_draw_frustum(VkrDebugDraw *draw,
                                Mat4 inverse_view_projection,
                                VkrDebugDrawStyle style);
uint32_t vkr_debug_draw_label(VkrDebugDraw *draw, Vec3 at, const char *text,
                              VkrDebugDrawStyle style);

/** Removes every shape of `group`, or of every group when `all`; returns
 * the shapes removed. */
uint32_t vkr_debug_draw_clear(VkrDebugDraw *draw, uint32_t group, bool8_t all);

/** Removes the shapes whose expiry is at or before `now`. */
void vkr_debug_draw_expire(VkrDebugDraw *draw, float64_t now);
