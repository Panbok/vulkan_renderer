#include "renderer/systems/views/vkr_view_world.h"

#include "containers/str.h"
#include "core/event.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "platform/vkr_platform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_view_system.h"

typedef enum VkrWorldMeshesLoadingState {
  VKR_WORLD_MESHES_LOADING_STATE_NOT_LOADED = 0,
  VKR_WORLD_MESHES_LOADING_STATE_LOADING = 1,
  VKR_WORLD_MESHES_LOADING_STATE_LOADED = 2,
} VkrWorldMeshesLoadingState;

typedef struct VkrViewWorldState {
  VkrShaderConfig shader_config;
  VkrPipelineHandle pipeline;
  VkrPipelineHandle transparent_pipeline;
  VkrMaterialHandle default_material;
  volatile VkrWorldMeshesLoadingState world_meshes_state;
} VkrViewWorldState;

vkr_internal bool8_t vkr_view_world_on_load_meshes_event(Event *event,
                                                         UserData user_data) {
  assert_log(event != NULL, "Event is NULL");
  assert_log(user_data != NULL, "User data is NULL");

  VkrViewWorldState *state = (VkrViewWorldState *)user_data;

  if (state->world_meshes_state == VKR_WORLD_MESHES_LOADING_STATE_LOADING) {
    log_warn("World meshes are loading...");
    return true_v;
  }

  if (state->world_meshes_state == VKR_WORLD_MESHES_LOADING_STATE_LOADED) {
    log_warn("World meshes are already loaded");
    return true_v;
  }

  state->world_meshes_state = VKR_WORLD_MESHES_LOADING_STATE_LOADING;

  return true_v;
}

vkr_internal void vkr_view_world_load_demo_meshes(RendererFrontend *rf,
                                                  VkrViewWorldState *state) {
  float64_t start_time = vkr_platform_get_absolute_time();
  log_info("[%.3fs] Starting world meshes batch load...", start_time);

  // Define all meshes to load
  VkrMeshLoadDesc mesh_descs[] = {
      {
          .mesh_path = string8_lit("assets/models/falcon.obj"),
          .transform = vkr_transform_from_position_scale_rotation(
              vec3_new(0.0f, 0.2f, -15.0f), vec3_new(0.2f, 0.2f, 0.2f),
              vkr_quat_identity()),
          .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
          .shader_override = {0},
      },
      {
          .mesh_path = string8_lit("assets/models/sponza.obj"),
          .transform = vkr_transform_from_position_scale_rotation(
              vec3_new(0.0f, 0.0f, -15.0f), vec3_new(0.0085f, 0.0085f, 0.0085f),
              vkr_quat_identity()),
          .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
          .shader_override = {0},
      },
  };

  uint32_t mesh_count = ArrayCount(mesh_descs);
  uint32_t mesh_indices[2] = {VKR_INVALID_ID, VKR_INVALID_ID};
  VkrRendererError mesh_errors[2] = {VKR_RENDERER_ERROR_NONE,
                                     VKR_RENDERER_ERROR_NONE};

  // Batch load all meshes with parallel I/O, material, and texture loading
  uint32_t loaded = vkr_mesh_manager_load_batch(
      &rf->mesh_manager, mesh_descs, mesh_count, mesh_indices, mesh_errors);

  float64_t end_time = vkr_platform_get_absolute_time();
  float64_t elapsed_ms = (end_time - start_time) * 1000.0;

  // Report results
  const char *mesh_names[] = {"falcon", "sponza"};
  for (uint32_t i = 0; i < mesh_count; i++) {
    if (mesh_indices[i] != VKR_INVALID_ID) {
      log_info("Loaded %s mesh at index %u", mesh_names[i], mesh_indices[i]);
    } else {
      String8 err = vkr_renderer_get_error_string(mesh_errors[i]);
      log_error("Failed to load %s mesh: %s", mesh_names[i],
                string8_cstr(&err));
    }
  }

  log_info("[%.3fs] World meshes batch load complete: %u/%u meshes loaded in "
           "%.2f ms",
           end_time, loaded, mesh_count, elapsed_ms);

  state->world_meshes_state = VKR_WORLD_MESHES_LOADING_STATE_LOADED;
}

typedef struct VkrTransparentSubmeshEntry {
  uint32_t mesh_index;
  uint32_t submesh_index;
  float32_t distance;
} VkrTransparentSubmeshEntry;

vkr_internal int vkr_transparent_submesh_compare(const void *a, const void *b) {
  const VkrTransparentSubmeshEntry *entry_a =
      (const VkrTransparentSubmeshEntry *)a;
  const VkrTransparentSubmeshEntry *entry_b =
      (const VkrTransparentSubmeshEntry *)b;
  // Sort descending (back-to-front): larger distance first
  if (entry_a->distance > entry_b->distance)
    return -1;
  if (entry_a->distance < entry_b->distance)
    return 1;
  return 0;
}

vkr_internal bool8_t vkr_submesh_has_transparency(RendererFrontend *rf,
                                                  VkrMaterial *material) {
  if (!material)
    return false_v;

  VkrMaterialTexture *diffuse_tex =
      &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
  if (diffuse_tex->enabled) {
    VkrTexture *texture = vkr_texture_system_get_by_handle(&rf->texture_system,
                                                           diffuse_tex->handle);
    if (texture && bitset8_is_set(&texture->description.properties,
                                  VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT)) {
      return true_v;
    }
  }

  return false_v;
}

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

  if (!vkr_shader_system_create(&rf->shader_system, &state->shader_config)) {
    log_error("Failed to create shader system from config");
    return false_v;
  }

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

  // Create transparent world pipeline (same shader, different domain settings)
  VkrRendererError transparent_pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->shader_config,
          VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
          string8_lit("world_transparent"), &state->transparent_pipeline,
          &transparent_pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(transparent_pipeline_error);
    log_error("Config world transparent pipeline failed: %s",
              string8_cstr(&err_str));
    return false_v;
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
      .owns_geometry = false_v,
      .owns_material = false_v,
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

  vkr_view_world_load_demo_meshes(rf, state);

  event_manager_subscribe(rf->event_manager, EVENT_TYPE_LOAD_WORLD_MESHES,
                          vkr_view_world_on_load_meshes_event, state);

  log_info(
      "World view initialized. Press 'L' to load falcon and sponza meshes.");

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

vkr_internal void vkr_view_world_render_submesh(RendererFrontend *rf,
                                                VkrViewWorldState *state,
                                                uint32_t mesh_index,
                                                uint32_t submesh_index,
                                                VkrPipelineDomain domain,
                                                bool8_t *globals_applied) {
  VkrMesh *mesh = vkr_mesh_manager_get(&rf->mesh_manager, mesh_index);
  if (!mesh || mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED)
    return;

  VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(&rf->mesh_manager,
                                                     mesh_index, submesh_index);
  if (!submesh)
    return;

  Mat4 model = mesh->model;

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &rf->material_system, submesh->material);
  const char *material_shader =
      (material && material->shader_name && material->shader_name[0] != '\0')
          ? material->shader_name
          : "shader.default.world";
  if (!vkr_shader_system_use(&rf->shader_system, material_shader)) {
    vkr_shader_system_use(&rf->shader_system, "shader.default.world");
  }

  VkrPipelineHandle resolved = (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT)
                                   ? state->transparent_pipeline
                                   : state->pipeline;

  VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_refresh_pipeline(&rf->mesh_manager, mesh_index,
                                         submesh_index, resolved,
                                         &refresh_err)) {
    String8 err_str = vkr_renderer_get_error_string(refresh_err);
    log_error("Mesh %u submesh %u failed to refresh pipeline: %s", mesh_index,
              submesh_index, string8_cstr(&err_str));
    return;
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

  if (!*globals_applied) {
    vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                     VKR_PIPELINE_DOMAIN_WORLD);
    *globals_applied = true_v;
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

  vkr_geometry_system_render(rf, &rf->geometry_system, submesh->geometry, 1);
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

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    log_error("World view state is NULL");
    return;
  }

  // if (state->world_meshes_state == VKR_WORLD_MESHES_LOADING_STATE_LOADING) {
  //   state->world_meshes_state = VKR_WORLD_MESHES_LOADING_STATE_LOADED;
  //   vkr_view_world_load_demo_meshes(rf, state);
  // }

  uint32_t mesh_capacity = vkr_mesh_manager_capacity(&rf->mesh_manager);
  bool8_t globals_applied = false_v;

  Vec3 camera_pos = rf->globals.view_position;

  Scratch scratch = scratch_create(rf->scratch_arena);
  uint32_t max_transparent_entries = 0;

  for (uint32_t i = 0; i < mesh_capacity; i++) {
    VkrMesh *mesh = vkr_mesh_manager_get(&rf->mesh_manager, i);
    if (!mesh)
      continue;
    max_transparent_entries += vkr_mesh_manager_submesh_count(mesh);
  }

  VkrTransparentSubmeshEntry *transparent_entries = NULL;
  uint32_t transparent_count = 0;

  if (max_transparent_entries > 0) {
    transparent_entries = arena_alloc(scratch.arena,
                                      sizeof(VkrTransparentSubmeshEntry) *
                                          max_transparent_entries,
                                      ARENA_MEMORY_TAG_ARRAY);
  }

  // first pass: render opaque submeshes and collect transparent ones
  for (uint32_t i = 0; i < mesh_capacity; i++) {
    VkrMesh *mesh = vkr_mesh_manager_get(&rf->mesh_manager, i);
    if (!mesh)
      continue;

    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    if (submesh_count == 0)
      continue;

    Mat4 mesh_world_pos = vkr_transform_get_world(&mesh->transform);

    for (uint32_t submesh_index = 0; submesh_index < submesh_count;
         ++submesh_index) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, i, submesh_index);
      if (!submesh)
        continue;

      VkrMaterial *material = vkr_material_system_get_by_handle(
          &rf->material_system, submesh->material);

      if (vkr_submesh_has_transparency(rf, material)) {
        if (transparent_entries &&
            transparent_count < max_transparent_entries) {
          Vec3 mesh_pos =
              vec3_new(mesh_world_pos.elements[12], mesh_world_pos.elements[13],
                       mesh_world_pos.elements[14]);
          float32_t diff = vec3_distance(mesh_pos, camera_pos);
          transparent_entries[transparent_count++] =
              (VkrTransparentSubmeshEntry){
                  .mesh_index = i,
                  .submesh_index = submesh_index,
                  .distance = vkr_abs_f32(diff),
              };
        }
      } else {
        vkr_view_world_render_submesh(rf, state, i, submesh_index,
                                      VKR_PIPELINE_DOMAIN_WORLD,
                                      &globals_applied);
      }
    }
  }

  // second pass: render transparent submeshes sorted by distance
  // (back-to-front)
  if (transparent_count > 0 && transparent_entries) {
    qsort(transparent_entries, transparent_count,
          sizeof(VkrTransparentSubmeshEntry), vkr_transparent_submesh_compare);

    for (uint32_t t = 0; t < transparent_count; ++t) {
      VkrTransparentSubmeshEntry *entry = &transparent_entries[t];
      vkr_view_world_render_submesh(
          rf, state, entry->mesh_index, entry->submesh_index,
          VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT, &globals_applied);
    }
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
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

  event_manager_unsubscribe(rf->event_manager, EVENT_TYPE_LOAD_WORLD_MESHES,
                            vkr_view_world_on_load_meshes_event);

  VkrViewWorldState *state =
      (VkrViewWorldState *)vkr_layer_context_get_user_data(ctx);
  if (state) {
    if (state->pipeline.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->pipeline);
    }
    if (state->transparent_pipeline.id) {
      vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                             state->transparent_pipeline);
    }
  }
}
