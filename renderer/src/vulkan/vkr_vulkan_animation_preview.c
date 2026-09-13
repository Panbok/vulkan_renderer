#include "vulkan/vkr_vulkan_internal.h"

typedef struct VkrVulkanPreviewDrawUpload {
  VkrVulkanAnimationPreviewRoot root;
  VkBuffer indices;
  uint64_t index_offset;
  uint64_t index_size;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
} VkrVulkanPreviewDrawUpload;

_Static_assert(sizeof(VkrVulkanPreviewDrawUpload) <= 256u,
               "Preview draws must fit the graph upload reservation");

bool8_t vkr_vk_prepare_animation_preview(VkrVulkanRenderer *renderer,
                                         VkrVulkanPreparedPreview *out) {
  const VkrAnimationPreviewInput *preview =
      renderer->graph->packet->input.animation_preview;
  if (!preview || !preview->draw_count) {
    return true_v;
  }
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t address = 0u;
  VkrVulkanPreviewDrawUpload *draws = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)preview->draw_count * sizeof(*draws),
      _Alignof(VkrVulkanPreviewDrawUpload), &address, NULL);
  if (!draws) {
    return false_v;
  }
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < preview->draw_count; ++i) {
    const VkrAnimationPreviewDraw *source = &preview->draws[i];
    VkrVulkanPublishedGeometry *geometry =
        vkr_vk_resolve_geometry(renderer, source->geometry);
    if (!geometry || source->submesh_index >= geometry->submesh_count) {
      return false_v;
    }
    if (geometry->pending_initialization_count) {
      continue;
    }
    const VkrVulkanSubmeshRange *range =
        &geometry->submeshes[source->submesh_index];
    const uint64_t index_offset =
        (uint64_t)geometry->gpu_row.first_index * sizeof(uint32_t);
    draws[count++] = (VkrVulkanPreviewDrawUpload){
        .root = {.model = source->model,
                 .view_projection = preview->view_projection,
                 .vertices = geometry->gpu_row.vertex_address,
                 .decode = geometry->gpu_row.decode_address,
                 .deformation_address =
                     source->skinning_index
                         ? slot->skinning_addresses[source->skinning_index - 1u]
                         : 0u,
                 .tint = {0.72f, 0.76f, 0.82f, 1.0f},
                 .first_vertex = geometry->gpu_row.first_vertex,
                 .decode_index = range->decode_index},
        .indices = geometry->indices.handle,
        .index_offset = index_offset,
        .index_size = geometry->indices.size - index_offset,
        .first_index = range->first_index,
        .index_count = range->index_count,
        .vertex_offset = range->vertex_offset,
    };
    geometry->last_use_submit_value = renderer->submit_value + 1u;
  }
  *out = (VkrVulkanPreparedPreview){
      .draws = draws, .roots_address = address, .count = count};
  return true_v;
}

void vkr_vk_record_animation_preview(VkrVulkanRenderer *renderer,
                                     VkCommandBuffer command,
                                     const VkrVulkanPreparedPreview *preview) {
  if (!preview->count) {
    return;
  }
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_GRAPHICS,
      renderer->packet_pipelines[VKR_VULKAN_PACKET_PIPELINE_ANIMATION_PREVIEW]);
  const VkrVulkanPreviewDrawUpload *draws = preview->draws;
  for (uint32_t i = 0u; i < preview->count; ++i) {
    const VkrVulkanPreviewDrawUpload *draw = &draws[i];
    const VkrVulkanPushConstants push = {.root = preview->roots_address +
                                                 i * sizeof(*draw)};
    vkCmdBindIndexBuffer2(command, draw->indices, draw->index_offset,
                          draw->index_size, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
    vkCmdDrawIndexed(command, draw->index_count, 1u, draw->first_index,
                     draw->vertex_offset, 0u);
  }
  renderer->frame_slots[renderer->active_frame_slot].indexed_draw_count +=
      preview->count;
}
