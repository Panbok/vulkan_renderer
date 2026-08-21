#include "renderer/vulkan/vkr_vulkan_internal.h"

vkr_internal void vkr_vk_cmd_ibl_image_barrier(
    VkCommandBuffer command, VkImage image, uint32_t mip_level,
    uint32_t layer_count, VkPipelineStageFlags2 src_stage,
    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
    VkAccessFlags2 dst_access, VkImageLayout old_layout,
    VkImageLayout new_layout) {
  const VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = mip_level,
              .levelCount = 1u,
              .baseArrayLayer = 0u,
              .layerCount = layer_count,
          },
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1u,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
}

vkr_internal bool8_t vkr_vk_record_ibl_dispatch(
    VkrVulkanRenderer *renderer, VkCommandBuffer command,
    VkrVulkanIblPipeline pipeline, const VkrVulkanPublishedTexture *source,
    const VkrVulkanPublishedTexture *target, uint32_t target_mip,
    uint32_t sample_count, float32_t roughness) {
  if (!source || !target || target_mip >= target->storage_slot_count ||
      source->sampler_record_index >= renderer->config.sampler_capacity)
    return false_v;
  const VkrVulkanPublishedSampler *sampler =
      &renderer->published_samplers[source->sampler_record_index];
  if (!sampler->live)
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t root_address = 0u;
  VkrVulkanIblRoot *root = vkr_vk_frame_upload_allocate(
      slot, sizeof(*root), _Alignof(VkrVulkanIblRoot), &root_address, NULL);
  if (!root)
    return false_v;
  *root = (VkrVulkanIblRoot){
      .source_texture = source->sampled_slot.index,
      .source_sampler = sampler->slot.index,
      .target_texture = target->storage_slots[target_mip].index,
      .target_size = Max(1u, target->image.width >> target_mip),
      .sample_count = sample_count,
      .source_face_size = source->image.width,
      .source_mip_count = source->image.mip_levels,
      .roughness = roughness,
  };
  const VkrVulkanPushConstants push = {.root = root_address};
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->ibl_pipelines[pipeline]);
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  vkCmdDispatch(command, (root->target_size + 7u) / 8u,
                (root->target_size + 7u) / 8u, target->image.array_layers);
  return true_v;
}

vkr_internal void
vkr_vk_record_ibl_source_mips(VkCommandBuffer command,
                              VkrVulkanPublishedTexture *source) {
  if (source->image.mip_levels == 1u) {
    vkr_vk_cmd_ibl_image_barrier(
        command, source->image.handle, 0u, source->image.array_layers,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL);
    return;
  }
  for (uint32_t mip = 1u; mip < source->image.mip_levels; ++mip) {
    vkr_vk_cmd_ibl_image_barrier(
        command, source->image.handle, mip - 1u, source->image.array_layers,
        mip == 1u ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                  : VK_PIPELINE_STAGE_2_BLIT_BIT,
        mip == 1u ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                  : VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        mip == 1u ? VK_IMAGE_LAYOUT_GENERAL
                  : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkr_vk_cmd_ibl_image_barrier(
        command, source->image.handle, mip, source->image.array_layers,
        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const int32_t source_width =
        (int32_t)Max(1u, source->image.width >> (mip - 1u));
    const int32_t source_height =
        (int32_t)Max(1u, source->image.height >> (mip - 1u));
    const int32_t target_width = (int32_t)Max(1u, source->image.width >> mip);
    const int32_t target_height = (int32_t)Max(1u, source->image.height >> mip);
    const VkImageBlit2 region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = mip - 1u,
                .baseArrayLayer = 0u,
                .layerCount = source->image.array_layers,
            },
        .srcOffsets = {{0, 0, 0}, {source_width, source_height, 1}},
        .dstSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = mip,
                .baseArrayLayer = 0u,
                .layerCount = source->image.array_layers,
            },
        .dstOffsets = {{0, 0, 0}, {target_width, target_height, 1}},
    };
    const VkBlitImageInfo2 blit = {
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = source->image.handle,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = source->image.handle,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1u,
        .pRegions = &region,
        .filter = VK_FILTER_LINEAR,
    };
    vkCmdBlitImage2(command, &blit);
  }
  for (uint32_t mip = 0u; mip < source->image.mip_levels; ++mip) {
    const bool8_t last = mip == source->image.mip_levels - 1u;
    vkr_vk_cmd_ibl_image_barrier(
        command, source->image.handle, mip, source->image.array_layers,
        VK_PIPELINE_STAGE_2_BLIT_BIT,
        last ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        last ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
             : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL);
  }
}

bool8_t vkr_vk_record_ibl_bakes(VkrVulkanRenderer *renderer,
                                VkCommandBuffer command) {
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    VkrVulkanPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    job->recorded = false_v;
    VkrVulkanPublishedTexture *source =
        vkr_vk_texture_publication(renderer, job->source);
    VkrVulkanPublishedTexture *irradiance =
        vkr_vk_texture_publication(renderer, job->irradiance);
    VkrVulkanPublishedTexture *prefilter =
        vkr_vk_texture_publication(renderer, job->prefilter);
    VkrVulkanPublishedTexture *equirect =
        job->convert_equirect
            ? vkr_vk_texture_publication(renderer, job->equirect)
            : NULL;
    if (!source || !irradiance || !prefilter ||
        (job->convert_equirect && !equirect))
      return false_v;
    const VkrVulkanPublishedTexture *input =
        job->convert_equirect ? equirect : source;
    if (input->initialization_pending) {
      continue;
    }
    if (job->convert_equirect) {
      if (!vkr_vk_record_ibl_dispatch(renderer, command,
                                      VKR_VULKAN_IBL_PIPELINE_EQUIRECT,
                                      equirect, source, 0u, 1u, 0.0f))
        return false_v;
      vkr_vk_record_ibl_source_mips(command, source);
    }
    if (!vkr_vk_record_ibl_dispatch(renderer, command,
                                    VKR_VULKAN_IBL_PIPELINE_IRRADIANCE, source,
                                    irradiance, 0u, 128u, 0.0f))
      return false_v;
    vkr_vk_cmd_ibl_image_barrier(
        command, irradiance->image.handle, 0u, irradiance->image.array_layers,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t mip = 0u; mip < prefilter->image.mip_levels; ++mip) {
      const float32_t roughness =
          prefilter->image.mip_levels > 1u
              ? (float32_t)mip / (float32_t)(prefilter->image.mip_levels - 1u)
              : 0.0f;
      if (!vkr_vk_record_ibl_dispatch(renderer, command,
                                      VKR_VULKAN_IBL_PIPELINE_PREFILTER, source,
                                      prefilter, mip, 256u, roughness))
        return false_v;
      vkr_vk_cmd_ibl_image_barrier(
          command, prefilter->image.handle, mip, prefilter->image.array_layers,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_GENERAL);
    }
    job->recorded = true_v;
  }
  return true_v;
}
