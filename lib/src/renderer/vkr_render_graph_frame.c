#include "renderer/vkr_render_graph_frame.h"

void vkr_render_graph_prepare_frame(const VkrPreparedFrame *packet,
                                    const VkrBloomConfig *bloom_config,
                                    const VkrGtaoConfig *gtao_config,
                                    VkrRenderGraphFrameInfo *frame,
                                    VkrGtaoGpuParams *gtao_params) {
  frame->editor_enabled = packet->input.frame.editor_enabled;
  frame->viewport_width = packet->input.frame.viewport_width
                              ? packet->input.frame.viewport_width
                              : frame->target_width;
  frame->viewport_height = packet->input.frame.viewport_height
                               ? packet->input.frame.viewport_height
                               : frame->target_height;
  frame->exposure_automatic =
      packet->exposure.mode == VKR_EXPOSURE_MODE_AUTOMATIC;
  frame->picking_pending =
      packet->input.picking && packet->input.picking->pending;
  frame->transmission_pending =
      packet->input.world &&
      packet->input.world->transmission_gpu_candidate_count > 0u;
  frame->transmission_depth_diagnostic_enabled =
      frame->transmission_pending && packet->input.debug &&
      (packet->input.debug->transmission_depth_diagnostic_enabled ||
       vkr_renderer_capture_request_contains(
           packet->input.debug->capture,
           "transmission_visibility_ids_layer_4"));
  frame->timing_enabled = packet->input.debug &&
                          packet->input.debug->enable_timing &&
                          packet->input.debug->capture_pass_timestamps;
  frame->sdsm_enabled =
      packet->input.shadow && packet->input.shadow->sdsm_enabled;
  frame->shadow_cascade_count = packet->input.shadow
                                    ? Min(packet->input.shadow->cascade_count,
                                          VKR_SHADOW_CASCADE_COUNT_MAX)
                                    : 0u;
  frame->shadow_cascade_render_mask =
      packet->input.shadow ? packet->input.shadow->cascade_render_mask : 0u;

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
