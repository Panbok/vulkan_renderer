#include "renderer/vkr_render_graph_frame.h"

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
  frame->editor_enabled = packet->input.frame.editor_enabled;
  frame->scene_rendering = packet->scene_rendering;
  frame->editor_image_available = packet->editor_image_available;
  frame->editor_overlay_enabled =
      packet->scene_rendering && packet->input.editor &&
      packet->input.editor->overlay_draw_count > 0u;
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
  frame->local_shadow_map_size = packet->input.local_shadow
                                     ? packet->input.local_shadow->map_size
                                     : VKR_LOCAL_SHADOW_MAP_SIZE_DEFAULT;
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
  frame->hzb_reduce_pass_count = hzb_mip_count - 1u;
  frame->transmission_rough_mip_pass_count = Min(hzb_mip_count, 6u) - 1u;

  /* Bloom requires enough viewport extent for both reduction and upsampling. */
  frame->bloom_mip_count =
      packet->bloom.enabled
          ? vkr_bloom_mip_count(bloom_config, frame->viewport_width,
                                frame->viewport_height)
          : 0u;
  frame->bloom_enabled = frame->bloom_mip_count > 0u;
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
