#include "editor_brush_grid.h"

#include "editor_agent.h"
#include "editor_blockout.h"
#include "editor_internal.h"
#include "editor_level.h"
#include "level/vkr_brush.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_brush.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* At most this many grid cells along a face side; a larger face doubles its
   grid step until it fits, so the overlay stays readable and in budget. */
#define GRID_CELLS_MAX 24.0f

/* How near the pointer a target takes it, in points on screen: corners
   (and the patch arrow and the clip tool's crossings), edges and grid
   lines. */
#define GRID_CORNER_PT 8.0f
#define GRID_EDGE_PT 6.0f
#define GRID_LINE_PT 5.0f

/* At most this many corners one reshape moves: brush.reshape's limit. */
#define GRID_GATHER_MAX 8u

typedef enum GridHit {
  GRID_HIT_NONE = 0,
  GRID_HIT_CELL,
  GRID_HIT_LINE,
  GRID_HIT_EDGE,
  GRID_HIT_VERTEX,
  GRID_HIT_PATCH_ARROW,
  /* Clip mode: a point a cut passes through. */
  GRID_HIT_POINT,
} GridHit;

/* At most this many points define a cut: two cut across the face of the
   first, three make a slanted cut. */
#define GRID_CLIP_POINT_MAX 3u

/* What the pointer touches: the corners a reshape moves, the plane a grid
   line splits the brush along, the face whose normal a drag follows, and
   where the ray meets that face. */
typedef struct GridTarget {
  GridHit hit;
  uint32_t face;
  Vec3 normal;
  Vec3 u;
  Vec3 v;
  float32_t step;
  Vec3 points[GRID_GATHER_MAX];
  uint32_t point_count;
  bool8_t split;
  Vec3 split_normal;
  float32_t split_distance;
  float32_t cell[2];
  Vec3 at;
} GridTarget;

typedef struct VkrEditorBrushGrid {
  VkrEntityId brush;
  VkrEditorBrushGridMode mode;
  const VkrScene *scene;
  /* A blockout corridor, when the grid lies on one of its walls instead of
     a brush: its tiles only select, and Delete cuts them as an opening of
     it. `shape_piece` is the wall's piece. */
  VkrEntityId shape_group;
  uint32_t shape_piece;
  /* The brush in world space, rebuilt every update. */
  uint32_t plane_count;
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  VkrBrushGeometry geometry;
  VkrBrushGeometry scratch;
  GridTarget hot;
  /* The selected patch: a rectangle of grid cells on one face, in the
     face's grid coordinates. */
  bool8_t patch;
  VkrEntityId patch_face;
  Vec3 patch_normal;
  Vec3 patch_u;
  Vec3 patch_v;
  float32_t patch_plane;
  float32_t patch_step;
  float32_t patch_anchor[2];
  float32_t patch_min[2];
  float32_t patch_max[2];
  float32_t arrow_length;
  /* The last cell press, for a double click that selects a whole face. */
  VkrEntityId click_face;
  float64_t click_time;
  /* The drag in progress: CELL selects a patch, PATCH_ARROW pulls or
     pushes it, and LINE, EDGE or VERTEX reshapes the brush. */
  GridHit drag;
  GridTarget target;
  bool8_t slide;
  Vec3 press_point;
  float32_t press_param;
  float32_t distance;
  Vec3 delta;
  Vec3 preview_delta;
  bool8_t valid;
  char message[128];
  uint32_t preview_count;
  VkrBrushPiece preview[VKR_BRUSH_RESHAPE_PIECE_MAX];
  /* Clip mode: the points clicked so far and the normals of the faces they
     lie on, and the cut the pointer would make. */
  uint32_t clip_count;
  Vec3 clip_points[GRID_CLIP_POINT_MAX];
  Vec3 clip_normals[GRID_CLIP_POINT_MAX];
  bool8_t clip_valid;
  VkrBrushPlane clip_plane;
  /* Corners Ctrl+click gathered; a drag on one of them moves them all. */
  uint32_t gathered_count;
  Vec3 gathered[GRID_GATHER_MAX];
  /* The hint naming the selected patch's size. */
  char patch_hint[160];
  uint32_t line_count;
  VkrEditorBrushGridLine lines[VKR_EDITOR_BRUSH_GRID_LINE_MAX];
} VkrEditorBrushGrid;

static const Vec4 GRID_LINE_COLOR = {0.85f, 0.9f, 1.0f, 0.35f};
static const Vec4 GRID_EDGE_COLOR = {1.0f, 0.85f, 0.2f, 0.55f};
static const Vec4 GRID_CORNER_COLOR = {1.0f, 0.85f, 0.2f, 0.8f};
static const Vec4 GRID_HOT_COLOR = {1.0f, 1.0f, 0.55f, 1.0f};
static const Vec4 GRID_PATCH_COLOR = {0.4f, 1.0f, 0.45f, 1.0f};
static const Vec4 GRID_PREVIEW_COLOR = {1.0f, 0.6f, 0.2f, 1.0f};
static const Vec4 GRID_REFUSED_COLOR = {1.0f, 0.3f, 0.25f, 1.0f};

// =============================================================================
// Geometry
// =============================================================================

static void grid_line(VkrEditorBrushGrid *grid, Vec3 from, Vec3 to,
                      Vec4 color) {
  if (grid->line_count < VKR_EDITOR_BRUSH_GRID_LINE_MAX) {
    grid->lines[grid->line_count++] =
        (VkrEditorBrushGridLine){.from = from, .to = to, .color = color};
  }
}

static void grid_cross(VkrEditorBrushGrid *grid, Vec3 at, float32_t size,
                       Vec4 color) {
  static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    const Vec3 half = vec3_scale(axes[axis], size);
    grid_line(grid, vec3_sub(at, half), vec3_add(at, half), color);
  }
}

/* The plane distance of polygon `face` along its unit normal. */
static float32_t grid_face_plane(const VkrEditorBrushGrid *grid,
                                 uint32_t face) {
  return vec3_dot(grid->geometry.normals[face],
                  grid->geometry.vertices[grid->geometry.polygons[face].first]);
}

/* The face's grid axes and its step: the editing grid's, doubled until the
   face spans at most GRID_CELLS_MAX cells each way. */
static void grid_face_axes(const VkrEditorBrushGrid *grid, uint32_t face,
                           float32_t base, Vec3 *u, Vec3 *v, float32_t *step) {
  vkr_brush_grid_axes(grid->geometry.normals[face], u, v);
  const VkrBrushPolygon polygon = grid->geometry.polygons[face];
  float32_t lo[2] = {INFINITY, INFINITY};
  float32_t hi[2] = {-INFINITY, -INFINITY};
  for (uint32_t c = 0; c < polygon.count; ++c) {
    const Vec3 corner = grid->geometry.vertices[polygon.first + c];
    const float32_t s = vec3_dot(corner, *u);
    const float32_t t = vec3_dot(corner, *v);
    lo[0] = Min(lo[0], s);
    hi[0] = Max(hi[0], s);
    lo[1] = Min(lo[1], t);
    hi[1] = Max(hi[1], t);
  }
  *step = base;
  while ((hi[0] - lo[0]) / *step > GRID_CELLS_MAX ||
         (hi[1] - lo[1]) / *step > GRID_CELLS_MAX) {
    *step *= 2.0f;
  }
}

/* The segment of polygon `face` where dot(x, axis) = value; false when the
   line misses the face or runs along one of its edges. */
static bool8_t grid_clip(const VkrEditorBrushGrid *grid, uint32_t face,
                         Vec3 axis, float32_t value, Vec3 *out_a, Vec3 *out_b) {
  const VkrBrushPolygon polygon = grid->geometry.polygons[face];
  Vec3 found[2];
  uint32_t count = 0u;
  for (uint32_t c = 0; c < polygon.count; ++c) {
    const Vec3 a = grid->geometry.vertices[polygon.first + c];
    const Vec3 b =
        grid->geometry.vertices[polygon.first + (c + 1u) % polygon.count];
    const float32_t sa = vec3_dot(a, axis) - value;
    const float32_t sb = vec3_dot(b, axis) - value;
    if (fabsf(sa) < 1.0e-4f && fabsf(sb) < 1.0e-4f) {
      return false_v;
    }
    if ((sa < 0.0f) == (sb < 0.0f)) {
      continue;
    }
    const Vec3 point = vec3_add(a, vec3_scale(vec3_sub(b, a), sa / (sa - sb)));
    if (count == 1u && vec3_length(vec3_sub(point, found[0])) < 1.0e-4f) {
      continue;
    }
    if (count < 2u) {
      found[count++] = point;
    }
  }
  if (count < 2u) {
    return false_v;
  }
  *out_a = found[0];
  *out_b = found[1];
  return true_v;
}

/* A point of the face plane from its grid coordinates. */
static Vec3 grid_plane_point(Vec3 normal, float32_t plane, Vec3 u, Vec3 v,
                             float32_t s, float32_t t) {
  return vec3_add(vec3_scale(normal, plane),
                  vec3_add(vec3_scale(u, s), vec3_scale(v, t)));
}

/* Where the ray meets the plane through `point` facing `normal`. */
static bool8_t grid_ray_plane(Vec3 origin, Vec3 direction, Vec3 normal,
                              float32_t plane, Vec3 *out) {
  const float32_t facing = vec3_dot(normal, direction);
  if (fabsf(facing) < 1.0e-6f) {
    return false_v;
  }
  const float32_t t = (plane - vec3_dot(normal, origin)) / facing;
  if (!(t > 0.0f)) {
    return false_v;
  }
  *out = vec3_add(origin, vec3_scale(direction, t));
  return true_v;
}

/* The parameter along the line through `anchor` along unit `axis` closest
   to the ray. */
static bool8_t grid_axis_param(Vec3 anchor, Vec3 axis, Vec3 origin,
                               Vec3 direction, float32_t *out) {
  const Vec3 w = vec3_sub(anchor, origin);
  const float32_t b = vec3_dot(axis, direction);
  const float32_t denominator = 1.0f - b * b;
  if (denominator < 1.0e-6f) {
    return false_v;
  }
  *out = (b * vec3_dot(direction, w) - vec3_dot(axis, w)) / denominator;
  return isfinite(*out);
}

/* The parameter in [0, 1] of the point of segment a-b closest to the
   ray. */
static float32_t grid_segment_param(Vec3 origin, Vec3 direction, Vec3 a,
                                    Vec3 b) {
  const Vec3 e = vec3_sub(b, a);
  const Vec3 w = vec3_sub(origin, a);
  const float32_t be = vec3_dot(direction, e);
  const float32_t ee = vec3_dot(e, e);
  const float32_t denominator = ee - be * be;
  if (denominator <= 1.0e-8f) {
    return 0.0f;
  }
  const float32_t s =
      (vec3_dot(e, w) - be * vec3_dot(direction, w)) / denominator;
  return Min(Max(s, 0.0f), 1.0f);
}

/* The distance from the ray to segment a-b, and how far along the ray the
   closest point lies. */
static float32_t grid_ray_segment(Vec3 origin, Vec3 direction, Vec3 a, Vec3 b,
                                  float32_t *out_along) {
  const Vec3 point =
      vec3_add(a, vec3_scale(vec3_sub(b, a),
                             grid_segment_param(origin, direction, a, b)));
  const float32_t along = vec3_dot(vec3_sub(point, origin), direction);
  *out_along = along;
  return vec3_length(
      vec3_sub(vec3_add(origin, vec3_scale(direction, along)), point));
}

/* The first face the ray enters and how far along, else UINT32_MAX. */
static uint32_t grid_ray_face(const VkrEditorBrushGrid *grid, Vec3 origin,
                              Vec3 direction, float32_t *out_distance) {
  float32_t enter = -INFINITY;
  float32_t leave = INFINITY;
  uint32_t entered = UINT32_MAX;
  for (uint32_t i = 0; i < grid->plane_count; ++i) {
    const float32_t facing = vec3_dot(grid->planes[i].normal, direction);
    const float32_t gap =
        grid->planes[i].distance - vec3_dot(grid->planes[i].normal, origin);
    if (fabsf(facing) < 1.0e-6f) {
      if (gap < 0.0f) {
        return UINT32_MAX;
      }
      continue;
    }
    const float32_t t = gap / facing;
    if (facing < 0.0f) {
      if (t > enter) {
        enter = t;
        entered = i;
      }
    } else {
      leave = Min(leave, t);
    }
  }
  if (entered == UINT32_MAX || enter > leave || enter <= 0.0f) {
    return UINT32_MAX;
  }
  *out_distance = enter;
  return entered;
}

// =============================================================================
// Picking
// =============================================================================

/* How far the pointer lies from segment a-b on screen, in points; INFINITY
   when an end is behind the eye. */
static float32_t grid_pointer_gap(const VkrSampleUiFrame *frame, Vec3 a,
                                  Vec3 b) {
  Vec2 pa = {0};
  Vec2 pb = {0};
  if (!vkr_editor_viewport_pixel(frame, a, &pa) ||
      !vkr_editor_viewport_pixel(frame, b, &pb)) {
    return INFINITY;
  }
  const Vec2 mouse = {(float32_t)frame->ui->mouse_x,
                      (float32_t)frame->ui->mouse_y};
  const Vec2 e = {pb.x - pa.x, pb.y - pa.y};
  const float32_t length = e.x * e.x + e.y * e.y;
  float32_t s = length > 1.0e-6f
                    ? ((mouse.x - pa.x) * e.x + (mouse.y - pa.y) * e.y) / length
                    : 0.0f;
  s = Min(Max(s, 0.0f), 1.0f);
  const float32_t dx = pa.x + e.x * s - mouse.x;
  const float32_t dy = pa.y + e.y * s - mouse.y;
  return sqrtf(dx * dx + dy * dy) / Max(frame->ui->content_scale, 1.0e-3f);
}

static float32_t grid_base_step(const VkrSampleUiFrame *frame) {
  return frame->view_state.grid_spacing > 0.0f ? frame->view_state.grid_spacing
                                               : 1.0f;
}

/* The patch arrow: from the patch center out along the face normal. */
static void grid_patch_arrow(const VkrEditorBrushGrid *grid, Vec3 *from,
                             Vec3 *to) {
  *from = grid_plane_point(grid->patch_normal, grid->patch_plane, grid->patch_u,
                           grid->patch_v,
                           (grid->patch_min[0] + grid->patch_max[0]) * 0.5f,
                           (grid->patch_min[1] + grid->patch_max[1]) * 0.5f);
  *to = vec3_add(*from, vec3_scale(grid->patch_normal, grid->arrow_length));
}

/* What the ray touches, nearest kind first: the patch arrow, a corner, an
   edge, then a grid line or a cell of the face it enters. Only faces turned
   toward the ray count, so the far side's lines stay out of reach. */
/* The cell of face `face` that the ray hits `distance` along. */
static GridTarget grid_cell(const VkrEditorBrushGrid *grid,
                            const VkrSampleUiFrame *frame, Vec3 origin,
                            Vec3 direction, uint32_t face, float32_t distance) {
  Vec3 u = {0};
  Vec3 v = {0};
  float32_t step = 1.0f;
  grid_face_axes(grid, face, grid_base_step(frame), &u, &v, &step);
  const Vec3 point = vec3_add(origin, vec3_scale(direction, distance));
  return (GridTarget){
      .hit = GRID_HIT_CELL,
      .face = face,
      .normal = grid->geometry.normals[face],
      .u = u,
      .v = v,
      .step = step,
      .cell = {floorf(vec3_dot(point, u) / step) * step,
               floorf(vec3_dot(point, v) / step) * step},
      .at = point,
  };
}

static GridTarget grid_pick(const VkrEditorBrushGrid *grid,
                            const VkrSampleUiFrame *frame, Vec3 origin,
                            Vec3 direction, bool8_t cells) {
  GridTarget target = {.hit = GRID_HIT_NONE};
  const VkrBrushGeometry *geometry = &grid->geometry;
  float32_t hit_distance = INFINITY;
  const uint32_t hit_face =
      grid_ray_face(grid, origin, direction, &hit_distance);
  /* On a shape's wall only cells answer: its tiles cut openings. */
  if (grid->shape_group.u64) {
    return cells && hit_face != UINT32_MAX
               ? grid_cell(grid, frame, origin, direction, hit_face,
                           hit_distance)
               : target;
  }
  /* Hidden corners and edges lie behind the face the ray enters. */
  const float32_t reach =
      hit_face != UINT32_MAX ? hit_distance * 1.02f + 1.0e-3f : INFINITY;

  if (grid->patch) {
    Vec3 from = {0};
    Vec3 to = {0};
    grid_patch_arrow(grid, &from, &to);
    if (grid_pointer_gap(frame, from, to) < GRID_CORNER_PT) {
      target.hit = GRID_HIT_PATCH_ARROW;
      return target;
    }
  }

  float32_t best = INFINITY;
  for (uint32_t f = 0; f < geometry->face_count; ++f) {
    const Vec3 normal = geometry->normals[f];
    if (vec3_dot(normal, direction) >= 0.0f) {
      continue;
    }
    const VkrBrushPolygon polygon = geometry->polygons[f];
    for (uint32_t c = 0; c < polygon.count; ++c) {
      const Vec3 corner = geometry->vertices[polygon.first + c];
      const float32_t along = vec3_dot(vec3_sub(corner, origin), direction);
      const float32_t gap = grid_pointer_gap(frame, corner, corner);
      /* The face the ray enters wins a corner its neighbours share. */
      const float32_t score = gap - (f == hit_face ? 1.0e-3f : 0.0f);
      if (along > 0.0f && along <= reach && gap < GRID_CORNER_PT &&
          score < best) {
        best = score;
        target = (GridTarget){.hit = GRID_HIT_VERTEX,
                              .face = f,
                              .normal = normal,
                              .points = {corner},
                              .point_count = 1u};
      }
    }
  }
  if (target.hit != GRID_HIT_NONE) {
    return target;
  }

  for (uint32_t f = 0; f < geometry->face_count; ++f) {
    const Vec3 normal = geometry->normals[f];
    if (vec3_dot(normal, direction) >= 0.0f) {
      continue;
    }
    const VkrBrushPolygon polygon = geometry->polygons[f];
    for (uint32_t c = 0; c < polygon.count; ++c) {
      const Vec3 a = geometry->vertices[polygon.first + c];
      const Vec3 b =
          geometry->vertices[polygon.first + (c + 1u) % polygon.count];
      float32_t along = 0.0f;
      (void)grid_ray_segment(origin, direction, a, b, &along);
      const float32_t gap = grid_pointer_gap(frame, a, b);
      const float32_t score = gap - (f == hit_face ? 1.0e-3f : 0.0f);
      if (along > 0.0f && along <= reach && gap < GRID_EDGE_PT &&
          score < best) {
        best = score;
        target = (GridTarget){.hit = GRID_HIT_EDGE,
                              .face = f,
                              .normal = normal,
                              .points = {a, b},
                              .point_count = 2u};
      }
    }
  }
  if (target.hit != GRID_HIT_NONE || hit_face == UINT32_MAX) {
    return target;
  }

  /* On the face: a grid line within reach, else the cell. */
  Vec3 u = {0};
  Vec3 v = {0};
  float32_t step = 1.0f;
  grid_face_axes(grid, hit_face, grid_base_step(frame), &u, &v, &step);
  const Vec3 point = vec3_add(origin, vec3_scale(direction, hit_distance));
  const float32_t coordinates[2] = {vec3_dot(point, u), vec3_dot(point, v)};
  const Vec3 axes[2] = {u, v};
  for (uint32_t axis = 0; axis < 2u; ++axis) {
    const float32_t value = roundf(coordinates[axis] / step) * step;
    Vec3 a = {0};
    Vec3 b = {0};
    if (grid_clip(grid, hit_face, axes[axis], value, &a, &b) &&
        grid_pointer_gap(frame, a, b) < GRID_LINE_PT) {
      return (GridTarget){.hit = GRID_HIT_LINE,
                          .face = hit_face,
                          .normal = geometry->normals[hit_face],
                          .u = u,
                          .v = v,
                          .step = step,
                          .points = {a, b},
                          .point_count = 2u,
                          .split = true_v,
                          .split_normal = axes[axis],
                          .split_distance = value,
                          .at = point};
    }
  }
  if (!cells) {
    return target;
  }
  return grid_cell(grid, frame, origin, direction, hit_face, hit_distance);
}

// =============================================================================
// Clipping
// =============================================================================

/* The point of edge `target` nearest the ray, moved along the edge to where
   it crosses the nearest line of the face grid `u`, `v`, `step`. */
static Vec3 grid_edge_point(const GridTarget *target, Vec3 u, Vec3 v,
                            float32_t step, Vec3 origin, Vec3 direction) {
  const Vec3 a = target->points[0];
  const Vec3 e = vec3_sub(target->points[1], a);
  const float32_t s =
      grid_segment_param(origin, direction, a, target->points[1]);
  /* The grid axis the edge runs along most. */
  const Vec3 axis = fabsf(vec3_dot(e, u)) >= fabsf(vec3_dot(e, v)) ? u : v;
  const float32_t run = vec3_dot(e, axis);
  if (fabsf(run) < 1.0e-6f) {
    return vec3_add(a, vec3_scale(e, s));
  }
  const float32_t value =
      roundf(vec3_dot(vec3_add(a, vec3_scale(e, s)), axis) / step) * step;
  const float32_t snapped = (value - vec3_dot(a, axis)) / run;
  return vec3_add(a, vec3_scale(e, Min(Max(snapped, 0.0f), 1.0f)));
}

/* The Clip tool's target: a corner, an edge where it crosses the face grid,
   or the grid crossing near the pointer, as a point of the cut; else, before
   the first point, the grid line to cut along. Once the cut has a point,
   the pointer anywhere on a face takes its nearest crossing. */
static GridTarget grid_pick_clip(const VkrEditorBrushGrid *grid,
                                 const VkrSampleUiFrame *frame, Vec3 origin,
                                 Vec3 direction) {
  GridTarget target = grid_pick(grid, frame, origin, direction, true_v);
  switch (target.hit) {
  case GRID_HIT_VERTEX:
    break;
  case GRID_HIT_EDGE: {
    Vec3 u = {0};
    Vec3 v = {0};
    float32_t step = 1.0f;
    grid_face_axes(grid, target.face, grid_base_step(frame), &u, &v, &step);
    target.points[0] = grid_edge_point(&target, u, v, step, origin, direction);
    break;
  }
  case GRID_HIT_LINE:
  case GRID_HIT_CELL: {
    const float32_t s = vec3_dot(target.at, target.u);
    const float32_t t = vec3_dot(target.at, target.v);
    const float32_t cs = roundf(s / target.step) * target.step;
    const float32_t ct = roundf(t / target.step) * target.step;
    const Vec3 crossing =
        vec3_add(target.at, vec3_add(vec3_scale(target.u, cs - s),
                                     vec3_scale(target.v, ct - t)));
    if (!grid->clip_count &&
        grid_pointer_gap(frame, crossing, crossing) >= GRID_CORNER_PT) {
      return target.hit == GRID_HIT_LINE ? target
                                         : (GridTarget){.hit = GRID_HIT_NONE};
    }
    target.points[0] = crossing;
    break;
  }
  default:
    return (GridTarget){.hit = GRID_HIT_NONE};
  }
  target.hit = GRID_HIT_POINT;
  target.point_count = 1u;
  target.split = false_v;
  return target;
}

/* The two pieces `plane` cuts the brush into, as the preview; false when it
   misses the brush. */
static bool8_t grid_preview_clip(VkrEditorBrushGrid *grid,
                                 VkrBrushPlane plane) {
  grid->preview_count = 0u;
  if (grid->plane_count >= VKR_BRUSH_FACE_MAX) {
    return false_v;
  }
  for (uint32_t side = 0; side < 2u; ++side) {
    VkrBrushPiece *piece = &grid->preview[side];
    piece->count = grid->plane_count;
    for (uint32_t i = 0; i < grid->plane_count; ++i) {
      piece->planes[i] = grid->planes[i];
      piece->source[i] = i;
    }
    piece->planes[piece->count] =
        side == 0u ? plane
                   : (VkrBrushPlane){.normal = vec3_scale(plane.normal, -1.0f),
                                     .distance = -plane.distance};
    piece->source[piece->count++] = VKR_BRUSH_SOURCE_NEW;
    if (!vkr_brush_prune(piece, &grid->scratch)) {
      return false_v;
    }
  }
  grid->preview_count = 2u;
  return true_v;
}

/* The cut through `count` points that lie on faces facing `normals`, with
   its preview: three points cut through all of them; two cut straight
   across a face they share, else across the face either was picked on,
   whichever cuts the brush. Writes whether the points fix a plane at all
   to `out_plane`. */
static bool8_t grid_clip_cut(VkrEditorBrushGrid *grid, const Vec3 *points,
                             const Vec3 *normals, uint32_t count,
                             bool8_t *out_plane) {
  *out_plane = false_v;
  const Vec3 along = vec3_sub(points[1], points[0]);
  if (count < 2u || vec3_length(along) < 1.0e-4f) {
    return false_v;
  }
  if (count > 2u) {
    const Vec3 normal = vec3_cross(along, vec3_sub(points[2], points[0]));
    if (vec3_length(normal) >= 1.0e-5f) {
      const Vec3 unit = vec3_normalize(normal);
      grid->clip_plane = (VkrBrushPlane){.normal = unit,
                                         .distance = vec3_dot(unit, points[0])};
      *out_plane = true_v;
      return grid_preview_clip(grid, grid->clip_plane);
    }
  }
  /* Faces through both points first, then the faces they were picked on. */
  Vec3 faces[VKR_BRUSH_FACE_MAX + 2u];
  uint32_t face_count = 0u;
  for (uint32_t f = 0; f < grid->geometry.face_count; ++f) {
    const float32_t plane = grid_face_plane(grid, f);
    const Vec3 normal = grid->geometry.normals[f];
    if (fabsf(vec3_dot(normal, points[0]) - plane) < 1.0e-3f &&
        fabsf(vec3_dot(normal, points[1]) - plane) < 1.0e-3f) {
      faces[face_count++] = normal;
    }
  }
  faces[face_count++] = normals[0];
  faces[face_count++] = normals[1];
  for (uint32_t i = 0; i < face_count; ++i) {
    const Vec3 normal = vec3_cross(along, faces[i]);
    if (vec3_length(normal) < 1.0e-5f) {
      continue;
    }
    const Vec3 unit = vec3_normalize(normal);
    grid->clip_plane =
        (VkrBrushPlane){.normal = unit, .distance = vec3_dot(unit, points[0])};
    *out_plane = true_v;
    if (grid_preview_clip(grid, grid->clip_plane)) {
      return true_v;
    }
  }
  return false_v;
}

// =============================================================================
// Drags
// =============================================================================

/* The step a move snaps to: the editing grid's, or 1/16 m with Free
   snapping. */
static float32_t grid_move_step(const VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  return editor->placement.target == VKR_EDITOR_SNAP_FREE
             ? 0.0625f
             : grid_base_step(frame);
}

static void grid_entity_text(VkrEntityId entity, char *out, size_t capacity) {
  snprintf(out, capacity, "%u:%u:%u", (unsigned)entity.parts.world,
           (unsigned)entity.parts.index, (unsigned)entity.parts.generation);
}

/* Submits `op` with the members `args`. */
static void grid_send(VkrEditorUi *editor, const VkrEditorBrushGrid *grid,
                      const char *op, const char *args) {
  (void)grid;
  char line[GRID_GATHER_MAX * 48u + 1024u];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"grid\",\"op\":\"%s\",\"args\":{%s,"
           "\"review\":false,\"select\":true}}",
           op, args);
  (void)vkr_editor_agent_submit(editor->agent, line);
}

static void grid_submit_patch(VkrEditorUi *editor,
                              const VkrEditorBrushGrid *grid) {
  char face[48];
  grid_entity_text(grid->patch_face, face, sizeof(face));
  char args[384];
  snprintf(args, sizeof(args),
           "\"face\":\"%s\",\"min\":[%.9g,%.9g],\"max\":[%.9g,%.9g],"
           "\"distance\":%.9g",
           face, grid->patch_min[0], grid->patch_min[1], grid->patch_max[0],
           grid->patch_max[1], grid->distance);
  grid_send(editor, grid, "brush.patch", args);
}

static void grid_submit_reshape(VkrEditorUi *editor,
                                const VkrEditorBrushGrid *grid) {
  const GridTarget *target = &grid->target;
  char brush[48];
  grid_entity_text(grid->brush, brush, sizeof(brush));
  char points[GRID_GATHER_MAX * 48u] = {0};
  size_t used = 0u;
  for (uint32_t i = 0; i < target->point_count && used < sizeof(points); ++i) {
    used += (size_t)snprintf(points + used, sizeof(points) - used,
                             "%s[%.9g,%.9g,%.9g]", i ? "," : "",
                             target->points[i].x, target->points[i].y,
                             target->points[i].z);
  }
  char split[256] = {0};
  if (target->split) {
    const Vec3 on_plane =
        vec3_scale(target->split_normal, target->split_distance);
    snprintf(split, sizeof(split),
             ",\"split\":{\"point\":[%.9g,%.9g,%.9g],"
             "\"normal\":[%.9g,%.9g,%.9g]}",
             on_plane.x, on_plane.y, on_plane.z, target->split_normal.x,
             target->split_normal.y, target->split_normal.z);
  }
  char args[GRID_GATHER_MAX * 48u + 512u];
  snprintf(args, sizeof(args),
           "\"brush\":\"%s\",\"points\":[%s],\"delta\":[%.9g,%.9g,%.9g]%s",
           brush, points, grid->delta.x, grid->delta.y, grid->delta.z, split);
  grid_send(editor, grid, "brush.reshape", args);
}

static void grid_submit_clip(VkrEditorUi *editor,
                             const VkrEditorBrushGrid *grid) {
  char brush[48];
  grid_entity_text(grid->brush, brush, sizeof(brush));
  const VkrBrushPlane plane = grid->clip_plane;
  const Vec3 point = vec3_scale(plane.normal, plane.distance);
  char args[384];
  snprintf(args, sizeof(args),
           "\"brush\":\"%s\",\"point\":[%.9g,%.9g,%.9g],"
           "\"normal\":[%.9g,%.9g,%.9g],\"keep\":\"both\"",
           brush, point.x, point.y, point.z, plane.normal.x, plane.normal.y,
           plane.normal.z);
  grid_send(editor, grid, "brush.clip", args);
}

/* Runs the reshape the drag would make, for its preview and verdict. */
static void grid_preview_reshape(VkrEditorBrushGrid *grid) {
  if (vec3_length(vec3_sub(grid->delta, grid->preview_delta)) < 1.0e-5f &&
      (grid->preview_count || grid->message[0])) {
    return;
  }
  grid->preview_delta = grid->delta;
  grid->preview_count = 0u;
  grid->message[0] = '\0';
  grid->valid = vec3_length(grid->delta) >= 1.0e-4f;
  if (!grid->valid) {
    return;
  }
  const GridTarget *target = &grid->target;
  const VkrBrushPlane split = {target->split_normal, target->split_distance};
  const VkrBrushError error = vkr_brush_reshape(
      grid->planes, grid->plane_count, target->split ? &split : NULL,
      target->points, target->point_count, grid->delta, grid->preview,
      ArrayCount(grid->preview), &grid->preview_count, &grid->scratch);
  grid->valid = error == VKR_BRUSH_OK;
  if (!grid->valid) {
    grid->preview_count = 0u;
    snprintf(grid->message, sizeof(grid->message), "Can't: %s",
             vkr_brush_error_text(error));
  }
}

static void grid_drag(VkrEditorUi *editor, VkrEditorBrushGrid *grid,
                      const VkrSampleUiFrame *frame, Vec3 origin,
                      Vec3 direction, bool8_t has_ray) {
  VkrUiSystem *ui = frame->ui;
  if (input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    if (grid->drag == GRID_HIT_CELL) {
      grid->patch = false_v;
    }
    grid->drag = GRID_HIT_NONE;
    return;
  }
  const float32_t step = grid_move_step(editor, frame);
  const bool8_t released =
      !input_is_button_down(frame->input, BUTTON_LEFT) || ui->mouse_released;

  switch (grid->drag) {
  case GRID_HIT_CELL: {
    Vec3 point = {0};
    if (has_ray && grid_ray_plane(origin, direction, grid->patch_normal,
                                  grid->patch_plane, &point)) {
      const float32_t s =
          floorf(vec3_dot(point, grid->patch_u) / grid->patch_step) *
          grid->patch_step;
      const float32_t t =
          floorf(vec3_dot(point, grid->patch_v) / grid->patch_step) *
          grid->patch_step;
      grid->patch_min[0] = Min(grid->patch_anchor[0], s);
      grid->patch_min[1] = Min(grid->patch_anchor[1], t);
      grid->patch_max[0] = Max(grid->patch_anchor[0], s) + grid->patch_step;
      grid->patch_max[1] = Max(grid->patch_anchor[1], t) + grid->patch_step;
    }
    if (released) {
      grid->drag = GRID_HIT_NONE;
    }
    return;
  }
  case GRID_HIT_PATCH_ARROW: {
    Vec3 from = {0};
    Vec3 to = {0};
    grid_patch_arrow(grid, &from, &to);
    float32_t param = 0.0f;
    if (has_ray &&
        grid_axis_param(from, grid->patch_normal, origin, direction, &param)) {
      grid->distance = roundf((param - grid->press_param) / step) * step;
    }
    if (released) {
      grid->drag = GRID_HIT_NONE;
      if (fabsf(grid->distance) >= 1.0e-4f) {
        grid_submit_patch(editor, grid);
        grid->patch = false_v;
      }
    }
    return;
  }
  default:
    break;
  }

  /* Reshape: along the face normal, or along the face with Shift. */
  const GridTarget *target = &grid->target;
  if (has_ray && grid->slide) {
    Vec3 point = {0};
    if (grid_ray_plane(origin, direction, target->normal,
                       vec3_dot(target->normal, target->points[0]), &point)) {
      Vec3 u = {0};
      Vec3 v = {0};
      vkr_brush_grid_axes(target->normal, &u, &v);
      const Vec3 move = vec3_sub(point, grid->press_point);
      grid->delta =
          vec3_add(vec3_scale(u, roundf(vec3_dot(move, u) / step) * step),
                   vec3_scale(v, roundf(vec3_dot(move, v) / step) * step));
    }
  } else if (has_ray) {
    float32_t param = 0.0f;
    if (grid_axis_param(target->points[0], target->normal, origin, direction,
                        &param)) {
      grid->delta = vec3_scale(
          target->normal, roundf((param - grid->press_param) / step) * step);
    }
  }
  grid_preview_reshape(grid);
  if (released) {
    grid->drag = GRID_HIT_NONE;
    if (grid->valid) {
      grid_submit_reshape(editor, grid);
      grid->gathered_count = 0u;
    }
  }
}

/* Whether `point` is among the gathered corners. */
static bool8_t grid_gathered(const VkrEditorBrushGrid *grid, Vec3 point) {
  for (uint32_t i = 0; i < grid->gathered_count; ++i) {
    if (vec3_length(vec3_sub(grid->gathered[i], point)) < 1.0e-3f) {
      return true_v;
    }
  }
  return false_v;
}

/* Ctrl+click on a corner or an edge adds its corners to the gathered ones,
   or takes them out when all are in already. */
static void grid_gather(VkrEditorBrushGrid *grid, const GridTarget *target) {
  bool8_t all = true_v;
  for (uint32_t i = 0; i < target->point_count; ++i) {
    all = all && grid_gathered(grid, target->points[i]);
  }
  for (uint32_t i = 0; i < target->point_count; ++i) {
    const Vec3 point = target->points[i];
    if (all) {
      for (uint32_t g = 0; g < grid->gathered_count; ++g) {
        if (vec3_length(vec3_sub(grid->gathered[g], point)) < 1.0e-3f) {
          grid->gathered[g] = grid->gathered[--grid->gathered_count];
          break;
        }
      }
    } else if (!grid_gathered(grid, point) &&
               grid->gathered_count < GRID_GATHER_MAX) {
      grid->gathered[grid->gathered_count++] = point;
    }
  }
}

static void grid_press(VkrEditorBrushGrid *grid, const VkrSampleUiFrame *frame,
                       Vec3 origin, Vec3 direction) {
  GridTarget hot = grid->hot;
  /* A drag on a gathered corner or edge moves every gathered corner. */
  if ((hot.hit == GRID_HIT_VERTEX || hot.hit == GRID_HIT_EDGE) &&
      grid->gathered_count) {
    bool8_t member = false_v;
    for (uint32_t i = 0; i < hot.point_count; ++i) {
      member = member || grid_gathered(grid, hot.points[i]);
    }
    if (member) {
      uint32_t count = grid->gathered_count;
      Vec3 points[GRID_GATHER_MAX];
      MemCopy(points, grid->gathered, count * sizeof(points[0]));
      for (uint32_t i = 0; i < hot.point_count && count < GRID_GATHER_MAX;
           ++i) {
        if (!grid_gathered(grid, hot.points[i])) {
          points[count++] = hot.points[i];
        }
      }
      /* The press point stays first: the drag follows it. */
      for (uint32_t i = 0; i < count; ++i) {
        if (vec3_length(vec3_sub(points[i], hot.points[0])) < 1.0e-3f) {
          points[i] = points[0];
          points[0] = hot.points[0];
          break;
        }
      }
      MemCopy(hot.points, points, count * sizeof(points[0]));
      hot.point_count = count;
    } else {
      grid->gathered_count = 0u;
    }
  }
  grid->drag = hot.hit;
  grid->target = hot;
  grid->distance = 0.0f;
  grid->delta = vec3_zero();
  grid->preview_delta = vec3_zero();
  grid->preview_count = 0u;
  grid->message[0] = '\0';
  grid->valid = false_v;
  switch (hot.hit) {
  case GRID_HIT_CELL: {
    grid->patch = true_v;
    grid->patch_face = grid->faces[hot.face];
    grid->patch_normal = hot.normal;
    grid->patch_u = hot.u;
    grid->patch_v = hot.v;
    grid->patch_plane = grid_face_plane(grid, hot.face);
    grid->patch_step = hot.step;
    grid->patch_anchor[0] = hot.cell[0];
    grid->patch_anchor[1] = hot.cell[1];
    grid->patch_min[0] = hot.cell[0];
    grid->patch_min[1] = hot.cell[1];
    grid->patch_max[0] = hot.cell[0] + hot.step;
    grid->patch_max[1] = hot.cell[1] + hot.step;
    /* A second press on the face within 0.4 s selects all of it. */
    const float64_t now = vkr_platform_get_absolute_time();
    if (grid->click_face.u64 == grid->patch_face.u64 &&
        now - grid->click_time < 0.4) {
      const VkrBrushPolygon polygon = grid->geometry.polygons[hot.face];
      for (uint32_t axis = 0; axis < 2u; ++axis) {
        const Vec3 face_axis = axis ? hot.v : hot.u;
        grid->patch_min[axis] = INFINITY;
        grid->patch_max[axis] = -INFINITY;
        for (uint32_t c = 0; c < polygon.count; ++c) {
          const float32_t value =
              vec3_dot(grid->geometry.vertices[polygon.first + c], face_axis);
          grid->patch_min[axis] = Min(grid->patch_min[axis], value);
          grid->patch_max[axis] = Max(grid->patch_max[axis], value);
        }
      }
      grid->drag = GRID_HIT_NONE;
      grid->click_face = VKR_ENTITY_ID_INVALID;
      break;
    }
    grid->click_face = grid->patch_face;
    grid->click_time = now;
    break;
  }
  case GRID_HIT_PATCH_ARROW: {
    Vec3 from = {0};
    Vec3 to = {0};
    grid_patch_arrow(grid, &from, &to);
    if (!grid_axis_param(from, grid->patch_normal, origin, direction,
                         &grid->press_param)) {
      grid->drag = GRID_HIT_NONE;
    }
    break;
  }
  default:
    grid->slide = input_is_key_down(frame->input, KEY_SHIFT);
    if (grid->slide) {
      if (!grid_ray_plane(origin, direction, hot.normal,
                          vec3_dot(hot.normal, hot.points[0]),
                          &grid->press_point)) {
        grid->drag = GRID_HIT_NONE;
      }
    } else if (!grid_axis_param(hot.points[0], hot.normal, origin, direction,
                                &grid->press_param)) {
      grid->drag = GRID_HIT_NONE;
    }
    break;
  }
}

/* The Clip tool on the grid: the pointer previews the cut it would make, a
   click on a grid line cuts along it, and clicks on points build a cut that
   applies with its second point, or its third when Shift held the second.
   Escape drops the points. */
static void grid_clip_pointer(VkrEditorUi *editor, VkrEditorBrushGrid *grid,
                              const VkrSampleUiFrame *frame, Vec3 origin,
                              Vec3 direction, bool8_t has_ray, bool8_t inside) {
  VkrUiSystem *ui = frame->ui;
  if (grid->clip_count && input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    grid->clip_count = 0u;
  }
  grid->clip_valid = false_v;
  grid->preview_count = 0u;
  grid->message[0] = '\0';
  if (!has_ray || !inside || vkr_editor_selection_modifier(frame)) {
    return;
  }
  grid->hot = grid_pick_clip(grid, frame, origin, direction);

  /* The cut the pointer would make, held points and all. */
  bool8_t plane = false_v;
  if (grid->hot.hit == GRID_HIT_LINE) {
    grid->clip_plane = (VkrBrushPlane){.normal = grid->hot.split_normal,
                                       .distance = grid->hot.split_distance};
    plane = true_v;
    grid->clip_valid = grid_preview_clip(grid, grid->clip_plane);
  } else if (grid->hot.hit == GRID_HIT_POINT && grid->clip_count) {
    Vec3 points[GRID_CLIP_POINT_MAX];
    Vec3 normals[GRID_CLIP_POINT_MAX];
    for (uint32_t i = 0; i < grid->clip_count; ++i) {
      points[i] = grid->clip_points[i];
      normals[i] = grid->clip_normals[i];
    }
    points[grid->clip_count] = grid->hot.points[0];
    normals[grid->clip_count] = grid->hot.normal;
    grid->clip_valid =
        grid_clip_cut(grid, points, normals, grid->clip_count + 1u, &plane);
  }
  if (plane && !grid->clip_valid) {
    snprintf(grid->message, sizeof(grid->message),
             "Can't: the cut misses the brush");
  }

  if (grid->hot.hit != GRID_HIT_NONE || grid->clip_count) {
    /* The Scene stops picking objects under a target or mid-cut. */
    const Vec4 image = frame->mapping.image_rect_px;
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
        (VkrUiRect){image.x, image.y, image.z, image.w});
  }
  if (!ui->mouse_pressed || grid->hot.hit == GRID_HIT_NONE) {
    return;
  }
  if (grid->hot.hit == GRID_HIT_LINE) {
    if (grid->clip_valid) {
      grid_submit_clip(editor, grid);
    }
    return;
  }
  const Vec3 point = grid->hot.points[0];
  for (uint32_t i = 0; i < grid->clip_count; ++i) {
    if (vec3_length(vec3_sub(point, grid->clip_points[i])) < 1.0e-4f) {
      return;
    }
  }
  const uint32_t count = grid->clip_count + 1u;
  if (count >= 2u && (count == GRID_CLIP_POINT_MAX ||
                      !input_is_key_down(frame->input, KEY_SHIFT))) {
    if (grid->clip_valid) {
      grid_submit_clip(editor, grid);
      grid->clip_count = 0u;
    }
    return;
  }
  grid->clip_points[grid->clip_count] = point;
  grid->clip_normals[grid->clip_count] = grid->hot.normal;
  grid->clip_count = count;
}

// =============================================================================
// Overlay
// =============================================================================

/* The edges of the preview pieces; the faces a cut made stand out. */
static void grid_preview_lines(VkrEditorBrushGrid *grid) {
  for (uint32_t p = 0; p < grid->preview_count; ++p) {
    const VkrBrushPiece *piece = &grid->preview[p];
    if (vkr_brush_build(piece->planes, piece->count, &grid->scratch, NULL) !=
        VKR_BRUSH_OK) {
      continue;
    }
    for (uint32_t f = 0; f < grid->scratch.face_count; ++f) {
      const VkrBrushPolygon polygon = grid->scratch.polygons[f];
      const Vec4 color = f < piece->count &&
                                 piece->source[f] == VKR_BRUSH_SOURCE_NEW &&
                                 grid->mode == VKR_EDITOR_BRUSH_GRID_CLIP
                             ? GRID_HOT_COLOR
                             : GRID_PREVIEW_COLOR;
      for (uint32_t c = 0; c < polygon.count; ++c) {
        grid_line(
            grid, grid->scratch.vertices[polygon.first + c],
            grid->scratch.vertices[polygon.first + (c + 1u) % polygon.count],
            color);
      }
    }
  }
}

static void grid_patch_lines(VkrEditorBrushGrid *grid, float32_t offset,
                             Vec4 color) {
  const float32_t s[4] = {grid->patch_min[0], grid->patch_max[0],
                          grid->patch_max[0], grid->patch_min[0]};
  const float32_t t[4] = {grid->patch_min[1], grid->patch_min[1],
                          grid->patch_max[1], grid->patch_max[1]};
  Vec3 corners[4];
  for (uint32_t i = 0; i < 4u; ++i) {
    corners[i] =
        grid_plane_point(grid->patch_normal, grid->patch_plane + offset,
                         grid->patch_u, grid->patch_v, s[i], t[i]);
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    grid_line(grid, corners[i], corners[(i + 1u) % 4u], color);
    if (offset != 0.0f) {
      grid_line(grid, corners[i],
                vec3_sub(corners[i], vec3_scale(grid->patch_normal, offset)),
                color);
    }
  }
}

static void grid_build_lines(VkrEditorBrushGrid *grid,
                             const VkrSampleUiFrame *frame, Vec3 eye) {
  grid->line_count = 0u;
  const VkrBrushGeometry *geometry = &grid->geometry;
  const float32_t base = grid_base_step(frame);
  for (uint32_t f = 0; f < geometry->face_count; ++f) {
    const VkrBrushPolygon polygon = geometry->polygons[f];
    for (uint32_t c = 0; c < polygon.count; ++c) {
      grid_line(grid, geometry->vertices[polygon.first + c],
                geometry->vertices[polygon.first + (c + 1u) % polygon.count],
                GRID_EDGE_COLOR);
    }
  }
  for (uint32_t f = 0; f < geometry->face_count; ++f) {
    const VkrBrushPolygon polygon = geometry->polygons[f];
    const Vec3 first = geometry->vertices[polygon.first];
    if (vec3_dot(geometry->normals[f], vec3_sub(eye, first)) <= 0.0f) {
      continue;
    }
    Vec3 axes[2];
    float32_t step = 1.0f;
    grid_face_axes(grid, f, base, &axes[0], &axes[1], &step);
    for (uint32_t axis = 0; axis < 2u; ++axis) {
      float32_t lo = INFINITY;
      float32_t hi = -INFINITY;
      for (uint32_t c = 0; c < polygon.count; ++c) {
        const float32_t s =
            vec3_dot(geometry->vertices[polygon.first + c], axes[axis]);
        lo = Min(lo, s);
        hi = Max(hi, s);
      }
      for (float32_t value = ceilf(lo / step) * step; value < hi;
           value += step) {
        Vec3 a = {0};
        Vec3 b = {0};
        if (grid_clip(grid, f, axes[axis], value, &a, &b)) {
          grid_line(grid, a, b, GRID_LINE_COLOR);
        }
      }
    }
    for (uint32_t c = 0; c < polygon.count; ++c) {
      const Vec3 corner = geometry->vertices[polygon.first + c];
      grid_cross(grid, corner, 0.008f * vec3_length(vec3_sub(eye, corner)),
                 GRID_CORNER_COLOR);
    }
  }

  /* The target under the pointer, or the one being dragged. */
  const GridTarget *shown =
      grid->drag != GRID_HIT_NONE ? &grid->target : &grid->hot;
  switch (shown->hit) {
  case GRID_HIT_VERTEX:
  case GRID_HIT_POINT:
    grid_cross(grid, shown->points[0],
               0.02f * vec3_length(vec3_sub(eye, shown->points[0])),
               GRID_HOT_COLOR);
    break;
  case GRID_HIT_EDGE:
  case GRID_HIT_LINE:
    grid_line(grid, shown->points[0], shown->points[1], GRID_HOT_COLOR);
    break;
  case GRID_HIT_CELL:
    if (grid->drag == GRID_HIT_NONE) {
      const float32_t plane = grid_face_plane(grid, shown->face);
      const float32_t s[4] = {shown->cell[0], shown->cell[0] + shown->step,
                              shown->cell[0] + shown->step, shown->cell[0]};
      const float32_t t[4] = {shown->cell[1], shown->cell[1],
                              shown->cell[1] + shown->step,
                              shown->cell[1] + shown->step};
      for (uint32_t i = 0; i < 4u; ++i) {
        grid_line(grid,
                  grid_plane_point(shown->normal, plane, shown->u, shown->v,
                                   s[i], t[i]),
                  grid_plane_point(shown->normal, plane, shown->u, shown->v,
                                   s[(i + 1u) % 4u], t[(i + 1u) % 4u]),
                  GRID_HOT_COLOR);
      }
    }
    break;
  default:
    break;
  }

  if (grid->patch && grid->shape_group.u64) {
    grid_patch_lines(grid, 0.0f, GRID_PATCH_COLOR);
  } else if (grid->patch) {
    grid_patch_lines(grid, 0.0f, GRID_PATCH_COLOR);
    Vec3 from = {0};
    Vec3 to = {0};
    grid_patch_arrow(grid, &from, &to);
    const bool8_t hot = grid->hot.hit == GRID_HIT_PATCH_ARROW ||
                        grid->drag == GRID_HIT_PATCH_ARROW;
    const Vec4 color = hot ? GRID_HOT_COLOR : GRID_PATCH_COLOR;
    grid_line(grid, from, to, color);
    grid_cross(grid, to, grid->arrow_length * 0.12f, color);
    if (grid->drag == GRID_HIT_PATCH_ARROW && grid->distance != 0.0f) {
      grid_patch_lines(grid, grid->distance, GRID_PREVIEW_COLOR);
    }
  }

  for (uint32_t i = 0; i < grid->gathered_count; ++i) {
    const Vec3 point = grid->gathered[i];
    grid_cross(grid, point, 0.02f * vec3_length(vec3_sub(eye, point)),
               GRID_PATCH_COLOR);
  }

  if (grid->mode == VKR_EDITOR_BRUSH_GRID_CLIP) {
    /* The points so far, joined, and on to the one under the pointer. */
    for (uint32_t i = 0; i < grid->clip_count; ++i) {
      const Vec3 point = grid->clip_points[i];
      grid_cross(grid, point, 0.02f * vec3_length(vec3_sub(eye, point)),
                 GRID_PATCH_COLOR);
      const bool8_t last = i + 1u == grid->clip_count;
      if (!last || grid->hot.hit == GRID_HIT_POINT) {
        grid_line(grid, point,
                  last ? grid->hot.points[0] : grid->clip_points[i + 1u],
                  last ? GRID_HOT_COLOR : GRID_PATCH_COLOR);
      }
    }
    grid_preview_lines(grid);
  }

  if (grid->drag == GRID_HIT_LINE || grid->drag == GRID_HIT_EDGE ||
      grid->drag == GRID_HIT_VERTEX) {
    grid_preview_lines(grid);
    if (!grid->valid && vec3_length(grid->delta) >= 1.0e-4f) {
      for (uint32_t i = 0; i < grid->target.point_count; ++i) {
        grid_line(grid, grid->target.points[i],
                  vec3_add(grid->target.points[i], grid->delta),
                  GRID_REFUSED_COLOR);
      }
    }
  }
}

// =============================================================================
// Update
// =============================================================================

/* Puts the grid on a wall of corridor `shape_entity`: the one under the
   pointer, or the one its patch or drag lies on. Its faces carry made-up
   ids, which only tell them apart. False when there is none. */
static bool8_t grid_shape_wall(VkrEditorBrushGrid *grid, const VkrScene *scene,
                               VkrEntityId shape_entity, Vec3 origin,
                               Vec3 direction, bool8_t has_ray) {
  VkrBlockoutPiece *pieces = NULL;
  const uint32_t count = vkr_editor_shape_pieces(scene, shape_entity, &pieces);
  const bool8_t keep = grid->patch || grid->drag != GRID_HIT_NONE;
  uint32_t chosen =
      keep && grid->shape_piece < count ? grid->shape_piece : UINT32_MAX;
  float32_t nearest = INFINITY;
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  for (uint32_t i = 0; i < count && !keep && has_ray; ++i) {
    if (pieces[i].kind != VKR_BLOCKOUT_PIECE_WALL) {
      continue;
    }
    const uint32_t plane_count = vkr_brush_hull(
        pieces[i].points, pieces[i].point_count, planes, VKR_BRUSH_FACE_MAX);
    /* Where the ray enters and leaves the wall's half-spaces. */
    float32_t enter = 0.0f;
    float32_t leave = INFINITY;
    bool8_t hit = plane_count >= VKR_BRUSH_FACE_MIN;
    for (uint32_t p = 0; p < plane_count && hit; ++p) {
      const float32_t facing = vec3_dot(planes[p].normal, direction);
      const float32_t gap =
          planes[p].distance - vec3_dot(planes[p].normal, origin);
      if (fabsf(facing) < 1.0e-8f) {
        hit = gap >= 0.0f;
        continue;
      }
      if (facing < 0.0f) {
        enter = Max(enter, gap / facing);
      } else {
        leave = Min(leave, gap / facing);
      }
      hit = enter <= leave;
    }
    if (hit && enter < nearest) {
      nearest = enter;
      chosen = i;
    }
  }
  grid->shape_piece = chosen;
  bool8_t ok = false_v;
  if (chosen != UINT32_MAX && pieces[chosen].kind == VKR_BLOCKOUT_PIECE_WALL) {
    grid->plane_count =
        vkr_brush_hull(pieces[chosen].points, pieces[chosen].point_count,
                       grid->planes, VKR_BRUSH_FACE_MAX);
    ok = grid->plane_count &&
         vkr_brush_build(grid->planes, grid->plane_count, &grid->geometry,
                         NULL) == VKR_BRUSH_OK;
    for (uint32_t f = 0; ok && f < grid->geometry.face_count; ++f) {
      grid->faces[f] = (VkrEntityId){.u64 = (UINT64_C(1) << 63) |
                                            ((uint64_t)chosen << 8) | f};
    }
  }
  free(pieces);
  return ok;
}

static VkrEditorBrushGrid *grid_state(VkrEditorUi *editor) {
  if (!editor->brush_grid) {
    editor->brush_grid = calloc(1u, sizeof(*editor->brush_grid));
  }
  return editor->brush_grid;
}

void vkr_editor_brush_grid_update(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  const VkrScene *scene, VkrEntityId brush,
                                  VkrEditorBrushGridMode mode, Vec3 origin,
                                  Vec3 direction, bool8_t has_ray,
                                  bool8_t inside, bool8_t blocked) {
  if (!brush.u64 && !editor->brush_grid) {
    return;
  }
  VkrEditorBrushGrid *grid = grid_state(editor);
  if (!grid) {
    return;
  }
  grid->hot = (GridTarget){.hit = GRID_HIT_NONE};
  grid->line_count = 0u;
  /* Another brush, or none, or another mode ends the patch, any drag and
     the cut's points. */
  if (brush.u64 != grid->brush.u64 || mode != grid->mode) {
    grid->brush = brush;
    grid->mode = mode;
    grid->patch = false_v;
    grid->drag = GRID_HIT_NONE;
    grid->clip_count = 0u;
    grid->gathered_count = 0u;
  }
  if (!brush.u64 || !scene) {
    return;
  }
  grid->scene = scene;
  /* A corridor shows the grid of the wall under the pointer; other shapes
     have none. */
  const SceneBlockout *shape =
      vkr_scene_get_typed(scene, brush, &vkr_scene_blockout_type);
  grid->shape_group = shape ? brush : VKR_ENTITY_ID_INVALID;
  bool8_t built = false_v;
  if (shape) {
    built = shape->shape == SCENE_BLOCKOUT_CORRIDOR &&
            mode == VKR_EDITOR_BRUSH_GRID_EDIT &&
            grid_shape_wall(grid, scene, brush, origin, direction,
                            has_ray && inside && !blocked);
  } else {
    grid->plane_count = vkr_editor_brush_world_planes(
        scene, brush, grid->planes, VKR_BRUSH_FACE_MAX);
    built = grid->plane_count &&
            vkr_editor_brush_build(scene, brush, &grid->geometry,
                                   grid->faces) == grid->plane_count;
  }
  if (!built) {
    grid->plane_count = 0u;
    grid->patch = false_v;
    grid->drag = GRID_HIT_NONE;
    grid->clip_count = 0u;
    return;
  }
  /* A patch lives on a face of this brush. */
  if (grid->patch) {
    bool8_t found = false_v;
    for (uint32_t f = 0; f < grid->geometry.face_count && !found; ++f) {
      found = grid->faces[f].u64 == grid->patch_face.u64;
    }
    grid->patch = found;
  }
  Vec3 eye = origin;
  if (grid->patch) {
    Vec3 from = {0};
    Vec3 to = {0};
    grid->arrow_length = 0.0f;
    grid_patch_arrow(grid, &from, &to);
    grid->arrow_length = Max(0.25f, 0.08f * vec3_length(vec3_sub(eye, from)));
  }

  VkrUiSystem *ui = frame->ui;
  const Vec4 image = frame->mapping.image_rect_px;
  if (mode == VKR_EDITOR_BRUSH_GRID_CLIP) {
    grid_clip_pointer(editor, grid, frame, origin, direction, has_ray,
                      inside && !blocked);
  } else if (grid->drag != GRID_HIT_NONE) {
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
        (VkrUiRect){image.x, image.y, image.z, image.w});
    grid_drag(editor, grid, frame, origin, direction, has_ray);
  } else if (has_ray && inside && !blocked) {
    /* With Ctrl only corners and edges answer, to gather them; elsewhere
       Ctrl+click still selects objects. */
    const bool8_t gather = vkr_editor_selection_modifier(frame);
    grid->hot = grid_pick(grid, frame, origin, direction,
                          mode == VKR_EDITOR_BRUSH_GRID_EDIT && !gather);
    if (gather && grid->hot.hit != GRID_HIT_VERTEX &&
        grid->hot.hit != GRID_HIT_EDGE) {
      grid->hot = (GridTarget){.hit = GRID_HIT_NONE};
    }
    if (grid->hot.hit != GRID_HIT_NONE) {
      /* The Scene stops picking objects under a grid target. */
      (void)vkr_ui_input_layer_register(
          ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
          (VkrUiRect){image.x, image.y, image.z, image.w});
      if (ui->mouse_pressed && gather) {
        grid_gather(grid, &grid->hot);
      } else if (ui->mouse_pressed) {
        grid_press(grid, frame, origin, direction);
      }
    }
  }
  if (grid->drag == GRID_HIT_NONE && (grid->patch || grid->gathered_count) &&
      input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    grid->patch = false_v;
    grid->gathered_count = 0u;
  }
  grid_build_lines(grid, frame, eye);
}

bool8_t vkr_editor_brush_grid_busy(const VkrEditorUi *editor) {
  const VkrEditorBrushGrid *grid = editor->brush_grid;
  return grid && grid->brush.u64 &&
         (grid->hot.hit != GRID_HIT_NONE || grid->drag != GRID_HIT_NONE);
}

/* Tiles selected on a wall of a blockout corridor become an opening of the
   corridor, which keeps it through later builds. False when the patch is not
   on such a wall. */
static bool8_t grid_cut_opening(VkrEditorUi *editor,
                                const VkrEditorBrushGrid *grid) {
  const VkrScene *scene = grid->scene;
  const SceneBlockout *shape =
      grid->shape_group.u64 && scene
          ? vkr_scene_get_typed(scene, grid->shape_group,
                                &vkr_scene_blockout_type)
          : NULL;
  const SceneTransform *group =
      shape ? vkr_entity_get_component(scene->world, grid->shape_group,
                                       scene->comp_transform)
            : NULL;
  if (!group || shape->shape != SCENE_BLOCKOUT_CORRIDOR ||
      fabsf(grid->patch_normal.y) > 0.5f ||
      shape->opening_count >= SCENE_BLOCKOUT_OPENING_MAX) {
    return false_v;
  }
  /* The patch's corners in the corridor's space, measured along its wall
     and above its floor. */
  const Mat4 inverse = mat4_inverse_affine(group->world);
  const float32_t s[2] = {grid->patch_min[0], grid->patch_max[0]};
  const float32_t t[2] = {grid->patch_min[1], grid->patch_max[1]};
  uint32_t wall = UINT32_MAX;
  Vec4 opening = vec4_new(INFINITY, -INFINITY, INFINITY, -INFINITY);
  for (uint32_t c = 0; c < 4u; ++c) {
    const Vec3 world =
        grid_plane_point(grid->patch_normal, grid->patch_plane, grid->patch_u,
                         grid->patch_v, s[c & 1u], t[c >> 1u]);
    const Vec3 local = mat4_mul_vec3(inverse, world);
    uint32_t at_wall = 0u;
    float32_t along = 0.0f;
    float32_t floor = 0.0f;
    if (!vkr_blockout_corridor_wall_at(shape, local, &at_wall, &along,
                                       &floor) ||
        (wall != UINT32_MAX && at_wall != wall)) {
      return false_v;
    }
    wall = at_wall;
    opening.x = Min(opening.x, along);
    opening.y = Max(opening.y, along);
    opening.z = Min(opening.z, local.y - floor);
    opening.w = Max(opening.w, local.y - floor);
  }
  SceneBlockout next = *shape;
  next.walls[next.opening_count] = wall;
  next.openings[next.opening_count] = opening;
  next.opening_count++;
  vkr_editor_blockout_submit(editor, grid->shape_group, &next);
  return true_v;
}

bool8_t vkr_editor_brush_grid_patch_cut(VkrEditorUi *editor) {
  VkrEditorBrushGrid *grid = editor->brush_grid;
  if (!grid || !grid->brush.u64 || !grid->patch || !grid->plane_count ||
      grid->drag != GRID_HIT_NONE) {
    return false_v;
  }
  /* The brush's depth behind the patch's face, and a little more, so the
     cut leaves no skin at the back. */
  float32_t deepest = 0.0f;
  for (uint32_t v = 0; v < grid->geometry.vertex_count; ++v) {
    deepest =
        Max(deepest, grid->patch_plane - vec3_dot(grid->patch_normal,
                                                  grid->geometry.vertices[v]));
  }
  if (deepest < 1.0e-3f) {
    return false_v;
  }
  if (grid_cut_opening(editor, grid)) {
    grid->patch = false_v;
    return true_v;
  }
  /* A shape's tiles cut only as openings of a corridor's wall. */
  if (grid->shape_group.u64) {
    snprintf(grid->message, sizeof(grid->message),
             "These tiles do not cut: select them on one wall, and below "
             "eight openings");
    return true_v;
  }
  grid->distance = -(deepest + 0.05f);
  grid_submit_patch(editor, grid);
  grid->patch = false_v;
  return true_v;
}

bool8_t vkr_editor_brush_grid_clip_pending(const VkrEditorUi *editor) {
  const VkrEditorBrushGrid *grid = editor->brush_grid;
  return grid && grid->brush.u64 && grid->mode == VKR_EDITOR_BRUSH_GRID_CLIP &&
         grid->clip_count > 0u;
}

/* What the Clip tool's next click does. */
static const char *grid_clip_hint(const VkrEditorBrushGrid *grid) {
  if (grid->message[0]) {
    return grid->message;
  }
  if (grid->clip_count == 1u) {
    return "Click the cut's second point; Shift+click adds a third for a "
           "slanted cut. Esc drops the points.";
  }
  if (grid->clip_count > 1u) {
    return "Click the slanted cut's third point. Esc drops the points.";
  }
  switch (grid->hot.hit) {
  case GRID_HIT_LINE:
    return "Click to cut the brush along this grid line.";
  case GRID_HIT_POINT:
    return "Click to start a cut through this point.";
  default:
    break;
  }
  return "Click a grid line to cut along it, or a corner, an edge or a grid "
         "crossing to start a cut. Esc stops.";
}

const char *vkr_editor_brush_grid_hint(const VkrEditorUi *editor) {
  const VkrEditorBrushGrid *grid = editor->brush_grid;
  if (!grid || !grid->brush.u64) {
    return NULL;
  }
  if (grid->mode == VKR_EDITOR_BRUSH_GRID_CLIP) {
    return grid_clip_hint(grid);
  }
  switch (grid->drag) {
  case GRID_HIT_CELL:
    return "Drag across cells to grow the patch.";
  case GRID_HIT_PATCH_ARROW:
    return grid->distance > 0.0f   ? "Release to pull the patch out."
           : grid->distance < 0.0f ? "Release to push the patch in."
                                   : "Drag out to pull the patch, in to push "
                                     "it. Esc cancels.";
  case GRID_HIT_LINE:
  case GRID_HIT_EDGE:
  case GRID_HIT_VERTEX:
    return grid->message[0] ? grid->message
                            : "Release to reshape the brush. Esc cancels.";
  default:
    break;
  }
  if (grid->gathered_count && grid->drag == GRID_HIT_NONE) {
    VkrEditorBrushGrid *hint = editor->brush_grid;
    snprintf(hint->patch_hint, sizeof(hint->patch_hint),
             "%u corners gathered: drag one to move them together. "
             "Ctrl+click adds or takes out corners and edges; Esc clears.",
             grid->gathered_count);
    return hint->patch_hint;
  }
  switch (grid->hot.hit) {
  case GRID_HIT_CELL:
    return "Drag across grid cells to select a patch; double click selects "
           "the face.";
  case GRID_HIT_LINE:
    return "Drag the grid line to bend the face; Shift slides it along.";
  case GRID_HIT_EDGE:
    return "Drag the edge in or out; Shift slides it along the face.";
  case GRID_HIT_VERTEX:
    return "Drag the corner in or out; Shift slides it along the face; "
           "Ctrl+click gathers corners to move together.";
  case GRID_HIT_PATCH_ARROW:
    return "Drag the arrow out to pull the patch, in to push it.";
  default:
    break;
  }
  if (grid->patch && grid->message[0]) {
    return grid->message;
  }
  if (grid->patch) {
    VkrEditorBrushGrid *hint = editor->brush_grid;
    snprintf(hint->patch_hint, sizeof(hint->patch_hint),
             grid->shape_group.u64
                 ? "Tiles %g x %g m: Delete cuts them out as an opening. Esc "
                   "clears them."
                 : "Tiles %g x %g m: Delete cuts them out, or drag the arrow "
                   "to pull or push them. Esc clears them.",
             grid->patch_max[0] - grid->patch_min[0],
             grid->patch_max[1] - grid->patch_min[1]);
    return hint->patch_hint;
  }
  return grid->shape_group.u64
             ? "Drag across a wall's tiles, or double click it, and press "
               "Delete to cut an opening; Bake turns the corridor into "
               "brushes to edit."
             : "Drag across grid cells to select tiles, or double click a "
               "face for all of it; Delete cuts selected tiles out.";
}

uint32_t vkr_editor_brush_grid_lines(const VkrEditorUi *editor,
                                     const VkrEditorBrushGridLine **out) {
  const VkrEditorBrushGrid *grid = editor->brush_grid;
  if (out) {
    *out = grid ? grid->lines : NULL;
  }
  return grid && grid->brush.u64 ? grid->line_count : 0u;
}

void vkr_editor_brush_grid_destroy(VkrEditorUi *editor) {
  free(editor->brush_grid);
  editor->brush_grid = NULL;
}
