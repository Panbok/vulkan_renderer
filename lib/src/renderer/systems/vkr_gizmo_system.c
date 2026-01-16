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
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_shader_system.h"

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

vkr_internal float32_t vkr_gizmo_compute_screen_scale(
    const VkrGizmoSystem *system, const VkrCamera *camera,
    uint32_t viewport_height) {
  if (!system || !camera || viewport_height == 0) {
    return 1.0f;
  }

  float32_t distance =
      vec3_length(vec3_sub(system->position, camera->position));

  if (camera->type == VKR_CAMERA_TYPE_PERSPECTIVE) {
    float32_t fov_rad = camera->zoom * (VKR_PI / 180.0f);
    float32_t world_size_per_pixel =
        (2.0f * distance * tanf(fov_rad * 0.5f)) / (float32_t)viewport_height;
    return system->config.screen_size * world_size_per_pixel;
  }

  float32_t ortho_height = camera->top_clip - camera->bottom_clip;
  float32_t world_size_per_pixel = ortho_height / (float32_t)viewport_height;
  return system->config.screen_size * world_size_per_pixel;
}

vkr_internal Mat4 vkr_gizmo_build_model(const VkrGizmoSystem *system,
                                        const VkrCamera *camera,
                                        uint32_t viewport_height) {
  float32_t scale =
      vkr_gizmo_compute_screen_scale(system, camera, viewport_height);
  Mat4 translation = mat4_translate(system->position);
  Mat4 rotation = vkr_quat_to_mat4(system->orientation);
  Mat4 scale_mat = mat4_scale(vec3_new(scale, scale, scale));
  return mat4_mul(mat4_mul(translation, rotation), scale_mat);
}

vkr_local_persist const VkrGizmoHandle g_gizmo_submesh_handles[] = {
    VKR_GIZMO_HANDLE_TRANSLATE_X, VKR_GIZMO_HANDLE_TRANSLATE_Y,
    VKR_GIZMO_HANDLE_TRANSLATE_Z, VKR_GIZMO_HANDLE_ROTATE_X,
    VKR_GIZMO_HANDLE_ROTATE_Y,    VKR_GIZMO_HANDLE_ROTATE_Z,
    VKR_GIZMO_HANDLE_SCALE_X,     VKR_GIZMO_HANDLE_SCALE_Y,
    VKR_GIZMO_HANDLE_SCALE_Z,
};

vkr_internal VkrGizmoHandle
vkr_gizmo_handle_from_submesh(uint32_t submesh_index) {
  if (submesh_index >= ArrayCount(g_gizmo_submesh_handles)) {
    return VKR_GIZMO_HANDLE_NONE;
  }
  return g_gizmo_submesh_handles[submesh_index];
}

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
  system->pipeline = VKR_PIPELINE_HANDLE_INVALID;
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

  VkrRendererError pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_by_name(
          &renderer->pipeline_registry, string8_lit("world_overlay"), false_v,
          &system->pipeline, &pipeline_err)) {
    (void)vkr_pipeline_registry_acquire_by_name(
        &renderer->pipeline_registry, string8_lit("world"), false_v,
        &system->pipeline, &pipeline_err);
  }

  if (system->pipeline.id == 0) {
    String8 err = vkr_renderer_get_error_string(pipeline_err);
    log_error("Gizmo pipeline acquire failed: %s", string8_cstr(&err));
    goto gizmo_geometry_cleanup;
  }

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

void vkr_gizmo_system_render(VkrGizmoSystem *system,
                             struct s_RendererFrontend *renderer,
                             const VkrCamera *camera, uint32_t viewport_height,
                             VkrPipelineHandle pipeline_override) {
  assert_log(system != NULL, "System is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system->initialized, "System is not initialized");
  if (!system->visible) {
    return;
  }

  VkrPipelineHandle pipeline = pipeline_override;
  if (pipeline.id == 0) {
    pipeline = system->pipeline;
  }

  if (!camera || system->gizmo_mesh_index == VKR_INVALID_ID ||
      pipeline.id == 0) {
    log_error(
        "Gizmo system render failed: camera is NULL or pipeline is invalid");
    return;
  }

  VkrMesh *mesh =
      vkr_mesh_manager_get(&renderer->mesh_manager, system->gizmo_mesh_index);
  if (!mesh || mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    log_error("Gizmo system render failed: mesh is not loaded");
    return;
  }

  Mat4 model = vkr_gizmo_build_model(system, camera, viewport_height);

  bool8_t globals_applied = false_v;
  uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
  for (uint32_t i = 0; i < submesh_count; ++i) {
    VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(
        &renderer->mesh_manager, system->gizmo_mesh_index, i);
    if (!submesh) {
      continue;
    }

    VkrGizmoHandle handle = vkr_gizmo_handle_from_submesh(i);
    bool8_t is_hot =
        (handle != VKR_GIZMO_HANDLE_NONE && handle == system->hot_handle);
    bool8_t is_active =
        (handle != VKR_GIZMO_HANDLE_NONE && handle == system->active_handle);

    VkrMaterial *material = vkr_material_system_get_by_handle(
        &renderer->material_system, submesh->material);
    const char *shader_name =
        (material && material->shader_name && material->shader_name[0] != '\0')
            ? material->shader_name
            : "shader.default.world";
    if (!vkr_shader_system_use(&renderer->shader_system, shader_name)) {
      vkr_shader_system_use(&renderer->shader_system, "shader.default.world");
    }

    VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_mesh_manager_refresh_pipeline(&renderer->mesh_manager,
                                           system->gizmo_mesh_index, i,
                                           pipeline, &refresh_err)) {
      String8 err = vkr_renderer_get_error_string(refresh_err);
      log_warn("Gizmo submesh pipeline refresh failed: %s", string8_cstr(&err));
      continue;
    }

    renderer->draw_state.instance_state = submesh->instance_state;

    VkrPipelineHandle current_pipeline =
        vkr_pipeline_registry_get_current_pipeline(
            &renderer->pipeline_registry);
    if (current_pipeline.id != pipeline.id ||
        current_pipeline.generation != pipeline.generation) {
      VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_pipeline_registry_bind_pipeline(&renderer->pipeline_registry,
                                               pipeline, &bind_err)) {
        String8 err = vkr_renderer_get_error_string(bind_err);
        log_warn("Gizmo pipeline bind failed: %s", string8_cstr(&err));
      }
    }

    if (!globals_applied) {
      vkr_material_system_apply_global(&renderer->material_system,
                                       &renderer->globals,
                                       VKR_PIPELINE_DOMAIN_WORLD);
      globals_applied = true_v;
    }

    vkr_material_system_apply_local(
        &renderer->material_system,
        &(VkrLocalMaterialState){.model = model, .object_id = 0});

    if (material) {
      vkr_shader_system_bind_instance(&renderer->shader_system,
                                      submesh->instance_state.id);
      if (submesh->last_render_frame != renderer->frame_number) {
        vkr_material_system_apply_instance(&renderer->material_system, material,
                                           VKR_PIPELINE_DOMAIN_WORLD);
        submesh->last_render_frame = renderer->frame_number;
      }
    }

    if ((is_hot || is_active) && material) {
      float32_t boost = is_active ? 0.65f : 0.35f;
      Vec4 emission = material->phong.emission_color;
      emission.x = vkr_min_f32(emission.x + boost, 1.0f);
      emission.y = vkr_min_f32(emission.y + boost, 1.0f);
      emission.z = vkr_min_f32(emission.z + boost, 1.0f);
      vkr_shader_system_uniform_set(&renderer->shader_system, "emission_color",
                                    &emission);
      vkr_shader_system_apply_instance(&renderer->shader_system);
    }

    vkr_geometry_system_render(renderer, &renderer->geometry_system,
                               submesh->geometry, 1);
  }
}

void vkr_gizmo_system_render_picking(VkrGizmoSystem *system,
                                     struct s_RendererFrontend *renderer,
                                     const VkrCamera *camera,
                                     uint32_t viewport_height) {
  assert_log(system != NULL, "System is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system->initialized, "System is not initialized");
  assert_log(camera != NULL, "Camera is NULL");

  if (!system->visible) {
    return;
  }

  if (system->gizmo_mesh_index == VKR_INVALID_ID) {
    log_error("Gizmo system render picking failed: mesh is not loaded");
    return;
  }

  VkrMesh *mesh =
      vkr_mesh_manager_get(&renderer->mesh_manager, system->gizmo_mesh_index);
  if (!mesh || mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    log_error("Gizmo system render picking failed: mesh is not loaded");
    return;
  }

  Mat4 model = vkr_gizmo_build_model(system, camera, viewport_height);
  float32_t alpha_cutoff = 0.0f;

  uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
  for (uint32_t i = 0; i < submesh_count; ++i) {
    VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(
        &renderer->mesh_manager, system->gizmo_mesh_index, i);
    if (!submesh) {
      continue;
    }

    VkrGizmoHandle handle = vkr_gizmo_handle_from_submesh(i);
    if (handle == VKR_GIZMO_HANDLE_NONE) {
      continue;
    }

    uint32_t object_id = vkr_gizmo_encode_picking_id(handle);
    vkr_material_system_apply_local(
        &renderer->material_system,
        &(VkrLocalMaterialState){.model = model, .object_id = object_id});

    vkr_shader_system_uniform_set(&renderer->shader_system, "alpha_cutoff",
                                  &alpha_cutoff);

    if (!vkr_shader_system_apply_instance(&renderer->shader_system)) {
      continue;
    }

    vkr_geometry_system_render(renderer, &renderer->geometry_system,
                               submesh->geometry, 1);
  }
}
