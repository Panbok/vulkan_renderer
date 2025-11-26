#include "renderer/systems/views/vkr_view_world.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_view_system.h"

typedef struct VkrViewWorldState {
  VkrShaderConfig shader_config;
  VkrPipelineHandle pipeline;
  VkrMaterialHandle default_material;
} VkrViewWorldState;

vkr_internal bool32_t vkr_view_world_on_create(VkrLayerContext *ctx);
vkr_internal void vkr_view_world_on_attach(VkrLayerContext *ctx);
vkr_internal void vkr_view_world_on_resize(VkrLayerContext *ctx, uint32_t width,
                                           uint32_t height);
vkr_internal void vkr_view_world_on_render(VkrLayerContext *ctx,
                                           const VkrLayerRenderInfo *info);
vkr_internal void vkr_view_world_on_detach(VkrLayerContext *ctx);
vkr_internal void vkr_view_world_on_destroy(VkrLayerContext *ctx);

bool32_t vkr_view_world_register(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (!rf->view_system.initialized) {
    log_error("View system not initialized; cannot register world view");
    return false_v;
  }

  if (rf->world_layer.id != 0) {
    return true_v;
  }

  VkrLayerPassConfig world_passes[1] = {{
      .renderpass_name = string8_lit("Renderpass.Builtin.World"),
      .use_swapchain_color = true_v,
      .use_depth = true_v,
  }};

  VkrViewWorldState *state = arena_alloc(rf->arena, sizeof(VkrViewWorldState),
                                         ARENA_MEMORY_TAG_STRUCT);
  if (!state) {
    log_error("Failed to allocate world view state");
    return false_v;
  }
  MemZero(state, sizeof(*state));

  VkrLayerConfig world_cfg = {
      .name = string8_lit("Layer.World"),
      .order = 0,
      .width = 0,
      .height = 0,
      .view = rf->globals.view,
      .projection = rf->globals.projection,
      .pass_count = ArrayCount(world_passes),
      .passes = world_passes,
      .callbacks = {.on_create = vkr_view_world_on_create,
                    .on_attach = vkr_view_world_on_attach,
                    .on_resize = vkr_view_world_on_resize,
                    .on_render = vkr_view_world_on_render,
                    .on_detach = vkr_view_world_on_detach,
                    .on_destroy = vkr_view_world_on_destroy},
      .user_data = state,
  };

  VkrRendererError layer_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_view_system_register_layer(rf, &world_cfg, &rf->world_layer,
                                      &layer_err)) {
    String8 err = vkr_renderer_get_error_string(layer_err);
    log_error("Failed to register world view: %s", string8_cstr(&err));
    return false_v;
  }

  return true_v;
}

vkr_internal bool32_t vkr_view_world_on_create(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return false_v;
  }

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return false_v;
  }

  VkrResourceHandleInfo world_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.world.shadercfg"),
          rf->scratch_arena, &world_cfg_info, &shadercfg_err)) {
    state->shader_config = *(VkrShaderConfig *)world_cfg_info.as.custom;
  } else {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("World shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  vkr_shader_system_create(&rf->shader_system, &state->shader_config);

  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->shader_config,
          VKR_PIPELINE_DOMAIN_WORLD, string8_lit("world"), &state->pipeline,
          &pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_error);
    log_error("Config world pipeline failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  if (state->shader_config.name.str && state->shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, state->pipeline, state->shader_config.name,
        &alias_err);
  }

  VkrResourceHandleInfo default_material_info = {0};
  VkrRendererError material_load_error = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                               string8_lit("assets/materials/default.world.mt"),
                               rf->scratch_arena, &default_material_info,
                               &material_load_error)) {
    state->default_material = default_material_info.as.material;
  } else {
    String8 error_string = vkr_renderer_get_error_string(material_load_error);
    log_warn(
        "Failed to load default world material; using built-in default: %s",
        string8_cstr(&error_string));
  }

  // Writable texture example
  // VkrTextureDescription writable_desc = {
  //     .width = 128,
  //     .height = 128,
  //     .channels = 4,
  //     .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
  //     .type = VKR_TEXTURE_TYPE_2D,
  //     .properties = vkr_texture_property_flags_create(),
  //     .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
  //     .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
  //     .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
  //     .min_filter = VKR_FILTER_LINEAR,
  //     .mag_filter = VKR_FILTER_LINEAR,
  //     .mip_filter = VKR_MIP_FILTER_NONE,
  //     .anisotropy_enable = false_v,
  //     .generation = VKR_INVALID_ID,
  // };
  // VkrTextureHandle ui_runtime_texture = VKR_TEXTURE_HANDLE_INVALID;
  // VkrRendererError writable_err = VKR_RENDERER_ERROR_NONE;
  // if (vkr_texture_system_create_writable(
  //         &rf->texture_system, string8_lit("ui.runtime.writable"),
  //         &writable_desc, &ui_runtime_texture, &writable_err)) {
  //   uint64_t pixel_count =
  //       (uint64_t)writable_desc.width * (uint64_t)writable_desc.height;
  //   uint64_t buffer_size = pixel_count * (uint64_t)writable_desc.channels;

  //   Scratch tex_scratch = scratch_create(rf->scratch_arena);
  //   uint8_t *base_pixels =
  //       arena_alloc(tex_scratch.arena, buffer_size,
  //       ARENA_MEMORY_TAG_TEXTURE);
  //   if (base_pixels) {
  //     for (uint32_t y = 0; y < writable_desc.height; ++y) {
  //       for (uint32_t x = 0; x < writable_desc.width; ++x) {
  //         uint32_t idx = (y * writable_desc.width + x) *
  //         writable_desc.channels; base_pixels[idx + 0] = (uint8_t)((x * 255)
  //         / writable_desc.width); base_pixels[idx + 1] = (uint8_t)((y * 255)
  //         / writable_desc.height); base_pixels[idx + 2] = 180;
  //         base_pixels[idx + 3] = 255;
  //       }
  //     }

  //     VkrRendererError write_err = vkr_texture_system_write(
  //         &rf->texture_system, ui_runtime_texture, base_pixels, buffer_size);
  //     if (write_err != VKR_RENDERER_ERROR_NONE) {
  //       String8 err = vkr_renderer_get_error_string(write_err);
  //       log_warn("Writable UI texture upload failed: %s",
  //       string8_cstr(&err));
  //     }
  //   } else {
  //     log_warn("Failed to allocate base pixels for writable texture");
  //   }

  //   const uint32_t block_w = 48;
  //   const uint32_t block_h = 48;
  //   uint64_t region_size =
  //       (uint64_t)block_w * (uint64_t)block_h * writable_desc.channels;
  //   uint8_t *region_pixels =
  //       arena_alloc(tex_scratch.arena, region_size,
  //       ARENA_MEMORY_TAG_TEXTURE);
  //   if (region_pixels) {
  //     for (uint32_t i = 0; i < block_w * block_h; ++i) {
  //       region_pixels[i * writable_desc.channels + 0] = 30;
  //       region_pixels[i * writable_desc.channels + 1] = 220;
  //       region_pixels[i * writable_desc.channels + 2] = 120;
  //       region_pixels[i * writable_desc.channels + 3] = 255;
  //     }

  //     VkrTextureWriteRegion write_region = {
  //         .mip_level = 0,
  //         .array_layer = 0,
  //         .x = (writable_desc.width - block_w) / 2,
  //         .y = (writable_desc.height - block_h) / 2,
  //         .width = block_w,
  //         .height = block_h,
  //     };

  //     VkrRendererError region_err = vkr_texture_system_write_region(
  //         &rf->texture_system, ui_runtime_texture, &write_region,
  //         region_pixels, region_size);
  //     if (region_err != VKR_RENDERER_ERROR_NONE) {
  //       String8 err = vkr_renderer_get_error_string(region_err);
  //       log_warn("Writable texture region upload failed: %s",
  //                string8_cstr(&err));
  //     }
  //   }

  //   VkrRendererError resize_err = VKR_RENDERER_ERROR_NONE;
  //   VkrTextureHandle resized_handle = ui_runtime_texture;
  //   if (vkr_texture_system_resize(&rf->texture_system, ui_runtime_texture,
  //   192,
  //                                 128, true_v, &resized_handle, &resize_err))
  //                                 {
  //     ui_runtime_texture = resized_handle;
  //   } else if (resize_err != VKR_RENDERER_ERROR_NONE) {
  //     String8 err = vkr_renderer_get_error_string(resize_err);
  //     log_warn("Writable texture resize failed: %s", string8_cstr(&err));
  //   }

  //   VkrMaterial *ui_mat = vkr_material_system_get_by_handle(
  //       &rf->material_system, rf->ui_material);
  //   if (ui_mat) {
  //     ui_mat->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle = ui_runtime_texture;
  //     ui_mat->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true_v;
  //   }

  //   scratch_destroy(tex_scratch, ARENA_MEMORY_TAG_TEXTURE);
  // } else {
  //   String8 err = vkr_renderer_get_error_string(writable_err);
  //   log_warn("Failed to create writable UI texture: %s", string8_cstr(&err));
  // }

  VkrRendererError mesh_error = VKR_RENDERER_ERROR_NONE;
  VkrSubMeshDesc cube_submeshes[] = {{
      .geometry =
          vkr_geometry_system_get_default_geometry(&rf->geometry_system),
      .material = state->default_material,
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .owns_geometry = true_v,
      .owns_material = true_v,
  }};
  VkrMeshDesc cube_desc = {
      .transform = vkr_transform_from_position_scale_rotation(
          vec3_new(0.0f, 3.0f, -16.0f), vec3_new(0.15f, 0.15f, 0.15f),
          vkr_quat_identity()),
      .submeshes = cube_submeshes,
      .submesh_count = ArrayCount(cube_submeshes),
  };

  VkrMesh *cube_mesh_ptr = NULL;
  if (!vkr_mesh_manager_create(&rf->mesh_manager, &cube_desc, &mesh_error,
                               &cube_mesh_ptr)) {
    String8 err_str = vkr_renderer_get_error_string(mesh_error);
    log_fatal("Failed to create cube mesh: %s", string8_cstr(&err_str));
    return false_v;
  }

  VkrSubMeshDesc cube2_submeshes[] = {{
      .geometry =
          vkr_geometry_system_get_default_geometry(&rf->geometry_system),
      .material = state->default_material,
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .owns_geometry = true_v,
      .owns_material = true_v,
  }};
  VkrMeshDesc cube_desc_2 = {
      .transform = vkr_transform_from_position_scale_rotation(
          vec3_new(12.0f, 0.0f, 0.0f), vec3_new(0.5f, 0.5f, 0.5f),
          vkr_quat_identity()),
      .submeshes = cube2_submeshes,
      .submesh_count = ArrayCount(cube2_submeshes),
  };

  VkrMesh *cube_mesh_2_ptr = NULL;
  if (!vkr_mesh_manager_create(&rf->mesh_manager, &cube_desc_2, &mesh_error,
                               &cube_mesh_2_ptr)) {
    String8 err_str = vkr_renderer_get_error_string(mesh_error);
    log_fatal("Failed to create cube mesh 2: %s", string8_cstr(&err_str));
    return false_v;
  }
  vkr_transform_set_parent(&cube_mesh_2_ptr->transform,
                           &cube_mesh_ptr->transform);

  VkrSubMeshDesc cube3_submeshes[] = {{
      .geometry =
          vkr_geometry_system_get_default_geometry(&rf->geometry_system),
      .material = state->default_material,
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .owns_geometry = true_v,
      .owns_material = true_v,
  }};
  VkrMeshDesc cube_desc_3 = {
      .transform = vkr_transform_from_position_scale_rotation(
          vec3_new(10.0f, 0.0f, 0.0f), vec3_new(0.3f, 0.3f, 0.3f),
          vkr_quat_identity()),
      .submeshes = cube3_submeshes,
      .submesh_count = ArrayCount(cube3_submeshes),
  };

  VkrMesh *cube_mesh_3_ptr = NULL;
  if (!vkr_mesh_manager_create(&rf->mesh_manager, &cube_desc_3, &mesh_error,
                               &cube_mesh_3_ptr)) {
    String8 err_str = vkr_renderer_get_error_string(mesh_error);
    log_fatal("Failed to create cube mesh 3: %s", string8_cstr(&err_str));
    return false_v;
  }
  vkr_transform_set_parent(&cube_mesh_3_ptr->transform,
                           &cube_mesh_2_ptr->transform);

  // Load demo meshes
  VkrRendererError mesh_load_err = VKR_RENDERER_ERROR_NONE;
  uint32_t falcon_mesh_index = VKR_INVALID_ID;
  VkrMeshLoadDesc falcon_desc = {
      .mesh_path = string8_lit("assets/models/falcon.obj"),
      .transform = vkr_transform_from_position_scale_rotation(
          vec3_new(0.0f, 0.2f, -15.0f), vec3_new(0.2f, 0.2f, 0.2f),
          vkr_quat_identity()),
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .shader_override = {0},
  };
  if (!vkr_mesh_manager_load(&rf->mesh_manager, &falcon_desc,
                             &falcon_mesh_index, NULL, &mesh_load_err)) {
    String8 err = vkr_renderer_get_error_string(mesh_load_err);
    log_error("Failed to load falcon mesh: %s", string8_cstr(&err));
  } else {
    VkrMesh *falcon_mesh =
        vkr_mesh_manager_get(&rf->mesh_manager, falcon_mesh_index);
    if (falcon_mesh) {
      VkrSubMeshDesc *falcon_submeshes =
          arena_alloc(rf->mesh_manager.arena,
                      sizeof(VkrSubMeshDesc) * falcon_mesh->submeshes.length,
                      ARENA_MEMORY_TAG_ARRAY);
      if (falcon_submeshes) {
        for (uint32_t i = 0; i < falcon_mesh->submeshes.length; i++) {
          falcon_submeshes[i] = (VkrSubMeshDesc){
              .geometry = falcon_mesh->submeshes.data[i].geometry,
              .material = falcon_mesh->submeshes.data[i].material,
              .pipeline_domain = falcon_mesh->submeshes.data[i].pipeline_domain,
              .shader_override = falcon_mesh->submeshes.data[i].shader_override,
              .owns_geometry = falcon_mesh->submeshes.data[i].owns_geometry,
              .owns_material = falcon_mesh->submeshes.data[i].owns_material,
          };
        }

        VkrMeshDesc falcon_desc2 = {
            .transform = vkr_transform_from_position_scale_rotation(
                vec3_new(5.0f, 0.2f, -15.0f), vec3_new(0.2f, 0.2f, 0.2f),
                vkr_quat_identity()),
            .submeshes = falcon_submeshes,
            .submesh_count = falcon_mesh->submeshes.length,
        };
        if (!vkr_mesh_manager_add(&rf->mesh_manager, &falcon_desc2, NULL,
                                  &mesh_load_err)) {
          String8 err = vkr_renderer_get_error_string(mesh_load_err);
          log_error("Failed to add falcon mesh clone: %s", string8_cstr(&err));
        }
      }
    }
  }

  VkrMeshLoadDesc sponza_desc = {
      .mesh_path = string8_lit("assets/models/sponza.obj"),
      .transform = vkr_transform_from_position_scale_rotation(
          vec3_new(0.0f, 0.0f, -15.0f), vec3_new(0.0085f, 0.0085f, 0.0085f),
          vkr_quat_identity()),
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .shader_override = {0},
  };
  mesh_load_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_load(&rf->mesh_manager, &sponza_desc, NULL, NULL,
                             &mesh_load_err)) {
    String8 err = vkr_renderer_get_error_string(mesh_load_err);
    log_error("Failed to load sponza mesh: %s", string8_cstr(&err));
  }

  return true_v;
}

vkr_internal void vkr_view_world_on_attach(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  vkr_layer_context_set_camera(ctx, &rf->globals.view, &rf->globals.projection);
}

vkr_internal void vkr_view_world_on_resize(VkrLayerContext *ctx, uint32_t width,
                                           uint32_t height) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  vkr_camera_registry_resize_all(&rf->camera_system, width, height);
}

vkr_internal void vkr_view_world_on_render(VkrLayerContext *ctx,
                                           const VkrLayerRenderInfo *info) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(info != NULL, "Layer render info is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  uint32_t mesh_capacity = vkr_mesh_manager_capacity(&rf->mesh_manager);
  bool8_t globals_applied = false_v;

  for (uint32_t i = 0; i < mesh_capacity; i++) {
    VkrMesh *mesh = vkr_mesh_manager_get(&rf->mesh_manager, i);
    if (!mesh)
      continue;

    Mat4 model = mesh->model;
    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    if (submesh_count == 0)
      continue;

    for (uint32_t submesh_index = 0; submesh_index < submesh_count;
         ++submesh_index) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, i, submesh_index);
      if (!submesh)
        continue;

      VkrMaterial *material = vkr_material_system_get_by_handle(
          &rf->material_system, submesh->material);
      const char *material_shader = (material && material->shader_name &&
                                     material->shader_name[0] != '\0')
                                        ? material->shader_name
                                        : "shader.default.world";
      if (!vkr_shader_system_use(&rf->shader_system, material_shader)) {
        vkr_shader_system_use(&rf->shader_system, "shader.default.world");
      }

      uint32_t mat_pipeline_id =
          (material && material->pipeline_id != VKR_INVALID_ID)
              ? material->pipeline_id
              : (uint32_t)submesh->pipeline_domain;

      VkrPipelineHandle resolved = VKR_PIPELINE_HANDLE_INVALID;
      VkrRendererError get_err = VKR_RENDERER_ERROR_NONE;
      vkr_pipeline_registry_get_pipeline_for_material(
          &rf->pipeline_registry, NULL, mat_pipeline_id, &resolved, &get_err);

      VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_mesh_manager_refresh_pipeline(
              &rf->mesh_manager, i, submesh_index, resolved, &refresh_err)) {
        String8 err_str = vkr_renderer_get_error_string(refresh_err);
        log_error("Mesh %u submesh %u failed to refresh pipeline: %s", i,
                  submesh_index, string8_cstr(&err_str));
        continue;
      }

      rf->draw_state.instance_state = submesh->instance_state;

      VkrPipelineHandle current_pipeline =
          vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);
      if (current_pipeline.id != resolved.id ||
          current_pipeline.generation != resolved.generation) {
        VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
        vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, resolved,
                                            &bind_err);
      }

      if (!globals_applied) {
        vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                         VKR_PIPELINE_DOMAIN_WORLD);
        globals_applied = true_v;
      }

      vkr_material_system_apply_local(&rf->material_system,
                                      &(VkrLocalMaterialState){.model = model});

      if (material) {
        vkr_shader_system_bind_instance(&rf->shader_system,
                                        submesh->instance_state.id);

        bool8_t should_apply_instance =
            (submesh->last_render_frame != rf->frame_number);
        if (should_apply_instance) {
          vkr_material_system_apply_instance(&rf->material_system, material,
                                             VKR_PIPELINE_DOMAIN_WORLD);
          submesh->last_render_frame = rf->frame_number;
        }
      }

      vkr_geometry_system_render(rf, &rf->geometry_system, submesh->geometry,
                                 1);
    }
  }
}

vkr_internal void vkr_view_world_on_detach(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  log_debug("World view detached");
}

vkr_internal void vkr_view_world_on_destroy(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (state && state->pipeline.id) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->pipeline);
  }
}
