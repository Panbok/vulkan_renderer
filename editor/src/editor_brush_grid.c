#include "editor_brush_grid.h"

#include "editor_agent.h"
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

typedef enum GridHit {
  GRID_HIT_NONE = 0,
  GRID_HIT_CELL,
  GRID_HIT_LINE,
  GRID_HIT_EDGE,
  GRID_HIT_VERTEX,
  GRID_HIT_PATCH_ARROW,
} GridHit;

/* What the pointer touches: the corners a reshape moves, the plane a grid
   line splits the brush along, and the face whose normal a drag follows. */
typedef struct GridTarget {
  GridHit hit;
  uint32_t face;
  Vec3 normal;
  Vec3 u;
  Vec3 v;
  float32_t step;
  Vec3 points[2];
  uint32_t point_count;
  bool8_t split;
  Vec3 split_normal;
  float32_t split_distance;
  float32_t cell[2];
} GridTarget;

typedef struct VkrEditorBrushGrid {
  VkrEntityId brush;
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
  VkrBrushPiece preview[2];
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

/* The distance from the ray to segment a-b, and how far along the ray the
   closest point lies. */
static float32_t grid_ray_segment(Vec3 origin, Vec3 direction, Vec3 a, Vec3 b,
                                  float32_t *out_along) {
  const Vec3 e = vec3_sub(b, a);
  const Vec3 w = vec3_sub(origin, a);
  const float32_t be = vec3_dot(direction, e);
  const float32_t ee = vec3_dot(e, e);
  const float32_t denominator = ee - be * be;
  float32_t s = 0.0f;
  if (denominator > 1.0e-8f) {
    s = (ee * 0.0f + vec3_dot(e, w) - be * vec3_dot(direction, w)) /
        denominator;
    s = Min(Max(s, 0.0f), 1.0f);
  }
  const Vec3 point = vec3_add(a, vec3_scale(e, s));
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
static GridTarget grid_pick(const VkrEditorBrushGrid *grid,
                            const VkrSampleUiFrame *frame, Vec3 origin,
                            Vec3 direction, bool8_t cells) {
  GridTarget target = {.hit = GRID_HIT_NONE};
  const VkrBrushGeometry *geometry = &grid->geometry;
  float32_t hit_distance = INFINITY;
  const uint32_t hit_face =
      grid_ray_face(grid, origin, direction, &hit_distance);
  /* Hidden corners and edges lie behind the face the ray enters. */
  const float32_t reach =
      hit_face != UINT32_MAX ? hit_distance * 1.02f + 1.0e-3f : INFINITY;

  if (grid->patch) {
    Vec3 from = {0};
    Vec3 to = {0};
    grid_patch_arrow(grid, &from, &to);
    float32_t along = 0.0f;
    const float32_t gap = grid_ray_segment(origin, direction, from, to, &along);
    if (along > 0.0f && gap < 0.03f * along) {
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
      const Vec3 to_corner = vec3_sub(corner, origin);
      const float32_t along = vec3_dot(to_corner, direction);
      const float32_t gap =
          vec3_length(vec3_sub(to_corner, vec3_scale(direction, along)));
      /* The face the ray enters wins a corner its neighbours share. */
      const float32_t score = gap - (f == hit_face ? 1.0e-4f : 0.0f);
      if (along > 0.0f && along <= reach && gap < 0.025f * along &&
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
      const float32_t gap = grid_ray_segment(origin, direction, a, b, &along);
      const float32_t score = gap - (f == hit_face ? 1.0e-4f : 0.0f);
      if (along > 0.0f && along <= reach && gap < 0.015f * along &&
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
  const float32_t tolerance = 0.012f * hit_distance;
  for (uint32_t axis = 0; axis < 2u; ++axis) {
    const float32_t value = roundf(coordinates[axis] / step) * step;
    Vec3 a = {0};
    Vec3 b = {0};
    if (fabsf(coordinates[axis] - value) < tolerance &&
        grid_clip(grid, hit_face, axes[axis], value, &a, &b)) {
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
                          .split_distance = value};
    }
  }
  if (!cells) {
    return target;
  }
  return (GridTarget){
      .hit = GRID_HIT_CELL,
      .face = hit_face,
      .normal = geometry->normals[hit_face],
      .u = u,
      .v = v,
      .step = step,
      .cell = {floorf(coordinates[0] / step) * step,
               floorf(coordinates[1] / step) * step},
  };
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

static void grid_submit_patch(VkrEditorUi *editor,
                              const VkrEditorBrushGrid *grid) {
  char face[48];
  grid_entity_text(grid->patch_face, face, sizeof(face));
  char line[512];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"grid\",\"op\":\"brush.patch\",\"args\":{"
           "\"face\":\"%s\",\"min\":[%.9g,%.9g],\"max\":[%.9g,%.9g],"
           "\"distance\":%.9g,\"review\":false,\"select\":true}}",
           face, grid->patch_min[0], grid->patch_min[1], grid->patch_max[0],
           grid->patch_max[1], grid->distance);
  (void)vkr_editor_agent_submit(editor->agent, line);
}

static void grid_submit_reshape(VkrEditorUi *editor,
                                const VkrEditorBrushGrid *grid) {
  const GridTarget *target = &grid->target;
  char brush[48];
  grid_entity_text(grid->brush, brush, sizeof(brush));
  char points[256] = {0};
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
  char line[1024];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"grid\",\"op\":\"brush.reshape\",\"args\":{"
           "\"brush\":\"%s\",\"points\":[%s],\"delta\":[%.9g,%.9g,%.9g]%s,"
           "\"review\":false,\"select\":true}}",
           brush, points, grid->delta.x, grid->delta.y, grid->delta.z, split);
  (void)vkr_editor_agent_submit(editor->agent, line);
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
      &grid->preview_count, &grid->scratch);
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
    }
  }
}

static void grid_press(VkrEditorBrushGrid *grid, const VkrSampleUiFrame *frame,
                       Vec3 origin, Vec3 direction) {
  const GridTarget hot = grid->hot;
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

// =============================================================================
// Overlay
// =============================================================================

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

  if (grid->patch) {
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

  if (grid->drag == GRID_HIT_LINE || grid->drag == GRID_HIT_EDGE ||
      grid->drag == GRID_HIT_VERTEX) {
    for (uint32_t p = 0; p < grid->preview_count; ++p) {
      if (vkr_brush_build(grid->preview[p].planes, grid->preview[p].count,
                          &grid->scratch, NULL) != VKR_BRUSH_OK) {
        continue;
      }
      for (uint32_t f = 0; f < grid->scratch.face_count; ++f) {
        const VkrBrushPolygon polygon = grid->scratch.polygons[f];
        for (uint32_t c = 0; c < polygon.count; ++c) {
          grid_line(
              grid, grid->scratch.vertices[polygon.first + c],
              grid->scratch.vertices[polygon.first + (c + 1u) % polygon.count],
              GRID_PREVIEW_COLOR);
        }
      }
    }
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

static VkrEditorBrushGrid *grid_state(VkrEditorUi *editor) {
  if (!editor->brush_grid) {
    editor->brush_grid = calloc(1u, sizeof(*editor->brush_grid));
  }
  return editor->brush_grid;
}

void vkr_editor_brush_grid_update(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  const VkrScene *scene, VkrEntityId brush,
                                  bool8_t cells, Vec3 origin, Vec3 direction,
                                  bool8_t has_ray, bool8_t inside,
                                  bool8_t blocked) {
  if (!brush.u64 && !editor->brush_grid) {
    return;
  }
  VkrEditorBrushGrid *grid = grid_state(editor);
  if (!grid) {
    return;
  }
  grid->hot = (GridTarget){.hit = GRID_HIT_NONE};
  grid->line_count = 0u;
  /* Another brush, or none, ends the patch and any drag. */
  if (brush.u64 != grid->brush.u64) {
    grid->brush = brush;
    grid->patch = false_v;
    grid->drag = GRID_HIT_NONE;
  }
  if (!brush.u64 || !scene) {
    return;
  }
  grid->plane_count = vkr_editor_brush_world_planes(scene, brush, grid->planes,
                                                    VKR_BRUSH_FACE_MAX);
  if (!grid->plane_count ||
      vkr_editor_brush_build(scene, brush, &grid->geometry, grid->faces) !=
          grid->plane_count) {
    grid->plane_count = 0u;
    grid->patch = false_v;
    grid->drag = GRID_HIT_NONE;
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
  if (grid->drag != GRID_HIT_NONE) {
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
        (VkrUiRect){image.x, image.y, image.z, image.w});
    grid_drag(editor, grid, frame, origin, direction, has_ray);
  } else if (has_ray && inside && !blocked &&
             !vkr_editor_selection_modifier(frame)) {
    grid->hot = grid_pick(grid, frame, origin, direction, cells);
    if (grid->hot.hit != GRID_HIT_NONE) {
      /* The Scene stops picking objects under a grid target. */
      (void)vkr_ui_input_layer_register(
          ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
          (VkrUiRect){image.x, image.y, image.z, image.w});
      if (ui->mouse_pressed) {
        grid_press(grid, frame, origin, direction);
      }
    }
  }
  if (grid->drag == GRID_HIT_NONE && grid->patch &&
      input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    grid->patch = false_v;
  }
  grid_build_lines(grid, frame, eye);
}

bool8_t vkr_editor_brush_grid_busy(const VkrEditorUi *editor) {
  const VkrEditorBrushGrid *grid = editor->brush_grid;
  return grid && grid->brush.u64 &&
         (grid->hot.hit != GRID_HIT_NONE || grid->drag != GRID_HIT_NONE);
}

const char *vkr_editor_brush_grid_hint(const VkrEditorUi *editor) {
  const VkrEditorBrushGrid *grid = editor->brush_grid;
  if (!grid || !grid->brush.u64) {
    return NULL;
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
  switch (grid->hot.hit) {
  case GRID_HIT_CELL:
    return "Drag across grid cells to select a patch; double click selects "
           "the face.";
  case GRID_HIT_LINE:
    return "Drag the grid line to bend the face; Shift slides it along.";
  case GRID_HIT_EDGE:
    return "Drag the edge in or out; Shift slides it along the face.";
  case GRID_HIT_VERTEX:
    return "Drag the corner in or out; Shift slides it along the face.";
  case GRID_HIT_PATCH_ARROW:
    return "Drag the arrow out to pull the patch, in to push it.";
  default:
    break;
  }
  return grid->patch ? "Drag the patch's arrow to pull or push it. Esc clears "
                       "the patch."
                     : "Drag across grid cells to select a patch, or double "
                       "click a face for all of it.";
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
