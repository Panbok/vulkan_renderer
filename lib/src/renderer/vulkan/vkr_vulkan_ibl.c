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

/**
 * Establishes the black sentinel once per renderer. Every slot starts zeroed,
 * so a slot referenced before its projection lands reads black rather than
 * stale bytes.
 */
vkr_internal void vkr_vk_record_sh_clear(VkrVulkanRenderer *renderer,
                                         VkCommandBuffer command) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (renderer->sh_coefficients_cleared ||
      slot->sh_coefficients_clear_recorded) {
    return;
  }
  vkCmdFillBuffer(command, renderer->sh_coefficients.handle, 0u,
                  (VkDeviceSize)VKR_SH_BUFFER_BYTES, 0u);
  const VkBufferMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = renderer->sh_coefficients.handle,
      .offset = 0u,
      .size = (VkDeviceSize)VKR_SH_BUFFER_BYTES,
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
  slot->sh_coefficients_clear_recorded = true_v;
}

/**
 * Projects one source cubemap into the reserved slot. Returns false without
 * consuming the slot when the source cannot supply the selected mip, so the
 * caller can abandon the reservation and keep its prior publication.
 */
vkr_internal bool8_t vkr_vk_record_ibl_sh_projection(
    VkrVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrVulkanPublishedTexture *source, uint32_t slot,
    float32_t deringing) {
  uint32_t projection_mip = 0u;
  uint32_t projection_extent = 0u;
  if (!vkr_ibl_sh_projection_mip(source->image.width, source->image.mip_levels,
                                 &projection_mip, &projection_extent) ||
      source->sampler_record_index >= renderer->config.sampler_capacity ||
      slot >= VKR_SH_SLOT_CAPACITY) {
    return false_v;
  }
  const VkrVulkanPublishedSampler *sampler =
      &renderer->published_samplers[source->sampler_record_index];
  if (!sampler->live) {
    return false_v;
  }

  VkrVulkanFrameSlot *frame_slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t root_address = 0u;
  VkrVulkanIblShRoot *root = vkr_vk_frame_upload_allocate(
      frame_slot, sizeof(*root), _Alignof(VkrVulkanIblShRoot), &root_address,
      NULL);
  if (!root) {
    return false_v;
  }
  *root = (VkrVulkanIblShRoot){
      .destination = renderer->sh_coefficients.address +
                     (uint64_t)slot * VKR_SH_SLOT_BYTES,
      .source_texture = source->ibl_sh_texel_slot.index,
      .source_sampler = sampler->slot.index,
      .source_face_size = projection_extent,
      .source_mip = projection_mip,
      // The CPU owns pow(sinc_pi(l/3), sh_deringing) so the kernel carries no
      // pow() and both sides cannot drift.
      .window_band_0 = vkr_ibl_sh_window_factor(0u, deringing),
      .window_band_1 = vkr_ibl_sh_window_factor(1u, deringing),
      .window_band_2 = vkr_ibl_sh_window_factor(2u, deringing),
  };

  const VkrVulkanPushConstants push = {.root = root_address};
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->ibl_pipelines[VKR_VULKAN_IBL_PIPELINE_SH]);
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  // One workgroup per destination slot; the kernel's fixed reduction is what
  // makes the result deterministic, so this must stay a single group.
  vkCmdDispatch(command, 1u, 1u, 1u);
  return true_v;
}

/**
 * Carries projection writes to every later lighting read. A frame may consume
 * the slot it just baked, so this barrier must precede lighting in the same
 * command stream; the pool separately proves GPU completion before reuse.
 */
vkr_internal void vkr_vk_record_sh_visibility(VkrVulkanRenderer *renderer,
                                              VkCommandBuffer command) {
  const VkBufferMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = renderer->sh_coefficients.handle,
      .offset = 0u,
      .size = (VkDeviceSize)VKR_SH_BUFFER_BYTES,
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
}

bool8_t vkr_vk_record_ibl_bakes(VkrVulkanRenderer *renderer,
                                VkCommandBuffer command) {
  vkr_vk_record_sh_clear(renderer, command);
  bool8_t projected_any = false_v;
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    VkrVulkanPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    job->recorded = false_v;
    VkrVulkanPublishedTexture *source =
        vkr_vk_texture_publication(renderer, job->source);
    VkrVulkanPublishedTexture *prefilter =
        vkr_vk_texture_publication(renderer, job->prefilter);
    VkrVulkanPublishedTexture *equirect =
        job->convert_equirect
            ? vkr_vk_texture_publication(renderer, job->equirect)
            : NULL;
    if (!source || !prefilter || (job->convert_equirect && !equirect))
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
    /* Exhaustion and projection failure are cold-path errors: the logical
       source keeps its prior publication, or black. */
    job->sh_slot = VKR_SH_SLOT_BLACK;
    uint32_t candidate_slot = VKR_SH_SLOT_BLACK;
    if (vkr_ibl_sh_pool_reserve(&renderer->sh_pool, &candidate_slot) ==
        VKR_SH_POOL_STATUS_OK) {
      if (vkr_vk_record_ibl_sh_projection(renderer, command, source,
                                          candidate_slot, job->sh_deringing) &&
          vkr_ibl_sh_pool_mark_recorded(&renderer->sh_pool, candidate_slot) ==
              VKR_SH_POOL_STATUS_OK) {
        job->sh_slot = candidate_slot;
        projected_any = true_v;
      } else {
        (void)vkr_ibl_sh_pool_abandon(&renderer->sh_pool, candidate_slot);
        log_warn("Vulkan SH projection could not record for texture %u:%u",
                 job->source.id, job->source.generation);
      }
    } else {
      log_error("Vulkan SH coefficient pool exhausted; texture %u:%u keeps its "
                "previous coefficients",
                job->source.id, job->source.generation);
    }
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
  /* A frame may consume the slot it just baked, so projection writes must be
     visible to lighting inside this same command stream. */
  if (projected_any) {
    vkr_vk_record_sh_visibility(renderer, command);
  }
  return true_v;
}

void vkr_vk_discard_ibl_bakes(VkrVulkanRenderer *renderer) {
  vkr_vk_abandon_ibl_bake_recordings(renderer);
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    const VkrVulkanPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    const VkrTextureHandle handles[] = {job->equirect, job->source,
                                        job->prefilter};
    for (uint32_t handle_index = job->convert_equirect ? 0u : 1u;
         handle_index < ArrayCount(handles); ++handle_index) {
      VkrVulkanPublishedTexture *texture =
          vkr_vk_texture_publication(renderer, handles[handle_index]);
      if (!texture || !texture->ibl_reference_count) {
        log_error("Vulkan discarded IBL bake lost texture %u:%u ownership",
                  handles[handle_index].id, handles[handle_index].generation);
        continue;
      }
      texture->ibl_reference_count--;
    }
    MemZero(&renderer->pending_ibl_bakes[i],
            sizeof(renderer->pending_ibl_bakes[i]));
  }
  renderer->pending_ibl_bake_count = 0u;
}

void vkr_vk_abandon_ibl_bake_recordings(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    VkrVulkanPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    /* Frame cancellation proves the command buffer carrying this write was not
       submitted. Keep the queued bake and its texture ownership, but return
       the candidate so the retry does not leak one pool slot per cancellation.
     */
    if (job->sh_slot != VKR_SH_SLOT_BLACK) {
      (void)vkr_ibl_sh_pool_abandon(&renderer->sh_pool, job->sh_slot);
      job->sh_slot = VKR_SH_SLOT_BLACK;
    }
    job->recorded = false_v;
  }
}
