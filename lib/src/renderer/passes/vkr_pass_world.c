#include "renderer/passes/vkr_pass_world.h"

#include "containers/str.h"
#include "math/vec.h"
#include "renderer/passes/internal/vkr_pass_draw_utils.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_render_packet.h"

vkr_internal void vkr_pass_world_apply_shadow_globals(
    RendererFrontend *rf, const VkrFrameInfo *frame,
    const VkrShadowFrameData *data, bool8_t shadow_valid) {
  if (!rf) {
    return;
  }

  uint32_t shadow_enabled = 0;
  uint32_t cascade_count = 0;
  Vec4 shadow_map_inv_size[2] = {vec4_zero(), vec4_zero()};
  float32_t shadow_pcf_radius = 0.0f;
  Vec4 shadow_split_far[2] = {vec4_zero(), vec4_zero()};
  Vec4 shadow_world_units_per_texel[2] = {vec4_zero(), vec4_zero()};
  Vec4 shadow_light_space_origin_x[2] = {vec4_zero(), vec4_zero()};
  Vec4 shadow_light_space_origin_y[2] = {vec4_zero(), vec4_zero()};
  Vec4 shadow_uv_margin_scale[2] = {vec4_one(), vec4_one()};
  Vec4 shadow_uv_soft_margin_scale[2] = {vec4_one(), vec4_one()};
  Vec4 shadow_uv_kernel_margin_scale[2] = {vec4_one(), vec4_one()};
  float32_t shadow_bias = 0.0f;
  float32_t shadow_normal_bias = 0.0f;
  float32_t shadow_slope_bias = 0.0f;
  float32_t shadow_bias_texel_scale = 0.0f;
  float32_t shadow_slope_bias_texel_scale = 0.0f;
  float32_t shadow_distance_fade_range = 0.0f;
  float32_t shadow_cascade_blend_range = 0.0f;
  uint32_t shadow_debug_cascades = 0;
  uint32_t shadow_debug_mode = 0;
  Mat4 shadow_view_projection[VKR_SHADOW_CASCADE_COUNT_MAX];

  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    shadow_view_projection[i] = mat4_identity();
  }

  if (shadow_valid && data) {
    shadow_enabled = data->enabled ? 1u : 0u;
    cascade_count = data->cascade_count;
    for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
      uint32_t vec_index = i / 4;
      uint32_t lane = i % 4;
      float32_t inv = data->shadow_map_inv_size[i];
      float32_t split = data->split_far[i];
      float32_t wupt = data->world_units_per_texel[i];
      float32_t origin_x = data->light_space_origin[i].x;
      float32_t origin_y = data->light_space_origin[i].y;
      float32_t uv_margin_scale = data->shadow_uv_margin_scale[i];
      float32_t uv_soft_margin_scale = data->shadow_uv_soft_margin_scale[i];
      float32_t uv_kernel_margin_scale = data->shadow_uv_kernel_margin_scale[i];

      if (lane == 0) {
        shadow_map_inv_size[vec_index].x = inv;
        shadow_split_far[vec_index].x = split;
        shadow_world_units_per_texel[vec_index].x = wupt;
        shadow_light_space_origin_x[vec_index].x = origin_x;
        shadow_light_space_origin_y[vec_index].x = origin_y;
        shadow_uv_margin_scale[vec_index].x = uv_margin_scale;
        shadow_uv_soft_margin_scale[vec_index].x = uv_soft_margin_scale;
        shadow_uv_kernel_margin_scale[vec_index].x = uv_kernel_margin_scale;
      } else if (lane == 1) {
        shadow_map_inv_size[vec_index].y = inv;
        shadow_split_far[vec_index].y = split;
        shadow_world_units_per_texel[vec_index].y = wupt;
        shadow_light_space_origin_x[vec_index].y = origin_x;
        shadow_light_space_origin_y[vec_index].y = origin_y;
        shadow_uv_margin_scale[vec_index].y = uv_margin_scale;
        shadow_uv_soft_margin_scale[vec_index].y = uv_soft_margin_scale;
        shadow_uv_kernel_margin_scale[vec_index].y = uv_kernel_margin_scale;
      } else if (lane == 2) {
        shadow_map_inv_size[vec_index].z = inv;
        shadow_split_far[vec_index].z = split;
        shadow_world_units_per_texel[vec_index].z = wupt;
        shadow_light_space_origin_x[vec_index].z = origin_x;
        shadow_light_space_origin_y[vec_index].z = origin_y;
        shadow_uv_margin_scale[vec_index].z = uv_margin_scale;
        shadow_uv_soft_margin_scale[vec_index].z = uv_soft_margin_scale;
        shadow_uv_kernel_margin_scale[vec_index].z = uv_kernel_margin_scale;
      } else {
        shadow_map_inv_size[vec_index].w = inv;
        shadow_split_far[vec_index].w = split;
        shadow_world_units_per_texel[vec_index].w = wupt;
        shadow_light_space_origin_x[vec_index].w = origin_x;
        shadow_light_space_origin_y[vec_index].w = origin_y;
        shadow_uv_margin_scale[vec_index].w = uv_margin_scale;
        shadow_uv_soft_margin_scale[vec_index].w = uv_soft_margin_scale;
        shadow_uv_kernel_margin_scale[vec_index].w = uv_kernel_margin_scale;
      }
    }
    shadow_pcf_radius = data->pcf_radius;
    shadow_bias = data->shadow_bias;
    shadow_normal_bias = data->normal_bias;
    shadow_slope_bias = data->shadow_slope_bias;
    shadow_bias_texel_scale = data->shadow_bias_texel_scale;
    shadow_slope_bias_texel_scale = data->shadow_slope_bias_texel_scale;
    shadow_distance_fade_range = data->shadow_distance_fade_range;
    shadow_cascade_blend_range = data->cascade_blend_range;
    shadow_debug_cascades = data->debug_show_cascades ? 1u : 0u;
    for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
      shadow_view_projection[i] = data->view_projection[i];
    }
  }

  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_enabled",
                                &shadow_enabled);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_cascade_count",
                                &cascade_count);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_map_inv_size",
                                shadow_map_inv_size);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_pcf_radius",
                                &shadow_pcf_radius);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_split_far",
                                shadow_split_far);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_world_units_per_texel",
                                shadow_world_units_per_texel);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_light_space_origin_x",
                                shadow_light_space_origin_x);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_light_space_origin_y",
                                shadow_light_space_origin_y);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_uv_margin_scale",
                                shadow_uv_margin_scale);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_uv_soft_margin_scale",
                                shadow_uv_soft_margin_scale);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_uv_kernel_margin_scale",
                                shadow_uv_kernel_margin_scale);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_bias",
                                &shadow_bias);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_normal_bias",
                                &shadow_normal_bias);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_slope_bias",
                                &shadow_slope_bias);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_bias_texel_scale",
                                &shadow_bias_texel_scale);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_slope_bias_texel_scale",
                                &shadow_slope_bias_texel_scale);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_distance_fade_range",
                                &shadow_distance_fade_range);
  vkr_shader_system_uniform_set(&rf->shader_system,
                                "shadow_cascade_blend_range",
                                &shadow_cascade_blend_range);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_debug_cascades",
                                &shadow_debug_cascades);
  shadow_debug_mode = rf->shadow_debug_mode;
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_debug_mode",
                                &shadow_debug_mode);
  vkr_shader_system_uniform_set(&rf->shader_system, "shadow_view_projection",
                                shadow_view_projection);

  Vec4 screen_params = vec4_zero();
  uint32_t width = frame ? frame->viewport_width : 0;
  uint32_t height = frame ? frame->viewport_height : 0;
  if (width == 0 || height == 0) {
    width = frame ? frame->window_width : 0;
    height = frame ? frame->window_height : 0;
  }
  if (width > 0 && height > 0) {
    screen_params.x = 1.0f / (float32_t)width;
    screen_params.y = 1.0f / (float32_t)height;
    screen_params.z = (float32_t)width;
    screen_params.w = (float32_t)height;
  }
  vkr_shader_system_uniform_set(&rf->shader_system, "screen_params",
                                &screen_params);

  if (shadow_valid && data) {
    vkr_material_system_set_shadow_map(&rf->material_system, data->shadow_map,
                                       true_v);
  } else {
    vkr_material_system_set_shadow_map(&rf->material_system, NULL, false_v);
  }
}

/** Resolves material by handle with default fallback. Returns NULL if invalid.
 */
static VkrMaterial *vkr_pass_world_resolve_material(RendererFrontend *rf,
                                                    VkrMaterialHandle handle) {
  VkrMaterial *material =
      vkr_material_system_get_by_handle(&rf->material_system, handle);
  if (!material && rf->material_system.default_material.id != 0) {
    material = vkr_material_system_get_by_handle(
        &rf->material_system, rf->material_system.default_material);
  }
  return material;
}

static void vkr_pass_world_apply_probe_slots_for_draw(
    RendererFrontend *rf, const VkrWorldPassPayload *payload,
    const VkrDrawItem *draw, Vec3 probe_sample_position) {
  if (!rf || !payload || !draw || !rf->world_resources.initialized) {
    return;
  }

  VkrWorldIblProbeSlot world_slots[2] = {0};
  vkr_world_resources_select_probe_slots_for_position(
      rf, &rf->world_resources, rf->active_scene, probe_sample_position,
      world_slots);

  VkrMaterialIblProbeSlot material_slots[2] = {0};
  for (uint32_t i = 0; i < 2u; ++i) {
    material_slots[i] = (VkrMaterialIblProbeSlot){
        .irradiance_map = world_slots[i].irradiance_map,
        .prefilter_map = world_slots[i].prefilter_map,
        .center = world_slots[i].center,
        .extents = world_slots[i].extents,
        .blend_distance = world_slots[i].blend_distance,
        .weight = world_slots[i].weight,
        .intensity = world_slots[i].intensity,
        .diffuse_intensity = world_slots[i].diffuse_intensity,
        .specular_intensity = world_slots[i].specular_intensity,
        .box_projection_enabled = world_slots[i].box_projection_enabled,
    };
  }
  vkr_material_system_set_ibl_probe_slots(&rf->material_system, material_slots);
}

/**
 * @brief Returns the probe-selection sample position for a draw call.
 *
 * Probe selection must be driven by world-space draw location, not camera
 * position, otherwise local probe blending appears camera-anchored.
 */
static Vec3 vkr_pass_world_probe_sample_position_for_draw(
    const VkrWorldPassPayload *payload, const VkrDrawItem *draw,
    Vec3 fallback_position) {
  if (!payload || !payload->instances || !draw || draw->instance_count == 0) {
    return fallback_position;
  }

  if (draw->first_instance >= payload->instance_count) {
    return fallback_position;
  }

  const VkrInstanceDataGPU *instance =
      &payload->instances[draw->first_instance];
  return vec3_new(instance->model.elements[12], instance->model.elements[13],
                  instance->model.elements[14]);
}

/** Binds pipeline/globals when pipeline changed, binds instance, applies
 * material, draws. */
/** Upper bound on draws collapsed into one indirect call; bounded so the batch
 *  lives on the stack and never allocates in the draw loop. */
#define VKR_PASS_MDI_MAX_BATCH 64u

/**
 * @brief Consecutive draws that can share one indirect submission.
 *
 * Draws may only share a call while every piece of bound state is identical:
 * pipeline, material (which owns the descriptor writes), instance state,
 * geometry buffers, and the IBL probe slots applied per draw. Anything that
 * differs must break the batch, because an indirect call binds once and draws
 * many times -- batching across a state change would silently render with the
 * wrong state rather than fail.
 */
typedef struct VkrPassMdiBatch {
  VkrIndirectDrawCommand commands[VKR_PASS_MDI_MAX_BATCH];
  uint32_t count;

  VkrPipelineHandle pipeline;
  const VkrMaterial *material;
  VkrGeometryHandle geometry;
  const VkrIndexBuffer *index_buffer;
  uint32_t instance_state_id;
  Vec3 probe_position;
  const VkrDrawItem *first_draw;
  bool8_t open;
} VkrPassMdiBatch;

vkr_internal bool8_t vkr_pass_mdi_key_matches(
    const VkrPassMdiBatch *batch, VkrPipelineHandle pipeline,
    const VkrMaterial *material, VkrGeometryHandle geometry,
    const VkrIndexBuffer *index_buffer, uint32_t instance_state_id,
    Vec3 probe_position) {
  return batch->open && batch->count < VKR_PASS_MDI_MAX_BATCH &&
         batch->pipeline.id == pipeline.id &&
         batch->pipeline.generation == pipeline.generation &&
         batch->material == material && batch->geometry.id == geometry.id &&
         batch->geometry.generation == geometry.generation &&
         batch->index_buffer == index_buffer &&
         batch->instance_state_id == instance_state_id &&
         batch->probe_position.x == probe_position.x &&
         batch->probe_position.y == probe_position.y &&
         batch->probe_position.z == probe_position.z;
}

/**
 * @brief Binds the batch's shared state and submits its draws.
 *
 * Uses one vkCmdDrawIndexedIndirect when the batch holds more than one draw and
 * the device supports it; otherwise falls back to individual indexed draws,
 * which produce identical output. The fallback is not a rare path: a scene
 * whose draws never share binding state will take it for every batch.
 */
vkr_internal void vkr_pass_world_flush_mdi_batch(
    RendererFrontend *rf, const VkrFrameInfo *frame,
    const VkrWorldPassPayload *payload, VkrPassMdiBatch *batch,
    uint32_t base_instance, VkrPipelineDomain domain,
    const VkrGlobalMaterialState *globals,
    const VkrShadowFrameData *shadow_data, bool8_t shadow_valid,
    VkrPipelineHandle *inout_globals_pipeline) {
  if (!batch->open || batch->count == 0) {
    batch->open = false_v;
    batch->count = 0;
    return;
  }

  // Resolving the next draw selects its shader before this deferred batch is
  // flushed. Restore the shader recorded by the batch before writing globals,
  // uniforms, or samplers, otherwise alternating Phong/PBR draws populate one
  // manifest with the other's descriptor locations.
  VkrPipeline *batch_pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(&rf->pipeline_registry,
                                          batch->pipeline, &batch_pipeline) ||
      !batch_pipeline || !batch_pipeline->shader_name.str ||
      batch_pipeline->shader_name.length == 0 ||
      !vkr_shader_system_use(&rf->shader_system,
                             (const char *)batch_pipeline->shader_name.str)) {
    batch->open = false_v;
    batch->count = 0;
    return;
  }

  if (batch->pipeline.id != inout_globals_pipeline->id ||
      batch->pipeline.generation != inout_globals_pipeline->generation) {
    VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry,
                                             batch->pipeline, &bind_err)) {
      batch->open = false_v;
      batch->count = 0;
      return;
    }
    vkr_lighting_system_apply_uniforms(&rf->lighting_system);
    vkr_pass_world_apply_shadow_globals(rf, frame, shadow_data, shadow_valid);
    vkr_material_system_apply_global(&rf->material_system, globals, domain);
    *inout_globals_pipeline = batch->pipeline;
  }

  vkr_pass_world_apply_probe_slots_for_draw(rf, payload, batch->first_draw,
                                            batch->probe_position);
  vkr_shader_system_bind_instance(&rf->shader_system, batch->instance_state_id);
  vkr_material_system_apply_instance(&rf->material_system, batch->material,
                                     domain);

  const uint32_t submitted = batch->count;
  VkrPassIndirectBatch indirect = {
      .count = batch->count,
      .geometry = batch->geometry,
      .index_buffer = batch->index_buffer,
  };
  MemCopy(indirect.commands, batch->commands,
          sizeof(VkrIndirectDrawCommand) * (uint64_t)batch->count);
  vkr_pass_indirect_batch_submit(rf, &indirect);

  rf->frame_metrics.world.draws_issued += submitted;
  rf->frame_metrics.world.batches_created++;
  if (submitted > rf->frame_metrics.world.max_batch_size) {
    rf->frame_metrics.world.max_batch_size = submitted;
  }

  (void)base_instance;
  batch->open = false_v;
  batch->count = 0;
}

/**
 * @brief Adds one resolved draw to the pending indirect batch.
 *
 * Flushes first when the draw cannot share the open batch's bound state, so a
 * batch only ever contains draws that would have produced identical binds.
 */
static void vkr_pass_world_bind_globals_and_draw(
    RendererFrontend *rf, const VkrFrameInfo *frame,
    const VkrWorldPassPayload *payload, const VkrDrawItem *draw,
    uint32_t base_instance, VkrPipelineDomain domain,
    const VkrGlobalMaterialState *globals,
    const VkrShadowFrameData *shadow_data, bool8_t shadow_valid,
    VkrPipelineHandle *inout_globals_pipeline, VkrPipelineHandle pipeline,
    uint32_t instance_id, const VkrMaterial *material,
    VkrGeometryHandle geometry, const VkrPassDrawRange *range,
    Vec3 probe_position, VkrPassMdiBatch *batch) {
  if (!vkr_pass_mdi_key_matches(batch, pipeline, material, geometry,
                                range->index_buffer, instance_id,
                                probe_position)) {
    vkr_pass_world_flush_mdi_batch(rf, frame, payload, batch, base_instance,
                                   domain, globals, shadow_data, shadow_valid,
                                   inout_globals_pipeline);
    batch->pipeline = pipeline;
    batch->material = material;
    batch->geometry = geometry;
    batch->index_buffer = range->index_buffer;
    batch->instance_state_id = instance_id;
    batch->probe_position = probe_position;
    batch->first_draw = draw;
    batch->open = true_v;
  }

  batch->commands[batch->count++] = (VkrIndirectDrawCommand){
      .index_count = range->index_count,
      .instance_count = draw->instance_count,
      .first_index = range->first_index,
      .vertex_offset = range->vertex_offset,
      .first_instance = base_instance + draw->first_instance,
  };
}

vkr_internal void vkr_pass_world_draw_list(
    RendererFrontend *rf, const VkrFrameInfo *frame,
    const VkrWorldPassPayload *payload, const VkrDrawItem *draws,
    uint32_t draw_count, uint32_t base_instance, bool8_t allow_opaque,
    VkrPipelineDomain domain, const VkrGlobalMaterialState *globals,
    const VkrShadowFrameData *shadow_data, bool8_t shadow_valid) {
  if (!rf || !payload || !draws || draw_count == 0) {
    return;
  }

  VkrPipelineHandle globals_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  Vec3 fallback_probe_sample_position =
      globals ? globals->view_position : vec3_zero();
  VkrPassMdiBatch batch = {0};

  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->instance_count == 0) {
      continue;
    }

    Vec3 probe_sample_position = vkr_pass_world_probe_sample_position_for_draw(
        payload, draw, fallback_probe_sample_position);

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

      VkrMaterialHandle material_handle =
          draw->material.id == 0 ? submesh->material : draw->material;
      VkrMaterial *material =
          vkr_pass_world_resolve_material(rf, material_handle);
      if (!material) {
        continue;
      }

      VkrPipelineHandle pipeline = VKR_PIPELINE_HANDLE_INVALID;
      if (!vkr_pass_packet_resolve_pipeline(
              rf, domain, material, draw->pipeline_override, &pipeline)) {
        continue;
      }

      VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_mesh_manager_instance_refresh_pipeline(
              &rf->mesh_manager, draw->mesh, draw->submesh_index, pipeline,
              &refresh_err)) {
        continue;
      }

      VkrPassDrawRange range = {0};
      if (!vkr_pass_packet_resolve_draw_range(rf, submesh, allow_opaque,
                                              &range)) {
        continue;
      }

      vkr_pass_world_bind_globals_and_draw(
          rf, frame, payload, draw, base_instance, domain, globals, shadow_data,
          shadow_valid, &globals_pipeline, pipeline,
          inst_state->instance_state.id, material, submesh->geometry, &range,
          probe_sample_position, &batch);
    } else {
      uint32_t mesh_index = draw->mesh.id - 1u;
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
          vkr_pass_world_resolve_material(rf, material_handle);
      if (!material) {
        continue;
      }

      VkrPipelineHandle pipeline = VKR_PIPELINE_HANDLE_INVALID;
      if (!vkr_pass_packet_resolve_pipeline(
              rf, domain, material, draw->pipeline_override, &pipeline)) {
        continue;
      }

      VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_mesh_manager_refresh_pipeline(&rf->mesh_manager, mesh_index,
                                             draw->submesh_index, pipeline,
                                             &refresh_err)) {
        continue;
      }

      VkrPassDrawRange range = {0};
      if (!vkr_pass_packet_resolve_draw_range_mesh(rf, submesh, allow_opaque,
                                                   &range)) {
        continue;
      }

      vkr_pass_world_bind_globals_and_draw(
          rf, frame, payload, draw, base_instance, domain, globals, shadow_data,
          shadow_valid, &globals_pipeline, pipeline, submesh->instance_state.id,
          material, submesh->geometry, &range, probe_sample_position, &batch);
    }
  }

  // The final batch has no following draw to trigger its flush.
  vkr_pass_world_flush_mdi_batch(rf, frame, payload, &batch, base_instance,
                                 domain, globals, shadow_data, shadow_valid,
                                 &globals_pipeline);
}

vkr_internal void vkr_pass_world_execute(VkrRgPassContext *ctx,
                                         void *user_data) {
  (void)user_data;

  if (!ctx || !ctx->renderer) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)ctx->renderer;
  const VkrRenderPacket *packet = vkr_rg_pass_get_packet(ctx);
  const VkrWorldPassPayload *payload = vkr_rg_pass_get_world_payload(ctx);
  if (!packet || !payload) {
    return;
  }

  const VkrShadowPassPayload *shadow_payload =
      vkr_rg_pass_get_shadow_payload(ctx);
  VkrShadowFrameData shadow_data = {0};
  if (rf->shadow_system.initialized) {
    vkr_shadow_system_get_frame_data(&rf->shadow_system, ctx->image_index,
                                     &shadow_data);
  }

  VkrTextureOpaqueHandle shadow_map = shadow_data.shadow_map;
  if (ctx->graph) {
    VkrRgImageHandle shadow_handle =
        vkr_rg_find_image(ctx->graph, string8_lit("shadow_map"));
    if (vkr_rg_image_handle_valid(shadow_handle)) {
      VkrTextureOpaqueHandle graph_map =
          vkr_rg_get_image_texture(ctx->graph, shadow_handle, ctx->image_index);
      if (graph_map) {
        shadow_map = graph_map;
      }
    }
  }

  if (!shadow_payload) {
    MemZero(&shadow_data, sizeof(shadow_data));
  } else if (shadow_data.cascade_count > shadow_payload->cascade_count) {
    shadow_data.cascade_count = shadow_payload->cascade_count;
  }

  shadow_data.shadow_map = shadow_map;
  bool8_t shadow_valid = shadow_map != NULL;

  uint32_t base_instance = 0;
  if (!vkr_pass_packet_upload_instances(
          rf, payload->instances, payload->instance_count, &base_instance)) {
    return;
  }

  VkrGlobalMaterialState globals = {
      .projection = packet->globals.projection,
      .view = packet->globals.view,
      .ui_projection = mat4_identity(),
      .ui_view = mat4_identity(),
      .ambient_color = packet->globals.ambient_color,
      .view_position = packet->globals.view_position,
      .render_mode = (VkrRenderMode)packet->globals.render_mode,
  };

  vkr_pass_world_draw_list(rf, &packet->frame, payload, payload->opaque_draws,
                           payload->opaque_draw_count, base_instance, true_v,
                           VKR_PIPELINE_DOMAIN_WORLD, &globals, &shadow_data,
                           shadow_valid);
  vkr_pass_world_draw_list(rf, &packet->frame, payload,
                           payload->transparent_draws,
                           payload->transparent_draw_count, base_instance,
                           false_v, VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
                           &globals, &shadow_data, shadow_valid);

  {
    uint32_t opaque_count = payload->opaque_draw_count;
    uint32_t transparent_count = payload->transparent_draw_count;
    uint32_t total_draws = opaque_count + transparent_count;

    // draws_issued, batches_created, max_batch_size and indirect_draws_issued
    // are accumulated by vkr_pass_world_flush_mdi_batch as batches submit.
    rf->frame_metrics.world.draws_collected = total_draws;
    rf->frame_metrics.world.opaque_draws = opaque_count;
    rf->frame_metrics.world.transparent_draws = transparent_count;
    rf->frame_metrics.world.opaque_batches =
        rf->frame_metrics.world.batches_created;
    rf->frame_metrics.world.draws_merged =
        rf->frame_metrics.world.draws_issued >
                rf->frame_metrics.world.batches_created
            ? rf->frame_metrics.world.draws_issued -
                  rf->frame_metrics.world.batches_created
            : 0u;
    rf->frame_metrics.world.avg_batch_size =
        rf->frame_metrics.world.batches_created > 0
            ? (float32_t)rf->frame_metrics.world.draws_issued /
                  (float32_t)rf->frame_metrics.world.batches_created
            : 0.0f;
  }

  if (rf->world_resources.initialized) {
    vkr_world_resources_render_text(rf, &rf->world_resources);
  }

  if (rf->gizmo_system.initialized) {
    uint32_t viewport_height = packet->frame.viewport_height;
    if (viewport_height == 0) {
      viewport_height = packet->frame.window_height;
    }
    VkrCamera *camera = vkr_camera_registry_get_by_handle(&rf->camera_system,
                                                          rf->active_camera);
    if (camera) {
      vkr_gizmo_system_render(&rf->gizmo_system, rf, camera, viewport_height,
                              VKR_PIPELINE_HANDLE_INVALID);
    }
  }
}

bool8_t vkr_pass_world_register(VkrRgExecutorRegistry *registry) {
  if (!registry) {
    return false_v;
  }

  VkrRgPassExecutor entry = {
      .name = string8_lit("pass.world"),
      .execute = vkr_pass_world_execute,
      .user_data = NULL,
  };

  return vkr_rg_executor_registry_register(registry, &entry);
}
