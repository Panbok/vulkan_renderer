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

#define ARROW_LENGTH 1.0f
#define ARROW_HEAD_LENGTH 0.25f
#define ARROW_SHAFT_RADIUS 0.03f
#define ARROW_HEAD_RADIUS 0.09f
#define CUBE_SIZE 0.1f
#define CUBE_OFFSET (ARROW_LENGTH + CUBE_SIZE * 0.5f)
#define RING_RADIUS 0.65f
#define RING_THICKNESS 0.02f
#define ARROW_SEGMENTS 24
#define RING_SEGMENTS 48
#define RING_SIDES 12

vkr_internal const VkrGizmoHandle g_gizmo_submesh_handles[] = {
    VKR_GIZMO_HANDLE_TRANSLATE_X, VKR_GIZMO_HANDLE_TRANSLATE_Y,
    VKR_GIZMO_HANDLE_TRANSLATE_Z, VKR_GIZMO_HANDLE_ROTATE_X,
    VKR_GIZMO_HANDLE_ROTATE_Y,    VKR_GIZMO_HANDLE_ROTATE_Z,
    VKR_GIZMO_HANDLE_SCALE_X,     VKR_GIZMO_HANDLE_SCALE_Y,
    VKR_GIZMO_HANDLE_SCALE_Z,
};

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
  system->space = VKR_GIZMO_SPACE_WORLD;
  system->selected_entity = VKR_ENTITY_ID_INVALID;
  system->position = vec3_zero();
  system->orientation = vkr_quat_identity();
  system->hot_handle = VKR_GIZMO_HANDLE_NONE;
  system->active_handle = VKR_GIZMO_HANDLE_NONE;
  system->visible = false_v;

  const Vec3 axes[] = {vec3_right(), vec3_up(), vec3_back()};
  vkr_local_persist const char *axis_names[] = {"x", "y", "z"};
  uint32_t geom_index = 0;
  VkrRendererError geom_err = VKR_RENDERER_ERROR_NONE;

  for (uint32_t axis_index = 0; axis_index < ArrayCount(axes); ++axis_index) {
    char name[GEOMETRY_NAME_MAX_LENGTH];
    string_format(name, sizeof(name), "gizmo_arrow_%s", axis_names[axis_index]);
    system->geometries[geom_index] = vkr_geometry_system_create_arrow(
        &assets->geometry_system, ARROW_LENGTH - ARROW_HEAD_LENGTH,
        ARROW_SHAFT_RADIUS, ARROW_HEAD_LENGTH, ARROW_HEAD_RADIUS,
        ARROW_SEGMENTS, axes[axis_index], vec3_zero(), name, &geom_err);
    if (system->geometries[geom_index].id == 0) {
      String8 err = vkr_renderer_get_error_string(geom_err);
      log_error("Gizmo arrow create failed: %s", string8_cstr(&err));
      goto gizmo_geometry_cleanup;
    }
    geom_index++;
  }

  for (uint32_t axis_index = 0; axis_index < ArrayCount(axes); ++axis_index) {
    char name[GEOMETRY_NAME_MAX_LENGTH];
    string_format(name, sizeof(name), "gizmo_ring_%s", axis_names[axis_index]);
    system->geometries[geom_index] = vkr_geometry_system_create_torus(
        &assets->geometry_system, RING_RADIUS, RING_THICKNESS, RING_SEGMENTS,
        RING_SIDES, axes[axis_index], vec3_zero(), name, &geom_err);
    if (system->geometries[geom_index].id == 0) {
      String8 err = vkr_renderer_get_error_string(geom_err);
      log_error("Gizmo ring create failed: %s", string8_cstr(&err));
      goto gizmo_geometry_cleanup;
    }
    geom_index++;
  }

  for (uint32_t axis_index = 0; axis_index < ArrayCount(axes); ++axis_index) {
    char name[GEOMETRY_NAME_MAX_LENGTH];
    string_format(name, sizeof(name), "gizmo_scale_%s", axis_names[axis_index]);
    Vec3 center = vec3_scale(axes[axis_index], CUBE_OFFSET);
    system->geometries[geom_index] = vkr_geometry_system_create_box(
        &assets->geometry_system, center, CUBE_SIZE, CUBE_SIZE, CUBE_SIZE,
        true_v, name, &geom_err);
    if (system->geometries[geom_index].id == 0) {
      String8 err = vkr_renderer_get_error_string(geom_err);
      log_error("Gizmo cube create failed: %s", string8_cstr(&err));
      goto gizmo_geometry_cleanup;
    }
    geom_index++;
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

uint32_t vkr_gizmo_system_build_draws(
    const VkrGizmoSystem *system, Mat4 view, Mat4 projection,
    const VkrViewportMapping *mapping,
    VkrEditorOverlayDraw out_draws[VKR_EDITOR_OVERLAY_DRAW_MAX]) {
  if (!system || !system->initialized || !system->visible || !mapping ||
      !out_draws || system->mode < VKR_GIZMO_MODE_TRANSLATE ||
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
  const float32_t scale = 2.0f * system->config.screen_size * clip.w /
                          (projection_y * mapping->image_rect_px.w);
  if (!isfinite(scale) || scale <= 0.0f)
    return 0u;
  const Mat4 model = mat4_mul(mat4_translate(system->position),
                              mat4_scale(vec3_new(scale, scale, scale)));
  const Vec4 colors[3] = {vec4_new(1.0f, 0.08f, 0.08f, 1.0f),
                          vec4_new(0.08f, 1.0f, 0.08f, 1.0f),
                          vec4_new(0.08f, 0.25f, 1.0f, 1.0f)};
  uint32_t order[VKR_EDITOR_OVERLAY_DRAW_MAX];
  uint32_t count = 0u;
  for (uint32_t shape = 0; shape < ArrayCount(g_gizmo_submesh_handles);
       ++shape) {
    const VkrGizmoHandle handle = g_gizmo_submesh_handles[shape];
    if (handle != system->hot_handle && handle != system->active_handle)
      order[count++] = shape;
  }
  for (uint32_t shape = 0; shape < ArrayCount(g_gizmo_submesh_handles);
       ++shape) {
    if (g_gizmo_submesh_handles[shape] == system->hot_handle &&
        system->hot_handle != system->active_handle)
      order[count++] = shape;
  }
  for (uint32_t shape = 0; shape < ArrayCount(g_gizmo_submesh_handles);
       ++shape) {
    if (g_gizmo_submesh_handles[shape] == system->active_handle)
      order[count++] = shape;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t shape = order[i];
    const VkrGizmoHandle handle = g_gizmo_submesh_handles[shape];
    const Vec4 color =
        handle == system->active_handle ? vec4_new(1.0f, 0.65f, 0.02f, 1.0f)
        : handle == system->hot_handle  ? vec4_new(1.0f, 1.0f, 0.3f, 1.0f)
                                        : colors[shape % 3u];
    out_draws[i] = (VkrEditorOverlayDraw){
        .geometry = system->geometries[shape],
        .submesh_index = 0u,
        .model = model,
        .color = color,
        .object_id = vkr_gizmo_encode_picking_id(handle),
    };
  }
  return count;
}
