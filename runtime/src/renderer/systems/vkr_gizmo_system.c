/**
 * @file vkr_gizmo_system.c
 * @brief Editor transform gizmo system implementation.
 */

#include "renderer/systems/vkr_gizmo_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vkr_math.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_render_assets.h"

/* Unit gizmo: one unit spans `screen_size` window pixels. Handles start
   beyond the center handles so every one keeps a clear pick area. */
#define HANDLE_START 0.18f
#define ARROW_SHAFT_END 0.80f
#define ARROW_SHAFT_RADIUS 0.022f
#define ARROW_HEAD_LENGTH 0.28f
#define ARROW_HEAD_RADIUS 0.075f
#define PLANE_OFFSET 0.34f
#define PLANE_SIZE 0.2f
#define PLANE_THICKNESS 0.012f
#define RING_RADIUS 1.18f
#define RING_THICKNESS 0.022f
#define SCALE_SHAFT_END 0.86f
#define SCALE_SHAFT_RADIUS 0.018f
#define SCALE_CUBE_SIZE 0.13f
#define SCALE_CUBE_OFFSET (SCALE_SHAFT_END + SCALE_CUBE_SIZE * 0.5f)
#define CENTER_SPHERE_RADIUS 0.085f
#define CENTER_CUBE_SIZE 0.15f
#define ARROW_SEGMENTS 24
#define RING_SEGMENTS 64
#define RING_SIDES 12
#define SPHERE_SEGMENTS 16

typedef enum GizmoShapeKind {
  GIZMO_SHAPE_ARROW,
  GIZMO_SHAPE_PLANE,
  GIZMO_SHAPE_RING,
  GIZMO_SHAPE_SCALE_SHAFT,
  GIZMO_SHAPE_SCALE_CUBE,
  GIZMO_SHAPE_CENTER,
} GizmoShapeKind;

/* One published mesh: the handle it picks as and the axis it runs along or,
   for a plane, its normal; 3 for the center. */
typedef struct GizmoShape {
  VkrGizmoHandle handle;
  GizmoShapeKind kind;
  uint32_t axis;
} GizmoShape;

vkr_internal const GizmoShape g_gizmo_shapes[VKR_GIZMO_GEOMETRY_COUNT] = {
    {VKR_GIZMO_HANDLE_TRANSLATE_X, GIZMO_SHAPE_ARROW, 0},
    {VKR_GIZMO_HANDLE_TRANSLATE_Y, GIZMO_SHAPE_ARROW, 1},
    {VKR_GIZMO_HANDLE_TRANSLATE_Z, GIZMO_SHAPE_ARROW, 2},
    {VKR_GIZMO_HANDLE_TRANSLATE_YZ, GIZMO_SHAPE_PLANE, 0},
    {VKR_GIZMO_HANDLE_TRANSLATE_XZ, GIZMO_SHAPE_PLANE, 1},
    {VKR_GIZMO_HANDLE_TRANSLATE_XY, GIZMO_SHAPE_PLANE, 2},
    {VKR_GIZMO_HANDLE_TRANSLATE_FREE, GIZMO_SHAPE_CENTER, 3},
    {VKR_GIZMO_HANDLE_ROTATE_X, GIZMO_SHAPE_RING, 0},
    {VKR_GIZMO_HANDLE_ROTATE_Y, GIZMO_SHAPE_RING, 1},
    {VKR_GIZMO_HANDLE_ROTATE_Z, GIZMO_SHAPE_RING, 2},
    {VKR_GIZMO_HANDLE_SCALE_X, GIZMO_SHAPE_SCALE_SHAFT, 0},
    {VKR_GIZMO_HANDLE_SCALE_Y, GIZMO_SHAPE_SCALE_SHAFT, 1},
    {VKR_GIZMO_HANDLE_SCALE_Z, GIZMO_SHAPE_SCALE_SHAFT, 2},
    {VKR_GIZMO_HANDLE_SCALE_X, GIZMO_SHAPE_SCALE_CUBE, 0},
    {VKR_GIZMO_HANDLE_SCALE_Y, GIZMO_SHAPE_SCALE_CUBE, 1},
    {VKR_GIZMO_HANDLE_SCALE_Z, GIZMO_SHAPE_SCALE_CUBE, 2},
    {VKR_GIZMO_HANDLE_SCALE_UNIFORM, GIZMO_SHAPE_CENTER, 3},
};

/* The mesh of shape `index` in gizmo units, before orientation. */
vkr_internal VkrGeometryHandle gizmo_create_shape(VkrGeometrySystem *geometry,
                                                  uint32_t index,
                                                  VkrRendererError *error) {
  const GizmoShape *shape = &g_gizmo_shapes[index];
  const Vec3 axes[] = {vec3_right(), vec3_up(), vec3_back()};
  const Vec3 axis = shape->axis < 3u ? axes[shape->axis] : vec3_zero();
  char name[GEOMETRY_NAME_MAX_LENGTH];
  string_format(name, sizeof(name), "gizmo_shape_%u", index);
  switch (shape->kind) {
  case GIZMO_SHAPE_ARROW:
    return vkr_geometry_system_create_arrow(
        geometry, ARROW_SHAFT_END - HANDLE_START, ARROW_SHAFT_RADIUS,
        ARROW_HEAD_LENGTH, ARROW_HEAD_RADIUS, ARROW_SEGMENTS, axis,
        vec3_scale(axis, HANDLE_START), name, error);
  case GIZMO_SHAPE_PLANE: {
    /* A thin square in the two other axes' quadrant; closed, so it shows
       from both sides. */
    Vec3 size = vec3_new(PLANE_SIZE, PLANE_SIZE, PLANE_SIZE);
    Vec3 center = vec3_new(PLANE_OFFSET, PLANE_OFFSET, PLANE_OFFSET);
    size.elements[shape->axis] = PLANE_THICKNESS;
    center.elements[shape->axis] = 0.0f;
    return vkr_geometry_system_create_box(geometry, center, size.x, size.y,
                                          size.z, true_v, name, error);
  }
  case GIZMO_SHAPE_RING:
    return vkr_geometry_system_create_torus(
        geometry, RING_RADIUS, RING_THICKNESS, RING_SEGMENTS, RING_SIDES, axis,
        vec3_zero(), name, error);
  case GIZMO_SHAPE_SCALE_SHAFT:
    return vkr_geometry_system_create_cylinder(
        geometry, SCALE_SHAFT_RADIUS, SCALE_SHAFT_END - HANDLE_START,
        ARROW_SEGMENTS, axis, vec3_scale(axis, HANDLE_START), true_v, true_v,
        name, error);
  case GIZMO_SHAPE_SCALE_CUBE:
    return vkr_geometry_system_create_box(
        geometry, vec3_scale(axis, SCALE_CUBE_OFFSET), SCALE_CUBE_SIZE,
        SCALE_CUBE_SIZE, SCALE_CUBE_SIZE, true_v, name, error);
  case GIZMO_SHAPE_CENTER:
    if (shape->handle == VKR_GIZMO_HANDLE_SCALE_UNIFORM) {
      return vkr_geometry_system_create_box(
          geometry, vec3_zero(), CENTER_CUBE_SIZE, CENTER_CUBE_SIZE,
          CENTER_CUBE_SIZE, true_v, name, error);
    }
    return vkr_geometry_system_create_sphere(
        geometry, CENTER_SPHERE_RADIUS, SPHERE_SEGMENTS, SPHERE_SEGMENTS,
        vec3_up(), vec3_zero(), name, error);
  }
  return VKR_GEOMETRY_HANDLE_INVALID;
}

bool8_t vkr_gizmo_system_init(VkrGizmoSystem *system,
                              struct VkrRenderAssets *assets,
                              const VkrGizmoConfig *config) {
  assert_log(system != NULL, "System is NULL");
  assert_log(assets != NULL, "Renderer is NULL");

  MemZero(system, sizeof(*system));
  system->config = config ? *config : VKR_GIZMO_CONFIG_DEFAULT;
  if (!isfinite(system->config.screen_size) ||
      system->config.screen_size <= 0.0f)
    return false_v;
  system->mode = VKR_GIZMO_MODE_TRANSLATE;
  system->tool = VKR_GIZMO_MODE_NONE;
  system->space = VKR_GIZMO_SPACE_WORLD;
  system->selected_entity = VKR_ENTITY_ID_INVALID;
  system->position = vec3_zero();
  system->orientation = vkr_quat_identity();
  system->hot_handle = VKR_GIZMO_HANDLE_NONE;
  system->active_handle = VKR_GIZMO_HANDLE_NONE;
  system->visible = false_v;

  for (uint32_t i = 0; i < VKR_GIZMO_GEOMETRY_COUNT; ++i) {
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    system->geometries[i] =
        gizmo_create_shape(&assets->geometry_system, i, &error);
    if (system->geometries[i].id == 0) {
      String8 message = vkr_renderer_get_error_string(error);
      log_error("Gizmo shape %u create failed: %s", i, string8_cstr(&message));
      goto gizmo_geometry_cleanup;
    }
  }

  system->initialized = true_v;
  return true_v;

gizmo_geometry_cleanup:
  vkr_gizmo_system_shutdown(system, assets);
  return false_v;
}

void vkr_gizmo_system_shutdown(VkrGizmoSystem *system,
                               struct VkrRenderAssets *assets) {
  if (!system || !assets)
    return;
  for (uint32_t i = 0; i < ArrayCount(system->geometries); ++i) {
    if (system->geometries[i].id)
      vkr_geometry_system_release(&assets->geometry_system,
                                  system->geometries[i]);
    system->geometries[i] = VKR_GEOMETRY_HANDLE_INVALID;
  }
  vkr_gizmo_system_clear_target(system);
  system->initialized = false_v;
}

void vkr_gizmo_system_set_target(VkrGizmoSystem *system, VkrEntityId entity,
                                 Vec3 position, VkrQuat orientation) {
  assert_log(system != NULL, "System is NULL");

  if (system->selected_entity.u64 != entity.u64) {
    system->hot_handle = VKR_GIZMO_HANDLE_NONE;
    system->active_handle = VKR_GIZMO_HANDLE_NONE;
  }

  system->selected_entity = entity;
  system->position = position;
  system->orientation = orientation;
  system->visible = (entity.u64 != VKR_ENTITY_ID_INVALID.u64);
}

void vkr_gizmo_system_clear_target(VkrGizmoSystem *system) {
  assert_log(system != NULL, "System is NULL");

  system->selected_entity = VKR_ENTITY_ID_INVALID;
  system->hot_handle = VKR_GIZMO_HANDLE_NONE;
  system->active_handle = VKR_GIZMO_HANDLE_NONE;
  system->visible = false_v;
}

void vkr_gizmo_system_set_hot_handle(VkrGizmoSystem *system,
                                     VkrGizmoHandle handle) {
  assert_log(system != NULL, "System is NULL");
  system->hot_handle = handle;
}

void vkr_gizmo_system_set_active_handle(VkrGizmoSystem *system,
                                        VkrGizmoHandle handle) {
  assert_log(system != NULL, "System is NULL");
  system->active_handle = handle;
}

Vec3 vkr_gizmo_system_axis(const VkrGizmoSystem *system, VkrGizmoMode mode,
                           uint32_t axis) {
  const Vec3 axes[] = {vec3_right(), vec3_up(), vec3_back()};
  if (!system || axis >= ArrayCount(axes)) {
    return vec3_zero();
  }
  const bool8_t local =
      mode == VKR_GIZMO_MODE_SCALE || system->space == VKR_GIZMO_SPACE_LOCAL;
  return local ? vec3_normalize(
                     vkr_quat_rotate_vec3(system->orientation, axes[axis]))
               : axes[axis];
}

typedef struct GizmoDraw {
  uint32_t shape;
  /* View-space depth of the handle's middle; larger is farther. */
  float32_t depth;
  /* 0 for ordinary handles, 1 hovered, 2 dragged: drawn last. */
  uint32_t rank;
  Vec3 mirror;
} GizmoDraw;

uint32_t vkr_gizmo_system_build_draws(
    const VkrGizmoSystem *system, Mat4 view, Mat4 projection,
    const VkrViewportMapping *mapping,
    VkrEditorOverlayDraw out_draws[VKR_EDITOR_OVERLAY_DRAW_MAX]) {
  /* The Select tool shows the selection outline and no handles. */
  if (!system || !system->initialized || !system->visible || !mapping ||
      !out_draws || system->tool == VKR_GIZMO_MODE_NONE ||
      system->mode < VKR_GIZMO_MODE_TRANSLATE ||
      system->mode > VKR_GIZMO_MODE_SCALE || mapping->image_rect_px.w <= 0.0f)
    return 0u;
  const Vec4 center =
      mat4_mul_vec4(view, vec4_new(system->position.x, system->position.y,
                                   system->position.z, 1.0f));
  const Vec4 clip = mat4_mul_vec4(projection, center);
  const float32_t projection_y = fabsf(projection.elements[5]);
  if (!isfinite(clip.w) || clip.w <= VKR_FLOAT_EPSILON || clip.z < 0.0f ||
      clip.z > clip.w || projection_y <= VKR_FLOAT_EPSILON)
    return 0u;
  const float32_t pixels =
      system->config.screen_size *
      (system->pixel_scale > 0.0f ? system->pixel_scale : 1.0f);
  const float32_t scale =
      2.0f * pixels * clip.w / (projection_y * mapping->image_rect_px.w);
  if (!isfinite(scale) || scale <= 0.0f)
    return 0u;
  /* The view ray through the center, in view space: toward the center for
     perspective, straight ahead for orthographic. */
  const bool8_t orthographic = fabsf(projection.elements[11]) < 0.5f;
  const Vec3 center_view = vec3_new(center.x, center.y, center.z);
  const Vec3 ray = orthographic || vec3_length(center_view) < VKR_FLOAT_EPSILON
                       ? vec3_new(0.0f, 0.0f, -1.0f)
                       : vec3_normalize(center_view);
  const VkrGizmoMode tool = system->tool;
  const VkrGizmoHandle active = system->active_handle;
  GizmoDraw draws[VKR_GIZMO_GEOMETRY_COUNT];
  uint32_t count = 0u;
  for (uint32_t index = 0; index < VKR_GIZMO_GEOMETRY_COUNT; ++index) {
    const GizmoShape *shape = &g_gizmo_shapes[index];
    const VkrGizmoMode mode = vkr_gizmo_handle_mode(shape->handle);
    const bool8_t shown = mode == tool;
    if (!shown || (active != VKR_GIZMO_HANDLE_NONE && shape->handle != active))
      continue;
    Vec3 mirror = vec3_one();
    Vec3 middle = vec3_zero();
    if (shape->axis < 3u) {
      const Vec3 axis = vkr_gizmo_system_axis(system, mode, shape->axis);
      const Vec4 axis_view4 =
          mat4_mul_vec4(view, vec4_new(axis.x, axis.y, axis.z, 0.0f));
      const float32_t facing = fabsf(
          vec3_dot(vec3_new(axis_view4.x, axis_view4.y, axis_view4.z), ray));
      /* An axis seen end-on and a plane seen edge-on cannot be dragged
         precisely; the dragged handle stays. */
      if (shape->handle != active &&
          (shape->kind == GIZMO_SHAPE_PLANE  ? facing < 0.2f
           : shape->kind == GIZMO_SHAPE_RING ? false_v
                                             : facing > 0.97f))
        continue;
      if (shape->kind == GIZMO_SHAPE_PLANE) {
        /* The square sits in the quadrant facing the camera. */
        for (uint32_t other = 0; other < 3u; ++other) {
          if (other == shape->axis)
            continue;
          const Vec3 in_plane = vkr_gizmo_system_axis(system, mode, other);
          const Vec4 in_view = mat4_mul_vec4(
              view, vec4_new(in_plane.x, in_plane.y, in_plane.z, 0.0f));
          const float32_t toward =
              -vec3_dot(vec3_new(in_view.x, in_view.y, in_view.z), ray);
          mirror.elements[other] = toward < 0.0f ? -1.0f : 1.0f;
          middle = vec3_add(
              middle,
              vec3_scale(in_plane, PLANE_OFFSET * mirror.elements[other]));
        }
      } else if (shape->kind != GIZMO_SHAPE_RING) {
        middle = vec3_scale(axis, 0.6f);
      }
    }
    const Vec3 point = vec3_add(system->position, vec3_scale(middle, scale));
    const Vec4 point_view =
        mat4_mul_vec4(view, vec4_new(point.x, point.y, point.z, 1.0f));
    draws[count++] = (GizmoDraw){
        .shape = index,
        /* Rings wrap everything else and draw first. */
        .depth = shape->kind == GIZMO_SHAPE_RING ? 1.0e30f : -point_view.z,
        .rank = shape->handle == active               ? 2u
                : shape->handle == system->hot_handle ? 1u
                                                      : 0u,
        .mirror = mirror,
    };
  }
  /* Far to near, hovered then dragged last: insertion sort of a few. */
  for (uint32_t i = 1; i < count; ++i) {
    const GizmoDraw key = draws[i];
    uint32_t j = i;
    while (j > 0 &&
           (draws[j - 1].rank > key.rank || (draws[j - 1].rank == key.rank &&
                                             draws[j - 1].depth < key.depth))) {
      draws[j] = draws[j - 1];
      --j;
    }
    draws[j] = key;
  }
  /* Linear values: red, green and blue axes, a light center, a yellow
     hover and an orange drag once the output encodes them as sRGB. */
  const Vec4 axis_colors[4] = {
      vec4_new(0.86f, 0.045f, 0.06f, 1.0f), vec4_new(0.24f, 0.66f, 0.03f, 1.0f),
      vec4_new(0.03f, 0.22f, 0.94f, 1.0f), vec4_new(0.78f, 0.79f, 0.83f, 1.0f)};
  const Vec4 hot_color = vec4_new(1.0f, 0.72f, 0.03f, 1.0f);
  const Vec4 active_color = vec4_new(1.0f, 0.34f, 0.01f, 1.0f);
  count = Min(count, VKR_EDITOR_OVERLAY_DRAW_MAX);
  for (uint32_t i = 0; i < count; ++i) {
    const GizmoShape *shape = &g_gizmo_shapes[draws[i].shape];
    const VkrGizmoMode mode = vkr_gizmo_handle_mode(shape->handle);
    const bool8_t oriented =
        mode == VKR_GIZMO_MODE_SCALE || system->space == VKR_GIZMO_SPACE_LOCAL;
    const Mat4 model =
        mat4_mul(mat4_mul(mat4_translate(system->position),
                          oriented ? vkr_quat_to_mat4(system->orientation)
                                   : mat4_identity()),
                 mat4_scale(vec3_scale(draws[i].mirror, scale)));
    out_draws[i] = (VkrEditorOverlayDraw){
        .geometry = system->geometries[draws[i].shape],
        .submesh_index = 0u,
        .model = model,
        .color = draws[i].rank == 2u   ? active_color
                 : draws[i].rank == 1u ? hot_color
                                       : axis_colors[shape->axis],
        .object_id = vkr_gizmo_encode_picking_id(shape->handle),
    };
  }
  return count;
}
