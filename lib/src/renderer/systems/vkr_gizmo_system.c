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
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"

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

vkr_local_persist const VkrGizmoHandle g_gizmo_submesh_handles[] = {
    VKR_GIZMO_HANDLE_TRANSLATE_X, VKR_GIZMO_HANDLE_TRANSLATE_Y,
    VKR_GIZMO_HANDLE_TRANSLATE_Z, VKR_GIZMO_HANDLE_ROTATE_X,
    VKR_GIZMO_HANDLE_ROTATE_Y,    VKR_GIZMO_HANDLE_ROTATE_Z,
    VKR_GIZMO_HANDLE_SCALE_X,     VKR_GIZMO_HANDLE_SCALE_Y,
    VKR_GIZMO_HANDLE_SCALE_Z,
};

bool8_t vkr_gizmo_system_init(VkrGizmoSystem *system,
                              struct s_RendererFrontend *renderer,
                              const VkrGizmoConfig *config) {
  assert_log(system != NULL, "System is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");

  MemZero(system, sizeof(*system));
  system->config = config ? *config : VKR_GIZMO_CONFIG_DEFAULT;
  system->mode = VKR_GIZMO_MODE_TRANSLATE;
  system->space = VKR_GIZMO_SPACE_WORLD;
  system->selected_entity = VKR_ENTITY_ID_INVALID;
  system->position = vec3_zero();
  system->orientation = vkr_quat_identity();
  system->hot_handle = VKR_GIZMO_HANDLE_NONE;
  system->active_handle = VKR_GIZMO_HANDLE_NONE;
  system->gizmo_mesh_index = VKR_INVALID_ID;
  system->visible = false_v;

  const Vec3 axes[] = {vec3_right(), vec3_up(), vec3_back()};
  vkr_local_persist const char *axis_names[] = {"x", "y", "z"};
  VkrMaterialHandle axis_materials[3] = {0};
  VkrRendererError mat_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_material_system_create_gizmo_materials(&renderer->material_system,
                                                  axis_materials, &mat_err)) {
    String8 err = vkr_renderer_get_error_string(mat_err);
    log_error("Gizmo material create failed: %s", string8_cstr(&err));
    goto gizmo_geometry_cleanup;
  }

  VkrGeometryHandle geometries[ArrayCount(g_gizmo_submesh_handles)] = {0};
  uint32_t geom_index = 0;
  VkrRendererError geom_err = VKR_RENDERER_ERROR_NONE;

  for (uint32_t axis_index = 0; axis_index < ArrayCount(axes); ++axis_index) {
    char name[GEOMETRY_NAME_MAX_LENGTH];
    string_format(name, sizeof(name), "gizmo_arrow_%s", axis_names[axis_index]);
    geometries[geom_index] = vkr_geometry_system_create_arrow(
        &renderer->geometry_system, ARROW_LENGTH - ARROW_HEAD_LENGTH,
        ARROW_SHAFT_RADIUS, ARROW_HEAD_LENGTH, ARROW_HEAD_RADIUS,
        ARROW_SEGMENTS, axes[axis_index], vec3_zero(), name, &geom_err);
    if (geometries[geom_index].id == 0) {
      String8 err = vkr_renderer_get_error_string(geom_err);
      log_error("Gizmo arrow create failed: %s", string8_cstr(&err));
      goto gizmo_geometry_cleanup;
    }
    geom_index++;
  }

  for (uint32_t axis_index = 0; axis_index < ArrayCount(axes); ++axis_index) {
    char name[GEOMETRY_NAME_MAX_LENGTH];
    string_format(name, sizeof(name), "gizmo_ring_%s", axis_names[axis_index]);
    geometries[geom_index] = vkr_geometry_system_create_torus(
        &renderer->geometry_system, RING_RADIUS, RING_THICKNESS, RING_SEGMENTS,
        RING_SIDES, axes[axis_index], vec3_zero(), name, &geom_err);
    if (geometries[geom_index].id == 0) {
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
    geometries[geom_index] = vkr_geometry_system_create_box(
        &renderer->geometry_system, center, CUBE_SIZE, CUBE_SIZE, CUBE_SIZE,
        true_v, name, &geom_err);
    if (geometries[geom_index].id == 0) {
      String8 err = vkr_renderer_get_error_string(geom_err);
      log_error("Gizmo cube create failed: %s", string8_cstr(&err));
      goto gizmo_geometry_cleanup;
    }
    geom_index++;
  }

  VkrSubMeshDesc submeshes[ArrayCount(g_gizmo_submesh_handles)] = {0};
  uint32_t submesh_count = ArrayCount(g_gizmo_submesh_handles);
  for (uint32_t index = 0; index < submesh_count; ++index) {
    uint32_t axis_index = index % ArrayCount(axes);
    submeshes[index] = (VkrSubMeshDesc){
        .geometry = geometries[index],
        .material = axis_materials[axis_index],
        .shader_override = (String8){0},
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
        .owns_geometry = true_v,
        .owns_material = false_v,
    };
  }

  VkrMeshDesc mesh_desc = {
      .transform = vkr_transform_identity(),
      .submeshes = submeshes,
      .submesh_count = submesh_count,
  };

  VkrRendererError mesh_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_add(&renderer->mesh_manager, &mesh_desc,
                            &system->gizmo_mesh_index, &mesh_err)) {
    String8 err = vkr_renderer_get_error_string(mesh_err);
    log_error("Gizmo mesh create failed: %s", string8_cstr(&err));
    goto gizmo_geometry_cleanup;
  }

  vkr_mesh_manager_update_model(&renderer->mesh_manager,
                                system->gizmo_mesh_index);

  (void)vkr_mesh_manager_set_visible(&renderer->mesh_manager,
                                     system->gizmo_mesh_index, false_v);

  system->initialized = true_v;
  return true_v;

gizmo_geometry_cleanup:
  if (system->gizmo_mesh_index != VKR_INVALID_ID) {
    vkr_mesh_manager_remove(&renderer->mesh_manager, system->gizmo_mesh_index);
    system->gizmo_mesh_index = VKR_INVALID_ID;
  }
  for (uint32_t index = 0; index < geom_index; ++index) {
    if (geometries[index].id != 0) {
      vkr_geometry_system_release(&renderer->geometry_system,
                                  geometries[index]);
    }
  }
  for (uint32_t index = 0; index < ArrayCount(axis_materials); ++index) {
    if (axis_materials[index].id != 0) {
      vkr_material_system_release(&renderer->material_system,
                                  axis_materials[index]);
    }
  }
  return false_v;
}

void vkr_gizmo_system_shutdown(VkrGizmoSystem *system,
                               struct s_RendererFrontend *renderer) {
  if (!system || !renderer) {
    return;
  }

  if (system->gizmo_mesh_index != VKR_INVALID_ID) {
    vkr_mesh_manager_remove(&renderer->mesh_manager, system->gizmo_mesh_index);
    system->gizmo_mesh_index = VKR_INVALID_ID;
  }

  system->visible = false_v;
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
