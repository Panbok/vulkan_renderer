#include "vkr_render_graph_frame.h"
#include "vkr_ssgi.h"
#include "vkr_ssr.h"

vkr_internal uint32_t vkr_render_graph_draw_capacity(uint32_t count) {
  uint32_t capacity = 1u;
  while (capacity < count)
    capacity *= 2u;
  return capacity;
}

void vkr_render_graph_prepare_frame(const VkrPreparedFrame *packet,
                                    const VkrBloomConfig *bloom_config,
                                    const VkrGtaoConfig *gtao_config,
                                    VkrRenderGraphFrameInfo *frame,
                                    VkrGtaoGpuParams *gtao_params) {
  frame->fsr31_enabled = packet->fsr31_enabled;
  frame->scene_output_width = packet->scene_output_width;
  frame->scene_output_height = packet->scene_output_height;
  frame->render_scale = packet->render_scale;
  frame->editor_enabled = packet->input.frame.editor_enabled;
  frame->scene_rendering = packet->scene_rendering;
  frame->post_transform_cache_enabled = packet->post_transform_cache_enabled;
  const VkrWorldPassPayload *world = packet->input.world;
  frame->skinning_enabled = world && world->skinning_count;
  uint32_t skinning_vertices = 0;
  for (uint32_t i = 0; world && i < world->skinning_count; ++i) {
    skinning_vertices += world->skinning[i].vertex_count;
  }
  frame->skinning_vertex_capacity =
      vkr_render_graph_draw_capacity(skinning_vertices);
  frame->animation_preview_enabled = packet->input.animation_preview != NULL;
  const uint32_t material_features = !world ? 0u
                                     : world->opaque_material_features_valid
                                         ? world->opaque_material_features
                                         : VKR_WORLD_MATERIAL_FEATURE_ALL;
  frame->clearcoat_enabled =
      packet->scene_rendering &&
      (material_features & VKR_WORLD_MATERIAL_FEATURE_CLEARCOAT) != 0u;
  frame->sheen_enabled =
      packet->scene_rendering &&
      (material_features & VKR_WORLD_MATERIAL_FEATURE_SHEEN) != 0u;
  frame->anisotropy_enabled =
      packet->scene_rendering &&
      (material_features & VKR_WORLD_MATERIAL_FEATURE_ANISOTROPY) != 0u;
  frame->lighting_layers_enabled = frame->clearcoat_enabled ||
                                   frame->sheen_enabled ||
                                   frame->anisotropy_enabled;
  frame->editor_image_available = packet->editor_image_available;
  frame->editor_overlay_enabled = packet->scene_rendering &&
                                  packet->input.editor &&
                                  packet->input.editor->overlay_draw_count > 0u;
  frame->editor_selection_enabled =
      packet->scene_rendering && packet->input.editor &&
      !packet->input.editor->scene_rendering_stopped &&
      packet->input.editor->selection_draw_count > 0u;
  frame->editor_image_width = packet->editor_image_width;
  frame->editor_image_height = packet->editor_image_height;
  frame->viewport_width = packet->input.frame.viewport_width
                              ? packet->input.frame.viewport_width
                              : frame->target_width;
  frame->viewport_height = packet->input.frame.viewport_height
                               ? packet->input.frame.viewport_height
                               : frame->target_height;
  frame->exposure_automatic =
      packet->scene_rendering &&
      packet->exposure.mode == VKR_EXPOSURE_MODE_AUTOMATIC;
  frame->picking_pending = packet->scene_rendering && packet->input.picking &&
                           packet->input.picking->pending;
  frame->transmission_pending =
      packet->input.world &&
      packet->input.world->transmission_gpu_candidate_count > 0u;
  frame->gpu_draw_candidate_capacity = vkr_render_graph_draw_capacity(
      packet->input.world ? packet->input.world->gpu_candidate_count : 0u);
  frame->transmission_gpu_draw_candidate_capacity =
      vkr_render_graph_draw_capacity(
          packet->input.world
              ? packet->input.world->transmission_gpu_candidate_count
              : 0u);
  /* Parity-aware buckets retain the accepted 65536-draw bucket ceiling.
     A smaller scene must still fit when every candidate uses one bucket. */
  frame->gpu_draw_visible_capacity =
      VKR_WORLD_DRAW_STATE_BUCKET_COUNT *
      Min(frame->gpu_draw_candidate_capacity, 65536u);
  frame->transmission_gpu_draw_visible_capacity =
      VKR_WORLD_DRAW_STATE_BUCKET_COUNT *
      Min(frame->transmission_gpu_draw_candidate_capacity, 65536u);
  frame->transmission_depth_diagnostic_enabled =
      frame->transmission_pending && packet->input.debug &&
      (packet->input.debug->transmission_depth_diagnostic_enabled ||
       vkr_renderer_capture_request_contains(
           packet->input.debug->capture,
           "transmission_visibility_ids_layer_4"));
  frame->deferred_emissive_capture_enabled =
      packet->scene_rendering && packet->input.debug &&
      vkr_renderer_capture_request_contains(packet->input.debug->capture,
                                            "deferred_emissive");
  frame->resolve_barycentric_lod_capture_enabled =
      packet->scene_rendering && packet->input.debug &&
      vkr_renderer_capture_request_contains(packet->input.debug->capture,
                                            "resolve_barycentric_lod");
  frame->timing_enabled = packet->input.debug &&
                          packet->input.debug->enable_timing &&
                          packet->input.debug->capture_pass_timestamps;
  frame->sdsm_enabled =
      packet->input.shadow && packet->input.shadow->sdsm_enabled;
  frame->shadow_cascade_count = packet->input.shadow
                                    ? Min(packet->input.shadow->cascade_count,
                                          VKR_SHADOW_CASCADE_COUNT_MAX)
                                    : 0u;
  frame->local_shadow_view_count =
      packet->input.local_shadow ? packet->input.local_shadow->view_count : 0u;
  frame->local_shadow_transmission_view_count =
      packet->input.world &&
              packet->input.world->transmission_gpu_candidate_count > 0u
          ? frame->local_shadow_view_count
          : 0u;
  frame->local_shadow_render_mask =
      packet->input.local_shadow ? packet->input.local_shadow->render_mask : 0u;
  frame->local_shadow_atlas_clear_mask =
      packet->input.local_shadow ? packet->input.local_shadow->atlas_clear_mask
                                 : 0u;
  frame->local_shadow_map_size = packet->input.local_shadow
                                     ? packet->input.local_shadow->map_size
                                     : VKR_LOCAL_SHADOW_MAP_SIZE_DEFAULT;
  frame->local_shadow_transmission_map_size = Min(
      frame->local_shadow_map_size, VKR_LOCAL_SHADOW_TRANSMISSION_MAP_SIZE_MAX);
  frame->local_shadow_map_layer_count =
      packet->input.local_shadow ? packet->input.local_shadow->face_budget : 1u;
  frame->shadow_cascade_render_mask =
      packet->input.shadow ? packet->input.shadow->cascade_render_mask : 0u;

  /* Exact-grid history reuse is useful for stable unjittered samples. Avoid
     producing depth history while temporal jitter changes that grid. */
  frame->hzb_build_enabled =
      packet->scene_rendering &&
      MemCompare(&packet->temporal.jittered_projection,
                 &packet->input.globals.projection, sizeof(Mat4)) == 0;
  uint32_t hzb_mip_count = 1u;
  uint32_t hzb_extent = Max(frame->viewport_width, frame->viewport_height);
  while (hzb_extent > 1u) {
    hzb_extent >>= 1u;
    hzb_mip_count++;
  }
  frame->fog_enabled =
      packet->scene_rendering && packet->fog.color_density.w > 0.0f;
  frame->froxel_fog_enabled =
      packet->scene_rendering &&
      packet->froxel_fog.grid_dimensions_cell_pixels[0] > 0u;
  frame->atmosphere_enabled = packet->scene_rendering && packet->input.sky &&
                              packet->input.sky->atmosphere.enabled;
  frame->aerial_perspective_enabled =
      frame->atmosphere_enabled && packet->sky.aerial.w > 0.0f;
  frame->clouds_enabled =
      frame->aerial_perspective_enabled && packet->sky.clouds.noise.w > 0.0f;
  /* Froxel application owns aerial perspective when froxel fog is enabled. */
  frame->fog_apply_enabled =
      frame->fog_enabled ||
      (frame->aerial_perspective_enabled && !frame->froxel_fog_enabled);
  frame->ssgi_enabled = packet->scene_rendering && packet->ssgi_enabled;
  frame->ssgi_depth_mip_count =
      frame->ssgi_enabled ? vkr_ssgi_depth_mip_count(frame->viewport_width,
                                                     frame->viewport_height)
                          : 0u;
  frame->ssr_enabled = packet->scene_rendering && packet->ssr_enabled;
  frame->ssr_depth_mip_count =
      frame->ssr_enabled ? vkr_ssr_depth_mip_count(frame->viewport_width,
                                                   frame->viewport_height)
                         : 0u;
  frame->hzb_reduce_pass_count = hzb_mip_count - 1u;
  frame->transmission_rough_mip_pass_count = Min(hzb_mip_count, 6u) - 1u;

  /* Bloom requires enough viewport extent for both reduction and upsampling. */
  const uint32_t bloom_width =
      frame->fsr31_enabled ? frame->scene_output_width : frame->viewport_width;
  const uint32_t bloom_height = frame->fsr31_enabled
                                    ? frame->scene_output_height
                                    : frame->viewport_height;
  frame->bloom_mip_count =
      packet->bloom.enabled
          ? vkr_bloom_mip_count(bloom_config, bloom_width, bloom_height)
          : 0u;
  frame->bloom_enabled = frame->bloom_mip_count > 0u;
  frame->subsurface_enabled =
      packet->scene_rendering && packet->subsurface_enabled;
  frame->dof_enabled = packet->scene_rendering && packet->dof_enabled;
  frame->motion_blur_enabled =
      packet->scene_rendering && packet->motion_blur_enabled;
  frame->gtao_depth_mip_count =
      packet->gtao.enabled
          ? vkr_gtao_depth_mip_count(gtao_config, frame->viewport_width,
                                     frame->viewport_height)
          : 0u;
  frame->gtao_enabled = frame->gtao_depth_mip_count > 0u;
  *gtao_params =
      frame->gtao_enabled
          ? vkr_gtao_gpu_params(
                gtao_config, &packet->gtao, packet->input.globals.view,
                packet->temporal.jittered_projection, frame->viewport_width,
                frame->viewport_height, packet->input.frame.frame_index,
                packet->temporal.enabled)
          : (VkrGtaoGpuParams){0};
}

typedef struct VkrRgExecutorSpec {
  const char *name;
  VkrRgPassType type;
} VkrRgExecutorSpec;

vkr_global const VkrRgExecutorSpec s_rg_executors[VKR_RG_EXECUTOR_COUNT] = {
    [VKR_RG_EXECUTOR_SHADOW] = {"pass.shadow.cascade",
                                VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_LOCAL_SHADOW] = {"pass.local_shadow",
                                      VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_LOCAL_SHADOW_TRANSMISSION0] =
        {"pass.local_shadow.transmission0", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_LOCAL_SHADOW_TRANSMISSION1] =
        {"pass.local_shadow.transmission1", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_LOCAL_SHADOW_TRANSMISSION_OVERFLOW] =
        {"pass.local_shadow.transmission_overflow", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_PICKING] = {"pass.picking", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_PICKING_DEPTH_SEED] = {"pass.picking.depth_seed",
                                            VKR_RG_PASS_TYPE_TRANSFER},
    [VKR_RG_EXECUTOR_PICKING_RESOLVE] = {"pass.picking.resolve",
                                         VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_PICKING_READBACK] = {"pass.picking.readback",
                                          VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_IBL_BAKE] = {"pass.ibl_bake", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_GPU_DRAW_UPLOAD] = {"pass.gpu_draw_upload",
                                         VKR_RG_PASS_TYPE_TRANSFER},
    [VKR_RG_EXECUTOR_GPU_DRAW_CLASSIFY] = {"pass.gpu_draw_classify",
                                           VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_GPU_DRAW_PREFIX] = {"pass.gpu_draw_prefix",
                                         VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_GPU_DRAW_ENCODE] = {"pass.gpu_draw_encode",
                                         VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SKINNING] = {"pass.animation.skinning",
                                  VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TEMPORAL_TRANSFORM] = {"pass.temporal.transform_history",
                                            VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD] =
        {"pass.transmission.gpu_draw_upload", VKR_RG_PASS_TYPE_TRANSFER},
    [VKR_RG_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY] =
        {"pass.transmission.gpu_draw_classify", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX] =
        {"pass.transmission.gpu_draw_prefix", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE] =
        {"pass.transmission.gpu_draw_encode", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TRANSMISSION_DEPTH_SEED] = {"pass.transmission.depth_seed",
                                                 VKR_RG_PASS_TYPE_TRANSFER},
    [VKR_RG_EXECUTOR_VBUFFER_OPAQUE] = {"pass.vbuffer.opaque",
                                        VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_VBUFFER_TRANSMISSION] = {"pass.vbuffer.transmission",
                                              VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_GBUFFER_RESOLVE] = {"pass.gbuffer.resolve",
                                         VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_GTAO_DEPTH_PREFILTER] = {"pass.gtao.depth_prefilter",
                                              VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_GTAO_DEPTH_MIP] = {"pass.gtao.depth_mip",
                                        VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_GTAO_EVALUATE] = {"pass.gtao.evaluate",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_GTAO_DENOISE] = {"pass.gtao.denoise",
                                      VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_LOCAL_SHADOW_MASK] = {"pass.local_shadow.mask",
                                           VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_LIGHTING_DEFERRED] = {"pass.lighting.deferred",
                                           VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_LIGHTING_DEFERRED_LAYERED] =
        {"pass.lighting.deferred.layered", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSGI_DEPTH_BASE] = {"pass.ssgi.depth_base",
                                         VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSGI_DEPTH_MIP] = {"pass.ssgi.depth_mip",
                                        VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSGI_TRACE] = {"pass.ssgi.trace",
                                    VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSGI_TEMPORAL] = {"pass.ssgi.temporal",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSGI_COMPOSITE] = {"pass.ssgi.composite",
                                        VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSR_DEPTH_BASE] = {"pass.ssr.depth_base",
                                        VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSR_DEPTH_MIP] = {"pass.ssr.depth_mip",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSR_TRACE] = {"pass.ssr.trace", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSR_TEMPORAL] = {"pass.ssr.temporal",
                                      VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SSR_COMPOSITE] = {"pass.ssr.composite",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_FOG_APPLY] = {"pass.fog.apply", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_FROXEL_INJECT] = {"pass.froxel.inject",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_FROXEL_INTEGRATE] = {"pass.froxel.integrate",
                                          VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_FROXEL_APPLY] = {"pass.froxel.apply",
                                      VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SKY_VIEW_LUT] = {"pass.sky.view_lut",
                                      VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_AERIAL_PERSPECTIVE] = {"pass.sky.aerial_perspective",
                                            VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_CLOUD_SHADOW] = {"pass.clouds.shadow",
                                      VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_CLOUD_TRACE] = {"pass.clouds.trace",
                                     VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TEMPORAL_RESOLVE] = {"pass.temporal.resolve",
                                          VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_METALFX_STAGE] = {"pass.metalfx.stage",
                                       VKR_RG_PASS_TYPE_TRANSFER},
    [VKR_RG_EXECUTOR_METALFX_TEMPORAL] = {"pass.metalfx.temporal",
                                          VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_METALFX_STABILIZE] = {"pass.metalfx.stabilize",
                                           VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_EXPOSURE_HISTOGRAM] = {"pass.exposure.histogram",
                                            VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_EXPOSURE_RESOLVE] = {"pass.exposure.resolve",
                                          VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SUBSURFACE_GATHER] = {"pass.subsurface.gather",
                                           VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_MOTION_BLUR_TILE_MAX] = {"pass.motion_blur.tile_max",
                                              VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_MOTION_BLUR_NEIGHBOR_MAX] =
        {"pass.motion_blur.neighbor_max", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_MOTION_BLUR_RECONSTRUCT] = {"pass.motion_blur.reconstruct",
                                                 VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_DOF_COC] = {"pass.dof.coc", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_DOF_DILATE_HORIZONTAL] = {"pass.dof.dilate_horizontal",
                                               VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_DOF_DILATE_VERTICAL] = {"pass.dof.dilate_vertical",
                                             VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_DOF_PREFILTER] = {"pass.dof.prefilter",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_DOF_GATHER] = {"pass.dof.gather",
                                    VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_DOF_COMPOSITE] = {"pass.dof.composite",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_BLOOM_PREFILTER] = {"pass.bloom.prefilter",
                                         VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_BLOOM_DOWNSAMPLE] = {"pass.bloom.downsample",
                                          VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_BLOOM_UPSAMPLE] = {"pass.bloom.upsample",
                                        VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_BLOOM_COMBINE] = {"pass.bloom.combine",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TRANSMISSION_DOWNSAMPLE] = {"pass.transmission.downsample",
                                                 VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TRANSMISSION_SHADE] = {"pass.transmission.shade",
                                            VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TRANSMISSION_COVERAGE] = {"pass.transmission.coverage",
                                               VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_TRANSMISSION_COMPACT] = {"pass.transmission.compact",
                                              VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_SDSM_REDUCE] = {"pass.sdsm.reduce",
                                     VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_HZB_BUILD] = {"pass.hzb.build", VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN] =
        {"pass.copy.pre_transmission.fullscreen", VKR_RG_PASS_TYPE_TRANSFER},
    [VKR_RG_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR] =
        {"pass.copy.pre_transmission.editor", VKR_RG_PASS_TYPE_TRANSFER},
    [VKR_RG_EXECUTOR_WORLD_BLEND] = {"pass.world.blend",
                                     VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_TONEMAP] = {"pass.tonemap", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_TONEMAP_PREPARE] = {"pass.tonemap.prepare",
                                         VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_EDITOR] = {"pass.editor", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_CLEAR] = {"pass.clear", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_EDITOR_OVERLAY] = {"pass.editor.overlay",
                                        VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_EDITOR_OVERLAY_PICKING] = {"pass.editor.overlay.picking",
                                                VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_EDITOR_SELECTION_MASK] = {"pass.editor.selection.mask",
                                               VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_EDITOR_SELECTION_OUTLINE] =
        {"pass.editor.selection.outline", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_ANIMATION_PREVIEW] = {"pass.animation.preview",
                                           VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_UI] = {"pass.ui", VKR_RG_PASS_TYPE_GRAPHICS},
    [VKR_RG_EXECUTOR_FSR31_PREPARE] = {"pass.fsr31.prepare",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_FSR31_UPSCALE] = {"pass.fsr31.upscale",
                                       VKR_RG_PASS_TYPE_COMPUTE},
    [VKR_RG_EXECUTOR_FSR31_STABILIZE] = {"pass.fsr31.stabilize",
                                         VKR_RG_PASS_TYPE_COMPUTE},
};

bool8_t vkr_render_graph_register_executors(VkrRgExecutorRegistry *registry) {
  for (uint32_t kind = 0; kind < VKR_RG_EXECUTOR_COUNT; ++kind) {
    const VkrRgExecutorSpec *spec = &s_rg_executors[kind];
    assert_log(spec->name != NULL, "Executor catalog entry is missing");
    const VkrRgPassExecutor entry = {
        .name = string8_create_from_cstr((const uint8_t *)spec->name,
                                         string_length(spec->name)),
        .id = kind + 1u,
        .type = spec->type,
    };
    if (!vkr_rg_executor_registry_register(registry, &entry)) {
      return false_v;
    }
  }
  return true_v;
}

const char *vkr_render_graph_executor_name(VkrRgExecutorKind kind) {
  return kind < VKR_RG_EXECUTOR_COUNT ? s_rg_executors[kind].name : "unknown";
}

VkrRgPassType vkr_render_graph_executor_type(VkrRgExecutorKind kind) {
  assert_log(kind < VKR_RG_EXECUTOR_COUNT, "Executor kind is out of range");
  return s_rg_executors[kind].type;
}
