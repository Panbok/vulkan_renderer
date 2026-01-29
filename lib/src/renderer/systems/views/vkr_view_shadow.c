#include "renderer/systems/views/vkr_view_shadow.h"

#include <ctype.h>

#include "containers/str.h"
#include "core/logger.h"
#include "math/vec.h"
#include "math/vkr_frustum.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_layer_messages.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_draw_batch.h"

#define VKR_VIEW_SHADOW_DRAW_BATCH_INITIAL_CAPACITY 1024

/**
 * @brief Unified submesh info for shadow rendering.
 * Works for both legacy VkrSubMesh and VkrMeshAssetSubmesh.
 */
typedef struct VkrShadowSubmeshInfo {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t opaque_first_index;
  uint32_t opaque_index_count;
  int32_t opaque_vertex_offset;
  uint32_t range_id;
  bool8_t valid;
} VkrShadowSubmeshInfo;

/**
 * @brief Get submesh info from draw command, handling both legacy and instance.
 */
vkr_internal VkrShadowSubmeshInfo
vkr_view_shadow_get_submesh_info(RendererFrontend *rf,
                                 const VkrDrawCommand *cmd) {
  VkrShadowSubmeshInfo info = {0};

  if (cmd->is_instance) {
    VkrMeshInstance *inst =
        vkr_mesh_manager_get_instance_by_index(&rf->mesh_manager,
                                               cmd->mesh_index);
    if (!inst) {
      return info;
    }
    VkrMeshAsset *asset =
        vkr_mesh_manager_get_asset(&rf->mesh_manager, inst->asset);
    if (!asset || cmd->submesh_index >= asset->submeshes.length) {
      return info;
    }
    VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[cmd->submesh_index];
    info.geometry = submesh->geometry;
    info.material = submesh->material;
    info.first_index = submesh->first_index;
    info.index_count = submesh->index_count;
    info.vertex_offset = submesh->vertex_offset;
    info.opaque_first_index = submesh->opaque_first_index;
    info.opaque_index_count = submesh->opaque_index_count;
    info.opaque_vertex_offset = submesh->opaque_vertex_offset;
    info.range_id = submesh->range_id;
    info.valid = true_v;
  } else {
    const VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(
        &rf->mesh_manager, cmd->mesh_index, cmd->submesh_index);
    if (!submesh) {
      return info;
    }
    info.geometry = submesh->geometry;
    info.material = submesh->material;
    info.first_index = submesh->first_index;
    info.index_count = submesh->index_count;
    info.vertex_offset = submesh->vertex_offset;
    info.opaque_first_index = submesh->opaque_first_index;
    info.opaque_index_count = submesh->opaque_index_count;
    info.opaque_vertex_offset = submesh->opaque_vertex_offset;
    info.range_id = submesh->range_id;
    info.valid = true_v;
  }

  return info;
}

typedef struct VkrViewShadowState {
  VkrShadowSystem shadow_system;
  VkrRenderTargetHandle *pass_targets[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t pass_target_count;
  VkrRendererInstanceStateHandle *material_instances_alpha;
  uint32_t material_instance_count;
  VkrDrawBatcher draw_batcher;
  uint64_t last_frame_updated;
  bool8_t initialized;
} VkrViewShadowState;

vkr_internal void vkr_view_shadow_free_pass_targets(RendererFrontend *rf,
                                                    VkrViewShadowState *state) {
  if (!state || !rf) {
    return;
  }

  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    if (!state->pass_targets[i]) {
      continue;
    }
    vkr_allocator_free(&rf->allocator, state->pass_targets[i],
                       sizeof(VkrRenderTargetHandle) *
                           (uint64_t)state->pass_target_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    state->pass_targets[i] = NULL;
  }
  state->pass_target_count = 0;
}

vkr_internal void vkr_view_shadow_release_material_instances(
    RendererFrontend *rf, VkrPipelineHandle pipeline,
    VkrRendererInstanceStateHandle *instances, uint32_t count) {
  if (!rf || pipeline.id == 0 || !instances) {
    return;
  }

  for (uint32_t i = 0; i < count; ++i) {
    if (instances[i].id == 0) {
      continue;
    }
    VkrRendererError release_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_release_instance_state(
            &rf->pipeline_registry, pipeline, instances[i], &release_err)) {
      String8 err = vkr_renderer_get_error_string(release_err);
      log_warn("Shadow view: failed to release instance state: %s",
               string8_cstr(&err));
    }
  }
}

vkr_internal void
vkr_view_shadow_free_material_instances(RendererFrontend *rf,
                                        VkrViewShadowState *state) {
  if (!rf || !state) {
    return;
  }

  vkr_view_shadow_release_material_instances(
      rf, state->shadow_system.shadow_pipeline_alpha,
      state->material_instances_alpha, state->material_instance_count);

  if (state->material_instances_alpha) {
    vkr_allocator_free(&rf->allocator, state->material_instances_alpha,
                       sizeof(VkrRendererInstanceStateHandle) *
                           (uint64_t)state->material_instance_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    state->material_instances_alpha = NULL;
  }

  state->material_instance_count = 0;
}

vkr_internal VkrRendererInstanceStateHandle
vkr_view_shadow_get_material_instance(RendererFrontend *rf,
                                      VkrViewShadowState *state,
                                      VkrPipelineHandle pipeline,
                                      VkrRendererInstanceStateHandle *instances,
                                      uint32_t material_index) {
  if (!rf || !state || !instances ||
      material_index >= state->material_instance_count) {
    return (VkrRendererInstanceStateHandle){0};
  }

  VkrRendererInstanceStateHandle handle = instances[material_index];
  if (handle.id != 0) {
    return handle;
  }

  VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, pipeline, &handle, &acquire_err)) {
    String8 err = vkr_renderer_get_error_string(acquire_err);
    log_warn("Shadow view: failed to acquire instance state: %s",
             string8_cstr(&err));
    return (VkrRendererInstanceStateHandle){0};
  }

  instances[material_index] = handle;
  return handle;
}

vkr_internal VkrTextureOpaqueHandle vkr_view_shadow_get_diffuse_texture(
    RendererFrontend *rf, const VkrMaterial *material) {
  if (!rf) {
    return NULL;
  }

  VkrTextureHandle diffuse_handle =
      vkr_texture_system_get_default_diffuse_handle(&rf->texture_system);
  if (material && material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled) {
    diffuse_handle = material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle;
  }

  VkrTexture *diffuse =
      vkr_texture_system_get_by_handle(&rf->texture_system, diffuse_handle);
  if (!diffuse || diffuse->description.type != VKR_TEXTURE_TYPE_2D) {
    VkrTextureHandle fallback =
        vkr_texture_system_get_default_diffuse_handle(&rf->texture_system);
    diffuse = vkr_texture_system_get_by_handle(&rf->texture_system, fallback);
  }

  return diffuse ? diffuse->handle : NULL;
}

vkr_internal bool8_t vkr_view_shadow_cstr_contains_i(const char *haystack,
                                                     const char *needle) {
  if (!haystack || !needle || !needle[0]) {
    return false_v;
  }

  uint64_t needle_len = string_length(needle);
  for (const char *h = haystack; *h; ++h) {
    const char *h_it = h;
    const char *n_it = needle;
    while (*h_it && *n_it &&
           tolower((unsigned char)*h_it) ==
               tolower((unsigned char)*n_it)) {
      ++h_it;
      ++n_it;
    }
    if ((uint64_t)(n_it - needle) == needle_len) {
      return true_v;
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_view_shadow_cstr_contains_any_i(
    const char *haystack, const char *const *keywords, uint32_t keyword_count) {
  if (!haystack || !keywords || keyword_count == 0) {
    return false_v;
  }

  for (uint32_t i = 0; i < keyword_count; ++i) {
    if (vkr_view_shadow_cstr_contains_i(haystack, keywords[i])) {
      return true_v;
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_view_shadow_material_is_foliage(
    RendererFrontend *rf, const VkrMaterial *material) {
  if (!rf || !material) {
    return false_v;
  }

  static const char *foliage_keywords[] = {"leaf", "foliage", "grass",
                                           "fern", "bush",    "ivy",
                                           "vine", "frond"};
  const uint32_t keyword_count =
      (uint32_t)(sizeof(foliage_keywords) / sizeof(foliage_keywords[0]));

  if (material->name &&
      vkr_view_shadow_cstr_contains_any_i(material->name, foliage_keywords,
                                          keyword_count)) {
    return true_v;
  }

  const VkrMaterialTexture *diffuse_tex =
      &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
  if (!diffuse_tex->enabled) {
    return false_v;
  }

  VkrTexture *diffuse =
      vkr_texture_system_get_by_handle(&rf->texture_system, diffuse_tex->handle);
  if (!diffuse || !diffuse->file_path.path.str) {
    return false_v;
  }

  return vkr_view_shadow_cstr_contains_any_i(
      (const char *)diffuse->file_path.path.str, foliage_keywords,
      keyword_count);
}

vkr_internal float32_t vkr_view_shadow_get_alpha_cutoff(
    RendererFrontend *rf, const VkrMaterial *material,
    const VkrShadowConfig *config) {
  if (!material) {
    return 0.0f;
  }
  if (material->alpha_cutoff <= 0.0f) {
    return 0.0f;
  }

  const VkrMaterialTexture *diffuse_tex =
      &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
  if (!diffuse_tex->enabled) {
    return 0.0f;
  }

  float32_t cutoff = material->alpha_cutoff;
  if (!config || config->foliage_alpha_cutoff_bias <= 0.0f) {
    return cutoff;
  }

  if (vkr_view_shadow_material_is_foliage(rf, material)) {
    cutoff += config->foliage_alpha_cutoff_bias;
    if (cutoff > 1.0f) {
      cutoff = 1.0f;
    }
  }
  return cutoff;
}

typedef struct VkrViewShadowDrawRange {
  const VkrIndexBuffer *index_buffer;
  uint32_t index_count;
  uint32_t first_index;
  int32_t vertex_offset;
  bool8_t uses_opaque_indices;
} VkrViewShadowDrawRange;

vkr_internal VkrViewShadowDrawRange vkr_view_shadow_resolve_draw_range(
    RendererFrontend *rf, const VkrSubMesh *submesh, bool8_t allow_opaque) {
  VkrViewShadowDrawRange range = {
      .index_buffer = NULL,
      .index_count = submesh->index_count,
      .first_index = submesh->first_index,
      .vertex_offset = submesh->vertex_offset,
      .uses_opaque_indices = false_v,
  };

  if (!allow_opaque || submesh->opaque_index_count == 0) {
    return range;
  }

  VkrGeometry *geometry =
      vkr_geometry_system_get_by_handle(&rf->geometry_system, submesh->geometry);
  if (!geometry || !geometry->opaque_index_buffer.handle) {
    return range;
  }

  range.index_buffer = &geometry->opaque_index_buffer;
  range.index_count = submesh->opaque_index_count;
  range.first_index = submesh->opaque_first_index;
  range.vertex_offset = submesh->opaque_vertex_offset;
  range.uses_opaque_indices = true_v;
  return range;
}

/**
 * @brief Resolve draw range from unified submesh info.
 */
vkr_internal VkrViewShadowDrawRange vkr_view_shadow_resolve_draw_range_info(
    RendererFrontend *rf, const VkrShadowSubmeshInfo *info, bool8_t allow_opaque) {
  VkrViewShadowDrawRange range = {
      .index_buffer = NULL,
      .index_count = info->index_count,
      .first_index = info->first_index,
      .vertex_offset = info->vertex_offset,
      .uses_opaque_indices = false_v,
  };

  if (!allow_opaque || info->opaque_index_count == 0) {
    return range;
  }

  VkrGeometry *geometry =
      vkr_geometry_system_get_by_handle(&rf->geometry_system, info->geometry);
  if (!geometry || !geometry->opaque_index_buffer.handle) {
    return range;
  }

  range.index_buffer = &geometry->opaque_index_buffer;
  range.index_count = info->opaque_index_count;
  range.first_index = info->opaque_first_index;
  range.vertex_offset = info->opaque_vertex_offset;
  range.uses_opaque_indices = true_v;
  return range;
}

vkr_internal bool32_t vkr_view_shadow_on_create(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return false_v;
  }

  VkrViewShadowState *state =
      (VkrViewShadowState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return false_v;
  }

  state->pass_target_count = 0;

  VkrShadowConfig cfg = VKR_SHADOW_CONFIG_DEFAULT;
  if (!vkr_shadow_system_init(&state->shadow_system, rf, &cfg)) {
    goto cleanup;
  }

  uint32_t material_count = rf->material_system.materials.length;
  if (material_count > 0) {
    state->material_instances_alpha = vkr_allocator_alloc(
        &rf->allocator,
        sizeof(VkrRendererInstanceStateHandle) * (uint64_t)material_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!state->material_instances_alpha) {
      log_error("Failed to allocate shadow material instance states");
      goto cleanup;
    }
    MemZero(state->material_instances_alpha,
            sizeof(VkrRendererInstanceStateHandle) * (uint64_t)material_count);
    state->material_instance_count = material_count;
  }

  if (!vkr_draw_batcher_init(&state->draw_batcher, &rf->allocator,
                             VKR_VIEW_SHADOW_DRAW_BATCH_INITIAL_CAPACITY)) {
    log_error("Failed to initialize shadow draw batcher");
    goto cleanup;
  }

  VkrLayer *layer = ctx->layer;
  if (!layer ||
      layer->pass_count != state->shadow_system.config.cascade_count) {
    log_error("Shadow layer pass count does not match cascade count");
    goto cleanup;
  }

  uint32_t frame_count = vkr_renderer_window_attachment_count(rf);
  state->pass_target_count = frame_count;

  for (uint32_t pass_index = 0; pass_index < layer->pass_count; ++pass_index) {
    VkrLayerPass *pass = array_get_VkrLayerPass(&layer->passes, pass_index);
    pass->use_custom_render_targets = true_v;

    if (frame_count == 0) {
      continue;
    }

    state->pass_targets[pass_index] = vkr_allocator_alloc(
        &rf->allocator, sizeof(VkrRenderTargetHandle) * (uint64_t)frame_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!state->pass_targets[pass_index]) {
      log_error("Failed to allocate shadow pass targets");
      goto cleanup;
    }

    for (uint32_t f = 0; f < frame_count; ++f) {
      state->pass_targets[pass_index][f] = vkr_shadow_system_get_render_target(
          &state->shadow_system, f, pass_index);
    }

    pass->render_targets = state->pass_targets[pass_index];
    pass->render_target_count = frame_count;
  }

  state->initialized = true_v;
  return true_v;

cleanup:
  vkr_view_shadow_free_pass_targets(rf, state);
  vkr_draw_batcher_shutdown(&state->draw_batcher);
  vkr_view_shadow_free_material_instances(rf, state);
  vkr_shadow_system_shutdown(&state->shadow_system, rf);
  return false_v;
}

vkr_internal void vkr_view_shadow_on_destroy(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  VkrViewShadowState *state =
      (VkrViewShadowState *)vkr_layer_context_get_user_data(ctx);
  if (!rf || !state) {
    return;
  }

  vkr_view_shadow_free_material_instances(rf, state);
  vkr_view_shadow_free_pass_targets(rf, state);
  vkr_draw_batcher_shutdown(&state->draw_batcher);
  vkr_shadow_system_shutdown(&state->shadow_system, rf);
  state->initialized = false_v;
}

vkr_internal void vkr_view_shadow_on_data_received(VkrLayerContext *ctx,
                                                   const VkrLayerMsgHeader *msg,
                                                   void *out_rsp,
                                                   uint64_t out_rsp_capacity,
                                                   uint64_t *out_rsp_size) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(msg != NULL, "Message is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  VkrViewShadowState *state =
      (VkrViewShadowState *)vkr_layer_context_get_user_data(ctx);
  if (!rf || !state || !state->initialized) {
    return;
  }

  switch (msg->kind) {
  case VKR_LAYER_MSG_SHADOW_INVALIDATE_INSTANCE_STATES: {
    vkr_view_shadow_free_material_instances(rf, state);

    uint32_t material_count = rf->material_system.materials.length;
    if (material_count > 0) {
      state->material_instances_alpha = vkr_allocator_alloc(
          &rf->allocator,
          sizeof(VkrRendererInstanceStateHandle) * (uint64_t)material_count,
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (state->material_instances_alpha) {
        MemZero(state->material_instances_alpha,
                sizeof(VkrRendererInstanceStateHandle) *
                    (uint64_t)material_count);
        state->material_instance_count = material_count;
      }
    }

    state->last_frame_updated = 0;
  } break;
  case VKR_LAYER_MSG_SHADOW_GET_FRAME_DATA: {
    if (!out_rsp || !out_rsp_size) {
      return;
    }
    if (out_rsp_capacity < sizeof(VkrLayerRsp_ShadowFrameData)) {
      return;
    }

    const VkrLayerMsg_ShadowGetFrameData *payload =
        (const VkrLayerMsg_ShadowGetFrameData *)msg;
    VkrLayerRsp_ShadowFrameData *rsp = (VkrLayerRsp_ShadowFrameData *)out_rsp;
    *rsp = (VkrLayerRsp_ShadowFrameData){
        .h = {.kind = VKR_LAYER_RSP_SHADOW_FRAME_DATA,
              .version = 1,
              .data_size = sizeof(VkrShadowFrameData),
              .error = VKR_RENDERER_ERROR_NONE},
    };
    vkr_shadow_system_get_frame_data(&state->shadow_system,
                                     payload->payload.frame_index, &rsp->data);
    *out_rsp_size = sizeof(*rsp);
  } break;
  default:
    break;
  }
}

vkr_internal void vkr_view_shadow_on_render(VkrLayerContext *ctx,
                                            const VkrLayerRenderInfo *info) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(info != NULL, "Layer render info is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  VkrViewShadowState *state =
      (VkrViewShadowState *)vkr_layer_context_get_user_data(ctx);
  if (!state || !state->initialized) {
    return;
  }

  uint32_t cascade_index = vkr_layer_context_get_pass_index(ctx);
  if (cascade_index >= state->shadow_system.config.cascade_count) {
    return;
  }

  if (state->last_frame_updated != rf->frame_number) {
    if (rf->active_scene) {
      vkr_lighting_system_sync_from_scene(&rf->lighting_system,
                                          rf->active_scene);
    }

    VkrCamera *camera = vkr_camera_registry_get_by_handle(
        &rf->camera_system, rf->camera_system.active_camera);
    if (camera) {
      vkr_shadow_system_update(&state->shadow_system, camera,
                               rf->lighting_system.directional.enabled,
                               rf->lighting_system.directional.direction);
    }

    state->last_frame_updated = rf->frame_number;
  }

  if (!state->shadow_system.light_enabled) {
    return;
  }

  uint32_t cascade_size =
      vkr_shadow_config_get_max_map_size(&state->shadow_system.config);
  if (rf->shadow_debug_mode != 0 && (rf->frame_number % 240u) == 0u) {
    VkrRenderTargetHandle rt =
        vkr_layer_context_get_render_target(ctx, info->image_index);
    const void *map = NULL;
    const void *target = (const void *)rt;
    if (state->shadow_system.frames &&
        info->image_index < state->shadow_system.frame_resource_count) {
      map = (const void *)
                state->shadow_system.frames[info->image_index].shadow_map;
    }

    const VkrCascadeData *cascade =
        &state->shadow_system.cascades[cascade_index];

    VkrCamera *camera = vkr_camera_registry_get_by_handle(
        &rf->camera_system, rf->camera_system.active_camera);
    if (camera && cascade_index < VKR_SHADOW_CASCADE_COUNT_MAX) {
      float32_t split_near = state->shadow_system.cascade_splits[cascade_index];
      float32_t split_far = cascade->split_far;
      float32_t split_mid = (split_near + split_far) * 0.5f;
      Vec3 forward = vec3_normalize(camera->forward);
      Vec3 test_pos =
          vec3_add(camera->position, vec3_scale(forward, split_mid));

      Vec4 clip =
          mat4_mul_vec4(cascade->view_projection, vec3_to_vec4(test_pos, 1.0f));
      if (clip.w != 0.0f) {
        clip = vec4_scale(clip, 1.0f / clip.w);
      }
    }
  }

  VkrInstanceBufferPool *instance_pool = &rf->instance_buffer_pool;
  if (!instance_pool->initialized) {
    log_error("Shadow view requires an initialized instance buffer pool");
    return;
  }

  VkrIndirectDrawSystem *indirect_system = &rf->indirect_draw_system;
  bool8_t use_mdi =
      indirect_system && indirect_system->initialized &&
      indirect_system->enabled && rf->backend.draw_indexed_indirect != NULL &&
      rf->supports_multi_draw_indirect &&
      rf->supports_draw_indirect_first_instance;
  bool8_t mdi_available = use_mdi;
  bool8_t mdi_warned = false_v;

  vkr_renderer_set_depth_bias(
      rf, state->shadow_system.config.depth_bias_constant_factor,
      state->shadow_system.config.depth_bias_clamp,
      state->shadow_system.config.depth_bias_slope_factor);

  VkrViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = (float32_t)cascade_size,
      .height = (float32_t)cascade_size,
      .min_depth = 0.0f,
      .max_depth = 1.0f,
  };
  VkrScissor scissor = {
      .x = 0,
      .y = 0,
      .width = cascade_size,
      .height = cascade_size,
  };

  vkr_renderer_set_viewport(rf, &viewport);
  vkr_renderer_set_scissor(rf, &scissor);

  VkrFrustum shadow_frustum = vkr_frustum_from_matrix(
      state->shadow_system.cascades[cascade_index].view_projection);

  uint32_t mesh_count = vkr_mesh_manager_count(&rf->mesh_manager);
  vkr_draw_batcher_reset(&state->draw_batcher);

  for (uint32_t m = 0; m < mesh_count; ++m) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh =
        vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager, m,
                                                &mesh_slot);
    if (!mesh || !mesh->visible ||
        mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }
    if (mesh->bounds_valid) {
      if (!vkr_frustum_test_sphere(&shadow_frustum, mesh->bounds_world_center,
                                   mesh->bounds_world_radius)) {
        continue;
      }
    }

    Mat4 model = mesh->model;

    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    for (uint32_t s = 0; s < submesh_count; ++s) {
      const VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, mesh_slot, s);
      if (!submesh) {
        continue;
      }
      if (submesh->pipeline_domain != VKR_PIPELINE_DOMAIN_WORLD &&
          submesh->pipeline_domain != VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) {
        continue;
      }

      VkrMaterial *material = vkr_material_system_get_by_handle(
          &rf->material_system, submesh->material);
      if (!material && rf->material_system.default_material.id != 0) {
        material = vkr_material_system_get_by_handle(
            &rf->material_system, rf->material_system.default_material);
      }

      float32_t alpha_cutoff = vkr_view_shadow_get_alpha_cutoff(
          rf, material, &state->shadow_system.config);
      bool8_t has_alpha = alpha_cutoff > 0.0f;
      VkrPipelineHandle pipeline = has_alpha
                                       ? state->shadow_system.shadow_pipeline_alpha
                                       : state->shadow_system.shadow_pipeline_opaque;
      if (pipeline.id == 0 && !has_alpha) {
        pipeline = state->shadow_system.shadow_pipeline_alpha;
        has_alpha = true_v;
      }
      if (pipeline.id == 0) {
        continue;
      }

      uint32_t range_id = use_mdi ? 0 : submesh->range_id;
      VkrDrawCommand cmd = {
          .key = {.pipeline_id = pipeline.id,
                  .material_id = has_alpha && material ? material->id : 0,
                  .geometry_id = submesh->geometry.id,
                  .range_id = range_id},
          .mesh_index = mesh_slot,
          .submesh_index = s,
          .model = model,
          .object_id = 0,
          .camera_distance = 0.0f,
          .is_instance = false_v,
      };
      vkr_draw_batcher_add_opaque(&state->draw_batcher, &cmd);
    }
  }

  // Instance iteration
  uint32_t instance_count =
      vkr_mesh_manager_instance_count(&rf->mesh_manager);
  for (uint32_t i = 0; i < instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *inst = vkr_mesh_manager_get_instance_by_live_index(
        &rf->mesh_manager, i, &instance_slot);
    if (!inst || !inst->visible ||
        inst->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }
    if (inst->bounds_valid) {
      if (!vkr_frustum_test_sphere(&shadow_frustum, inst->bounds_world_center,
                                   inst->bounds_world_radius)) {
        continue;
      }
    }

    VkrMeshAsset *asset =
        vkr_mesh_manager_get_asset(&rf->mesh_manager, inst->asset);
    if (!asset) {
      continue;
    }

    Mat4 model = inst->model;

    for (uint32_t s = 0; s < asset->submeshes.length; ++s) {
      VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];

      if (submesh->pipeline_domain != VKR_PIPELINE_DOMAIN_WORLD &&
          submesh->pipeline_domain != VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) {
        continue;
      }

      VkrMaterial *material = vkr_material_system_get_by_handle(
          &rf->material_system, submesh->material);
      if (!material && rf->material_system.default_material.id != 0) {
        material = vkr_material_system_get_by_handle(
            &rf->material_system, rf->material_system.default_material);
      }

      float32_t alpha_cutoff = vkr_view_shadow_get_alpha_cutoff(
          rf, material, &state->shadow_system.config);
      bool8_t has_alpha = alpha_cutoff > 0.0f;
      VkrPipelineHandle pipeline =
          has_alpha ? state->shadow_system.shadow_pipeline_alpha
                    : state->shadow_system.shadow_pipeline_opaque;
      if (pipeline.id == 0 && !has_alpha) {
        pipeline = state->shadow_system.shadow_pipeline_alpha;
        has_alpha = true_v;
      }
      if (pipeline.id == 0) {
        continue;
      }

      uint32_t range_id = use_mdi ? 0 : submesh->range_id;
      VkrDrawCommand cmd = {
          .key = {.pipeline_id = pipeline.id,
                  .material_id = has_alpha && material ? material->id : 0,
                  .geometry_id = submesh->geometry.id,
                  .range_id = range_id},
          .mesh_index = instance_slot,
          .submesh_index = s,
          .model = model,
          .object_id = 0,
          .camera_distance = 0.0f,
          .is_instance = true_v,
      };
      vkr_draw_batcher_add_opaque(&state->draw_batcher, &cmd);
    }
  }

  vkr_draw_batcher_finalize(&state->draw_batcher);

  uint32_t batch_count =
      vkr_draw_batcher_opaque_batch_count(&state->draw_batcher);
  uint32_t batch_count_opaque = 0;
  uint32_t batch_count_alpha = 0;
  for (uint32_t b = 0; b < batch_count; ++b) {
    const VkrDrawBatch *batch = &state->draw_batcher.opaque_batches.data[b];
    if (batch->key.pipeline_id ==
        state->shadow_system.shadow_pipeline_alpha.id) {
      batch_count_alpha += 1;
    } else {
      batch_count_opaque += 1;
    }
  }
  if (cascade_index < VKR_SHADOW_CASCADE_COUNT_MAX) {
    rf->frame_metrics.shadow.shadow_batches_opaque[cascade_index] =
        batch_count_opaque;
    rf->frame_metrics.shadow.shadow_batches_alpha[cascade_index] =
        batch_count_alpha;
  }
  for (uint32_t b = 0; b < batch_count; ++b) {
    VkrDrawBatch *batch = &state->draw_batcher.opaque_batches.data[b];
    VkrInstanceDataGPU *instances = NULL;
    uint32_t base_instance = 0;
    if (!vkr_instance_buffer_alloc(instance_pool, batch->command_count,
                                   &base_instance, &instances)) {
      log_warn("Shadow view: instance buffer allocation failed for batch");
      batch->command_count = 0;
      continue;
    }
    batch->first_instance = base_instance;

    for (uint32_t c = 0; c < batch->command_count; ++c) {
      const VkrDrawCommand *cmd =
          &state->draw_batcher.opaque_commands.data[batch->first_command + c];
      instances[c] = (VkrInstanceDataGPU){
          .model = cmd->model,
          .object_id = 0,
          .material_index = 0,
          .flags = 0,
          ._padding = 0,
      };
    }

    vkr_instance_buffer_flush_range(instance_pool, base_instance,
                                    batch->command_count);
  }

  uint32_t current_pipeline_id = 0;
  for (uint32_t b = 0; b < batch_count; ++b) {
    const VkrDrawBatch *batch = &state->draw_batcher.opaque_batches.data[b];
    if (batch->command_count == 0) {
      continue;
    }

    VkrPipelineHandle pipeline = VKR_PIPELINE_HANDLE_INVALID;
    const char *shader_name = NULL;
    VkrRendererInstanceStateHandle *material_instances = NULL;
    bool8_t needs_alpha_test = false_v;
    if (batch->key.pipeline_id == state->shadow_system.shadow_pipeline_alpha.id) {
      pipeline = state->shadow_system.shadow_pipeline_alpha;
      shader_name = "shader.shadow";
      material_instances = state->material_instances_alpha;
      needs_alpha_test = true_v;
    } else {
      pipeline = state->shadow_system.shadow_pipeline_opaque;
      shader_name = "shader.shadow.opaque";
      material_instances = NULL;
      needs_alpha_test = false_v;
    }

    if (pipeline.id == 0 || !shader_name) {
      continue;
    }

    if (current_pipeline_id != pipeline.id) {
      if (!vkr_shader_system_use(&rf->shader_system, shader_name)) {
        continue;
      }

      VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                               &bind_err)) {
        continue;
      }
      current_pipeline_id = pipeline.id;
    }

    const VkrDrawCommand *cmd =
        &state->draw_batcher.opaque_commands.data[batch->first_command];
    VkrShadowSubmeshInfo submesh_info =
        vkr_view_shadow_get_submesh_info(rf, cmd);
    if (!submesh_info.valid) {
      continue;
    }

    VkrViewShadowDrawRange batch_range =
        vkr_view_shadow_resolve_draw_range_info(rf, &submesh_info, !needs_alpha_test);
    const VkrIndexBuffer *opaque_index_buffer = batch_range.index_buffer;
    bool8_t use_opaque_indices = batch_range.uses_opaque_indices;

    VkrMaterial *material = NULL;
    VkrRendererInstanceStateHandle instance_state = {0};
    if (needs_alpha_test) {
      material = vkr_material_system_get_by_handle(
          &rf->material_system, submesh_info.material);
      if (!material && rf->material_system.default_material.id != 0) {
        material = vkr_material_system_get_by_handle(
            &rf->material_system, rf->material_system.default_material);
      }

      uint32_t material_index =
          (material && material->id > 0) ? (material->id - 1) : 0;
      instance_state =
          vkr_view_shadow_get_material_instance(rf, state, pipeline,
                                                material_instances,
                                                material_index);
    } else {
      instance_state = (VkrRendererInstanceStateHandle){.id = VKR_INVALID_ID};
    }
    if (needs_alpha_test && instance_state.id == 0) {
      continue;
    }

    VkrTextureOpaqueHandle diffuse = NULL;
    float32_t alpha_cutoff = 0.0f;
    bool8_t foliage_dither = false_v;
    if (needs_alpha_test) {
      diffuse = vkr_view_shadow_get_diffuse_texture(rf, material);
      if (!diffuse) {
        continue;
      }
      alpha_cutoff = vkr_view_shadow_get_alpha_cutoff(
          rf, material, &state->shadow_system.config);
      if (alpha_cutoff > 0.0f &&
          state->shadow_system.config.foliage_alpha_dither) {
        foliage_dither = vkr_view_shadow_material_is_foliage(rf, material);
      }
    }

    if (needs_alpha_test) {
      vkr_shader_system_bind_instance(&rf->shader_system, instance_state.id);
    }
    vkr_shader_system_uniform_set(
        &rf->shader_system, "light_view_projection",
        &state->shadow_system.cascades[cascade_index].view_projection);
    if (needs_alpha_test) {
      float32_t alpha_cutoff_uniform =
          foliage_dither ? -alpha_cutoff : alpha_cutoff;
      vkr_shader_system_uniform_set(&rf->shader_system, "alpha_cutoff",
                                    &alpha_cutoff_uniform);
      vkr_shader_system_sampler_set(&rf->shader_system, "diffuse_texture",
                                    diffuse);
    }
    vkr_shader_system_apply_instance(&rf->shader_system);

    uint32_t draw_calls_issued = 0;
    if (mdi_available) {
      uint32_t command_index = 0;
      while (command_index < batch->command_count) {
        uint32_t remaining = vkr_indirect_draw_remaining(indirect_system);
        if (remaining == 0) {
          if (!mdi_warned) {
            log_warn("Shadow view: indirect draw buffer full, falling back");
            mdi_warned = true_v;
          }
          mdi_available = false_v;
          break;
        }

        uint32_t pending = batch->command_count - command_index;
        uint32_t chunk = remaining < pending ? remaining : pending;

        uint32_t base_draw = 0;
        VkrIndirectDrawCommand *draw_cmds = NULL;
        if (!vkr_indirect_draw_alloc(indirect_system, chunk, &base_draw,
                                     &draw_cmds)) {
          if (!mdi_warned) {
            log_warn("Shadow view: indirect draw alloc failed, falling back");
            mdi_warned = true_v;
          }
          mdi_available = false_v;
          break;
        }

        bool8_t commands_valid = true_v;
        for (uint32_t c = 0; c < chunk; ++c) {
          const VkrDrawCommand *range_cmd =
              &state->draw_batcher.opaque_commands.data[batch->first_command +
                                                        command_index + c];
          VkrShadowSubmeshInfo range_info =
              vkr_view_shadow_get_submesh_info(rf, range_cmd);
          if (!range_info.valid) {
            commands_valid = false_v;
            break;
          }

          VkrViewShadowDrawRange draw_range =
              vkr_view_shadow_resolve_draw_range_info(rf, &range_info,
                                                      use_opaque_indices);
          if (use_opaque_indices && !draw_range.uses_opaque_indices) {
            commands_valid = false_v;
            break;
          }

          draw_cmds[c] = (VkrIndirectDrawCommand){
              .index_count = draw_range.index_count,
              .instance_count = 1,
              .first_index = draw_range.first_index,
              .vertex_offset = draw_range.vertex_offset,
              .first_instance = batch->first_instance + command_index + c,
          };
        }

        if (!commands_valid) {
          if (!mdi_warned) {
            log_warn("Shadow view: invalid submesh in MDI batch, falling back");
            mdi_warned = true_v;
          }
          mdi_available = false_v;
          break;
        }

        vkr_indirect_draw_flush_range(indirect_system, base_draw, chunk);
        uint64_t offset_bytes =
            (uint64_t)base_draw * sizeof(VkrIndirectDrawCommand);
        if (use_opaque_indices && opaque_index_buffer) {
          vkr_geometry_system_render_indirect_with_index_buffer(
              rf, &rf->geometry_system, submesh_info.geometry, opaque_index_buffer,
              vkr_indirect_draw_get_current(indirect_system), offset_bytes,
              chunk, sizeof(VkrIndirectDrawCommand));
        } else {
          vkr_geometry_system_render_indirect(
              rf, &rf->geometry_system, submesh_info.geometry,
              vkr_indirect_draw_get_current(indirect_system), offset_bytes,
              chunk, sizeof(VkrIndirectDrawCommand));
        }
        draw_calls_issued += 1;
        command_index += chunk;
      }

      if (command_index < batch->command_count) {
        for (uint32_t c = command_index; c < batch->command_count; ++c) {
          const VkrDrawCommand *fallback_cmd =
              &state->draw_batcher.opaque_commands.data[batch->first_command +
                                                        c];
          VkrShadowSubmeshInfo fallback_info =
              vkr_view_shadow_get_submesh_info(rf, fallback_cmd);
          if (!fallback_info.valid) {
            continue;
          }
          VkrViewShadowDrawRange draw_range =
              vkr_view_shadow_resolve_draw_range_info(rf, &fallback_info,
                                                      use_opaque_indices);
          if (draw_range.index_buffer) {
            vkr_geometry_system_render_instanced_range_with_index_buffer(
                rf, &rf->geometry_system, fallback_info.geometry,
                draw_range.index_buffer, draw_range.index_count,
                draw_range.first_index, draw_range.vertex_offset, 1,
                batch->first_instance + c);
          } else {
            vkr_geometry_system_render_instanced_range(
                rf, &rf->geometry_system, fallback_info.geometry,
                draw_range.index_count, draw_range.first_index,
                draw_range.vertex_offset, 1, batch->first_instance + c);
          }
          draw_calls_issued += 1;
        }
      }
    } else if (!use_mdi) {
      VkrViewShadowDrawRange draw_range =
          vkr_view_shadow_resolve_draw_range_info(rf, &submesh_info, use_opaque_indices);
      if (draw_range.index_buffer) {
        vkr_geometry_system_render_instanced_range_with_index_buffer(
            rf, &rf->geometry_system, submesh_info.geometry,
            draw_range.index_buffer, draw_range.index_count,
            draw_range.first_index, draw_range.vertex_offset,
            batch->command_count, batch->first_instance);
      } else {
        vkr_geometry_system_render_instanced_range(
            rf, &rf->geometry_system, submesh_info.geometry,
            draw_range.index_count, draw_range.first_index,
            draw_range.vertex_offset, batch->command_count,
            batch->first_instance);
      }
      draw_calls_issued = 1;
    } else {
      for (uint32_t c = 0; c < batch->command_count; ++c) {
        const VkrDrawCommand *fallback_cmd =
            &state->draw_batcher.opaque_commands.data[batch->first_command + c];
        VkrShadowSubmeshInfo fallback_info =
            vkr_view_shadow_get_submesh_info(rf, fallback_cmd);
        if (!fallback_info.valid) {
          continue;
        }
        VkrViewShadowDrawRange draw_range =
            vkr_view_shadow_resolve_draw_range_info(rf, &fallback_info,
                                                    use_opaque_indices);
        if (draw_range.index_buffer) {
          vkr_geometry_system_render_instanced_range_with_index_buffer(
              rf, &rf->geometry_system, fallback_info.geometry,
              draw_range.index_buffer, draw_range.index_count,
              draw_range.first_index, draw_range.vertex_offset, 1,
              batch->first_instance + c);
        } else {
          vkr_geometry_system_render_instanced_range(
              rf, &rf->geometry_system, fallback_info.geometry,
              draw_range.index_count, draw_range.first_index,
              draw_range.vertex_offset, 1, batch->first_instance + c);
        }
        draw_calls_issued += 1;
      }
    }
    if (cascade_index < VKR_SHADOW_CASCADE_COUNT_MAX) {
      if (needs_alpha_test) {
        rf->frame_metrics.shadow.shadow_draw_calls_alpha[cascade_index] +=
            draw_calls_issued;
      } else {
        rf->frame_metrics.shadow.shadow_draw_calls_opaque[cascade_index] +=
            draw_calls_issued;
      }
      if (needs_alpha_test && draw_calls_issued > 0) {
        rf->frame_metrics.shadow.shadow_descriptor_binds_set1[cascade_index] +=
            1;
      }
    }
  }

  // Prevent depth-bias state leaking into subsequent passes.
  vkr_renderer_set_depth_bias(rf, 0.0f, 0.0f, 0.0f);
}

bool32_t vkr_view_shadow_register(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (!rf->view_system.initialized) {
    log_error("View system not initialized; cannot register shadow view");
    return false_v;
  }

  if (rf->shadow_layer.id != 0) {
    return true_v;
  }

  VkrShadowConfig cfg = VKR_SHADOW_CONFIG_DEFAULT;
  if (cfg.cascade_count == 0 ||
      cfg.cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX) {
    cfg.cascade_count = VKR_SHADOW_CASCADE_COUNT_MAX;
  }

  VkrLayerPassConfig passes[VKR_SHADOW_CASCADE_COUNT_MAX];
  for (uint32_t i = 0; i < cfg.cascade_count; ++i) {
    passes[i] = (VkrLayerPassConfig){
        .renderpass_name = string8_lit("Renderpass.CSM.Shadow"),
        .use_swapchain_color = false_v,
        .use_depth = false_v,
    };
  }

  VkrViewShadowState *state =
      vkr_allocator_alloc(&rf->allocator, sizeof(VkrViewShadowState),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!state) {
    log_error("Failed to allocate shadow view state");
    return false_v;
  }
  MemZero(state, sizeof(*state));

  uint32_t max_shadow_size = vkr_shadow_config_get_max_map_size(&cfg);
  VkrLayerConfig cfg_layer = {
      .name = string8_lit("Layer.Shadow"),
      .order = -20,
      .width = max_shadow_size,
      .height = max_shadow_size,
      .view = mat4_identity(),
      .projection = mat4_identity(),
      .pass_count = (uint8_t)cfg.cascade_count,
      .passes = passes,
      .callbacks = {.on_create = vkr_view_shadow_on_create,
                    .on_render = vkr_view_shadow_on_render,
                    .on_data_received = vkr_view_shadow_on_data_received,
                    .on_destroy = vkr_view_shadow_on_destroy},
      .user_data = state,
      .enabled = true_v,
  };

  VkrRendererError layer_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_view_system_register_layer(rf, &cfg_layer, &rf->shadow_layer,
                                      &layer_err)) {
    String8 err = vkr_renderer_get_error_string(layer_err);
    log_error("Failed to register shadow view: %s", string8_cstr(&err));
    vkr_allocator_free(&rf->allocator, state, sizeof(*state),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return false_v;
  }

  return true_v;
}

void vkr_view_shadow_unregister(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (rf->shadow_layer.id == 0) {
    return;
  }

  vkr_view_system_unregister_layer(rf, rf->shadow_layer);
  rf->shadow_layer = VKR_LAYER_HANDLE_INVALID;
}
