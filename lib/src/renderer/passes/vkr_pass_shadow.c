#include "renderer/passes/vkr_pass_shadow.h"

#include <ctype.h>
#include <stdint.h>

#include "containers/str.h"
#include "core/logger.h"
#include "renderer/passes/internal/vkr_pass_draw_utils.h"
#include "renderer/renderer_frontend.h"
#include "renderer/vkr_render_packet.h"

vkr_internal bool8_t vkr_pass_shadow_cstr_contains_i(const char *haystack,
                                                     const char *needle) {
  if (!haystack || !needle || !needle[0]) {
    return false_v;
  }

  uint64_t needle_len = string_length(needle);
  for (const char *h = haystack; *h; ++h) {
    const char *h_it = h;
    const char *n_it = needle;
    while (*h_it && *n_it &&
           tolower((unsigned char)*h_it) == tolower((unsigned char)*n_it)) {
      ++h_it;
      ++n_it;
    }
    if ((uint64_t)(n_it - needle) == needle_len) {
      return true_v;
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_pass_shadow_cstr_contains_any_i(
    const char *haystack, const char **keywords, uint32_t keyword_count) {
  if (!haystack || !keywords || keyword_count == 0) {
    return false_v;
  }

  for (uint32_t i = 0; i < keyword_count; ++i) {
    if (vkr_pass_shadow_cstr_contains_i(haystack, keywords[i])) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_pass_shadow_material_is_foliage(
    RendererFrontend *rf, const VkrMaterial *material) {
  if (!rf || !material) {
    return false_v;
  }

  vkr_internal const char *foliage_keywords[] = {"leaf", "foliage", "grass",
                                                 "fern", "pine",    "tree",
                                                 "bush", "plant",   "hedge"};
  const uint32_t keyword_count =
      (uint32_t)(sizeof(foliage_keywords) / sizeof(foliage_keywords[0]));

  if (material->name && vkr_pass_shadow_cstr_contains_any_i(
                            material->name, foliage_keywords, keyword_count)) {
    return true_v;
  }

  const VkrMaterialTexture *diffuse_tex =
      &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
  if (!diffuse_tex->enabled) {
    return false_v;
  }

  VkrTexture *diffuse = vkr_texture_system_get_by_handle(&rf->texture_system,
                                                         diffuse_tex->handle);
  if (!diffuse || !diffuse->file_path.path.str) {
    return false_v;
  }

  return vkr_pass_shadow_cstr_contains_any_i(
      (const char *)diffuse->file_path.path.str, foliage_keywords,
      keyword_count);
}

vkr_internal float32_t vkr_pass_shadow_get_alpha_cutoff(
    RendererFrontend *rf, const VkrMaterial *material,
    const VkrShadowConfig *config) {
  if (!material) {
    return 0.0f;
  }

  float32_t cutoff =
      vkr_material_system_material_alpha_cutoff(&rf->material_system, material);
  if (cutoff <= 0.0f) {
    return 0.0f;
  }
  if (!config || config->foliage_alpha_cutoff_bias <= 0.0f) {
    return cutoff;
  }

  if (vkr_pass_shadow_material_is_foliage(rf, material)) {
    cutoff += config->foliage_alpha_cutoff_bias;
    if (cutoff > 1.0f) {
      cutoff = 1.0f;
    }
  }
  return cutoff;
}

vkr_internal VkrTextureOpaqueHandle vkr_pass_shadow_get_diffuse_texture(
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

vkr_internal bool8_t vkr_pass_shadow_ensure_alpha_instance_capacity(
    RendererFrontend *rf, VkrShadowSystem *system, uint32_t required) {
  if (!rf || !system) {
    return false_v;
  }
  if (required <= system->alpha_instance_state_capacity) {
    return true_v;
  }

  uint32_t new_capacity = system->alpha_instance_state_capacity > 0
                              ? system->alpha_instance_state_capacity * 2u
                              : 64u;
  while (new_capacity < required) {
    new_capacity *= 2u;
  }

  VkrRendererInstanceStateHandle *new_states = vkr_allocator_alloc(
      &rf->allocator, sizeof(VkrRendererInstanceStateHandle) * new_capacity,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!new_states) {
    log_error("Shadow pass: failed to grow alpha instance state pool to %u",
              new_capacity);
    return false_v;
  }

  for (uint32_t i = 0; i < new_capacity; ++i) {
    new_states[i].id = VKR_INVALID_ID;
  }

  if (system->alpha_instance_states && system->alpha_instance_state_count > 0) {
    MemCopy(new_states, system->alpha_instance_states,
            sizeof(VkrRendererInstanceStateHandle) *
                (uint64_t)system->alpha_instance_state_count);
  }

  if (system->alpha_instance_states) {
    vkr_allocator_free(&rf->allocator, system->alpha_instance_states,
                       sizeof(VkrRendererInstanceStateHandle) *
                           (uint64_t)system->alpha_instance_state_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  system->alpha_instance_states = new_states;
  system->alpha_instance_state_capacity = new_capacity;
  return true_v;
}

vkr_internal bool8_t vkr_pass_shadow_bind_next_alpha_instance(
    RendererFrontend *rf, VkrShadowSystem *system) {
  if (!rf || !system || system->shadow_pipeline_alpha.id == 0) {
    return false_v;
  }

  uint32_t slot = system->alpha_instance_cursor;
  if (!vkr_pass_shadow_ensure_alpha_instance_capacity(rf, system, slot + 1u)) {
    return false_v;
  }

  if (slot >= system->alpha_instance_state_count) {
    VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
    VkrRendererInstanceStateHandle state_handle = {.id = VKR_INVALID_ID};
    if (!vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, system->shadow_pipeline_alpha,
            &state_handle, &acquire_err)) {
      String8 err = vkr_renderer_get_error_string(acquire_err);
      log_warn("Shadow pass: failed to acquire alpha instance state: %s",
               string8_cstr(&err));
      return false_v;
    }

    system->alpha_instance_states[slot] = state_handle;
    system->alpha_instance_state_count = slot + 1u;
  }

  system->alpha_instance_cursor++;
  return vkr_shader_system_bind_instance(
      &rf->shader_system, system->alpha_instance_states[slot].id);
}

/** Resolves material by handle with default fallback. Returns NULL if invalid. */
static VkrMaterial *vkr_pass_shadow_resolve_material(RendererFrontend *rf,
                                                     VkrMaterialHandle handle) {
  VkrMaterial *material =
      vkr_material_system_get_by_handle(&rf->material_system, handle);
  if (!material && rf->material_system.default_material.id != 0) {
    material = vkr_material_system_get_by_handle(
        &rf->material_system, rf->material_system.default_material);
  }
  return material;
}

/**
 * Binds pipeline/shader when needed, applies alpha instance state when use_alpha,
 * then issues the draw. Updates inout_current_pipeline and inout_current_alpha.
 */
static bool8_t vkr_pass_shadow_bind_and_draw(
    RendererFrontend *rf, const VkrShadowConfig *config,
    VkrShadowSystem *shadow, const Mat4 *light_view_proj,
    const VkrDrawItem *draw, uint32_t base_instance, bool8_t alpha_list,
    const VkrMaterial *material, VkrGeometryHandle geometry,
    const VkrPassDrawRange *range, VkrPipelineHandle *inout_current_pipeline,
    bool8_t *inout_current_alpha) {
  bool8_t use_alpha = alpha_list;
  VkrPipelineHandle pipeline = use_alpha ? shadow->shadow_pipeline_alpha
                                         : shadow->shadow_pipeline_opaque;
  if (pipeline.id == 0 && !use_alpha) {
    pipeline = shadow->shadow_pipeline_alpha;
    use_alpha = true_v;
  }
  if (pipeline.id == 0) {
    return false_v;
  }

  if (pipeline.id != inout_current_pipeline->id ||
      pipeline.generation != inout_current_pipeline->generation ||
      use_alpha != *inout_current_alpha) {
    VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                             &bind_err)) {
      return false_v;
    }
    const char *shader_name =
        use_alpha ? "shader.shadow" : "shader.shadow.opaque";
    if (!vkr_shader_system_use(&rf->shader_system, shader_name)) {
      return false_v;
    }
    vkr_shader_system_uniform_set(&rf->shader_system, "light_view_projection",
                                  light_view_proj);
    vkr_shader_system_apply_global(&rf->shader_system);
    *inout_current_pipeline = pipeline;
    *inout_current_alpha = use_alpha;
    if (!use_alpha) {
      vkr_shader_system_apply_instance(&rf->shader_system);
    }
  }

  if (use_alpha) {
    if (!vkr_pass_shadow_bind_next_alpha_instance(rf, shadow)) {
      return false_v;
    }
    float32_t alpha_cutoff =
        vkr_pass_shadow_get_alpha_cutoff(rf, material, config);
    VkrTextureOpaqueHandle diffuse =
        vkr_pass_shadow_get_diffuse_texture(rf, material);
    vkr_shader_system_uniform_set(&rf->shader_system, "alpha_cutoff",
                                  &alpha_cutoff);
    vkr_shader_system_sampler_set(&rf->shader_system, "diffuse_texture",
                                  diffuse);
    vkr_shader_system_apply_instance(&rf->shader_system);
  }

  vkr_geometry_system_render_instanced_range_with_index_buffer(
      rf, &rf->geometry_system, geometry, range->index_buffer,
      range->index_count, range->first_index, range->vertex_offset,
      draw->instance_count, base_instance + draw->first_instance);
  return true_v;
}

vkr_internal void
vkr_pass_shadow_draw_list(RendererFrontend *rf, const VkrShadowConfig *config,
                          const Mat4 *light_view_proj, uint32_t base_instance,
                          const VkrDrawItem *draws, uint32_t draw_count,
                          bool8_t alpha_list) {
  if (!rf || !draws || draw_count == 0 || !light_view_proj) {
    return;
  }

  VkrShadowSystem *shadow = &rf->shadow_system;
  VkrPipelineHandle current_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  bool8_t current_alpha = false_v;

  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->instance_count == 0) {
      continue;
    }

    if (vkr_pass_packet_handle_is_instance(draw->mesh)) {
      VkrMeshInstance *instance = NULL;
      VkrMeshAsset *asset = NULL;
      VkrMeshAssetSubmesh *submesh = NULL;
      VkrMeshSubmeshInstanceState *inst_state = NULL;
      if (!vkr_pass_packet_resolve_instance(rf, draw->mesh, draw->submesh_index,
                                            &instance, &asset, &submesh,
                                            &inst_state)) {
        continue;
      }
      (void)instance;
      (void)asset;
      (void)inst_state;

      VkrMaterialHandle material_handle =
          draw->material.id == 0 ? submesh->material : draw->material;
      VkrMaterial *material =
          vkr_pass_shadow_resolve_material(rf, material_handle);
      if (!material) {
        continue;
      }

      VkrPassDrawRange range = {0};
      if (!vkr_pass_packet_resolve_draw_range(rf, submesh, !alpha_list,
                                              &range)) {
        continue;
      }

      vkr_pass_shadow_bind_and_draw(rf, config, shadow, light_view_proj, draw,
                                    base_instance, alpha_list, material,
                                    submesh->geometry, &range,
                                    &current_pipeline, &current_alpha);
    } else {
      VkrMesh *mesh = NULL;
      VkrSubMesh *submesh = NULL;
      if (!vkr_pass_packet_resolve_mesh(rf, draw->mesh, draw->submesh_index,
                                        &mesh, &submesh)) {
        continue;
      }
      (void)mesh;

      VkrMaterialHandle material_handle =
          draw->material.id == 0 ? submesh->material : draw->material;
      VkrMaterial *material =
          vkr_pass_shadow_resolve_material(rf, material_handle);
      if (!material) {
        continue;
      }

      VkrPassDrawRange range = {0};
      if (!vkr_pass_packet_resolve_draw_range_mesh(rf, submesh, !alpha_list,
                                                   &range)) {
        continue;
      }

      vkr_pass_shadow_bind_and_draw(rf, config, shadow, light_view_proj, draw,
                                    base_instance, alpha_list, material,
                                    submesh->geometry, &range,
                                    &current_pipeline, &current_alpha);
    }
  }
}

vkr_internal void vkr_pass_shadow_execute(VkrRgPassContext *ctx,
                                          void *user_data) {
  if (!ctx || !ctx->renderer) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)ctx->renderer;
  if (rf->shadow_system.alpha_instance_cursor_frame_number !=
      rf->frame_number) {
    rf->shadow_system.alpha_instance_cursor_frame_number = rf->frame_number;
    rf->shadow_system.alpha_instance_cursor = 0;
  }

  const VkrRenderPacket *packet = vkr_rg_pass_get_packet(ctx);
  const VkrShadowPassPayload *payload = vkr_rg_pass_get_shadow_payload(ctx);
  if (!packet || !payload) {
    return;
  }

  uint32_t cascade_index = (uint32_t)(uintptr_t)user_data;
  if (cascade_index >= VKR_SHADOW_CASCADE_COUNT_MAX) {
    return;
  }
  if (cascade_index >= payload->cascade_count) {
    return;
  }

  uint32_t base_instance = 0;
  if (!vkr_pass_packet_upload_instances(
          rf, payload->instances, payload->instance_count, &base_instance)) {
    return;
  }

  const VkrShadowConfig *config =
      rf->shadow_system.initialized ? &rf->shadow_system.config : NULL;
  float32_t depth_bias_constant =
      config ? config->depth_bias_constant_factor : 0.0f;
  float32_t depth_bias_slope = config ? config->depth_bias_slope_factor : 0.0f;
  float32_t depth_bias_clamp = config ? config->depth_bias_clamp : 0.0f;

  if (payload->config_override) {
    depth_bias_constant = payload->config_override->depth_bias_constant;
    depth_bias_slope = payload->config_override->depth_bias_slope;
    depth_bias_clamp = payload->config_override->depth_bias_clamp;
  }

  vkr_renderer_set_depth_bias(rf, depth_bias_constant, depth_bias_clamp,
                              depth_bias_slope);

  const Mat4 *light_view_proj = &payload->light_view_proj[cascade_index];
  vkr_pass_shadow_draw_list(rf, config, light_view_proj, base_instance,
                            payload->opaque_draws, payload->opaque_draw_count,
                            false_v);
  vkr_pass_shadow_draw_list(rf, config, light_view_proj, base_instance,
                            payload->alpha_draws, payload->alpha_draw_count,
                            true_v);

  vkr_renderer_set_depth_bias(rf, 0.0f, 0.0f, 0.0f);
}

bool8_t vkr_pass_shadow_register(VkrRgExecutorRegistry *registry) {
  if (!registry) {
    return false_v;
  }

  VkrRgPassExecutor entry = {
      .name = string8_lit("pass.shadow.cascade"),
      .execute = vkr_pass_shadow_execute,
      .user_data = NULL,
  };

  return vkr_rg_executor_registry_register(registry, &entry);
}
