#include "editor_blockout.h"

#include "editor_agent.h"
#include "editor_internal.h"
#include "editor_level.h"
#include "editor_ops.h"
#include "level/vkr_brush.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Most overlay segments a frame: the shape a drag previews and the
   handles. */
#define BLOCKOUT_LINE_MAX 1536u
/* Pieces a preview outlines at most; a longer shape shows every n-th piece
   and its last, so the whole of it stays in view. */
#define BLOCKOUT_PREVIEW_PIECE_MAX 112u
/* Most handles a shape shows: a corridor's points, the + between them, and
   its height and width. */
#define BLOCKOUT_HANDLE_MAX (3u * SCENE_BLOCKOUT_POINT_MAX + 2u)
/* How near the pointer a handle takes it, in points on screen. */
#define BLOCKOUT_HANDLE_PT 10.0f

typedef enum HandleKind {
  HANDLE_LENGTH = 0,
  HANDLE_HEIGHT,
  HANDLE_WIDTH,
  HANDLE_TURN,
  HANDLE_RADIUS,
  HANDLE_POINT,
  HANDLE_INSERT,
  /* A corridor corner's radius, from the arc's middle. */
  HANDLE_CORNER,
} HandleKind;

/* A handle in the group's space: where it sits and, for a length, height,
   width or radius, the axis a drag follows and how much the value grows per
   meter along it. */
typedef struct BlockoutHandle {
  HandleKind kind;
  uint32_t index;
  Vec3 at;
  Vec3 axis;
  float32_t rate;
} BlockoutHandle;

typedef struct VkrEditorBlockout {
  VkrEntityId group;
  /* The group's world matrix and its inverse this frame. */
  Mat4 world;
  Mat4 inverse;
  SceneBlockout shape;
  uint32_t handle_count;
  BlockoutHandle handles[BLOCKOUT_HANDLE_MAX];
  int32_t hot;
  /* The drag in progress: its handle, the shape as it started and as it
     would be now, and where the pointer pressed. */
  bool8_t dragging;
  BlockoutHandle drag;
  SceneBlockout start;
  SceneBlockout draft;
  float32_t press_param;
  Vec3 press_point;
  float32_t turn_accumulated;
  Vec3 turn_previous;
  /* A Details draft shown as a preview. */
  bool8_t previewing;
  VkrEntityId preview_group;
  SceneBlockout preview;
  char message[160];
  uint32_t line_count;
  VkrEditorBrushGridLine lines[BLOCKOUT_LINE_MAX];
  VkrBrushGeometry scratch;
} VkrEditorBlockout;

static const Vec4 BLOCKOUT_HANDLE_COLOR = {0.45f, 0.75f, 1.0f, 1.0f};
static const Vec4 BLOCKOUT_HOT_COLOR = {1.0f, 1.0f, 0.55f, 1.0f};
static const Vec4 BLOCKOUT_PREVIEW_COLOR = {1.0f, 0.6f, 0.2f, 1.0f};
static const Vec4 BLOCKOUT_INSERT_COLOR = {0.4f, 1.0f, 0.45f, 1.0f};

static VkrEditorBlockout *blockout_state(VkrEditorUi *editor) {
  if (!editor->blockout) {
    editor->blockout = calloc(1u, sizeof(*editor->blockout));
  }
  return editor->blockout;
}

static void blockout_line(VkrEditorBlockout *state, Vec3 from, Vec3 to,
                          Vec4 color) {
  if (state->line_count < BLOCKOUT_LINE_MAX) {
    state->lines[state->line_count++] =
        (VkrEditorBrushGridLine){.from = from, .to = to, .color = color};
  }
}

static Vec3 blockout_world(const VkrEditorBlockout *state, Vec3 local) {
  return mat4_mul_vec3(state->world, local);
}

static Vec3 blockout_world_direction(const VkrEditorBlockout *state,
                                     Vec3 local) {
  const Vec4 v = mat4_mul_vec4(state->world, vec3_to_vec4(local, 0.0f));
  return vec3_normalize(vec3_new(v.x, v.y, v.z));
}

// =============================================================================
// Handles
// =============================================================================

/* The center of a curved flight's arc, and a point of its arc. */
static Vec3 blockout_arc_center(const SceneBlockout *shape) {
  const float32_t side = shape->left ? -1.0f : 1.0f;
  return shape->stairs == SCENE_STAIRS_SPIRAL
             ? vec3_zero()
             : vec3_new(side * (shape->radius + shape->width * 0.5f), 0.0f,
                        0.0f);
}

static Vec3 blockout_arc_point(const SceneBlockout *shape, float32_t degrees,
                               float32_t radius) {
  const float32_t side = shape->left ? -1.0f : 1.0f;
  const float32_t a = degrees * 0.0174532925f;
  const Vec3 center = blockout_arc_center(shape);
  if (shape->stairs == SCENE_STAIRS_SPIRAL) {
    return vec3_new(side * sinf(a) * radius, 0.0f, cosf(a) * radius);
  }
  return vec3_add(center,
                  vec3_new(-side * cosf(a) * radius, 0.0f, sinf(a) * radius));
}

/* Corner `i` of a corridor: its half-turn's secant, 1 / cos(angle / 2), and
   the unit direction from the corner into the bend; false when straight. */
static bool8_t blockout_corner(const SceneBlockout *shape, uint32_t i,
                               float32_t *out_secant, Vec3 *out_inward) {
  Vec3 d0 = vec3_sub(shape->points[i], shape->points[i - 1u]);
  Vec3 d1 = vec3_sub(shape->points[i + 1u], shape->points[i]);
  d0.y = 0.0f;
  d1.y = 0.0f;
  if (vec3_length(d0) < 1.0e-3f || vec3_length(d1) < 1.0e-3f) {
    return false_v;
  }
  d0 = vec3_normalize(d0);
  d1 = vec3_normalize(d1);
  const Vec3 inward = vec3_sub(d1, d0);
  const float32_t cosine = vec3_dot(d0, d1);
  if (vec3_length(inward) < 1.0e-3f || cosine > 0.9998f) {
    return false_v;
  }
  /* The angle between the stretches is the turn; half of it sets how far
     the arc's middle lies from the corner. */
  const float32_t half_turn = acosf(vkr_clamp_f32(cosine, -1.0f, 1.0f)) * 0.5f;
  *out_secant = 1.0f / Max(cosf(half_turn), 0.05f);
  *out_inward = vec3_normalize(inward);
  return true_v;
}

/* How far the arc of corner `i` bulges from the corner, for its radius
   (shrunk to half its stretches, as the layout does). */
static float32_t blockout_corner_bulge(const SceneBlockout *shape, uint32_t i,
                                       float32_t radius, float32_t secant) {
  const float32_t len0 =
      vec3_length(vec3_sub(shape->points[i], shape->points[i - 1u]));
  const float32_t len1 =
      vec3_length(vec3_sub(shape->points[i + 1u], shape->points[i]));
  const float32_t tangent = sqrtf(Max(secant * secant - 1.0f, 0.0f));
  const float32_t reach = Min(radius * tangent, Min(len0, len1) * 0.5f);
  const float32_t fitted = tangent > 1.0e-4f ? reach / tangent : radius;
  return fitted * (secant - 1.0f);
}

static void blockout_add(VkrEditorBlockout *state, HandleKind kind,
                         uint32_t index, Vec3 at, Vec3 axis, float32_t rate) {
  if (state->handle_count < BLOCKOUT_HANDLE_MAX) {
    state->handles[state->handle_count++] = (BlockoutHandle){
        .kind = kind, .index = index, .at = at, .axis = axis, .rate = rate};
  }
}

/* The handles of `shape`, in the group's space. */
static void blockout_handles(VkrEditorBlockout *state,
                             const SceneBlockout *shape) {
  state->handle_count = 0u;
  const Vec3 up = {0.0f, 1.0f, 0.0f};
  const float32_t side = shape->left ? -1.0f : 1.0f;
  const float32_t half = shape->width * 0.5f;
  if (shape->shape == SCENE_BLOCKOUT_CORRIDOR) {
    const uint32_t n = shape->point_count;
    for (uint32_t i = 0; i < n; ++i) {
      blockout_add(state, HANDLE_POINT, i, shape->points[i], vec3_zero(), 0);
    }
    /* Each bend's radius from the middle of its arc, a little off the
       corner when the bend is sharp. */
    for (uint32_t i = 1; i + 1u < n; ++i) {
      float32_t secant = 1.0f;
      Vec3 inward = vec3_zero();
      if (!blockout_corner(shape, i, &secant, &inward)) {
        continue;
      }
      const float32_t radius =
          shape->corners[i] >= 0.0f ? shape->corners[i] : shape->radius;
      const float32_t bulge =
          Max(blockout_corner_bulge(shape, i, radius, secant), 0.4f);
      blockout_add(state, HANDLE_CORNER, i,
                   vec3_add(shape->points[i], vec3_scale(inward, bulge)),
                   inward, 1.0f);
    }
    for (uint32_t i = 0; i + 1u < n && n < SCENE_BLOCKOUT_POINT_MAX; ++i) {
      blockout_add(
          state, HANDLE_INSERT, i + 1u,
          vec3_scale(vec3_add(shape->points[i], shape->points[i + 1u]), 0.5f),
          vec3_zero(), 0);
    }
    if (n >= 2u) {
      Vec3 along = vec3_sub(shape->points[1], shape->points[0]);
      along.y = 0.0f;
      const Vec3 across =
          vec3_length(along) > 1.0e-4f
              ? vec3_normalize(vec3_new(along.z, 0.0f, -along.x))
              : vec3_new(1.0f, 0.0f, 0.0f);
      blockout_add(state, HANDLE_HEIGHT, 0u,
                   vec3_add(shape->points[0], vec3_scale(up, shape->height)),
                   up, 1.0f);
      blockout_add(state, HANDLE_WIDTH, 0u,
                   vec3_add(shape->points[0], vec3_scale(across, half)), across,
                   2.0f);
    }
    return;
  }
  switch (shape->stairs) {
  case SCENE_STAIRS_CURVED:
  case SCENE_STAIRS_SPIRAL: {
    const bool8_t spiral = shape->stairs == SCENE_STAIRS_SPIRAL;
    const float32_t inner = spiral ? Max(shape->radius, 0.1f) : shape->radius;
    const Vec3 end =
        blockout_arc_point(shape, shape->turn, inner + shape->width * 0.5f);
    blockout_add(state, HANDLE_TURN, 0u,
                 vec3_add(end, vec3_scale(up, shape->height)), vec3_zero(), 0);
    blockout_add(state, HANDLE_HEIGHT, 0u,
                 spiral ? vec3_new(0.0f, shape->height, 0.0f)
                        : vec3_add(end, vec3_scale(up, shape->height * 0.5f)),
                 up, 1.0f);
    const Vec3 rim = blockout_arc_point(shape, 0.0f, inner + shape->width);
    const Vec3 hub = blockout_arc_point(shape, 0.0f, inner);
    const Vec3 outward =
        vec3_normalize(vec3_sub(rim, blockout_arc_center(shape)));
    blockout_add(state, HANDLE_WIDTH, 0u, rim, outward, 1.0f);
    /* The inner edge moves the arc's center with the radius. */
    blockout_add(state, HANDLE_RADIUS, 0u, hub,
                 spiral ? outward : vec3_scale(outward, -1.0f), 1.0f);
    (void)side;
    break;
  }
  default: {
    const float32_t length = shape->length;
    blockout_add(state, HANDLE_LENGTH, 0u, vec3_new(0.0f, 0.0f, length),
                 vec3_new(0.0f, 0.0f, 1.0f), 1.0f);
    blockout_add(state, HANDLE_HEIGHT, 0u,
                 vec3_new(0.0f, shape->height, length), up, 1.0f);
    /* Turning stairs leave toward `side`; the width handle stays clear. */
    const float32_t across =
        shape->stairs == SCENE_STAIRS_STRAIGHT ? 1.0f : -side;
    blockout_add(state, HANDLE_WIDTH, 0u,
                 vec3_new(across * half, 0.0f, length * 0.5f),
                 vec3_new(across, 0.0f, 0.0f), 2.0f);
    break;
  }
  }
}

// =============================================================================
// Drags
// =============================================================================

/* The parameter along the line through `anchor` along unit `axis` closest
   to the ray. */
static bool8_t blockout_axis_param(Vec3 anchor, Vec3 axis, Vec3 origin,
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

/* Where the ray meets the horizontal plane at `height`. */
static bool8_t blockout_ray_level(Vec3 origin, Vec3 direction, float32_t height,
                                  Vec3 *out) {
  if (fabsf(direction.y) < 1.0e-6f) {
    return false_v;
  }
  const float32_t t = (height - origin.y) / direction.y;
  if (!(t > 0.0f)) {
    return false_v;
  }
  *out = vec3_add(origin, vec3_scale(direction, t));
  return true_v;
}

static float32_t blockout_step(const VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  return editor->placement.target == VKR_EDITOR_SNAP_FREE
             ? 0.0625f
             : (frame->view_state.grid_spacing > 0.0f
                    ? frame->view_state.grid_spacing
                    : 1.0f);
}

static float32_t *blockout_value(SceneBlockout *shape, HandleKind kind) {
  switch (kind) {
  case HANDLE_LENGTH:
    return &shape->length;
  case HANDLE_HEIGHT:
    return &shape->height;
  case HANDLE_WIDTH:
    return &shape->width;
  case HANDLE_RADIUS:
    return &shape->radius;
  default:
    return NULL;
  }
}

/* The drag's shape for the pointer now. */
static void blockout_drag(VkrEditorUi *editor, VkrEditorBlockout *state,
                          const VkrSampleUiFrame *frame, Vec3 origin,
                          Vec3 direction) {
  const BlockoutHandle *handle = &state->drag;
  const float32_t step = blockout_step(editor, frame);
  state->draft = state->start;
  switch (handle->kind) {
  case HANDLE_POINT: {
    /* The point moves on its own level, on the grid with grid snapping. */
    const Vec3 anchor =
        blockout_world(state, state->start.points[handle->index]);
    Vec3 hit = {0};
    if (!blockout_ray_level(origin, direction, anchor.y, &hit)) {
      return;
    }
    if (editor->placement.target != VKR_EDITOR_SNAP_FREE) {
      hit.x = roundf(hit.x / step) * step;
      hit.z = roundf(hit.z / step) * step;
    }
    Vec3 local = mat4_mul_vec3(state->inverse, hit);
    local.y = state->start.points[handle->index].y;
    state->draft.points[handle->index] = local;
    return;
  }
  case HANDLE_TURN: {
    /* The angle the pointer swept around the arc's center, in 15 degree
       steps (one with Free snapping). */
    const Vec3 center =
        blockout_world(state, blockout_arc_center(&state->start));
    Vec3 hit = {0};
    if (!blockout_ray_level(origin, direction, center.y, &hit)) {
      return;
    }
    Vec3 now = vec3_sub(hit, center);
    now.y = 0.0f;
    if (vec3_length(now) < 1.0e-3f) {
      return;
    }
    now = vec3_normalize(now);
    const Vec3 before = state->turn_previous;
    const float32_t crossed = before.z * now.x - before.x * now.z;
    const float32_t delta =
        atan2f(crossed, vec3_dot(before, now)) * 57.2957795f;
    state->turn_previous = now;
    /* Increasing turn moves the end the way the arc runs. */
    const Vec3 a0 =
        blockout_world(state, blockout_arc_point(&state->start, 0.0f, 1.0f));
    const Vec3 a1 =
        blockout_world(state, blockout_arc_point(&state->start, 10.0f, 1.0f));
    Vec3 r0 = vec3_sub(a0, center);
    Vec3 r1 = vec3_sub(a1, center);
    const float32_t orientation =
        r0.z * r1.x - r0.x * r1.z >= 0.0f ? 1.0f : -1.0f;
    state->turn_accumulated += delta * orientation;
    const float32_t snap =
        editor->placement.target == VKR_EDITOR_SNAP_FREE ? 1.0f : 15.0f;
    const bool8_t spiral = state->start.stairs == SCENE_STAIRS_SPIRAL;
    state->draft.turn = vkr_clamp_f32(
        roundf((state->start.turn + state->turn_accumulated) / snap) * snap,
        15.0f, spiral ? VKR_BLOCKOUT_TURN_MAX : 360.0f);
    /* A spiral's end climbs as it turns on, at the same pitch. */
    if (spiral && state->start.turn > 1.0e-3f) {
      state->draft.height =
          Min(state->start.height * state->draft.turn / state->start.turn,
              VKR_BLOCKOUT_HEIGHT_MAX);
    }
    return;
  }
  case HANDLE_CORNER: {
    float32_t param = 0.0f;
    float32_t secant = 1.0f;
    Vec3 inward = vec3_zero();
    if (!blockout_corner(&state->start, handle->index, &secant, &inward) ||
        !blockout_axis_param(blockout_world(state, handle->at),
                             blockout_world_direction(state, handle->axis),
                             origin, direction, &param)) {
      return;
    }
    /* The radius whose arc reaches the pointer's distance from the
       corner. */
    const float32_t start_radius = state->start.corners[handle->index] >= 0.0f
                                       ? state->start.corners[handle->index]
                                       : state->start.radius;
    const float32_t bulge =
        Max(blockout_corner_bulge(&state->start, handle->index, start_radius,
                                  secant),
            0.4f) +
        (param - state->press_param);
    const float32_t radius = Max(bulge, 0.0f) / Max(secant - 1.0f, 1.0e-3f);
    state->draft.corners[handle->index] =
        Min(roundf(radius / (step * 0.5f)) * (step * 0.5f), 64.0f);
    return;
  }
  default: {
    float32_t *value = blockout_value(&state->draft, handle->kind);
    float32_t param = 0.0f;
    if (!value ||
        !blockout_axis_param(blockout_world(state, handle->at),
                             blockout_world_direction(state, handle->axis),
                             origin, direction, &param)) {
      return;
    }
    const float32_t lowest = handle->kind == HANDLE_RADIUS ? 0.0f : 0.25f;
    const float32_t moved = (param - state->press_param) * handle->rate;
    *value = Max(
        lowest,
        roundf((*blockout_value(&state->start, handle->kind) + moved) / step) *
            step);
    if (handle->kind == HANDLE_HEIGHT) {
      *value = Min(*value, VKR_BLOCKOUT_HEIGHT_MAX);
    }
    /* A spiral climbs on at its pitch: dragging its top up turns it on. */
    if (handle->kind == HANDLE_HEIGHT &&
        state->start.stairs == SCENE_STAIRS_SPIRAL &&
        state->start.shape == SCENE_BLOCKOUT_STAIRS &&
        state->start.height > 1.0e-3f) {
      state->draft.turn = vkr_clamp_f32(
          state->start.turn * state->draft.height / state->start.height, 15.0f,
          VKR_BLOCKOUT_TURN_MAX);
    }
    return;
  }
  }
}

// =============================================================================
// Overlay
// =============================================================================

/* The edges of the shape `shape` would build, in world space. */
static void blockout_shape_lines(VkrEditorBlockout *state,
                                 const VkrSampleUiFrame *frame,
                                 const SceneBlockout *shape) {
  const uint32_t capacity = vkr_blockout_piece_capacity(shape);
  VkrBlockoutPiece *pieces = vkr_allocator_alloc(
      frame->ui->frame_allocator, capacity * sizeof(*pieces),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!pieces) {
    return;
  }
  state->message[0] = '\0';
  const uint32_t count = vkr_blockout_layout(
      shape, pieces, capacity, state->message, sizeof(state->message));
  const uint32_t stride =
      (count + BLOCKOUT_PREVIEW_PIECE_MAX - 1u) / BLOCKOUT_PREVIEW_PIECE_MAX;
  for (uint32_t i = 0; i < count; ++i) {
    if (stride > 1u && i % stride && i + 1u != count) {
      continue;
    }
    const VkrBlockoutPiece *piece = &pieces[i];
    Vec3 center = vec3_zero();
    for (uint32_t p = 0; p < piece->point_count; ++p) {
      center = vec3_add(center, piece->points[p]);
    }
    center = vec3_scale(center, 1.0f / (float32_t)piece->point_count);
    Vec3 local[VKR_BLOCKOUT_PIECE_POINT_MAX];
    for (uint32_t p = 0; p < piece->point_count; ++p) {
      local[p] = vec3_sub(piece->points[p], center);
    }
    VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
    const uint32_t plane_count =
        vkr_brush_hull(local, piece->point_count, planes, VKR_BRUSH_FACE_MAX);
    if (!plane_count || vkr_brush_build(planes, plane_count, &state->scratch,
                                        NULL) != VKR_BRUSH_OK) {
      continue;
    }
    for (uint32_t f = 0; f < state->scratch.face_count; ++f) {
      const VkrBrushPolygon polygon = state->scratch.polygons[f];
      for (uint32_t c = 0; c < polygon.count; ++c) {
        const Vec3 a =
            vec3_add(center, state->scratch.vertices[polygon.first + c]);
        const Vec3 b = vec3_add(
            center,
            state->scratch.vertices[polygon.first + (c + 1u) % polygon.count]);
        blockout_line(state, blockout_world(state, a), blockout_world(state, b),
                      BLOCKOUT_PREVIEW_COLOR);
      }
    }
  }
}

static void blockout_cross(VkrEditorBlockout *state, Vec3 at, float32_t size,
                           Vec4 color) {
  static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    const Vec3 half = vec3_scale(axes[axis], size);
    blockout_line(state, vec3_sub(at, half), vec3_add(at, half), color);
  }
}

static void blockout_handle_lines(VkrEditorBlockout *state, Vec3 eye) {
  for (uint32_t i = 0; i < state->handle_count; ++i) {
    const BlockoutHandle *handle = &state->handles[i];
    const Vec3 at = blockout_world(state, handle->at);
    const float32_t size = 0.012f * vec3_length(vec3_sub(eye, at));
    const bool8_t hot = (int32_t)i == state->hot ||
                        (state->dragging && handle->kind == state->drag.kind &&
                         handle->index == state->drag.index);
    const Vec4 color = hot                             ? BLOCKOUT_HOT_COLOR
                       : handle->kind == HANDLE_INSERT ? BLOCKOUT_INSERT_COLOR
                                                       : BLOCKOUT_HANDLE_COLOR;
    if (handle->kind == HANDLE_INSERT) {
      /* A + in the plane of the floor. */
      blockout_line(state, vec3_sub(at, vec3_new(size, 0, 0)),
                    vec3_add(at, vec3_new(size, 0, 0)), color);
      blockout_line(state, vec3_sub(at, vec3_new(0, 0, size)),
                    vec3_add(at, vec3_new(0, 0, size)), color);
      continue;
    }
    blockout_cross(state, at, size * (hot ? 1.6f : 1.0f), color);
    if (vec3_length(handle->axis) > 0.5f) {
      /* An arrow along the drag axis. */
      const Vec3 tip =
          vec3_add(at, vec3_scale(blockout_world_direction(state, handle->axis),
                                  size * 5.0f));
      blockout_line(state, at, tip, color);
    }
  }
}

// =============================================================================
// Update
// =============================================================================

void vkr_editor_blockout_submit(VkrEditorUi *editor, VkrEntityId group,
                                const SceneBlockout *shape) {
  char values[4096];
  if (!editor->agent ||
      !vkr_editor_ops_component_text(&vkr_scene_blockout_type, shape, values,
                                     sizeof(values))) {
    return;
  }
  char line[4352];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"blockout\",\"op\":\"blockout.build\",\"args\":{"
           "\"entity\":\"%u:%u:%u\",\"values\":%s,\"review\":false}}",
           (unsigned)group.parts.world, (unsigned)group.parts.index,
           (unsigned)group.parts.generation, values);
  (void)vkr_editor_agent_submit(editor->agent, line);
}

void vkr_editor_blockout_preview(VkrEditorUi *editor, VkrEntityId group,
                                 const SceneBlockout *draft) {
  VkrEditorBlockout *state = draft ? blockout_state(editor) : editor->blockout;
  if (!state) {
    return;
  }
  state->previewing = draft != NULL;
  state->preview_group = group;
  if (draft) {
    state->preview = *draft;
  }
}

/* The handle under the pointer, within BLOCKOUT_HANDLE_PT, or -1. */
static int32_t blockout_pick(const VkrEditorBlockout *state,
                             const VkrSampleUiFrame *frame) {
  int32_t best = -1;
  float32_t best_gap = BLOCKOUT_HANDLE_PT * frame->ui->content_scale;
  for (uint32_t i = 0; i < state->handle_count; ++i) {
    Vec2 pixel = {0};
    if (!vkr_editor_viewport_pixel(
            frame, blockout_world(state, state->handles[i].at), &pixel)) {
      continue;
    }
    const float32_t dx = pixel.x - (float32_t)frame->ui->mouse_x;
    const float32_t dy = pixel.y - (float32_t)frame->ui->mouse_y;
    const float32_t gap = sqrtf(dx * dx + dy * dy);
    if (gap < best_gap) {
      best_gap = gap;
      best = (int32_t)i;
    }
  }
  return best;
}

/* A press on a handle starts its drag; a + adds a point there first, and
   Ctrl+click on a point removes it. */
static void blockout_press(VkrEditorUi *editor, VkrEditorBlockout *state,
                           const VkrSampleUiFrame *frame, Vec3 origin,
                           Vec3 direction) {
  BlockoutHandle handle = state->handles[state->hot];
  SceneBlockout shape = state->shape;
  if (handle.kind == HANDLE_POINT && vkr_editor_selection_modifier(frame)) {
    if (shape.point_count > 2u) {
      for (uint32_t i = handle.index; i + 1u < shape.point_count; ++i) {
        shape.points[i] = shape.points[i + 1u];
        shape.corners[i] = shape.corners[i + 1u];
      }
      shape.point_count--;
      vkr_editor_blockout_submit(editor, state->group, &shape);
    }
    return;
  }
  if (handle.kind == HANDLE_INSERT) {
    for (uint32_t i = shape.point_count; i > handle.index; --i) {
      shape.points[i] = shape.points[i - 1u];
      shape.corners[i] = shape.corners[i - 1u];
    }
    shape.points[handle.index] = handle.at;
    shape.corners[handle.index] = -1.0f;
    shape.point_count++;
    handle.kind = HANDLE_POINT;
  }
  state->dragging = true_v;
  state->drag = handle;
  state->start = shape;
  state->draft = shape;
  state->turn_accumulated = 0.0f;
  if (handle.kind == HANDLE_TURN) {
    const Vec3 center = blockout_world(state, blockout_arc_center(&shape));
    Vec3 hit = {0};
    Vec3 radial = vec3_new(1.0f, 0.0f, 0.0f);
    if (blockout_ray_level(origin, direction, center.y, &hit)) {
      radial = vec3_sub(hit, center);
      radial.y = 0.0f;
    }
    state->turn_previous = vec3_length(radial) > 1.0e-3f
                               ? vec3_normalize(radial)
                               : vec3_new(1.0f, 0.0f, 0.0f);
  } else if (vec3_length(handle.axis) > 0.5f &&
             !blockout_axis_param(blockout_world(state, handle.at),
                                  blockout_world_direction(state, handle.axis),
                                  origin, direction, &state->press_param)) {
    state->dragging = false_v;
  }
}

void vkr_editor_blockout_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrEntityId group, Vec3 origin, Vec3 direction,
                                bool8_t has_ray, bool8_t inside) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, group);
  const SceneBlockout *shape =
      scene && vkr_scene_entity_alive(scene, group)
          ? vkr_scene_get_typed(scene, group, &vkr_scene_blockout_type)
          : NULL;
  const SceneTransform *transform =
      shape
          ? vkr_entity_get_component(scene->world, group, scene->comp_transform)
          : NULL;
  VkrEditorBlockout *state =
      transform ? blockout_state(editor) : editor->blockout;
  if (!state) {
    return;
  }
  state->line_count = 0u;
  state->hot = -1;
  if (!transform) {
    state->group = VKR_ENTITY_ID_INVALID;
    state->dragging = false_v;
    return;
  }
  if (group.u64 != state->group.u64) {
    state->dragging = false_v;
  }
  state->group = group;
  state->world = transform->world;
  state->inverse = mat4_inverse_affine(transform->world);
  state->shape = *shape;
  VkrUiSystem *ui = frame->ui;
  const Vec4 image = frame->mapping.image_rect_px;

  if (state->dragging) {
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
        (VkrUiRect){image.x, image.y, image.z, image.w});
    if (input_key_just_pressed(frame->input, KEY_ESCAPE)) {
      state->dragging = false_v;
    } else {
      if (has_ray) {
        blockout_drag(editor, state, frame, origin, direction);
      }
      if (!input_is_button_down(frame->input, BUTTON_LEFT) ||
          ui->mouse_released) {
        state->dragging = false_v;
        if (MemCompare(&state->draft, shape, sizeof(*shape)) != 0) {
          vkr_editor_blockout_submit(editor, group, &state->draft);
        }
      }
    }
  }
  const SceneBlockout *shown =
      state->dragging ? &state->draft
      : state->previewing && state->preview_group.u64 == group.u64
          ? &state->preview
          : shape;
  blockout_handles(state, shown);
  if (!state->dragging && has_ray && inside) {
    state->hot = blockout_pick(state, frame);
    if (state->hot >= 0) {
      /* The Scene stops picking objects under a handle. */
      (void)vkr_ui_input_layer_register(
          ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
          (VkrUiRect){image.x, image.y, image.z, image.w});
      if (ui->mouse_pressed) {
        blockout_press(editor, state, frame, origin, direction);
      }
    }
  }
  if (state->dragging || shown != shape) {
    blockout_shape_lines(state, frame, shown);
  }
  blockout_handle_lines(state, origin);
}

bool8_t vkr_editor_blockout_busy(const VkrEditorUi *editor) {
  const VkrEditorBlockout *state = editor->blockout;
  return state && state->group.u64 && (state->hot >= 0 || state->dragging);
}

const char *vkr_editor_blockout_hint(const VkrEditorUi *editor) {
  const VkrEditorBlockout *state = editor->blockout;
  if (!state || !state->group.u64) {
    return NULL;
  }
  if (state->message[0] && (state->dragging || state->previewing)) {
    return state->message;
  }
  const HandleKind kind = state->dragging   ? state->drag.kind
                          : state->hot >= 0 ? state->handles[state->hot].kind
                                            : (HandleKind)-1;
  switch (kind) {
  case HANDLE_LENGTH:
    return "Drag to lengthen or shorten the stairs.";
  case HANDLE_HEIGHT:
    return "Drag up or down to set the height; the steps share it.";
  case HANDLE_WIDTH:
    return "Drag to widen or narrow it.";
  case HANDLE_TURN:
    return "Drag around to turn the stairs on, in 15 degree steps; a spiral "
           "climbs on with it.";
  case HANDLE_RADIUS:
    return "Drag to set the inner radius.";
  case HANDLE_POINT:
    return "Drag the point; Ctrl+click removes it.";
  case HANDLE_INSERT:
    return "Click or drag to add a point here.";
  case HANDLE_CORNER:
    return "Drag in or out to round this bend more or less.";
  default:
    break;
  }
  return state->shape.shape == SCENE_BLOCKOUT_CORRIDOR
             ? "Drag the corridor's points, or a + between them to add one; "
               "Details sets its corner radius, width and height."
             : "Drag the handles to shape the stairs; Details sets their kind, "
               "steps and turn.";
}

uint32_t vkr_editor_blockout_lines(const VkrEditorUi *editor,
                                   const VkrEditorBrushGridLine **out) {
  const VkrEditorBlockout *state = editor->blockout;
  if (out) {
    *out = state ? state->lines : NULL;
  }
  return state && state->group.u64 ? state->line_count : 0u;
}

void vkr_editor_blockout_destroy(VkrEditorUi *editor) {
  free(editor->blockout);
  editor->blockout = NULL;
}
